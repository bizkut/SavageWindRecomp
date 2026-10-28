// BC4/BC5/BC7 DDS output for user-generated layout textures.
#include "hd_ui_dds.h"
#include "bc7enc.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

static void put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }

static void encode_bc4(const uint8_t *p, unsigned channels, unsigned channel, uint8_t *out) {
    uint8_t lo=255, hi=0;
    for (int i=0;i<16;i++) { uint8_t v=p[i*channels+channel]; lo=std::min(lo,v); hi=std::max(hi,v); }
    uint8_t pal[8]={hi,lo};
    if (hi>lo) for (int i=1;i<7;i++) pal[i+1]=((7-i)*hi+i*lo)/7;
    else { for (int i=1;i<5;i++) pal[i+1]=((5-i)*hi+i*lo)/5; pal[6]=0; pal[7]=255; }
    uint64_t indices=0;
    for (int i=0;i<16;i++) {
        unsigned best=0, distance=256;
        for (unsigned j=0;j<8;j++) {
            unsigned d=(unsigned)std::abs((int)p[i*channels+channel]-(int)pal[j]);
            if (d<distance) { best=j; distance=d; }
        }
        indices|=(uint64_t)best<<(3*i);
    }
    out[0]=hi; out[1]=lo;
    for (int i=0;i<6;i++) out[2+i]=(uint8_t)(indices>>(8*i));
}

static double lanczos(double x) {
    constexpr double pi=3.14159265358979323846;
    x=std::abs(x);
    if (x<1e-10) return 1;
    if (x>=3) return 0;
    return (std::sin(pi*x)/(pi*x))*(std::sin(pi*x/3)/(pi*x/3));
}

// Precompute each axis once; evaluating sine per pixel dominates CPU encoding.
struct Filter { int first, count; double weights[16]; };
static std::vector<Filter> filters(uint32_t from, uint32_t to) {
    std::vector<Filter> result(to);
    double scale=(double)from/to;
    for (uint32_t x=0;x<to;x++) {
        double center=(x+0.5)*scale-0.5, sum=0;
        int first=std::max(0,(int)std::ceil(center-3*scale));
        int last=std::min((int)from-1,(int)std::floor(center+3*scale));
        Filter &f=result[x]; f.first=first; f.count=last-first+1;
        for (int i=0;i<f.count;i++) sum+=(f.weights[i]=lanczos((first+i-center)/scale));
        for (int i=0;i<f.count;i++) f.weights[i]/=sum;
    }
    return result;
}

// Separable Lanczos-3 reduction, with clipped and renormalized boundary weights.
static std::vector<uint8_t> resize(const std::vector<uint8_t>& in, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh,
                                   unsigned channels) {
    std::vector<float> tmp((size_t)nw*h*channels);
    std::vector<uint8_t> out((size_t)nw*nh*channels);
    auto fx=filters(w,nw), fy=filters(h,nh);
    for (uint32_t x=0;x<nw;x++) {
        const Filter &f=fx[x];
        for (uint32_t y=0;y<h;y++) for (unsigned c=0;c<channels;c++) {
            double value=0;
            for (int i=0;i<f.count;i++) value+=in[((size_t)y*w+f.first+i)*channels+c]*f.weights[i];
            tmp[((size_t)y*nw+x)*channels+c]=(float)value;
        }
    }
    for (uint32_t y=0;y<nh;y++) {
        const Filter &f=fy[y];
        for (uint32_t x=0;x<nw;x++) for (unsigned c=0;c<channels;c++) {
            double value=0;
            for (int i=0;i<f.count;i++) value+=tmp[((size_t)(f.first+i)*nw+x)*channels+c]*f.weights[i];
            out[((size_t)y*nw+x)*channels+c]=(uint8_t)std::clamp((int)std::floor(value+0.5),0,255);
        }
    }
    return out;
}

int hd_ui_dds_encode(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t channels,
                     int srgb, uint8_t **out, size_t *out_size) {
    if (!pixels || !out || !out_size || !width || !height || width>4096 || height>4096 ||
        (channels!=1 && channels!=2 && channels!=4)) return -1;
    *out=nullptr; *out_size=0;
    try {
        size_t size=148; uint32_t w=width,h=height,levels=0;
        do { size+=(size_t)((w+3)/4)*((h+3)/4)*(channels==1?8:16); levels++; w=std::max(1u,w/2); h=std::max(1u,h/2); }
        while (w>1 || h>1);
        if (width>1 || height>1) { size+=channels==1?8:16; levels++; }
        std::vector<uint8_t> blob(size,0), image(pixels,pixels+(size_t)width*height*channels);
        memcpy(blob.data(),"DDS ",4); put32(blob.data()+4,124); put32(blob.data()+8,0xa1007);
        put32(blob.data()+12,height); put32(blob.data()+16,width);
        put32(blob.data()+20,((width+3)/4)*((height+3)/4)*(channels==1?8:16));
        put32(blob.data()+28,levels); put32(blob.data()+76,32); put32(blob.data()+80,4);
        memcpy(blob.data()+84,"DX10",4); put32(blob.data()+108,0x401008);
        put32(blob.data()+128,channels==1?80:channels==2?83:srgb?99:98);
        put32(blob.data()+132,3); put32(blob.data()+140,1);
        static std::once_flag once;
        if (channels==4) std::call_once(once,[]{ bc7enc_compress_block_init(); });
        bc7enc_compress_block_params params{};
        if (channels==4) { bc7enc_compress_block_params_init(&params); bc7enc_compress_block_params_init_linear_weights(&params); }
        size_t at=148; w=width; h=height;
        for (;;) {
            for (uint32_t by=0;by<(h+3)/4;by++) for (uint32_t bx=0;bx<(w+3)/4;bx++) {
                uint8_t block[64];
                for (unsigned i=0;i<16;i++) {
                    uint32_t x=std::min(w-1,bx*4+i%4), y=std::min(h-1,by*4+i/4);
                    memcpy(block+i*channels,&image[((size_t)y*w+x)*channels],channels);
                }
                if (channels==4) bc7enc_compress_block(blob.data()+at,block,&params);
                else { encode_bc4(block,channels,0,blob.data()+at); if (channels==2) encode_bc4(block,channels,1,blob.data()+at+8); }
                at+=channels==1?8:16;
            }
            if (w==1 && h==1) break;
            uint32_t nw=std::max(1u,w/2), nh=std::max(1u,h/2);
            image=resize(image,w,h,nw,nh,channels); w=nw; h=nh;
        }
        if (at!=size) return -1;
        uint8_t *result=(uint8_t *)malloc(size);
        if (!result) return -1;
        memcpy(result,blob.data(),size); *out=result; *out_size=size; return 0;
    } catch (...) { return -1; }
}
