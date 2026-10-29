// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_indexed.h"
#include <algorithm>
namespace lift_simd_shift {
using namespace lift_imm; using namespace lift_simd_basic; using lift_simd_diff::lop; using lift_simd_diff::gen; using lift_simd_same::vq;
inline int type(uint32_t w) { unsigned o=bits(w,28,4),p=bits(w,23,2),q=bits(w,19,4),r=bits(w,10,9); if((o&13)==5 && p==2 && (r&1)) return 2; if(!(o&9) && p==2 && (r&1)) return q?1:3; return 0; }
inline U expanded(unsigned op,unsigned c,unsigned imm) {
    unsigned k=c>>1; U v=imm;
    if(k<4) { v<<=8*k; return v<<32|v; }
    if(k==4 || k==5) { if(k==5) v<<=8; return v|v<<16|v<<32|v<<48; }
    if(k==6) { v=(c&1)?(v<<16)|0xffff:(v<<8)|0xff; return v<<32|v; }
    if(!(c&1) && !op) { U out=0; for(int i=0;i<8;i++) out|=v<<(8*i); return out; }
    if(!(c&1) && op) { U out=0; for(int i=0;i<8;i++) if((imm>>i)&1) out|=U(255)<<(8*i); return out; }
    if(!op) { v=lift_scalar_fp::expand(imm,32); return v<<32|v; }
    return lift_scalar_fp::expand(imm,64);
}
inline std::string lift(uint32_t w,int type) {
    unsigned Q=bits(w,30,1),U_=bits(w,29,1),nn=bits(w,5,5),d=bits(w,0,5);
    if(type==3) {
        unsigned c=bits(w,12,4),o2=bits(w,11,1),imm8=(bits(w,16,3)<<5)|bits(w,5,5);
        if(o2) throw std::runtime_error("fmov half imm");
        if(c==15 && U_ && !Q) throw std::runtime_error("fmov 1d");
        U imm=expanded(U_,c,imm8);
        if(c<12 && (c&1)) { if(U_) imm=~imm; return vwr("vu64",Q,d,vr("vu64",d)+(U_?" & ":" | ")+hex(imm)); }
        if(U_ && c!=14 && c!=15) imm=~imm;
        auto value=Q && imm?"((u128)"+hex(imm)+" << 64 | "+hex(imm)+")":hex(imm);
        return "c->v["+n(d)+"] = "+value+";";
    }
    bool scalar=type==2; unsigned immh=bits(w,19,4),immb=bits(w,16,3),opc=bits(w,11,5);
    if(!immh) throw std::runtime_error("shift immh");
    unsigned sz=0; for(unsigned v=immh;v>>=1;) ++sz;
    unsigned es=8u<<sz,ih=(immh<<3)|immb,rsh=2*es-ih,lsh=ih-es;
    if(scalar) Q=1;
    unsigned ne=scalar?1:(8u<<Q)>>sz;
    if(opc==28 || opc==31) {
        if(sz<2) throw std::runtime_error("fp16 fixed");
        if(sz==3 && !Q) throw std::runtime_error("fixed 1d");
        auto F=sz==2?"s":"d",VT=sz==2?"vf32":"vf64",VU=sz==2?"vu32":"vu64",SF=sz==2?"VSF32":"VSF64",SU=sz==2?"VS32":"VS64",SET=sz==2?"FP_SETS":"FP_SETD";
        if(opc==28) { if(scalar) return std::string(SET)+"("+n(d)+", fromint_"+F+"(c, "+SU+"("+n(nn)+"), "+n(es)+", "+n(U_)+", "+n(rsh)+"));"; return vwr(VT,Q,d,"vfromint_"+std::string(F)+"(c, "+vr(VU,nn)+", "+n(rsh)+", "+n(U_)+")"); }
        if(scalar) return "c->v["+n(d)+"] = (uint"+n(8u<<sz)+"_t)toint_"+F+"(c, "+SF+"("+n(nn)+"), 3, "+n(rsh)+", "+n(es)+", "+n(U_)+");";
        return vwr(VU,Q,d,"vtoint_"+std::string(F)+"(c, "+vq(VT,Q,nn)+", 3, "+n(rsh)+", "+n(U_)+")");
    }
    if(opc>=16 && opc<=19) {
        if(sz==3) throw std::runtime_error("narrow shift size");
        ne=scalar?1:8u>>sz;
        const char *op=opc==16?(U_?"QSHRUN":"SHRN"):opc==17?(U_?"QRSHRUN":"RSHRN"):opc==18?"QSHRN":"QRSHRN";
        if(scalar && opc<18 && !U_) throw std::runtime_error("scalar shrn");
        unsigned part=scalar?0:Q;
        return gen(lop(op,opc>=18?U_:0),sz,ne,d,sz,part*ne,part,nn,sz+1,0,-1,0,0,-1,rsh);
    }
    if(opc==20) { if(scalar || sz==3) throw std::runtime_error("sshll"); ne=8u>>sz; return gen(lop("SHLLI",U_),sz,ne,d,sz+1,0,0,nn,sz,Q*ne,-1,0,0,-1,lsh); }
    if(sz==3 && !Q) throw std::runtime_error("shift 1d");
    if(scalar && sz!=3 && opc!=12 && opc!=14) throw std::runtime_error("scalar shift size");
    auto T=vu(sz),S="vs"+n(8u<<sz); std::string e;
    if(opc==0 || opc==2) {
        if(U_) e=rsh>=es?"("+T+"){}":"("+vr(T,nn)+" >> "+n(rsh)+")";
        else e="("+T+")("+vr(S,nn)+" >> "+n(std::min(rsh,es-1))+")";
        if(opc==2) e=vr(T,d)+" + "+e;
        return scalar?"c->v["+n(d)+"] = (uint64_t)(("+e+")[0]);":vwr(T,Q,d,e);
    }
    auto sameimm=[&](const std::string &name,unsigned u,unsigned imm) { return gen(lop(name,u),sz,ne,d,sz,0,0,nn,sz,0,-1,sz,0,-1,imm); };
    if(opc==4 || opc==6) return sameimm(opc==4?"RSHRI":"RSRAI",U_,rsh);
    if(opc==8) { if(!U_) throw std::runtime_error("sri U"); return sameimm("SRI",0,rsh); }
    if(opc==10) { if(U_) return sameimm("SLI",0,lsh); e=vr(T,nn)+" << "+n(lsh); return scalar?"c->v["+n(d)+"] = (uint64_t)(("+e+")[0]);":vwr(T,Q,d,e); }
    if(opc==12) { if(!U_) throw std::runtime_error("sqshlu U"); return sameimm("QSHLUI",0,lsh); }
    if(opc==14) return sameimm("QSHLI",U_,lsh);
    throw std::runtime_error("shift imm opcode");
}
}
