// User-owned BNTX level-zero extraction for the UI pack generator.
#include "hd_ui_extract.h"
#include "hd_ui_policy.h"
#include "tegra.h"
#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static int span(size_t size, uint64_t off, uint64_t len) { return off <= size && len <= size - off; }

static void bc4(const uint8_t *b, uint8_t p[16]) {
    uint8_t pal[8] = {b[0], b[1]};
    if (pal[0] > pal[1]) for (int i = 1; i < 7; i++) pal[i+1] = ((7-i)*pal[0]+i*pal[1])/7;
    else { for (int i = 1; i < 5; i++) pal[i+1] = ((5-i)*pal[0]+i*pal[1])/5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[i+2] << (8*i);
    for (int i = 0; i < 16; i++) p[i] = pal[(bits >> (3*i)) & 7];
}

static void decode(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, uint32_t fmt, uint32_t channels) {
    if (fmt == 0x0201 || fmt == 0x0701 || fmt == 0x0901 || fmt == 0x0b01 || fmt == 0x0b06) {
        memcpy(dst, src, (size_t)w*h*channels);
        return;
    }
    uint32_t bw = (w+3)/4, bh = (h+3)/4;
    size_t block = fmt == 0x1d01 || fmt == 0x1a01 || fmt == 0x1a06 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *b = src + ((size_t)by*bw+bx)*block;
        uint8_t values[2][16];
        if (fmt == 0x1d01 || fmt == 0x1e01) {
            bc4(b, values[0]); if (channels == 2) bc4(b+8, values[1]);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx*4+i%4, y = by*4+i/4;
                if (x >= w || y >= h) continue;
                for (uint32_t c = 0; c < channels; c++) dst[((size_t)y*w+x)*channels+c] = values[c][i];
            }
            continue;
        }
        uint8_t alpha[16];
        if (block == 16) bc4(b, alpha); else memset(alpha, 255, sizeof alpha);
        const uint8_t *color = b + block - 8;
        uint16_t a = u16(color), c = u16(color+2);
        uint8_t pal[4][3];
        for (int k = 0; k < 2; k++) {
            uint16_t v = k ? c : a;
            pal[k][0] = ((v>>11)&31)*255/31; pal[k][1] = ((v>>5)&63)*255/63; pal[k][2] = (v&31)*255/31;
        }
        int opaque = block == 16 || a > c;
        for (int k = 0; k < 3; k++) {
            pal[2][k] = opaque ? (2*pal[0][k]+pal[1][k])/3 : (pal[0][k]+pal[1][k])/2;
            pal[3][k] = opaque ? (pal[0][k]+2*pal[1][k])/3 : 0;
        }
        uint32_t bits = u32(color+4);
        for (int i = 0; i < 16; i++) {
            uint32_t x = bx*4+i%4, y = by*4+i/4, k = (bits>>(2*i))&3;
            if (x >= w || y >= h) continue;
            uint8_t *p = dst + ((size_t)y*w+x)*4;
            memcpy(p, pal[k], 3); p[3] = !opaque && k == 3 ? 0 : alpha[i];
        }
    }
}

static int bntx(const uint8_t *b, size_t size, HdUiTextureCallback callback, void *context) {
    if (size < 0x30 || memcmp(b, "BNTX", 4)) return -1;
    uint32_t count = u32(b+0x24);
    uint64_t table = u64(b+0x28);
    if (!span(size, table, (uint64_t)count*8)) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t off = u64(b+table+8*i);
        if (!span(size, off, 0x78) || memcmp(b+off, "BRTI", 4)) return -1;
        const uint8_t *r = b+off;
        uint32_t fmt = u32(r+0x1c), w = u32(r+0x24), h = u32(r+0x28), channels, nvn;
        if (!hd_ui_source_format(fmt,NULL,NULL,&channels,&nvn)) return -1;
        int policy = hd_ui_texture_policy(w, h, 1024, channels, NULL, 0);
        if (policy == 1 || policy == 2) continue;
        uint32_t scale = hd_ui_texture_scale(w, h), bh = u32(r+0x34)&7;
        if (!w || !h || w > 4096/scale || h > 4096/scale || u32(r+0x30) != 1 || bh > 4) return -1;
        uint64_t np = u64(r+0x60), offsets = u64(r+0x70);
        if (!span(size,np,2) || !span(size,offsets,8)) return -1;
        size_t name_len = u16(b+np);
        if (!span(size,np+2,name_len)) return -1;
        TexLayout t = {.width=w,.height=h,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(nvn),.block_height_log2=bh};
        LevelLayout level = tegra_level(&t,0);
        uint64_t data = u64(b+offsets), storage = tegra_layer_size(&t);
        size_t linear_size = (size_t)level.wb * level.hb * t.fmt->bpb;
        if (!span(size,data,storage)) return -1;
        uint8_t *linear = malloc(linear_size), *pixels = malloc((size_t)w*h*channels);
        if (!linear || !pixels) { free(linear); free(pixels); return -1; }
        tegra_deswizzle(b+data,linear,level.wb,level.hb,1,t.fmt->bpb,level.block_height_log2,0);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (size_t j=0; j<linear_size; j++) hash=(hash^linear[j])*0x100000001b3ull;
        decode(linear,pixels,w,h,fmt,channels);
        policy = hd_ui_texture_policy(w,h,1024,channels,pixels,(size_t)w*channels);
        int result = 0;
        if (!policy) {
            HdUiTexture item = {(const char *)b+np+2,name_len,fmt,w,h,channels,scale,hash,linear,linear_size,pixels};
            result = callback(context,&item);
        }
        free(linear); free(pixels);
        if (result) return -1;
    }
    return 0;
}

int hd_ui_extract_layout(const uint8_t *archive, size_t size, HdUiTextureCallback callback, void *context) {
    if (!archive || !callback) return -1;
    for (uint32_t i=0;;i++) {
        const char *name; const uint8_t *payload; size_t name_len, payload_size;
        int r = hd_ui_sarc_entry(archive,size,i,&name,&name_len,&payload,&payload_size);
        if (r <= 0) return r < 0 ? -1 : 0;
        if (name_len < 5) continue;
        const char *p = name+name_len-5;
        if ((p[0] == '.') && (p[1] == 'b' || p[1] == 'B') && (p[2] == 'n' || p[2] == 'N') &&
            (p[3] == 't' || p[3] == 'T') && (p[4] == 'x' || p[4] == 'X') &&
            bntx(payload,payload_size,callback,context)) return -1;
    }
}
