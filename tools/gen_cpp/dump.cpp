// SPDX-License-Identifier: GPL-3.0-or-later
// Native counterpart to dump_py.py; all offsets are guest-image relative unless noted.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using U = uint64_t;
using Bytes = std::vector<uint8_t>;
static void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
static U get(const Bytes &b, U off, unsigned n) {
    require(off <= b.size() && n <= b.size() - off, "truncated input");
    U x = 0; for (unsigned i = 0; i < n; ++i) x |= U(b[off + i]) << (8 * i); return x;
}
static std::string hexbytes(const uint8_t *p, size_t n) {
    static const char h[] = "0123456789abcdef"; std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 15]; } return s;
}
static Bytes file(const fs::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate); require(bool(f), "cannot open file");
    auto len = f.tellg(); require(len >= 0, "cannot size file"); Bytes b(static_cast<size_t>(len));
    f.seekg(0); require(bool(f.read(reinterpret_cast<char *>(b.data()), len)), "cannot read file"); return b;
}
static void decompress(const uint8_t *src, size_t size, uint8_t *dst, size_t len) {
    size_t s = 0, d = 0;
    auto extra = [&](size_t x) { if (x == 15) { uint8_t v; do { require(s < size, "truncated lz4 length"); v = src[s++]; x += v; } while (v == 255); } return x; };
    while (s < size) {
        unsigned tok = src[s++]; size_t n = extra(tok >> 4);
        require(n <= size - s && n <= len - d, "invalid lz4 literals");
        std::memcpy(dst + d, src + s, n); s += n; d += n;
        if (s == size) break;
        require(size - s >= 2, "truncated lz4 offset"); size_t off = src[s] | (size_t(src[s+1]) << 8); s += 2;
        require(off && off <= d, "invalid lz4 offset"); n = extra(tok & 15) + 4;
        require(n <= len - d, "invalid lz4 match"); for (size_t i = 0; i < n; ++i) dst[d+i] = dst[d+i-off]; d += n;
    }
    require(d == len, "wrong lz4 decoded size");
}
struct Header {
    std::array<U,3> off{}, size{}, fileoff{}, compressed{}; U bss = 0, flags = 0; std::string id;
    explicit Header(const Bytes &r) {
        require(r.size() >= 0x100 && !std::memcmp(r.data(), "NSO0", 4), "invalid NSO header");
        flags = get(r,12,4); bss = get(r,0x3c,4); id = hexbytes(r.data()+0x40,32);
        for (int i=0;i<3;i++) { fileoff[i]=get(r,0x10+i*16,4); off[i]=get(r,0x14+i*16,4); size[i]=get(r,0x18+i*16,4); compressed[i]=get(r,0x60+i*4,4); }
    }
    U image_size() const { U end=std::max({off[0]+size[0],off[1]+size[1],off[2]+size[2]+bss}); return (end+4095)&~U(4095); }
};
struct Layout { std::string name; U base, size; std::string id; };
static std::vector<Layout> layout(const fs::path &dir) {
    std::vector<std::string> names{"main"}; for (int i=0;i<10;i++) names.push_back("subsdk"+std::to_string(i)); names.push_back("sdk");
    require(fs::is_regular_file(dir/"main") && fs::is_regular_file(dir/"sdk"), "main or sdk absent");
    Header main(file(dir/"main")); bool known = main.id.substr(0,16)=="de0b6ac4efc1dda7" || main.id.substr(0,16)=="cd57b23fa4bbad65";
    U base=0x7000000000ULL; std::vector<Layout> out;
    for (const auto &name:names) {
        if (!fs::is_regular_file(dir/name) || (known && name.compare(0,6,"subsdk")==0 && name!="subsdk0")) continue;
        Header h(file(dir/name)); U size=h.image_size(); require(size <= 0x7080000000ULL-base, "layout exceeds module region");
        out.push_back({name,base,size,h.id}); base+=size;
    }
    return out;
}
struct Symbol { std::string name; U info, shndx, value, size; };
struct Reloc { U off, type, sym, add; };
struct Nso {
    Header h; Bytes image; U bss, dynamic, bss_start, bss_end, eh_start, eh_end, module_obj;
    std::map<U,U> dyn; std::vector<std::string> needed; std::vector<Symbol> syms; std::vector<Reloc> relocs;
    explicit Nso(const Bytes &raw):h(raw), bss(h.bss) {
        // Match Nso's image sizing (last segment's end, rather than max header extent).
        U end=h.off[2]+h.size[2]+bss; require(end <= std::numeric_limits<size_t>::max()-4095, "image too large"); image.resize((end+4095)&~U(4095));
        for(int i=0;i<3;i++) {
            require(h.fileoff[i]<=raw.size() && h.compressed[i]<=raw.size()-h.fileoff[i], "truncated segment");
            require(h.off[i]<=image.size() && h.size[i]<=image.size()-h.off[i], "segment outside image");
            if (h.flags & (1<<i)) decompress(raw.data()+h.fileoff[i], h.compressed[i], image.data()+h.off[i], h.size[i]);
            else { require(h.compressed[i]==h.size[i], "uncompressed segment size mismatch"); std::memcpy(image.data()+h.off[i],raw.data()+h.fileoff[i],h.size[i]); }
        }
        U mod=u32(4); require(mod<=image.size() && image.size()-mod>=28 && !std::memcmp(image.data()+mod,"MOD0",4), "invalid MOD0");
        auto rel=[&](int i)->U { return mod + int64_t(int32_t(u32(mod+4+i*4))); };
        dynamic=rel(0); bss_start=rel(1); bss_end=rel(2); eh_start=rel(3); eh_end=rel(4); module_obj=rel(5);
        std::vector<U> needed_offsets;
        for(U p=dynamic;;p+=16) { U tag=u64(p), val=u64(p+8); if (!tag) break; if(tag==1) needed_offsets.push_back(val); dyn.emplace(tag,val); }
        U st=dyn.count(5)?dyn.at(5):0; for(U n:needed_offsets) needed.push_back(cstr(st+n));
        if(dyn.count(6)) {
            U count=0;
            if(dyn.count(4)) count=u32(dyn.at(4)+4);
            else if(dyn.count(0x6ffffef5)) {
                U p=dyn.at(0x6ffffef5), nb=u32(p), symoff=u32(p+4), bloom=u32(p+8), buckets=p+16+8*bloom, last=0;
                for(U i=0;i<nb;i++) last=std::max(last,u32(buckets+4*i));
                if(last<symoff) count=symoff; else { U chain=buckets+4*nb; while(!(u32(chain+4*(last-symoff))&1)) ++last; count=last+1; }
            }
            for(U i=0;i<count;i++) { U p=dyn.at(6)+24*i; syms.push_back({cstr(st+u32(p)),get(image,p+4,1),get(image,p+6,2),u64(p+8),u64(p+16)}); }
        }
        for (auto tags: {std::pair<U,U>{7,8},{23,2}}) if(dyn.count(tags.first)) {
            require(dyn.count(tags.second), "relocation size missing");
            for(U i=0;i<dyn.at(tags.second)/24;i++) { U p=dyn.at(tags.first)+i*24, info=u64(p+8); relocs.push_back({u64(p),info&0xffffffff,info>>32,u64(p+16)}); }
        }
        if(dyn.count(36)) {
            U where=0; bool have=false; U count=dyn.count(35)?dyn.at(35)/8:0;
            for(U i=0;i<count;i++) {
                U e=u64(dyn.at(36)+8*i);
                if(!(e&1)) { relocs.push_back({e,1027,0,u64(e)}); where=e+8; have=true; }
                else { require(have,"invalid RELR bitmap"); for(int b=0;b<63;b++) if((e>>(b+1))&1) relocs.push_back({where+8*b,1027,0,u64(where+8*b)}); where+=63*8; }
            }
        }
    }
    U u32(U p) const { return get(image,p,4); } U u64(U p) const { return get(image,p,8); }
    std::string cstr(U p) const { require(p<image.size(),"string outside image"); U e=p; while(e<image.size() && image[e]) ++e; require(e<image.size(),"unterminated string"); return std::string(reinterpret_cast<const char *>(image.data()+p),e-p); }
    std::pair<U,U> encoded(U p, unsigned enc, U base) const {
        unsigned fmt=enc&15, sz; U v;
        if(fmt==0xb || fmt==3) { sz=4; v=u32(p); if(fmt==0xb) v=U(int64_t(int32_t(v))); }
        else if(fmt==0xc || fmt==4) { sz=8; v=u64(p); }
        else throw std::runtime_error("unsupported unwind encoding");
        if((enc&0x70)==0x10) v+=p; else if((enc&0x70)==0x30) v+=base; return {v,sz};
    }
    U leb(U &p) const { U v=0; unsigned shift=0; for(;;) { U b=get(image,p++,1); require(shift<64,"invalid leb128"); v|=(b&127)<<shift; shift+=7; if(!(b&128)) return v; } }
    std::vector<std::pair<U,U>> unwind_table() const {
        std::vector<std::pair<U,U>> out; if(!eh_start || eh_start>=image.size()) return out;
        U p=eh_start+4; unsigned ptr=get(image,eh_start+1,1), cnt=get(image,eh_start+2,1), table=get(image,eh_start+3,1);
        p+=encoded(p,ptr,eh_start).second; auto count=encoded(p,cnt,eh_start); p+=count.second;
        for(U i=0;i<count.first;i++) { auto loc=encoded(p,table,eh_start); p+=loc.second; auto fde=encoded(p,table,eh_start); p+=fde.second; out.emplace_back(loc.first,fde.first); } return out;
    }
    std::vector<std::pair<U,U>> extents(const std::vector<std::pair<U,U>> &table) const {
        std::vector<std::pair<U,U>> out; std::map<U,unsigned> encs;
        try { for(auto [loc,fde]:table) {
            U cie=fde+4-u32(fde+4); if(!encs.count(cie)) {
                U p=cie+8, version=get(image,p,1); std::string aug=cstr(p+1); p+=2+aug.size(); leb(p); leb(p); if(version==1) ++p; else leb(p);
                unsigned enc=0; if(!aug.empty() && aug[0]=='z') { leb(p); for(size_t i=1;i<aug.size();i++) {
                    if(aug[i]=='R') enc=get(image,p++,1); else if(aug[i]=='P') { unsigned e=get(image,p,1); p+=1+encoded(p+1,e,0).second; } else if(aug[i]=='L') ++p;
                }} encs[cie]=enc;
            }
            unsigned enc=encs.at(cie); auto pc=encoded(fde+8,enc,0); auto size=encoded(fde+8+pc.second,enc&15,0); if(pc.first==loc) out.emplace_back(loc,loc+size.first);
        }} catch(const std::runtime_error &) { return {}; } return out;
    }
};
static U fnv(const uint8_t *p, U n) { U h=14695981039346656037ULL; for(U i=0;i<n;i++) h=(h^p[i])*1099511628211ULL; return h; }
int main(int argc,char **argv) try {
    require(argc==4,"usage: dump EXEFS LAYOUT MODULE"); fs::path dir=argv[1]; auto lay=layout(dir);
    std::ifstream lf(argv[2]); require(bool(lf),"cannot open layout"); std::vector<Layout> expected; std::string line;
    while(std::getline(lf,line)) { if(line.empty() || line[0]=='#') continue; std::istringstream s(line); Layout l; std::string b,z; require(bool(s>>l.name>>b>>z>>l.id),"bad layout record"); l.base=std::stoull(b,nullptr,16); l.size=std::stoull(z,nullptr,16); expected.push_back(l); }
    require(lay.size()==expected.size(),"layout mismatch");
    for(size_t i=0;i<lay.size();i++) require(lay[i].name==expected[i].name && lay[i].base==expected[i].base && lay[i].size==expected[i].size && lay[i].id==expected[i].id,"layout mismatch");
    std::cout<<std::hex; for(auto &l:lay) std::cout<<"layout "<<l.name<<' '<<l.base<<' '<<l.size<<' '<<l.id<<'\n';
    auto it=std::find_if(lay.begin(),lay.end(),[&](const Layout &l){return l.name==argv[3];}); require(it!=lay.end(),"module absent in layout");
    Nso n(file(dir/it->name)); require(n.h.id==it->id,"build id mismatch"); U base=it->base;
    std::cout<<"module "<<it->name<<' '<<base<<' '<<n.image.size()<<' '<<n.bss<<'\n';
    const char *seg[]={"text","rodata","data"}; for(int i=0;i<3;i++) std::cout<<"segment "<<seg[i]<<' '<<n.h.off[i]<<' '<<n.h.size[i]<<' '<<std::setw(16)<<std::setfill('0')<<fnv(n.image.data()+n.h.off[i],n.h.size[i])<<std::setfill(' ')<<'\n';
    std::cout<<"mod0 "<<n.dynamic<<' '<<n.bss_start<<' '<<n.bss_end<<' '<<n.eh_start<<' '<<n.eh_end<<' '<<n.module_obj<<'\n';
    for(auto [tag,val]:n.dyn) std::cout<<"dynamic "<<tag<<' '<<val<<'\n';
    for(auto &s:n.needed) std::cout<<"needed "<<hexbytes(reinterpret_cast<const uint8_t *>(s.data()),s.size())<<'\n';
    for(auto &s:n.syms) { auto name=hexbytes(reinterpret_cast<const uint8_t *>(s.name.data()),s.name.size()); std::cout<<"symbol "<<(name.empty()?"-":name)<<' '<<s.info<<' '<<s.shndx<<' '<<s.value<<' '<<s.size<<'\n'; }
    for(auto &r:n.relocs) std::cout<<"reloc "<<r.off<<' '<<r.type<<' '<<r.sym<<' '<<r.add<<'\n';
    auto table=n.unwind_table(); auto extents=n.extents(table); for(auto [a,b]:table) std::cout<<"eh "<<a<<'\n'; for(auto [a,b]:extents) std::cout<<"fde "<<a<<' '<<b<<'\n';
    std::set<U> starts; auto add=[&](U a) { U lo=base+n.h.off[0], hi=lo+n.h.size[0]; if(a>=lo && a<hi && !(a&3)) starts.insert(a); };
    for(auto [a,b]:table) add(base+a);
    for(auto &s:n.syms) if(s.shndx && (s.info&15)==2) add(base+s.value);
    for(auto &r:n.relocs) if(r.type==1027) add(base+r.add);
    for(U a:starts) std::cout<<"boundary "<<a<<'\n';
    return 0;
} catch(const std::exception &e) { std::cerr<<"dump: "<<e.what()<<'\n'; return 1; }
