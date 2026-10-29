// Font coverage reconstruction: subpixel edge estimates with separable Euclidean transforms.
#pragma once
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
extern long syscall(long number, ...);
#endif

static void text_low_priority(void) {
#ifdef __PROSPERO__
    struct sched_param p = { .sched_priority = sched_get_priority_min(SCHED_OTHER) };
    if (p.sched_priority >= 0) pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}

static float text_edge_distance(float gx, float gy, float a) {
    gx=fabsf(gx); gy=fabsf(gy);
    if (!gx || !gy) return 0.5f-a;
    float len=sqrtf(gx*gx+gy*gy); gx/=len; gy/=len;
    if(gx<gy){float t=gx;gx=gy;gy=t;}
    float a1=0.5f*gy/gx;
    if(a<a1)return 0.5f*(gx+gy)-sqrtf(2*gx*gy*a);
    if(a<1-a1)return (0.5f-a)*gx;
    return -0.5f*(gx+gy)+sqrtf(2*gx*gy*(1-a));
}

// Lower envelope of parabolas (Felzenszwalb/Huttenlocher); f is squared distance.
static void text_edt1(const float *f,float *d,int n,int *v,float *z) {
    int k=0; v[0]=0; z[0]=-1e20f; z[1]=1e20f;
    for(int q=1;q<n;q++) {
        float s;
        do { int p=v[k]; s=((f[q]+(float)q*q)-(f[p]+(float)p*p))/(2.0f*(q-p)); if(s<=z[k])k--;else break; } while(k>=0);
        if(k<0)k=0;else k++; v[k]=q;z[k]=s;z[k+1]=1e20f;
    }
    k=0;for(int q=0;q<n;q++){while(z[k+1]<q)k++;float a=(float)(q-v[k]);d[q]=a*a+f[v[k]];}
}
static void text_distance(const float *a,float *out,float *tmp,int w,int h,float *f,float *d,int *v,float *z) {
    for(int y=0;y<h;y++) {
        for(int x=0;x<w;x++) {
            float c=a[y*w+x];
            if(c<=0) f[x]=1e10f;
            else if(c>=0.5f) f[x]=0;
            else {
                int l=x?x-1:x,r=x+1<w?x+1:x,u=y?y-1:y,b=y+1<h?y+1:y;
                float gx=a[u*w+r]-a[u*w+l]+1.41421356f*(a[y*w+r]-a[y*w+l])+a[b*w+r]-a[b*w+l];
                float gy=a[b*w+l]-a[u*w+l]+1.41421356f*(a[b*w+x]-a[u*w+x])+a[b*w+r]-a[u*w+r];
                float edge=text_edge_distance(gx,gy,c);f[x]=edge*edge;
            }
        }
        text_edt1(f,d,w,v,z);memcpy(tmp+y*w,d,(size_t)w*sizeof(float));
    }
    for(int x=0;x<w;x++) {
        for(int y=0;y<h;y++)f[y]=tmp[y*w+x]; text_edt1(f,d,h,v,z);
        for(int y=0;y<h;y++){int p=y*w+x;float c=a[p];out[p]=(c>0 && c<1)?0.5f-c:sqrtf(d[y]);}
    }
}
static float text_smooth(float x){x=x<0?0:x>1?1:x;return x*x*(3-2*x);}
// The game's shader applies its own coverage threshold. Keep source-resolution edge
// profiles instead of turning the atlas into a one-output-pixel coverage ramp.
static float text_shader(float x,float lo,float hi) { return text_smooth((x-lo)/(hi-lo)); }
typedef struct {
    const uint8_t *src; const uint32_t *ink; uint16_t *field; uint8_t *profiles;
    int w,h,cw,ch,cols,rows,scale,first,last,force_bilinear; int failed, sdf_count, sharpen_count;
} TextSdfJob;
static void *text_sdf_rows(void *arg) {
    TextSdfJob *j=arg;text_low_priority();int cw=j->cw,ch=j->ch,scale=j->scale,w=j->w,ow=w*scale;
    size_t n=(size_t)cw*ch; int m=cw>ch?cw:ch;
    float *a=malloc(n*sizeof(float)),*out=malloc(n*sizeof(float)),*in=malloc(n*sizeof(float)),*tmp=malloc(n*sizeof(float));
    float *f=malloc((size_t)m*sizeof(float)),*d=malloc((size_t)m*sizeof(float)),*z=malloc((size_t)(m+1)*sizeof(float)); int *v=malloc((size_t)m*sizeof(int));
    if(!a||!out||!in||!tmp||!f||!d||!z||!v){j->failed=1;goto done;}
    for(int row=j->first;row<j->last;row++)for(int col=0;col<j->cols;col++) {
        int x0=col*(cw+1),y0=j->h-row*(ch+1)-ch; unsigned sum=0;
        for(int y=0;y<ch;y++)for(int x=0;x<cw;x++)sum+=j->src[(y0+y)*w+x0+x];
        if(!sum)continue;
        int left=j->ink?j->ink[row*j->cols+col]&255:0,right=j->ink?(j->ink[row*j->cols+col]>>8)&255:cw;
        if(left>=right||right>cw)continue;
        int iw=right-left; size_t count=(size_t)iw*ch;
        for(int y=0;y<ch;y++)for(int x=0;x<iw;x++)a[y*iw+x]=j->src[(y0+y)*w+x0+left+x]/255.0f;
        text_distance(a,out,tmp,iw,ch,f,d,v,z);for(size_t i=0;i<count;i++)a[i]=1-a[i];
        text_distance(a,in,tmp,iw,ch,f,d,v,z);for(size_t i=0;i<count;i++)out[i]-=in[i];
        // A whole glyph takes one path. Ridge maxima estimate stroke half-width; many
        // crossings or three or more separated runs in both axes indicate dense glyphs.
        float min_ridge=1e10f; int junctions=0,max_xruns=0,max_yruns=0;
        for(int py=0;py<ch;py++) {
            int runs=0,was=0;
            for(int px=0;px<iw;px++) {
                int value=j->src[(y0+py)*w+x0+left+px],filled=value>=64;
                if(filled&&!was)runs++;was=filled;
                if(value<240)continue;
                int l=px?px-1:px,r=px+1<iw?px+1:px,u=py?py-1:py,b=py+1<ch?py+1:py;
                if(j->src[(y0+py)*w+x0+left+l]>64 && j->src[(y0+py)*w+x0+left+r]>64 &&
                   j->src[(y0+u)*w+x0+left+px]>64 && j->src[(y0+b)*w+x0+left+px]>64)junctions++;
                int along=(j->src[(y0+py)*w+x0+left+l]>128 && j->src[(y0+py)*w+x0+left+r]>128) ||
                          (j->src[(y0+u)*w+x0+left+px]>128 && j->src[(y0+b)*w+x0+left+px]>128);
                float ridge=in[py*iw+px]; int maximum=along;
                for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++) {
                    int x=px+dx,y=py+dy;
                    if(x>=0&&x<iw&&y>=0&&y<ch&&in[y*iw+x]>ridge+0.001f)maximum=0;
                }
                if(maximum&&ridge>0.05f&&ridge<min_ridge)min_ridge=ridge;
            }
            if(runs>max_xruns)max_xruns=runs;
        }
        for(int px=0;px<iw;px++) {
            int runs=0,was=0;
            for(int py=0;py<ch;py++) {int filled=j->src[(y0+py)*w+x0+left+px]>=64;if(filled&&!was)runs++;was=filled;}
            if(runs>max_yruns)max_yruns=runs;
        }
        int sharpen=j->force_bilinear || (j->ink && (j->ink[row*j->cols+col]&0x10000)) ||
                    (min_ridge<1.0f)||(junctions>256)||(max_xruns>=3&&max_yruns>=3);
        if(sharpen)j->sharpen_count++;else j->sdf_count++;
        // Fit coverage against signed distance in source pixels. Empty bins are
        // interpolated, and the fitted profile is constrained to be non-increasing.
        double sums[128]={0}; unsigned counts[128]={0}; uint8_t *profile=j->profiles+(size_t)(row*j->cols+col)*128;
        for(int py=0;py<ch;py++)for(int px=0;px<iw;px++) {
            float distance=out[py*iw+px]; int bin=(int)roundf(distance*8)+64;
            if(bin<0)bin=0;if(bin>127)bin=127;
            sums[bin]+=j->src[(y0+py)*w+x0+left+px];counts[bin]++;
        }
        int previous=-1;
        for(int bin=0;bin<128;bin++)if(counts[bin]) {
            int value=(int)(sums[bin]/counts[bin]+0.5);
            if(previous<0)for(int k=0;k<bin;k++)profile[k]=(uint8_t)value;
            else {
                int start=profile[previous];
                for(int k=previous+1;k<bin;k++)profile[k]=(uint8_t)(start+(value-start)*(k-previous)/(bin-previous));
            }
            profile[bin]=(uint8_t)value; previous=bin;
        }
        if(previous<0)memset(profile,0,128);else for(int k=previous+1;k<128;k++)profile[k]=profile[previous];
        for(int k=1;k<128;k++)if(profile[k]>profile[k-1])profile[k]=profile[k-1];
        for(int y=0;y<ch*scale;y++)for(int x=0;x<cw*scale;x++) {
            float sx=(x+0.5f)/scale-0.5f,sy=(y+0.5f)/scale-0.5f;int ix=(int)floorf(sx),iy=(int)floorf(sy);float fx=sx-ix,fy=sy-iy;
            int bx=ix+1,by=iy+1;if(ix<0)ix=0;if(iy<0)iy=0;if(bx>=cw)bx=cw-1;if(by>=ch)by=ch-1;
            const uint8_t *s=j->src;float bil=((s[(y0+iy)*w+x0+ix]*(1-fx)+s[(y0+iy)*w+x0+bx]*fx)*(1-fy)+(s[(y0+by)*w+x0+ix]*(1-fx)+s[(y0+by)*w+x0+bx]*fx)*fy)/255.0f;
            float dist=7.99f;
            if(x>=left*scale&&x<right*scale){float tx=sx-left;int ax=(int)floorf(tx),xx=ax+1;float txf=tx-ax;if(ax<0)ax=0;if(xx>=iw)xx=iw-1; dist=scale*((out[iy*iw+ax]*(1-txf)+out[iy*iw+xx]*txf)*(1-fy)+(out[by*iw+ax]*(1-txf)+out[by*iw+xx]*txf)*fy);}
            if(dist < -8)dist=-8;if(dist>7.75f)dist=7.75f;
            int di=(int)roundf(dist*8)+64;if(di<0)di=0;if(di>126)di=126;int bi=(int)(bil*255+0.5f);if(bi<0)bi=0;if(bi>255)bi=255;
            unsigned code=(unsigned)di|((unsigned)bi<<7)|(sharpen?32768u:0);j->field[(size_t)(y0*scale+y)*ow+x0*scale+x]=(uint16_t)code;
        }
    }
done: free(a);free(out);free(in);free(tmp);free(f);free(d);free(z);free(v);return NULL;
}
typedef struct { TextSdfJob *job; uint8_t *dst; } TextCorrection;
static uint8_t text_base(unsigned code,const uint8_t *profile,int scale,int force_bilinear) {
    if(force_bilinear || (code&32768))return (code>>7)&255;
    int bin=64+((int)(code&127)-64)/scale;
    if(bin<0)bin=0;if(bin>127)bin=127;
    return profile[bin];
}
static float text_tone(int value,float slope,float offset) {
    if(value==0)return 0;
    if(value==255)return 1;
    float v=(value/255.0f-0.5f)*slope+0.5f+offset;
    return v<0?0:v>1?1:v;
}
static void *text_sdf_correct(void *arg) {
    TextCorrection *c=arg; TextSdfJob *j=c->job; text_low_priority();
    int w=j->w,ow=w*j->scale,cw=j->cw,ch=j->ch,scale=j->scale;
    for(int row=j->first;row<j->last;row++)for(int col=0;col<j->cols;col++) {
        int x0=col*(cw+1),y0=j->h-row*(ch+1)-ch; unsigned original[256]={0},base[2][256]={{0}};
        uint8_t *profile=j->profiles+(size_t)(row*j->cols+col)*128; unsigned sum=0;
        for(int y=0;y<ch;y++)for(int x=0;x<cw;x++) {
            unsigned v=j->src[(y0+y)*w+x0+x];original[v]++;sum+=v;
        }
        if(!sum)continue;
        int can_fallback=0;
        for(int y=0;y<ch*scale;y++)for(int x=0;x<cw*scale;x++) {
            size_t p=(size_t)(y0*scale+y)*ow+x0*scale+x; unsigned code=j->field[p];
            if(code==65535){base[0][c->dst[p]]++;base[1][c->dst[p]]++;continue;}
            if(!(code&32768))can_fallback=1;
            base[0][text_base(code,profile,scale,0)]++;
            base[1][text_base(code,profile,scale,1)]++;
        }
        double target1=0,target2=0,target3=0;
        for(int k=0;k<256;k++) {
            target1+=original[k]*text_shader(k/255.0f,0.4f,0.6f)*scale*scale;
            target2+=original[k]*text_shader(k/255.0f,0.2f,0.5f)*scale*scale;
            target3+=original[k]*(k/255.0)*scale*scale;
        }
        // Prefer non-compressing curves, but allow a softer fit if preserving
        // contrast would miss the modeled ink weight by more than five percent.
        float best_slope=1,best_offset=0; int best_fallback=0; double best_error=1e30;
        for(int pass=0;pass<2;pass++) {
          for(int fallback=0;fallback<=can_fallback;fallback++) {
            for(int si=0;si<(pass?3:29);si++) {
                float slope=(pass?0.25f:1.0f)+si*0.25f,lo=-0.75f,hi=0.75f;
                for(int it=0;it<13;it++) {
                    float mid=(lo+hi)*0.5f;double v=0;
                    for(int k=0;k<256;k++)if(base[fallback][k])
                        v+=base[fallback][k]*text_shader(text_tone(k,slope,mid),0.4f,0.6f);
                    if(v<target1)lo=mid;else hi=mid;
                }
                for(int oi=-8;oi<=8;oi++) {
                    float offset=(lo+hi)*0.5f+oi*0.005f;double v1=0,v2=0;
                    for(int k=0;k<256;k++)if(base[fallback][k]) {
                        float v=text_tone(k,slope,offset);
                        v1+=base[fallback][k]*text_shader(v,0.4f,0.6f);
                        v2+=base[fallback][k]*text_shader(v,0.2f,0.5f);
                    }
                    double e1=fabs(v1-target1)/(target1>1?target1:1),e2=fabs(v2-target2)/(target2>1?target2:1);
                    double error=e1>e2?e1:e2;
                    if(error<best_error){best_error=error;best_slope=slope;best_offset=offset;best_fallback=fallback;}
                }
            }
            if(best_error<=0.015)break;
        }
          if(best_error<=0.05)break;
        }
        uint8_t lut[256];
        for(int k=0;k<256;k++)lut[k]=(uint8_t)(text_tone(k,best_slope,best_offset)*255+0.5f);
        // Preserve the source's peak opacity as well as the thresholded area.
        // Values above the modeled thresholds still carry intensity; lowering
        // them to match integrated coverage can make small glyphs grey.
        int source_peak=255, output_peak=0, peak_base=255;
        while(source_peak>0 && !original[source_peak])source_peak--;
        while(peak_base>0 && !base[best_fallback][peak_base])peak_base--;
        for(int k=0;k<256;k++)if(base[best_fallback][k] && lut[k]>output_peak)output_peak=lut[k];
        if(output_peak>153 && source_peak>153 && output_peak!=source_peak) {
            for(int k=0;k<256;k++)if(lut[k]>153) {
                int value=153+(lut[k]-153)*(source_peak-153)/(output_peak-153);
                lut[k]=(uint8_t)(value>255?255:value);
            }
        }
        // Match raw coverage where possible without dimming the peak or
        // moving either modeled shader threshold.
        int floor=source_peak*85/100;if(floor<153)floor=153;
        double raw=0,down=0,up=0;
        for(int k=0;k<256;k++) {
            int value=lut[k]; unsigned count=base[best_fallback][k];
            raw+=count*(value/255.0);
            if(value>153 && k!=peak_base) {
                if(value>floor)down+=count*((value-floor)/255.0);
                if(value<source_peak)up+=count*((source_peak-value)/255.0);
            }
        }
        double delta=target3-raw,capacity=delta<0?down:up;
        if(capacity>0 && source_peak>153 && output_peak>153) {
            double fraction=fabs(delta)/capacity;if(fraction>1)fraction=1;
            for(int k=0;k<256;k++)if(lut[k]>153 && k!=peak_base) {
                double value=lut[k];
                value=delta<0?(value>floor?value-fraction*(value-floor):value):
                              (value<source_peak?value+fraction*(source_peak-value):value);
                lut[k]=(uint8_t)(value+0.5);
            }
        }
        // A plausible thresholded area can still have pale interiors. If the
        // SDF loses most high-opacity samples, retain the filtered source for
        // this glyph rather than altering the neighboring glyphs or panes.
        if(can_fallback && !best_fallback) {
            unsigned original_high=0, output_high=0;
            for(int k=0;k<256;k++) {
                if(k>=220)original_high+=base[1][k];
                if(lut[k]>=220)output_high+=base[0][k];
            }
            if(original_high && (uint64_t)output_high*5<(uint64_t)original_high*4) {
                best_fallback=1;
                for(int k=0;k<256;k++)lut[k]=(uint8_t)k;
            }
        }
        if(best_fallback){j->sdf_count--;j->sharpen_count++;}
        for(int y=0;y<ch*scale;y++)for(int x=0;x<cw*scale;x++) {
            size_t p=(size_t)(y0*scale+y)*ow+x0*scale+x;unsigned code=j->field[p];
            if(code!=65535)c->dst[p]=lut[text_base(code,profile,scale,best_fallback)];
        }
    }
    return NULL;
}
static int text_sdf_sheet(const uint8_t *src,uint8_t *dst,int w,int h,int cw,int ch,int cols,int rows,int scale,const uint32_t *ink,int force_bilinear,int *sdf_count,int *sharpen_count) {
    size_t pixels=(size_t)w*h*scale*scale;uint16_t *field=malloc(pixels*sizeof(*field));
    uint8_t *profiles=malloc((size_t)cols*rows*128);
    if(!field||!profiles){free(field);free(profiles);return 0;}
    // A sentinel denotes padding or a truly empty cell, which keeps its existing output.
    memset(field,0xff,pixels*sizeof(*field));TextSdfJob jobs[4];pthread_t threads[3];int started[3]={0};
    for(int i=0;i<4;i++)jobs[i]=(TextSdfJob){src,ink,field,profiles,w,h,cw,ch,cols,rows,scale,rows*i/4,rows*(i+1)/4,force_bilinear,0,0,0};
    for(int i=1;i<4;i++){if(!pthread_create(&threads[i-1],NULL,text_sdf_rows,&jobs[i]))started[i-1]=1;else text_sdf_rows(&jobs[i]);}
    text_sdf_rows(&jobs[0]);for(int i=1;i<4;i++)if(started[i-1])pthread_join(threads[i-1],NULL);
    for(int i=0;i<4;i++)if(jobs[i].failed){free(field);free(profiles);return 0;}
    TextCorrection corrections[4]; memset(started,0,sizeof(started));
    for(int i=0;i<4;i++)corrections[i]=(TextCorrection){&jobs[i],dst};
    for(int i=1;i<4;i++){if(!pthread_create(&threads[i-1],NULL,text_sdf_correct,&corrections[i]))started[i-1]=1;else text_sdf_correct(&corrections[i]);}
    text_sdf_correct(&corrections[0]);for(int i=1;i<4;i++)if(started[i-1])pthread_join(threads[i-1],NULL);
    *sdf_count=0;*sharpen_count=0;
    for(int i=0;i<4;i++){*sdf_count+=jobs[i].sdf_count;*sharpen_count+=jobs[i].sharpen_count;}
    free(field);free(profiles);return 1;
}
