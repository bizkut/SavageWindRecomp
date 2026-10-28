#!/usr/bin/env python3
"""Generate a user-owned layout texture pack from extracted merged RomFS pack archives."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import tarfile
import sys
import urllib.request
import zipfile
from PIL import Image

URL = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip'
SHA256 = 'e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96'
BC7_URL = 'https://codeload.github.com/richgel999/bc7enc_rdo/tar.gz/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0'
BC7_SHA = 'bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6'
WIN_SHA = 'abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d'
SELECT = ('Nt_HeartDamage_00', 'Nt_HeartDamageB_00', 'Nt_HeartDamage_02', 'Nt_HeartNewMasterS_00',
          'Nt_HeartNewMasterS_01', 'Nt_HeartNewSh_00', 'Nt_GanbariLnn_00', 'Nt_GanbariLnnSpd_00',
          'Nt_GanbariSnn_00', 'Nt_GanbariSnnFull_00', 'Nt_GanbariExNN_00', 'Nt_GanbariExNNSpd_00')
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]
policy=None
def setup_policy(cache):
    global policy
    source=Path(__file__).resolve().parent.parent/'runtime/nvn/hd_ui_policy.c'
    header=source.with_suffix('.h')
    digest=hashlib.sha256(source.read_bytes()+header.read_bytes()).hexdigest()[:16]
    library=cache/('hd-ui-policy-'+digest+('.dll' if sys.platform=='win32' else '.so'))
    if not library.exists():
        subprocess.run([os.environ.get('CC','cc'),'-O2','-shared','-fPIC',str(source),'-o',str(library)],check=True)
    policy=ctypes.CDLL(str(library))
    policy.hd_ui_sarc_entry.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_size_t)]
    policy.hd_ui_sarc_layout.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_size_t)]
    policy.hd_ui_texture_policy.argtypes=[ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint32,
        ctypes.c_uint32,ctypes.c_void_p,ctypes.c_size_t]
    policy.hd_ui_texture_scale.argtypes=[ctypes.c_uint32,ctypes.c_uint32]
    policy.hd_ui_generator_version.restype=ctypes.c_uint32
    policy.hd_ui_source_format.argtypes=[ctypes.c_uint32]+[ctypes.POINTER(ctypes.c_uint32)]*4
def source_format(fmt):
    values=[ctypes.c_uint32() for _ in range(4)]
    if not policy.hd_ui_source_format(fmt,*(ctypes.byref(v) for v in values)):
        raise ValueError(f'unsupported BNTX format {fmt:#x}')
    return tuple(v.value for v in values)
def sarc(b, layouts=False):
    if policy is None: raise RuntimeError('shared policy is not initialized')
    raw=ctypes.c_char_p(b); cursor=ctypes.c_uint32(0)
    while True:
        name=ctypes.c_void_p(); length=ctypes.c_size_t(); payload=ctypes.c_void_p(); size=ctypes.c_size_t()
        if layouts:
            result=policy.hd_ui_sarc_layout(raw,len(b),ctypes.byref(cursor),ctypes.byref(name),ctypes.byref(length),ctypes.byref(payload),ctypes.byref(size))
        else:
            result=policy.hd_ui_sarc_entry(raw,len(b),cursor.value,ctypes.byref(name),ctypes.byref(length),ctypes.byref(payload),ctypes.byref(size))
            cursor.value+=1
        if result<0: raise ValueError('invalid SARC')
        if not result: break
        if name.value: yield ctypes.string_at(name,length.value).decode(),ctypes.string_at(payload,size.value)
def yaz(b):
    if b[:4] != b'Yaz0': return b
    n, pos, out = int.from_bytes(b[4:8], 'big'), 16, bytearray()
    while len(out) < n:
        flags = b[pos]; pos += 1
        for bit in range(8):
            if flags & (128 >> bit): out.append(b[pos]); pos += 1
            else:
                a, c = b[pos:pos + 2]; pos += 2
                dist, length = ((a & 15) << 8 | c) + 1, a >> 4
                if not length: length = b[pos] + 18; pos += 1
                else: length += 2
                for _ in range(length): out.append(out[-dist])
            if len(out) >= n: break
    return bytes(out)
def deswizzle(src, w, h, bpb, bh, block=4):
    wb, hb = (w + block - 1) // block, (h + block - 1) // block
    while bh and (8 << (bh - 1)) >= hb: bh -= 1
    row, ng = wb * bpb, (wb * bpb + 63) // 64
    out = bytearray(row * hb)
    for y in range(hb):
        for x in range(0, row, 16):
            gx, gy = x >> 6, y >> 3
            off = ((gy >> bh) * ng + gx) * (512 << bh) + (gy & ((1 << bh) - 1)) * 512
            xi, yi = x & 63, y & 7
            off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 15)
            take = min(16, row-x); out[y*row+x:y*row+x+take] = src[off:off+take]
    return bytes(out)
def content_hash(data):
    h=0xcbf29ce484222325
    for byte in data: h=((h^byte)*0x100000001b3)&0xffffffffffffffff
    return h
def unpack_raw(data,w,h,channels):
    return [Image.frombytes('L',(w,h),data[c::channels]) for c in range(channels)]
def palette(a, b):
    return [a,b] + ([((7-i)*a+i*b)//7 for i in range(1,7)] if a>b else [((5-i)*a+i*b)//5 for i in range(1,5)] + [0,255])
def unpack_bc4(block):
    p=palette(block[0],block[1]); bits=int.from_bytes(block[2:8], 'little')
    return [p[(bits>>(3*i))&7] for i in range(16)]
def unpack(data,w,h,channels):
    planes=[bytearray(w*h) for _ in range(channels)]; bw=(w+3)//4
    for by in range((h+3)//4):
        for bx in range(bw):
            block=data[(by*bw+bx)*8*channels:(by*bw+bx+1)*8*channels]
            for c in range(channels):
                vals=unpack_bc4(block[c*8:c*8+8])
                for i,v in enumerate(vals):
                    x,y=bx*4+i%4,by*4+i//4
                    if x<w and y<h: planes[c][y*w+x]=v
    return [Image.frombytes('L',(w,h),bytes(p)) for p in planes]
def unpack_colour(data, w, h, fmt):
    out=bytearray(w*h*4)
    if fmt in (0x0b01,0x0b06):
        for i in range(w*h):
            out[4*i:4*i+4]=data[4*i:4*i+4]
    else:
        bw=(w+3)//4; blocksize=8 if fmt in (0x1a01,0x1a06) else 16
        for by in range((h+3)//4):
            for bx in range(bw):
                block=data[(by*bw+bx)*blocksize:(by*bw+bx+1)*blocksize]
                alpha=unpack_bc4(block[:8]) if blocksize==16 else [255]*16
                c=block[-8:]; a=int.from_bytes(c[:2],'little'); b=int.from_bytes(c[2:4],'little')
                def rgb(v): return (((v>>11)&31)*255//31,((v>>5)&63)*255//63,(v&31)*255//31)
                p0,p1=rgb(a),rgb(b); opaque=blocksize==16 or a>b
                palette_rgb=[p0,p1,tuple((2*x+y)//3 if opaque else (x+y)//2 for x,y in zip(p0,p1)),
                             tuple((x+2*y)//3 if opaque else 0 for x,y in zip(p0,p1))]
                indices=int.from_bytes(c[4:8],'little')
                for i in range(16):
                    x,y=bx*4+i%4,by*4+i//4
                    if x>=w or y>=h: continue
                    k=(indices>>(2*i))&3; a8=0 if not opaque and k==3 else alpha[i]
                    out[4*(y*w+x):4*(y*w+x)+4]=bytes((*palette_rgb[k],a8))
    return Image.frombytes('RGBA',(w,h),bytes(out))

def texture_policy(planes, max_size):
    w,h=planes[0].size
    mode={1:'L',2:'LA',4:'RGBA'}[len(planes)]
    pixels=(planes[0] if len(planes)==1 else Image.merge(mode,planes)).tobytes()
    return policy.hd_ui_texture_policy(w,h,max_size,len(planes),ctypes.c_char_p(pixels),w*len(planes))
def bc4(image):
    w,h=image.size; pixels=image.load(); output=bytearray()
    for by in range((h+3)//4):
        for bx in range((w+3)//4):
            v=[pixels[min(w-1,bx*4+i%4),min(h-1,by*4+i//4)] for i in range(16)]
            a,b=max(v),min(v); p=palette(a,b); bits=0
            for i,x in enumerate(v): bits |= min(range(8),key=lambda k:abs(p[k]-x)) << (3*i)
            output.extend(bytes((a,b))+bits.to_bytes(6,'little'))
    return bytes(output)
def dds(planes, bc7_exe=None, srgb=False):
    original=planes[0].size; w,h=original; channels=len(planes); mips=[]
    while True:
        if bc7_exe:
            rgba=Image.merge('RGBA',planes)
            mips.append(subprocess.run([str(bc7_exe),str(w),str(h)],input=rgba.tobytes(),stdout=subprocess.PIPE,check=True).stdout)
        else:
            encoded=[bc4(im) for im in planes]
            if channels==1: mips.append(encoded[0])
            else: mips.append(b''.join(encoded[0][i:i+8]+encoded[1][i:i+8] for i in range(0,len(encoded[0]),8)))
        if w==1 and h==1: break
        w,h=max(1,w//2),max(1,h//2)
        planes=[p.resize((w,h),Image.Resampling.LANCZOS) for p in planes]
    w,h=original
    hdr=bytearray(148); hdr[:4]=b'DDS '; struct.pack_into('<I',hdr,4,124); struct.pack_into('<I',hdr,8,0xa1007)
    struct.pack_into('<II',hdr,12,h,w); struct.pack_into('<I',hdr,20,len(mips[0])); struct.pack_into('<I',hdr,28,len(mips))
    struct.pack_into('<I',hdr,76,32); struct.pack_into('<I',hdr,80,4); hdr[84:88]=b'DX10'
    dxgi=(99 if srgb else 98) if bc7_exe else 80 if channels==1 else 83
    struct.pack_into('<I',hdr,108,0x401008); struct.pack_into('<IIIII',hdr,128,dxgi,3,0,1,0)
    return bytes(hdr)+b''.join(mips)
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('packs',type=Path,help='directory of archives from savagewind-hd-ui-extract')
    ap.add_argument('output',type=Path)
    ap.add_argument('--slice',action='store_true',help='only the heart/stamina test slice')
    ap.add_argument('--reuse-existing',action='store_true',help='resume a previous generation in this output directory')
    ap.add_argument('--reuse-from',type=Path,help='reuse validated images from another private pack directory')
    ap.add_argument('--max-size',type=int,default=1024,help='skip source textures larger than this (default 1024)')
    ap.add_argument('--cache',type=Path,default=Path.home()/'.cache/savagewind/hd-ui')
    args=ap.parse_args(); args.cache.mkdir(parents=True,exist_ok=True)
    if args.max_size<16: ap.error('--max-size must be at least 16')
    setup_policy(args.cache)
    windows=sys.platform=='win32'; platform='windows' if windows else 'ubuntu'
    archive=args.cache/f'realesrgan-20220424-{platform}.zip'
    url=URL.replace('ubuntu.zip',f'{platform}.zip')
    if not archive.exists(): urllib.request.urlretrieve(url,archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=(WIN_SHA if windows else SHA256): raise ValueError('Real-ESRGAN archive SHA-256 mismatch')
    exe=args.cache/('realesrgan-ncnn-vulkan.exe' if windows else 'realesrgan-ncnn-vulkan')
    if not exe.exists():
        with zipfile.ZipFile(archive) as z: z.extractall(args.cache)
        if not windows: exe.chmod(0o755)
    source=args.cache/'bc7enc-rdo.tar.gz'
    if not source.exists(): urllib.request.urlretrieve(BC7_URL,source)
    if hashlib.sha256(source.read_bytes()).hexdigest()!=BC7_SHA: raise ValueError('bc7enc source SHA-256 mismatch')
    encsrc=args.cache/'bc7enc-src'; encoder=args.cache/('hd-ui-bc7.exe' if windows else 'hd-ui-bc7')
    if not encoder.exists():
        encsrc.mkdir(exist_ok=True)
        with tarfile.open(source,'r:gz') as tar:
            for name in ('bc7enc.h','bc7enc.cpp'):
                member=next(m for m in tar.getmembers() if m.name.endswith('/'+name))
                (encsrc/name).write_bytes(tar.extractfile(member).read())
        wrapper=Path(__file__).with_name('hd_ui_bc7.cpp')
        subprocess.run([os.environ.get('CXX','c++'),'-O2','-std=c++17','-I'+str(encsrc),str(wrapper),str(encsrc/'bc7enc.cpp'),'-o',str(encoder)],check=True)
    args.output.mkdir(parents=True,exist_ok=True); records=[]; seen={}; started=time.monotonic()
    manifest=args.output/'manifest.json'
    previous={}; previous_rows=[]
    if manifest.exists():
        old=json.loads(manifest.read_text())
        if old.get('version')!=1: raise ValueError('unsupported manifest version')
        previous_rows=old['textures']; previous={r['hash']:r for r in previous_rows}
    def publish(final=False):
        tmp=manifest.with_suffix('.json.tmp')
        current={(r['hash'],r['archive'],r['name']) for r in records}
        retained=[] if final else [r for r in previous_rows if (r['hash'],r['archive'],r['name']) not in current]
        tmp.write_text(json.dumps({'version':1,'generator_version':policy.hd_ui_generator_version(),'textures':records+retained},indent=2)+'\n')
        os.replace(tmp,manifest)
    entries=[]
    for pack in sorted(args.packs.glob('*.pack')):
        for layout,compressed in sarc(pack.read_bytes(),layouts=True):
            for bname,bntx in sarc(yaz(compressed)):
                if not bname.lower().endswith('.bntx'): continue
                for i in range(u32(bntx,0x24)):
                    off=u64(bntx,u64(bntx,0x28)+8*i); np=u64(bntx,off+0x60)
                    name=bntx[np+2:np+2+u16(bntx,np)].decode()
                    if args.slice and name.split('^')[0] not in SELECT: continue
                    hud=any(word in name.lower() for word in ('heart','ganbari','stamina','minimap','icon'))
                    entries.append((0 if hud else 1,pack.name,layout,name,bntx,off))
    entries.sort(key=lambda e:(e[0],e[1],e[2],e[3]))
    skipped={1:0,2:0,3:0}; generated=0; elapsed_generation=0.0
    with tempfile.TemporaryDirectory(prefix='savagewind-hdui-') as temp:
        temp=Path(temp); inp=temp/'input.png'; out=temp/'output.png'
        for number,(_,pack,layout,name,bntx,off) in enumerate(entries,1):
            item_start=time.monotonic()
            fmt=u32(bntx,off+0x1c); w,h=u32(bntx,off+0x24),u32(bntx,off+0x28)
            block,bpb,channels,_=source_format(fmt); scale=policy.hd_ui_texture_scale(w,h)
            reason=policy.hd_ui_texture_policy(w,h,args.max_size,channels,None,0)
            if reason in (1,2):
                skipped[reason]+=1
            else:
                reason=0
                if not w or not h or max(w,h)*scale>4096 or u32(bntx,off+0x30)!=1: raise ValueError(f'unsupported texture dimensions: {name}')
                pos=u64(bntx,u64(bntx,off+0x70))
                data=deswizzle(bntx[pos:],w,h,bpb,u32(bntx,off+0x34)&7,block)
                key=content_hash(data)
                identity=(fmt,w,h,hashlib.sha256(data).digest()); key_text=f'{key:016x}'
                if key in seen:
                    previous_identity,record=seen[key]
                    if previous_identity!=identity: raise ValueError(f'content hash collision: {name}')
                    records.append({**record,'archive':f'Pack/{pack}:{layout}','name':name}); publish()
                else:
                    if fmt in (0x1d01,0x1e01): planes=unpack(data,w,h,channels)
                    elif channels<4: planes=unpack_raw(data,w,h,channels)
                    else: planes=list(unpack_colour(data,w,h,fmt).split())
                    reason=texture_policy(planes,args.max_size)
                    if reason:
                        old=previous.get(key_text); output=args.output/f'{key_text}.dds'
                        expected_format={1:'BC4',2:'BC5',4:'BC7'}[channels]
                        if old and old.get('format')!=expected_format and output.exists():
                            if (old.get('source_format')!=f'{fmt:04x}' or old.get('width')!=w or old.get('height')!=h or
                                old.get('sha256')!=hashlib.sha256(output.read_bytes()).hexdigest()):
                                raise ValueError(f'existing manifest mismatch: {output}')
                            output.unlink()
                        skipped[reason]+=1
                    else:
                        output=args.output/f'{key_text}.dds'; old=previous.get(key_text); blob=None; stale=False
                        candidate=output if output.exists() and (args.reuse_existing or old) else (
                            args.reuse_from/f'{key_text}.dds' if args.reuse_from else None)
                        if candidate and candidate.exists():
                            blob=candidate.read_bytes(); ow,oh=w*scale,h*scale
                            dxgi=80 if channels==1 else 83 if channels==2 else 99 if fmt in (0x0b06,0x1a06,0x1c06) else 98
                            tw,th,levels,size=ow,oh,0,148
                            while True:
                                levels+=1; size+=((tw+3)//4)*((th+3)//4)*(8 if channels==1 else 16)
                                if tw==th==1: break
                                tw=max(1,tw//2); th=max(1,th//2)
                            if old and (old['source_format']!=f'{fmt:04x}' or old['width']!=w or old['height']!=h or old['sha256']!=hashlib.sha256(blob).hexdigest()):
                                raise ValueError(f'existing manifest mismatch: {output}')
                            stale=bool(old and old.get('format')!={1:'BC4',2:'BC5',4:'BC7'}[channels])
                            if stale:
                                blob=None
                            elif len(blob)!=size or blob[:4]!=b'DDS ' or u32(blob,12)!=oh or u32(blob,16)!=ow or u32(blob,28)!=levels or u32(blob,128)!=dxgi:
                                raise ValueError(f'invalid existing output: {output}')
                            if blob is not None and candidate!=output:
                                tmp=output.with_suffix('.dds.tmp'); tmp.write_bytes(blob); os.replace(tmp,output)
                        if blob is None:
                            high=[]
                            if channels==4:
                                Image.merge('RGB',planes[:3]).save(inp)
                                subprocess.run([str(exe),'-i',str(inp),'-o',str(out),'-n','realesr-animevideov3','-s',str(scale),'-t','200','-m',str(args.cache/'models')],check=True)
                                high=list(Image.open(out).convert('RGB').split()); planes=planes[3:]
                            for img in planes:
                                img.convert('RGB').save(inp)
                                subprocess.run([str(exe),'-i',str(inp),'-o',str(out),'-n','realesr-animevideov3','-s',str(scale),'-t','200','-m',str(args.cache/'models')],check=True)
                                high.append(Image.open(out).convert('L').copy())
                            blob=dds(high,encoder if channels==4 else None,fmt in (0x0b06,0x1a06,0x1c06))
                            if output.exists() and not stale and output.read_bytes()!=blob: raise ValueError(f'existing output differs: {output}')
                            tmp=output.with_suffix('.dds.tmp'); tmp.write_bytes(blob); os.replace(tmp,output)
                            generated+=1; elapsed_generation+=time.monotonic()-item_start
                        record=dict(hash=key_text,archive=f'Pack/{pack}:{layout}',name=name,source_format=f'{fmt:04x}',width=w,height=h,output_width=w*scale,output_height=h*scale,format={1:'BC4',2:'BC5',4:'BC7'}[channels],sha256=hashlib.sha256(blob).hexdigest(),bytes=len(blob))
                        records.append(record); seen[key]=(identity,record); publish()
            print(f'{number}/{len(entries)} {name} {time.monotonic()-item_start:.2f}s'+(f' skipped={reason}' if reason else ''),flush=True)
            if number%25==0 and generated:
                eta=(len(entries)-number)*elapsed_generation/generated
                print(f'ETA ~{eta/60:.1f} minutes (mean generated texture {elapsed_generation/generated:.2f}s)',flush=True)
    publish(final=True)
    print(f'{len(records)} entries; skipped tiny={skipped[1]} oversized={skipped[2]} low-edge={skipped[3]}; {time.monotonic()-started:.1f}s',flush=True)
if __name__=='__main__': main()
