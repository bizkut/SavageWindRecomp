// cc -std=c11 -O2 tools/test_text_sdf.c -lm -pthread -o /tmp/test_text_sdf && /tmp/test_text_sdf
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/nvn/text_sdf.h"

static uint8_t bilinear(const uint8_t *src, int n, int scale, int x, int y) {
    float sx=(x+0.5f)/scale-0.5f, sy=(y+0.5f)/scale-0.5f;
    int ix=(int)floorf(sx), iy=(int)floorf(sy), bx=ix+1, by=iy+1;
    float fx=sx-ix, fy=sy-iy;
    if(ix<0)ix=0;if(iy<0)iy=0;if(bx>=n)bx=n-1;if(by>=n)by=n-1;
    float v=(src[iy*n+ix]*(1-fx)+src[iy*n+bx]*fx)*(1-fy)+
            (src[by*n+ix]*(1-fx)+src[by*n+bx]*fx)*fy;
    return (uint8_t)(v+0.5f);
}

static void check_shader_weight(const uint8_t *src, int n, const uint8_t *dst, int scale) {
    const float thresholds[2][2] = {{0.4f,0.6f},{0.2f,0.5f}};
    for (int t=0;t<2;t++) {
        double before=0,after=0;
        for(int i=0;i<n*n;i++) before+=text_shader(src[i]/255.0f,thresholds[t][0],thresholds[t][1]);
        for(int i=0;i<n*n*scale*scale;i++) after+=text_shader(dst[i]/255.0f,thresholds[t][0],thresholds[t][1]);
        assert(fabs(after/(scale*scale)-before)<before*0.02);
    }
}

static void check(int circle) {
    enum { N = 32, S = 4, O = N*S };
    uint8_t src[N*N] = {0}, dst[O*O] = {0};
    double input = 0, output = 0;
    // Supersampling models coverage of a circle or a square with boundaries at integer coordinates.
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int hits = 0;
        for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) {
            double xx = x+(i+0.5)/16.0-16, yy = y+(j+0.5)/16.0-16;
            hits += circle ? xx*xx+yy*yy < 64 : fabs(xx) < 8 && fabs(yy) < 8;
        }
        src[y*N+x] = (uint8_t)((hits*255+128)/256); input += src[y*N+x];
    }
    int sdf_count, sharpen_count;
    assert(text_sdf_sheet(src,dst,N,N,N,N,1,1,S,NULL,0,&sdf_count,&sharpen_count));
    for (int i = 0; i < O*O; i++) output += dst[i];
    assert(fabs(output/(S*S)-input) < input*0.02);
    check_shader_weight(src,N,dst,S);
    // Horizontal central edge is at x=8 and x=24 (within half a source pixel).
    int first = -1, last = -1;
    for (int x = 0; x < O; x++) if (dst[(O/2)*O+x] >= 128) { if (first < 0) first=x; last=x; }
    assert(fabs((first+0.5)/S-8) <= 0.5);
    assert(fabs((last+0.5)/S-24) <= 0.5);
    uint32_t cjk_ink = (N << 8) | 0x10000;
    memset(dst, 0, sizeof(dst));
    assert(text_sdf_sheet(src,dst,N,N,N,N,1,1,S,&cjk_ink,0,&sdf_count,&sharpen_count));
    assert(sdf_count == 0 && sharpen_count == 1);
    check_shader_weight(src,N,dst,S);
}
static void check_punctuation(void) {
    enum { N = 16, S = 4, O = N*S };
    uint8_t src[N*N] = {0}, dst[O*O] = {0};
    // A small, antialiased punctuation mark with a fully opaque center.
    for (int y=5;y<10;y++) for (int x=6;x<10;x++)
        src[y*N+x] = (x==6 || x==9 || y==5 || y==9) ? 100 : 255;
    uint32_t ink = (N << 8) | 0x10000; // coverage-sharpening path
    int sdf_count, sharpen_count;
    assert(text_sdf_sheet(src,dst,N,N,N,N,1,1,S,&ink,0,&sdf_count,&sharpen_count));
    assert(sdf_count == 0 && sharpen_count == 1);
    int peak=0; double before=0,after=0;
    for(int i=0;i<N*N;i++) before+=src[i];
    for(int i=0;i<O*O;i++) { if(dst[i]>peak)peak=dst[i]; after+=dst[i]; }
    assert(peak==255);
    assert(fabs(after/(S*S)-before)<before*0.12);
}
static void check_thin_contrast(void) {
    enum { N=24, S=4, O=N*S };
    static const uint8_t stroke[9][7] = {
        {0,160,96,96,0,0,0}, {160,160,255,96,96,0,0},
        {96,255,255,255,96,0,0}, {128,160,255,255,96,96,0},
        {0,96,255,255,255,96,0}, {0,128,160,255,255,96,96},
        {0,0,96,255,255,255,160}, {0,0,128,96,255,160,128},
        {0,0,0,128,160,128,0}
    };
    uint8_t src[N*N]={0}, dst[O*O]={0};
    for(int y=0;y<9;y++)memcpy(src+(y+13)*N+14,stroke[y],7);
    int sdf_count,sharpen_count, before=0,after=0;
    assert(text_sdf_sheet(src,dst,N,N,N,N,1,1,S,NULL,0,&sdf_count,&sharpen_count));
    for(int y=0;y<O;y++)for(int x=0;x<O;x++) {
        before+=bilinear(src,N,S,x,y)>=220;
        after+=dst[y*O+x]>=220;
    }
    // The old fit reduced high-coverage samples to 38% of the original.
    assert(before>0 && after*10>=before*6);
}
int main(void) { check(0); check(1); check_punctuation(); check_thin_contrast(); puts("text SDF: square, circle, punctuation and thin contrast passed"); }
