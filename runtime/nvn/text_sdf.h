// Antialiased Euclidean distance reconstruction for font atlas cells.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Distance from a pixel center to the straight edge implied by its area coverage and gradient.
// Gustavson and Strand, Anti-aliased Euclidean distance transform (2011).
static float text_edge_distance(float gx, float gy, float a) {
    gx = fabsf(gx); gy = fabsf(gy);
    if (gx == 0 || gy == 0) return 0.5f-a;
    float length = sqrtf(gx*gx+gy*gy); gx /= length; gy /= length;
    if (gx < gy) { float t = gx; gx = gy; gy = t; }
    float a1 = 0.5f*gy/gx;
    if (a < a1) return 0.5f*(gx+gy)-sqrtf(2*gx*gy*a);
    if (a < 1-a1) return (0.5f-a)*gx;
    return -0.5f*(gx+gy)+sqrtf(2*gx*gy*(1-a));
}

static void text_distance(const float *coverage, float *dist, int16_t *dx, int16_t *dy, int w, int h) {
    const float inf = 1e10f;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int p = y*w+x; float a = coverage[p]; dx[p]=dy[p]=0;
        if (a <= 0) dist[p] = inf;
        else if (a >= 1) dist[p] = 0;
        else {
            int l = x ? x-1 : x, r = x+1<w ? x+1 : x;
            int u = y ? y-1 : y, d = y+1<h ? y+1 : y;
            float gx = coverage[u*w+r]-coverage[u*w+l] + 1.41421356f*(coverage[y*w+r]-coverage[y*w+l]) + coverage[d*w+r]-coverage[d*w+l];
            float gy = coverage[d*w+l]-coverage[u*w+l] + 1.41421356f*(coverage[d*w+x]-coverage[u*w+x]) + coverage[d*w+r]-coverage[u*w+r];
            dist[p] = text_edge_distance(gx, gy, a);
        }
    }
    // Propagate closest edge vectors in both raster directions until no distance improves.
    int changed;
    do {
        changed = 0;
        for (int pass = 0; pass < 2; pass++) {
            int begin = pass ? w*h-1 : 0, end = pass ? -1 : w*h, step = pass ? -1 : 1;
            for (int p = begin; p != end; p += step) {
                int x=p%w, y=p/w;
                for (int oy=-1; oy<=1; oy++) for (int ox=-1; ox<=1; ox++) {
                    if ((!ox && !oy) || x+ox<0 || x+ox>=w || y+oy<0 || y+oy>=h) continue;
                    int n = (y+oy)*w+x+ox; if (dist[n] >= inf/2) continue;
                    int vx=dx[n]-ox, vy=dy[n]-oy, sx=x-vx, sy=y-vy;
                    if (sx<0 || sx>=w || sy<0 || sy>=h) continue;
                    float a=coverage[sy*w+sx]; if (a<=0) continue;
                    float candidate = sqrtf((float)(vx*vx+vy*vy)) + text_edge_distance((float)vx,(float)vy,a);
                    if (candidate < 0) candidate = 0;
                    if (candidate + 0.0001f < dist[p]) { dist[p]=candidate; dx[p]=vx; dy[p]=vy; changed=1; }
                }
            }
        }
    } while (changed);
}

static float text_smooth(float x) { x = x<0 ? 0 : x>1 ? 1 : x; return x*x*(3-2*x); }

// Reconstruct each cell separately, but use one weight-preserving offset for the entire sheet.
static int text_sdf_sheet(const uint8_t *src, uint8_t *dst, int w, int h, int cw, int ch, int cols, int rows, int scale, const uint16_t *ink) {
    size_t pixels = (size_t)w*h*scale*scale;
    float *field = malloc(pixels*sizeof(*field));
    size_t cells = (size_t)cw*ch;
    float *alpha=malloc(cells*sizeof(float)), *outside=malloc(cells*sizeof(float)), *inside=malloc(cells*sizeof(float));
    int16_t *dx=malloc(cells*sizeof(int16_t)), *dy=malloc(cells*sizeof(int16_t));
    if (!field || !alpha || !outside || !inside || !dx || !dy) { free(field); free(alpha); free(outside); free(inside); free(dx); free(dy); return 0; }
    int ow=w*scale; double target=0;
    // Sentinel keeps the existing padding pixels untouched.
    for (size_t p=0; p<pixels; p++) field[p]=1e20f;
    for (int row=0; row<rows; row++) for (int col=0; col<cols; col++) {
        int x0=col*(cw+1), y0=h-row*(ch+1)-ch;
        for (int y=0; y<ch; y++) for (int x=0; x<cw; x++)
            target += src[(y0+y)*w+x0+x] / 255.0 * scale*scale;
        int left=ink ? ink[row*cols+col]&255 : 0;
        int right=ink ? ink[row*cols+col]>>8 : cw;
        if (left >= right || right > cw) continue;
        int iw=right-left; size_t count=(size_t)iw*ch;
        for (int y=0; y<ch; y++) for (int x=0; x<iw; x++)
            alpha[y*iw+x]=src[(y0+y)*w+x0+left+x]/255.0f;
        text_distance(alpha,outside,dx,dy,iw,ch);
        for (size_t i=0; i<count; i++) alpha[i]=1-alpha[i];
        text_distance(alpha,inside,dx,dy,iw,ch);
        for (size_t i=0; i<count; i++) outside[i]-=inside[i];
        for (int y=0; y<ch*scale; y++) for (int x=0; x<iw*scale; x++) {
            float sx=(x+0.5f)/scale-0.5f, sy=(y+0.5f)/scale-0.5f;
            int ix=(int)floorf(sx), iy=(int)floorf(sy); float fx=sx-ix, fy=sy-iy;
            int jx=ix+1, jy=iy+1; if (ix<0) ix=0; if (iy<0) iy=0; if (jx>=iw) jx=iw-1; if (jy>=ch) jy=ch-1;
            float v=(outside[iy*iw+ix]*(1-fx)+outside[iy*iw+jx]*fx)*(1-fy)+(outside[jy*iw+ix]*(1-fx)+outside[jy*iw+jx]*fx)*fy;
            field[(size_t)(y0*scale+y)*ow+(x0+left)*scale+x]=v*scale;
        }
    }
    // The prior worker left the bilinear-derived sharpened image in dst; obtain a clean bilinear
    // floor directly from the source, per cell, in a second pass below.
    float lo=-8, hi=8;
    for (int iteration=0; iteration<22; iteration++) {
        float mid=(lo+hi)*0.5f; double sum=0;
        for (int row=0; row<rows; row++) for (int col=0; col<cols; col++) {
            int x0=col*(cw+1), y0=h-row*(ch+1)-ch;
            for (int y=0; y<ch*scale; y++) for (int x=0; x<cw*scale; x++) {
                float sx=(x+0.5f)/scale-0.5f, sy=(y+0.5f)/scale-0.5f; int ix=(int)floorf(sx), iy=(int)floorf(sy); float fx=sx-ix, fy=sy-iy;
                int jx=ix+1,jy=iy+1; if(ix<0)ix=0; if(iy<0)iy=0; if(jx>=cw)jx=cw-1; if(jy>=ch)jy=ch-1;
                float bil=((src[(y0+iy)*w+x0+ix]*(1-fx)+src[(y0+iy)*w+x0+jx]*fx)*(1-fy)+(src[(y0+jy)*w+x0+ix]*(1-fx)+src[(y0+jy)*w+x0+jx]*fx)*fy)/255.0f;
                float floor=bil-0.12f; float v=text_smooth(0.5f-field[(size_t)(y0*scale+y)*ow+x0*scale+x]+mid);
                sum += v>floor?v:floor;
            }
        }
        if(sum<target)lo=mid; else hi=mid;
    }
    float offset=(lo+hi)*0.5f;
    for (int row=0; row<rows; row++) for (int col=0; col<cols; col++) {
        int x0=col*(cw+1), y0=h-row*(ch+1)-ch;
        for (int y=0; y<ch*scale; y++) for (int x=0; x<cw*scale; x++) {
            float sx=(x+0.5f)/scale-0.5f, sy=(y+0.5f)/scale-0.5f; int ix=(int)floorf(sx), iy=(int)floorf(sy); float fx=sx-ix, fy=sy-iy;
            int jx=ix+1,jy=iy+1; if(ix<0)ix=0; if(iy<0)iy=0; if(jx>=cw)jx=cw-1; if(jy>=ch)jy=ch-1;
            float bil=((src[(y0+iy)*w+x0+ix]*(1-fx)+src[(y0+iy)*w+x0+jx]*fx)*(1-fy)+(src[(y0+jy)*w+x0+ix]*(1-fx)+src[(y0+jy)*w+x0+jx]*fx)*fy)/255.0f;
            size_t p=(size_t)(y0*scale+y)*ow+x0*scale+x; float v=text_smooth(0.5f-field[p]+offset), floor=bil-0.12f;
            if(v<floor)v=floor; dst[p]=(uint8_t)(v*255+0.5f);
        }
    }
    free(field); free(alpha); free(outside); free(inside); free(dx); free(dy); return 1;
}
