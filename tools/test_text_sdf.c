// cc -std=c11 -O2 tools/test_text_sdf.c -lm -o /tmp/test_text_sdf && /tmp/test_text_sdf [linear-NormalS.r8]
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/text_reconstruct.h"

static float shader(float x, float lo, float hi) {
    x = (x - lo) / (hi - lo);
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3 - 2 * x);
}

static double weight(const uint8_t *p, int w, int x, int y, int cw, int ch, float lo, float hi) {
    double sum = 0;
    for (int j = 0; j < ch; j++) for (int i = 0; i < cw; i++)
        sum += shader(p[(y + j) * w + x + i] / 255.0f, lo, hi);
    return sum;
}
static float bilinear_at(const uint8_t *src, int w, int x0, int y0, int cw, int ch, int scale, int x, int y) {
    float sx=(x+0.5f)/scale-0.5f, sy=(y+0.5f)/scale-0.5f;
    int ix=(int)floorf(sx), iy=(int)floorf(sy);
    float fx=sx-ix, fy=sy-iy;
    int l=text_clamp(ix,cw-1), r=text_clamp(ix+1,cw-1);
    int t=text_clamp(iy,ch-1), b=text_clamp(iy+1,ch-1);
    return (src[(y0+t)*w+x0+l]*(1-fx)+src[(y0+t)*w+x0+r]*fx)*(1-fy)+
           (src[(y0+b)*w+x0+l]*(1-fx)+src[(y0+b)*w+x0+r]*fx)*fy;
}


static void check_shape(int circle) {
    enum { N=32, S=4, O=N*S };
    uint8_t src[N*N]={0}, dst[O*O]={0};
    for (int y=0;y<N;y++) for (int x=0;x<N;x++) {
        int hits=0;
        for (int j=0;j<16;j++) for (int i=0;i<16;i++) {
            double xx=x+(i+0.5)/16-16, yy=y+(j+0.5)/16-16;
            hits += circle ? xx*xx+yy*yy<64 : fabs(xx)<8 && fabs(yy)<8;
        }
        src[y*N+x]=(hits*255+128)/256;
    }
    text_reconstruct_sheet(src,dst,N,N,N,N,1,1,S);
    const float limits[2][2]={{0.4f,0.6f},{0.2f,0.5f}};
    for(int t=0;t<2;t++) {
        double before=weight(src,N,0,0,N,N,limits[t][0],limits[t][1]);
        double after=weight(dst,O,0,0,O,O,limits[t][0],limits[t][1])/(S*S);
        assert(fabs(after-before)<before*0.02);
    }
    double before=0,after=0;
    for(int i=0;i<N*N;i++)before+=src[i];
    for(int i=0;i<O*O;i++)after+=dst[i];
    assert(fabs(after/(S*S)-before)<before*0.02);
}

static void check_consistent_stems(void) {
    enum { CW=24, CH=24, W=49, S=4, OW=W*S };
    uint8_t src[W*CH]={0}, dst[OW*CH*S]={0};
    for(int y=4;y<20;y++) for(int x=8;x<12;x++) {
        uint8_t v=(x==8 || x==11)?128:255;
        src[y*W+x]=v;
        src[y*W+25+x]=v;
    }
    for(int y=5;y<8;y++) for(int x=4;x<17;x++) src[y*W+25+x]=255;
    text_reconstruct_sheet(src,dst,W,CH,CW,CH,2,1,S);
    for(int y=12*S;y<18*S;y++) for(int x=6*S;x<14*S;x++)
        assert(dst[y*OW+x]==dst[y*OW+25*S+x]);
    for(int c=0;c<2;c++) {
        double a=weight(src,W,c*25,0,CW,CH,0.4f,0.6f);
        double b=weight(dst,OW,c*25*S,0,CW*S,CH*S,0.4f,0.6f)/(S*S);
        assert(fabs(b/a-1)<0.05);
    }
    assert(dst[15*S*OW+10*S]>=250);
    // Cubic interpolation must not invent a halo outside a constant zero region.
    for(int y=10*S;y<18*S;y++) for(int x=0;x<6*S;x++) assert(dst[y*OW+x]==0);
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
    text_reconstruct_sheet(src,dst,N,N,N,N,1,1,S);
    int before=0,after=0;
    for(int y=0;y<O;y++) for(int x=0;x<O;x++) {
        before+=bilinear_at(src,N,0,0,N,N,S,x,y)>=220;
        after+=dst[y*O+x]>=220;
    }
    assert(before>0 && after*10>=before*6);
}

static void check_punctuation(void) {
    enum { N=16, S=4, O=N*S };
    uint8_t src[N*N]={0}, dst[O*O]={0};
    for(int y=5;y<10;y++) for(int x=6;x<10;x++)
        src[y*N+x]=(x==6 || x==9 || y==5 || y==9)?100:255;
    text_reconstruct_sheet(src,dst,N,N,N,N,1,1,S);
    int peak=0;
    double before=0,after=0;
    for(int i=0;i<N*N;i++)before+=src[i];
    for(int i=0;i<O*O;i++) {
        if(dst[i]>peak)peak=dst[i];
        after+=dst[i];
    }
    assert(peak==255);
    assert(fabs(after/(S*S)-before)<before*0.12);
}

static void check_no_ringing(void) {
    const float values[]={0, 32, 128, 200, 255};
    for(int a=0;a<5;a++) for(int b=0;b<5;b++)
        for(int c=0;c<5;c++) for(int d=0;d<5;d++) for(int t=0;t<=16;t++) {
            float v=text_cubic(values[a],values[b],values[c],values[d],t/16.0f);
            assert(v>=fminf(values[b],values[c])-0.001f);
            assert(v<=fmaxf(values[b],values[c])+0.001f);
        }
}

static void check_menu_atlas(const char *path) {
    enum { W=1024, H=1024, CW=24, CH=30, S=4 };
    uint8_t *atlas=malloc(W*H), *dst=calloc(W*H*S*S,1);
    FILE *f=fopen(path,"rb");
    assert(atlas && dst && f && fread(atlas,1,W*H,f)==W*H);
    fclose(f);
    const char *letters="Contiue";
    text_reconstruct_sheet(atlas,dst,W,H,CW,CH,40,33,S);
    double min_error=1, max_error=-1;
    for(int k=0;letters[k];k++) {
        int index=letters[k]-32, x=index%40*25, y=H-index/40*31-CH;
        double before=weight(atlas,W,x,y,CW,CH,0.4f,0.6f);
        double after=weight(dst,W*S,x*S,y*S,CW*S,CH*S,0.4f,0.6f)/(S*S);
        double error=after/before-1;
        if(error<min_error)min_error=error;
        if(error>max_error)max_error=error;
        int src_peak=0,dst_peak=0, source_high=0, output_high=0;
        for(int j=0;j<CH;j++) for(int i=0;i<CW;i++)
            if(atlas[(y+j)*W+x+i]>src_peak)src_peak=atlas[(y+j)*W+x+i];
        for(int j=0;j<CH*S;j++) for(int i=0;i<CW*S;i++) {
            int value=dst[(y*S+j)*W*S+x*S+i];
            if(value>dst_peak)dst_peak=value;
            source_high+=bilinear_at(atlas,W,x,y,CW,CH,S,i,j)>=220;
            output_high+=value>=220;
        }
        assert(dst_peak>=src_peak-5);
        assert(output_high*5>=source_high*4);
    }
    free(atlas);
    free(dst);
    assert(max_error-min_error<0.05);
}
int main(int argc, char **argv) {
    check_shape(0);
    check_shape(1);
    check_consistent_stems();
    check_punctuation();
    check_thin_contrast();
    check_no_ringing();
    if(argc>1)check_menu_atlas(argv[1]);
    puts("text reconstruction tests passed");
}
