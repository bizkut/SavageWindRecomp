// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_diff.h"
namespace lift_simd_same {
using namespace lift_imm; using namespace lift_simd_basic; using lift_simd_diff::lop; using lift_simd_diff::gen;
inline int type(uint32_t w) {
    unsigned o=bits(w,28,4),p=bits(w,23,2),q=bits(w,19,4),r=bits(w,10,9);
    if(lift_simd_basic::type(w)) return 0;
    if((o&13)==5 && !(p&2) && (q&4) && (r&1)) return 2;
    if(!(o&9) && !(p&2) && (q&4) && (r&1)) return 1;
    return 0;
}
inline std::string same(const std::string &op,unsigned sz,unsigned ne,unsigned d,unsigned a,int b=-1) { return gen(op,sz,ne,d,sz,0,0,a,sz,0,b,sz,0); }
inline std::pair<std::vector<unsigned>,std::vector<unsigned>> pair_idx(unsigned Q,unsigned sz) {
    unsigned ne=(8u<<Q)>>sz,NL=16u>>sz; std::vector<unsigned> src,ev,od;
    for(unsigned i=0;i<ne;i++) src.push_back(i);
    for(unsigned i=0;i<ne;i++) src.push_back(NL+i);
    for(unsigned i=0;i<ne;i++) { ev.push_back(src[2*i]); od.push_back(src[2*i+1]); }
    ev.resize(NL,ne%NL); od.resize(NL,ne%NL); return {ev,od};
}
inline std::string vq(const std::string &t,unsigned Q,unsigned r) { return Q?vr(t,r):"VQ("+t+", 0, "+n(r)+")"; }
inline std::string fp(uint32_t w,bool scalar) {
    unsigned Q=bits(w,30,1),U_=bits(w,29,1),a=bits(w,23,1),sz=bits(w,22,1),m=bits(w,16,5),op=bits(w,11,5),nn=bits(w,5,5),d=bits(w,0,5);
    if(!scalar && sz && !Q) throw std::runtime_error("fp 2d Q");
    std::string F=sz?"d":"s",VT=sz?"vf64":"vf32",VU=sz?"vu64":"vu32",SF=sz?"VSF64":"VSF32",SET=sz?"FP_SETD":"FP_SETS";
    int cmp=-1; if(op==28) { if(!U_ && !a) cmp=0; else if(U_) cmp=a?2:1; } else if(op==29 && U_) cmp=a?4:3;
    if(cmp>=0) { if(scalar) return "c->v["+n(d)+"] = (uint"+n(32u<<sz)+"_t)vcmp_"+F+"(c, "+n(cmp)+", ("+VT+"){"+SF+"("+n(nn)+")}, ("+VT+"){"+SF+"("+n(m)+")})[0];"; return vwr(VU,Q,d,"vcmp_"+F+"(c, "+n(cmp)+", "+vq(VT,Q,nn)+", "+vq(VT,Q,m)+")"); }
    std::string f;
    if(!U_) { if(op==24) f=a?"FOP_MINNM":"FOP_MAXNM"; else if(op==26) f=a?"FOP_SUB":"FOP_ADD"; else if(op==27 && !a) f="FOP_MULX"; else if(op==30) f=a?"FOP_MIN":"FOP_MAX"; else if(op==31) f=a?"FOP_RSQRTS":"FOP_RECPS"; }
    else { if(op==27 && !a) f="FOP_MUL"; else if(op==31 && !a) f="FOP_DIV"; else if(op==26 && a) f="FOP_ABD"; }
    if(scalar) {
        if(!((!U_ && !a && (op==27 || op==31)) || (!U_ && a && op==31) || (U_ && a && op==26))) throw std::runtime_error("scalar fp 3same");
        auto A=SF+"("+n(nn)+")"; if(f=="FOP_RECPS" || f=="FOP_RSQRTS") A="neg_"+F+"("+A+")";
        return SET+"("+n(d)+", op_"+F+"(c, "+f+", "+A+", "+SF+"("+n(m)+"), 0));";
    }
    auto A=vq(VT,Q,nn),B=vq(VT,Q,m); std::string pair;
    if(U_ && (op==24 || op==30 || (op==26 && !a))) { if(op==24) pair=a?"FOP_MINNM":"FOP_MAXNM"; else if(op==30) pair=a?"FOP_MIN":"FOP_MAX"; else pair="FOP_ADD"; }
    auto upper=sz?"D":"S";
    if(!pair.empty()) { auto [ev,od]=pair_idx(Q,2+sz); return std::string("SV_FOP_")+upper+"("+pair+", "+n(Q)+", "+n(d)+", "+shuf(A,B,ev)+", "+shuf(A,B,od)+", ("+VT+"){});"; }
    if(!U_ && op==25) return std::string("SV_FOP_")+upper+"(FOP_MADD, "+n(Q)+", "+n(d)+", "+vq(VT,Q,d)+", "+(a?"vneg_"+F+"("+A+")":A)+", "+B+");";
    if(f.empty()) throw std::runtime_error("fp 3same opcode");
    if(f=="FOP_RECPS" || f=="FOP_RSQRTS") A="vneg_"+F+"("+A+")";
    return std::string("SV_FOP_")+upper+"("+f+", "+n(Q)+", "+n(d)+", "+A+", "+B+", ("+VT+"){});";
}
inline std::string lift(uint32_t w,int type) {
    bool scalar=type==2; unsigned Q=bits(w,30,1),U_=bits(w,29,1),sz=bits(w,22,2),m=bits(w,16,5),op=bits(w,11,5),nn=bits(w,5,5),d=bits(w,0,5);
    if(op>=24) return fp(w,scalar);
    if(scalar) {
        if(op==1 || op==5) return same(lop(op==1?(U_?"UQADD":"SQADD"):(U_?"UQSUB":"SQSUB")),sz,1,d,nn,m);
        if(op==9 || op==11) return same(lop(op==9?"QSHL":"QRSHL",U_),sz,1,d,nn,m);
        if(op==22) { if(sz!=1 && sz!=2) throw std::runtime_error("sqdmulh size"); return same(lop(U_?"SQRDMULH":"SQDMULH"),sz,1,d,nn,m); }
        if(sz!=3) throw std::runtime_error("scalar 3same size");
        std::string a="VS64("+n(nn)+")",b="VS64("+n(m)+")",x=U_?a:"(int64_t)"+a,y=U_?b:"(int64_t)"+b, v="c->v["+n(d)+"] = ";
        if(op==6 || op==7) return v+"("+x+(op==6?" > ":" >= ")+y+") ? ~0ull : 0;";
        if(op==8 || op==10) return same(lop(op==8?"SHL":"RSHL",U_),3,1,d,nn,m);
        if(op==16) return v+"(uint64_t)("+a+(U_?" - ":" + ")+b+");";
        if(op==17) return v+"("+a+(U_?" == ":" & ")+b+") ? ~0ull : 0;";
        throw std::runtime_error("scalar 3same opcode");
    }
    if(sz==3 && !Q && op!=3) throw std::runtime_error("3same 1d");
    unsigned ne=(8u<<Q)>>sz; auto T=vu(sz),S="vs"+n(8u<<sz),a=vr(T,nn),b=vr(T,m),o=vr(T,d),sa=vr(S,nn),sb=vr(S,m),x=U_?a:sa,y=U_?b:sb;
    if((op==0 || op==2 || op==4 || op==14 || op==15 || op==12 || op==13) && sz==3) throw std::runtime_error("3same size");
    if(op==0 || op==2 || op==4) return same(lop(op==0?"HADD":op==2?"RHADD":"HSUB",U_),sz,ne,d,nn,m);
    if(op==1 || op==5) return vwr(T,Q,d,"("+T+")__builtin_elementwise_"+(op==1?"add":"sub")+"_sat("+x+", "+y+")");
    if(op==3) { a=vr("vu64",nn);b=vr("vu64",m);o=vr("vu64",d); std::string e;
        if(!U_) { if(sz==0)e=a+" & "+b; else if(sz==1)e=a+" & ~"+b; else if(sz==2)e=a+" | "+b; else e=a+" | ~"+b; }
        else { if(sz==0)e=a+" ^ "+b; else if(sz==1)e="("+o+" & "+a+") | (~"+o+" & "+b+")"; else if(sz==2)e="("+o+" & ~"+b+") | ("+a+" & "+b+")"; else e="("+o+" & "+b+") | ("+a+" & ~"+b+")"; }
        return vwr("vu64",Q,d,e);
    }
    if(op==6 || op==7) return vwr(T,Q,d,"("+T+")("+x+(op==6?" > ":" >= ")+y+")");
    if(op>=8 && op<=11) { const char *ops[]={"SHL","QSHL","RSHL","QRSHL"}; return same(lop(ops[op-8],U_),sz,ne,d,nn,m); }
    if(op==12 || op==13) return vwr(T,Q,d,"("+T+")__builtin_elementwise_"+(op==12?"max":"min")+"("+x+", "+y+")");
    if(op==14 || op==15) { auto e="("+T+")__builtin_elementwise_max("+x+", "+y+") - ("+T+")__builtin_elementwise_min("+x+", "+y+")"; return vwr(T,Q,d,op==15?o+" + ("+e+")":e); }
    if(op==16) return vwr(T,Q,d,a+(U_?" - ":" + ")+b);
    if(op==17) return vwr(T,Q,d,U_?"("+T+")("+a+" == "+b+")":"("+T+")(("+a+" & "+b+") != 0)");
    if(sz==3 && op!=23) throw std::runtime_error("3same size");
    if(op==18) return vwr(T,Q,d,o+(U_?" - ":" + ")+a+" * "+b);
    if(op==19) { if(U_) { if(sz) throw std::runtime_error("pmul size"); return same(lop("PMUL"),0,ne,d,nn,m); } return vwr(T,Q,d,a+" * "+b); }
    if(op==20 || op==21 || op==23) { if(op==23 && U_) throw std::runtime_error("addp U"); auto [ev,od]=pair_idx(Q,sz); auto A=U_||op==23?a:sa,B=U_||op==23?b:sb,L=shuf(A,B,ev),R=shuf(A,B,od); return vwr(T,Q,d,op==23?"("+T+")("+L+" + "+R+")":"("+T+")__builtin_elementwise_"+(op==20?"max":"min")+"("+L+", "+R+")"); }
    if(op==22) { if(sz!=1 && sz!=2) throw std::runtime_error("sqdmulh size"); return same(lop(U_?"SQRDMULH":"SQDMULH"),sz,ne,d,nn,m); }
    throw std::runtime_error("3same opcode");
}
}
