// Runtime-only indexing and edge-directed enlargement of selected user-owned layout textures.
#include "sharp_ui.h"
#include "tegra.h"
#include "../rt.h"
#include "../framepace.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static uint64_t hash(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
typedef struct { uint64_t hash; uint32_t w, h, format; char name[64]; } Entry;
static Entry entries[128];
static size_t nentries;
static bool chosen(const char *s, size_t n) {
    static const char *const names[] = {"Logo_W_00", "Logo3DGameTitle_00_CN",
        "Logo3DGameTitle_00_JPja", "Logo3DGameTitle_00_TW", "TitleBGLink_00", "TitleBGMainA_00",
        "TitleBGMainA_01", "TitleBGMainB_00", "TitleBGMainB_01", "TitleBG_00", "TitleBG_01",
        "TitleBG_02", "TitleBG_03", "TitleBG_04"};
    const char *suffix = memchr(s, '^', n);
    size_t base = suffix ? (size_t)(suffix - s) : n;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i]) == base && !memcmp(s, names[i], base)) return true;
    return false;
}
static uint32_t nvn_format(uint32_t f) {
    switch (f) { case 0x0201: return 0x01; case 0x0b06: return 0x38;
    case 0x1a06: return 0x46; case 0x1c06: return 0x48; default: return 0; }
}
void sharp_ui_register(const uint8_t *b, size_t n) {
    if (n < 0x30 || memcmp(b, "BNTX", 4)) return;
    uint32_t count = u32(b + 0x24); uint64_t table = u64(b + 0x28);
    if (table > n || count > (n - table) / 8) return;
    for (uint32_t i = 0; i < count && nentries < sizeof entries / sizeof *entries; i++) {
        uint64_t off = u64(b + table + 8 * i);
        if (off > n || n - off < 0x78 || memcmp(b + off, "BRTI", 4)) continue;
        const uint8_t *r = b + off;
        uint64_t np = u64(r + 0x60), offsets = u64(r + 0x70);
        if (np > n || n - np < 2 || offsets > n || n - offsets < 8) continue;
        uint32_t len = u16(b + np); if (len > n - np - 2 || len >= 64 || !chosen((const char *)b + np + 2, len)) continue;
        uint32_t format = nvn_format(u32(r + 0x1c)), w = u32(r + 0x24), h = u32(r + 0x28);
        if (!format || !w || !h || w > 4096 || h > 4096 || u32(r + 0x30) != 1) continue;
        TexLayout t = {.width=w, .height=h, .depth=1, .layers=1, .levels=1, .fmt=nvn_format_info(format),
                       .block_height_log2=u32(r + 0x34) & 7};
        if (t.block_height_log2 > 4) continue;
        LevelLayout l = tegra_level(&t, 0); uint64_t data = u64(b + offsets), storage = tegra_layer_size(&t);
        size_t linear_size = (size_t)l.wb * l.hb * t.fmt->bpb;
        if (data > n || storage > n - data) continue;
        uint8_t *linear = malloc(linear_size); if (!linear) continue;
        tegra_deswizzle(b + data, linear, l.wb, l.hb, 1, t.fmt->bpb, l.block_height_log2, 0);
        uint64_t hsh = hash(linear, linear_size); free(linear);
        size_t j; for (j = 0; j < nentries; j++) if (entries[j].hash == hsh && entries[j].format == format && entries[j].w == w && entries[j].h == h) break;
        if (j == nentries) {
            Entry *e = &entries[nentries++]; *e = (Entry){.hash=hsh,.w=w,.h=h,.format=format};
            memcpy(e->name, b + np + 2, len); e->name[len] = 0;
            LOG("[sharp-ui] indexed %s %ux%u format=%#x hash=%016llx\n", e->name, w, h, format, (unsigned long long)hsh);
        }
    }
}

static void bc_alpha(const uint8_t *b, uint8_t v[16]) {
    uint8_t p[8] = {b[0], b[1]};
    if (p[0] > p[1]) for (int i=1;i<7;i++) p[i+1]=((7-i)*p[0]+i*p[1])/7;
    else { for (int i=1;i<5;i++) p[i+1]=((5-i)*p[0]+i*p[1])/5; p[6]=0; p[7]=255; }
    uint64_t bits=0; for (int i=0;i<6;i++) bits |= (uint64_t)b[2+i] << (8*i);
    for (int i=0;i<16;i++) v[i]=p[(bits>>(3*i))&7];
}
static void bc_color(const uint8_t *b, uint8_t p[4][4], bool opaque) {
    uint16_t a=u16(b), c=u16(b+2);
    for (int i=0;i<2;i++) { uint16_t x=i?c:a; p[i][0]=((x>>11)&31)*255/31; p[i][1]=((x>>5)&63)*255/63; p[i][2]=(x&31)*255/31; p[i][3]=255; }
    for (int k=0;k<3;k++) { p[2][k]=a>c||opaque ? (2*p[0][k]+p[1][k])/3 : (p[0][k]+p[1][k])/2; p[3][k]=a>c||opaque ? (p[0][k]+2*p[1][k])/3 : 0; }
    p[2][3]=255; p[3][3]=a>c||opaque ? 255 : 0;
}
static void decode(const uint8_t *in, uint8_t *out, uint32_t w, uint32_t h, uint32_t f) {
    if (f == 0x01 || f == 0x38) {
        for (size_t i=0;i<(size_t)w*h;i++) if (f==0x01) { out[4*i]=in[i]; out[4*i+1]=out[4*i+2]=0; out[4*i+3]=255; } else memcpy(out+4*i,in+4*i,4);
        return;
    }
    uint32_t bw=(w+3)/4, bh=(h+3)/4, stride=f==0x46?8:16;
    for (uint32_t y=0;y<bh;y++) for (uint32_t x=0;x<bw;x++) {
        const uint8_t *b=in+((size_t)y*bw+x)*stride; uint8_t a[16],p[4][4];
        if (f==0x48) bc_alpha(b,a);
        const uint8_t *c=b+(f==0x48?8:0); bc_color(c,p,f==0x48); uint32_t bits=u32(c+4);
        for (int i=0;i<16;i++) { uint32_t px=x*4+i%4,py=y*4+i/4; if(px>=w||py>=h) continue; uint8_t *d=out+((size_t)py*w+px)*4;
            memcpy(d,p[(bits>>(2*i))&3],4); if(f==0x48)d[3]=a[i];
        }
    }
}
// Scale2x-style corner decisions, with a small color-distance tolerance for antialiased art.
static unsigned dist(const uint8_t *a,const uint8_t *b) { unsigned d=0; for(int i=0;i<4;i++) { int t=(int)a[i]-b[i]; d+=(unsigned)(t*t); } return d; }
static void enlarge(const uint8_t *src, uint8_t *dst, uint32_t w,uint32_t h) {
    for(uint32_t y=0;y<h;y++) for(uint32_t x=0;x<w;x++) {
        const uint8_t *e=src+((size_t)y*w+x)*4, *b=src+((size_t)(y?y-1:y)*w+x)*4,
            *d=src+((size_t)y*w+(x?x-1:x))*4, *f=src+((size_t)y*w+(x+1<w?x+1:x))*4,
            *hh=src+((size_t)(y+1<h?y+1:y)*w+x)*4;
        const uint8_t *n[4]={d,f,d,f}, *m[4]={b,b,hh,hh};
        for(int q=0;q<4;q++) { uint8_t *o=dst+((size_t)(2*y+(q>>1))*2*w+2*x+(q&1))*4;
            bool edge=dist(n[q],m[q])<dist(b,hh) && dist(n[q],m[q])<dist(d,f) && dist(n[q],m[q])<2048;
            for(int c=0;c<4;c++) o[c]=edge ? (2*e[c]+n[q][c]+m[q][c]+2)/4 : e[c];
        }
    }
}
typedef struct { uint64_t hash; uint32_t w,h,format; uint8_t *input,*output; int state; char name[64]; } Job;
static Job jobs[128];
static void *worker(void *arg) {
    Job *j=arg; uint64_t start=SDL_GetTicksNS(); size_t px=(size_t)j->w*j->h;
    uint8_t *src=malloc(px*4), *dst=malloc(px*16);
    if (!src || !dst) { free(src); free(dst); free(j->input); j->input=NULL; __atomic_store_n(&j->state,3,__ATOMIC_RELEASE); return NULL; }
    decode(j->input,src,j->w,j->h,j->format); enlarge(src,dst,j->w,j->h);
    free(src); free(j->input); j->input=NULL;
    j->output=dst; __atomic_store_n(&j->state,2,__ATOMIC_RELEASE);
    LOG("[sharp-ui] %s worker %.2f ms finished_frame=%llu\n",j->name,(SDL_GetTicksNS()-start)/1e6,(unsigned long long)framepace_game_frame()); return NULL;
}
bool sharp_ui_ready(int pending) { return pending>0 && pending<=128 && __atomic_load_n(&jobs[pending-1].state,__ATOMIC_ACQUIRE)!=1; }
bool sharp_ui_candidate(uint32_t format, uint32_t w, uint32_t h) {
    for (size_t i=0;i<nentries;i++)
        if (entries[i].w==w && entries[i].h==h &&
            (entries[i].format==format || (entries[i].format==0x46 && format==0x45))) return true;
    return false;
}
bool sharp_ui_request(uint32_t format,uint32_t w,uint32_t h,const uint8_t *linear,uint8_t **rgba,int *pending) {
    const FormatInfo *fi=nvn_format_info(format); size_t bytes=(size_t)((w+fi->bw-1)/fi->bw)*((h+fi->bh-1)/fi->bh)*fi->bpb;
    size_t i; for(i=0;i<nentries;i++) if(entries[i].w==w && entries[i].h==h && (entries[i].format==format || (entries[i].format==0x46 && format==0x45))) break;
    if(i==nentries) return false; uint64_t hsh=hash(linear,bytes);
    for(;i<nentries;i++) if(entries[i].w==w && entries[i].h==h && (entries[i].format==format || (entries[i].format==0x46 && format==0x45)) && entries[i].hash==hsh) break;
    if(i==nentries) return false;
    size_t slot; for(slot=0;slot<128;slot++) if(__atomic_load_n(&jobs[slot].state,__ATOMIC_ACQUIRE) && jobs[slot].hash==hsh && jobs[slot].format==entries[i].format && jobs[slot].w==w && jobs[slot].h==h) break;
    if(slot==128) {
        for(slot=0;slot<128 && __atomic_load_n(&jobs[slot].state,__ATOMIC_ACQUIRE);slot++) {}
        if(slot==128) return true; Job *j=&jobs[slot]; j->input=malloc(bytes); if(!j->input) return true;
        memcpy(j->input,linear,bytes); j->hash=hsh; j->w=w; j->h=h; j->format=entries[i].format; memcpy(j->name,entries[i].name,sizeof j->name);
        __atomic_store_n(&j->state,1,__ATOMIC_RELEASE); pthread_t thread;
        if(pthread_create(&thread,NULL,worker,j)) { free(j->input); __atomic_store_n(&j->state,3,__ATOMIC_RELEASE); return true; }
        pthread_detach(thread); LOG("[sharp-ui] %s %ux%u matched hash=%016llx\n",j->name,w,h,(unsigned long long)hsh);
    }
    int state=__atomic_load_n(&jobs[slot].state,__ATOMIC_ACQUIRE); if(state==2)*rgba=jobs[slot].output; else if(state==1)*pending=(int)slot+1;
    return true;
}
