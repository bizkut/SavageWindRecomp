// cc -std=c11 -O2 tools/test_text_sdf.c -lm -o /tmp/test_text_sdf && /tmp/test_text_sdf
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/nvn/text_sdf.h"

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
    assert(text_sdf_sheet(src,dst,N,N,N,N,1,1,S,NULL,&sdf_count,&sharpen_count));
    for (int i = 0; i < O*O; i++) output += dst[i];
    assert(fabs(output/(S*S)-input) < input*0.02);
    // Horizontal central edge is at x=8 and x=24 (within half a source pixel).
    int first = -1, last = -1;
    for (int x = 0; x < O; x++) if (dst[(O/2)*O+x] >= 128) { if (first < 0) first=x; last=x; }
    assert(fabs((first+0.5)/S-8) <= 0.5);
    assert(fabs((last+0.5)/S-24) <= 0.5);
    uint32_t cjk_ink = (N << 8) | 0x10000;
    memset(dst, 0, sizeof(dst));
    assert(text_sdf_sheet(src,dst,N,N,N,N,1,1,S,&cjk_ink,&sdf_count,&sharpen_count));
    assert(sdf_count == 0 && sharpen_count == 1);
}
int main(void) { check(0); check(1); puts("text SDF: square and circle passed"); }
