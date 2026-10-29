// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_mem.h"
namespace lift_simd_mem {
using namespace lift_imm;
inline std::string vec(unsigned r) { return "c->v["+std::to_string(r)+"]"; }
inline std::string lift(uint32_t w,U pc) {
    unsigned rt=bits(w,0,5);
    if(bits(w,27,3)==3 && !bits(w,24,2)) {
        unsigned opc=bits(w,30,2); if(opc==3) throw std::runtime_error("ldr literal opc");
        const char *ld[]={"LD32","LD64","LD128"}; return vec(rt)+" = "+ld[opc]+"("+hex(pc+U(sext(bits(w,5,19),19)*4))+");";
    }
    if(!bits(w,31,1) && (bits(w,24,6)==12 || bits(w,24,6)==13)) {
        unsigned Q=bits(w,30,1),L=bits(w,22,1),post=bits(w,23,1),rm=bits(w,16,5),rn=bits(w,5,5),sz=bits(w,10,2),total=0;
        if(!post && rm) throw std::runtime_error("ldst struct rm");
        std::string acc;
        if(!bits(w,24,1)) {
            if(bits(w,21,1)) throw std::runtime_error("ldst multiple bit21");
            unsigned opc=bits(w,12,4),selem=0,rpt=0;
            switch(opc) { case 0:selem=4;rpt=1;break; case 2:selem=1;rpt=4;break; case 4:selem=3;rpt=1;break; case 6:selem=1;rpt=3;break; case 7:selem=1;rpt=1;break; case 8:selem=2;rpt=1;break; case 10:selem=1;rpt=2;break; default:throw std::runtime_error("ldst multiple opcode"); }
            if(sz==3 && !Q && selem>1) throw std::runtime_error("ldst multiple 1d");
            total=(8u<<Q)*selem*rpt;
            if(selem==1) { for(unsigned i=0;i<rpt;i++) {
                if(i) acc+=" ";
                unsigned r=(rt+i)%32;
                if(L) acc+=vec(r)+" = "+(Q?"LD128":"LD64")+"(ea + "+std::to_string(i*(Q?16:8))+");";
                else acc+=std::string(Q?"ST128":"ST64")+"(ea + "+std::to_string(i*(Q?16:8))+", "+(Q?vec(r):"VS64("+std::to_string(r)+")")+");";
            }} else acc=std::string(L?"sl_ldm(c, ":"sl_stm(c, ")+std::to_string(Q)+", "+std::to_string(sz)+", "+std::to_string(selem)+", "+std::to_string(rpt)+", "+std::to_string(rt)+", ea);";
        } else {
            unsigned opc=bits(w,13,3),S=bits(w,12,1),R=bits(w,21,1),scale=opc>>1,selem=(((opc&1)<<1)|R)+1,idx=0;
            if(scale==3) { if(!L || S) throw std::runtime_error("ldnr"); total=selem<<sz; acc="sl_ldr(c, "+std::to_string(Q)+", "+std::to_string(sz)+", "+std::to_string(selem)+", "+std::to_string(rt)+", ea);"; }
            else {
                if(scale==0) idx=(Q<<3)|(S<<2)|sz;
                else if(scale==1) { if(sz&1) throw std::runtime_error("ld1 h size"); idx=(Q<<2)|(S<<1)|(sz>>1); }
                else { if(sz&2) throw std::runtime_error("ld1 s size"); if(sz&1) { if(S) throw std::runtime_error("ld1 d S"); idx=Q; scale=3; } else idx=(Q<<1)|S; }
                total=selem<<scale; acc=std::string(L?"sl_lds(c, ":"sl_sts(c, ")+std::to_string(scale)+", "+std::to_string(selem)+", "+std::to_string(idx)+", "+std::to_string(rt)+", ea);";
            }
        }
        auto base=reg(1,rn,true); std::string out="{ uint64_t ea = "+base+"; "+acc;
        if(post) out+=" "+base+" = ea + "+(rm==31?std::to_string(total):x(rm))+";";
        return out+" }";
    }
    if(bits(w,27,3)==5) {
        unsigned opc=bits(w,30,2),mode=bits(w,23,2),L=bits(w,22,1),rt2=bits(w,10,5),rn=bits(w,5,5);
        if(opc==3) throw std::runtime_error("pair opc 3");
        unsigned scale=2+opc,sz=1u<<scale; auto n=std::to_string(sz); int64_t off=sext(bits(w,15,7),7)*sz;
        auto base=reg(1,rn,true); bool post=mode==1,wb=mode==1 || mode==3;
        std::string out="{ uint64_t ea = "+(post?base:base+" "+lift_mem::signed_offset(off))+"; ";
        const char *ld[]={"LD32","LD64","LD128"}, *st[]={"ST32","ST64","ST128"};
        if(L) out+="u128 a = "+std::string(ld[opc])+"(ea), b = "+ld[opc]+"(ea + "+n+"); "+vec(rt)+" = a; "+vec(rt2)+" = b;";
        else { auto v1=scale==4?vec(rt):"VS"+std::to_string(sz*8)+"("+std::to_string(rt)+")"; auto v2=scale==4?vec(rt2):"VS"+std::to_string(sz*8)+"("+std::to_string(rt2)+")"; out+=std::string(st[opc])+"(ea, "+v1+"); "+st[opc]+"(ea + "+n+", "+v2+");"; }
        if(wb) out+=" "+base+" = "+(post?"ea "+lift_mem::signed_offset(off):"ea")+";";
        return out+" }";
    }
    if(bits(w,27,3)==7) {
        unsigned size=bits(w,30,2),opc=bits(w,22,2),rn=bits(w,5,5),scale=size;
        if(opc&2) { if(size) throw std::runtime_error("ldst simd size"); scale=4; }
        auto base=reg(1,rn,true); std::string ea,wb;
        if(bits(w,24,2)==1) ea=base+" + "+hex(U(bits(w,10,12))<<scale);
        else if(!bits(w,24,2)) {
            unsigned kind=bits(w,10,2);
            if(bits(w,21,1)) { if(kind!=2) throw std::runtime_error("ldst simd reg kind"); unsigned rm=bits(w,16,5),opt=bits(w,13,3),S=bits(w,12,1); if(opt!=2 && opt!=3 && opt!=6 && opt!=7) throw std::runtime_error("ldst simd reg option"); ea=base+" + "+lift_reg::extend(rm,opt,S?scale:0); }
            else { auto si=lift_mem::signed_offset(sext(bits(w,12,9),9)); if(kind==0) ea=base+" "+si; else if(kind==1) { ea=base; wb=base+" "+si; } else if(kind==3) { ea=base+" "+si; wb="ea"; } else throw std::runtime_error("ldst simd unprivileged"); }
        } else throw std::runtime_error("ldst simd");
        const char *ld[]={"LD8","LD16","LD32","LD64","LD128"}, *st[]={"ST8","ST16","ST32","ST64","ST128"};
        std::string out="{ uint64_t ea = "+ea+"; ";
        if(opc&1) out+=vec(rt)+" = "+ld[scale]+"(ea);";
        else out+=std::string(st[scale])+"(ea, "+(scale==4?vec(rt):"VS"+std::to_string(8u<<scale)+"("+std::to_string(rt)+")")+");";
        if(!wb.empty()) out+=" "+base+" = "+wb+";";
        return out+" }";
    }
    throw std::runtime_error("ldst simd "+lift_branch::numberword(w));
}
}
