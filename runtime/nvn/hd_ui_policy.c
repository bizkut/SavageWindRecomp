// Shared, dependency-free UI pack selection and SARC enumeration.
#include "hd_ui_policy.h"
#include <string.h>
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p+2)<<16; }
static int delta(int a, int b) { int d=a-b; return d<0?-d:d; }
int hd_ui_sarc_entry(const uint8_t *d,size_t n,uint32_t i,const char **name,size_t *name_len,
                     const uint8_t **payload,size_t *payload_size) {
    if (!d || !name || !name_len || !payload || !payload_size || n<32 || memcmp(d,"SARC",4) ||
        d[6]!=0xff || d[7]!=0xfe || memcmp(d+20,"SFAT",4)) return -1;
    uint32_t count=u16(d+26), base=u32(d+12);
    size_t names=40+(size_t)count*16;
    if (names>n || base>n) return -1;
    if (i>=count) return 0;
    const uint8_t *e=d+32+(size_t)i*16;
    uint32_t attr=u32(e+4), start=u32(e+8), end=u32(e+12);
    if (start>end || end>n-base) return -1;
    *payload=d+base+start; *payload_size=end-start; *name=NULL; *name_len=0;
    if (attr>>24) {
        size_t pos=names+(size_t)(attr&0xffffff)*4;
        if (pos>=n) return -1;
        const uint8_t *term=memchr(d+pos,0,n-pos);
        if (!term) return -1;
        *name=(const char *)d+pos; *name_len=(size_t)(term-(d+pos));
    }
    return 1;
}
int hd_ui_sarc_layout(const uint8_t *d,size_t n,uint32_t *cursor,const char **name,size_t *name_len,
                      const uint8_t **payload,size_t *payload_size) {
    if (!cursor) return -1;
    for (;;) {
        int result=hd_ui_sarc_entry(d,n,*cursor,name,name_len,payload,payload_size);
        if (result<=0) return result;
        (*cursor)++;
        if (*name && *name_len>=15 && !memcmp(*name,"Layout/",7) &&
            !memcmp(*name+*name_len-7,".sblarc",7)) return 1;
    }
}
uint32_t hd_ui_texture_scale(uint32_t w,uint32_t h) { return w<=256 && h<=256 ? 4 : 2; }
uint32_t hd_ui_generator_version(void) { return HD_UI_GENERATOR_VERSION; }
int hd_ui_source_format(uint32_t f,uint32_t *block,uint32_t *bpb,uint32_t *channels,uint32_t *nvn) {
    uint32_t bl=1, bytes=0, ch=0, nf=0;
    switch (f) {
    case 0x0201: bytes=1; ch=1; nf=0x01; break;
    case 0x0701: case 0x0901: bytes=2; ch=2; nf=0x0d; break;
    case 0x0b01: bytes=4; ch=4; nf=0x25; break;
    case 0x0b06: bytes=4; ch=4; nf=0x38; break;
    case 0x1a01: bl=4; bytes=8; ch=4; nf=0x42; break;
    case 0x1a06: bl=4; bytes=8; ch=4; nf=0x46; break;
    case 0x1c01: bl=4; bytes=16; ch=4; nf=0x44; break;
    case 0x1c06: bl=4; bytes=16; ch=4; nf=0x48; break;
    case 0x1d01: bl=4; bytes=8; ch=1; nf=0x49; break;
    case 0x1e01: bl=4; bytes=16; ch=2; nf=0x4b; break;
    default: return 0;
    }
    if (block) *block=bl; if (bpb) *bpb=bytes;
    if (channels) *channels=ch; if (nvn) *nvn=nf;
    return 1;
}
int hd_ui_texture_policy(uint32_t w,uint32_t h,uint32_t max_size,uint32_t channels,const uint8_t *p,size_t stride) {
    if (w<16 || h<16) return 1;
    if (w>max_size || h>max_size) return 2;
    if (!p || channels<1 || channels>4 || stride<(size_t)w*channels) return 3;
    double energy=0; int peak=0, gradient=1;
    for (uint32_t c=0;c<channels;c++) {
        uint64_t sumx=0,sumy=0;
        for (uint32_t y=0;y<h;y++) for (uint32_t x=0;x<w;x++) {
            int value=p[(size_t)y*stride+(size_t)x*channels+c];
            if (x) {
                int d=delta(value,p[(size_t)y*stride+(size_t)(x-1)*channels+c]); sumx+=d;
                if (d>peak) peak=d;
                if (x>1) {
                    int prior=delta(p[(size_t)y*stride+(size_t)(x-1)*channels+c],p[(size_t)y*stride+(size_t)(x-2)*channels+c]);
                    if (delta(d,prior)>1) gradient=0;
                }
            }
            if (y) {
                int d=delta(value,p[(size_t)(y-1)*stride+(size_t)x*channels+c]); sumy+=d;
                if (d>peak) peak=d;
                if (y>1) {
                    int prior=delta(p[(size_t)(y-1)*stride+(size_t)x*channels+c],p[(size_t)(y-2)*stride+(size_t)x*channels+c]);
                    if (delta(d,prior)>1) gradient=0;
                }
            }
        }
        double e=(double)sumx/((uint64_t)(w-1)*h)+(double)sumy/((uint64_t)(h-1)*w);
        if (e>energy) energy=e;
    }
    return energy<(channels==1 ? 0.45 : 0.15) || peak<=1 || gradient ? 3 : 0;
}
