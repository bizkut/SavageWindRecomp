// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_shift.h"
namespace lift_simd_misc {
using namespace lift_imm; using namespace lift_simd_basic; using lift_simd_diff::lop; using lift_simd_diff::gen; using lift_simd_same::same; using lift_simd_same::vq;
inline int type(uint32_t w) {
    unsigned o=bits(w,28,4),p=bits(w,23,2),q=bits(w,19,4),r=bits(w,10,9); bool scalar=(o&13)==5;
    if(!scalar && (o&9)) return 0;
    if(!(p&2) && (q&7)==4 && (r&0x183)==2) return scalar?2:1;
    if(!(p&2) && (q&7)==6 && (r&0x183)==2) return scalar?4:3;
    return 0;
}
inline std::string fp(uint32_t w,bool scalar) {
    unsigned Q=bits(w,30,1),U_=bits(w,29,1),a=bits(w,23,1),sz=bits(w,22,1),opc=bits(w,12,5),nn=bits(w,5,5),d=bits(w,0,5);
    std::string F=sz?"d":"s",VT=sz?"vf64":"vf32",VU=sz?"vu64":"vu32",SF=sz?"VSF64":"VSF32",SU=sz?"VS64":"VS32",SET=sz?"FP_SETD":"FP_SETS";
    if(opc==22 || opc==23) {
        if(a) throw std::runtime_error("fcvtn a");
        if(opc==22 && U_) { if(!sz) throw std::runtime_error("fcvtxn size"); if(scalar) return "FP_SETS("+n(d)+", simd_d2s_odd(c, VSF64("+n(nn)+")));"; return "sv_fcvtn(c, 1, "+n(Q)+", 1, "+n(d)+", "+n(nn)+");"; }
        if(scalar || U_) throw std::runtime_error("fcvtn/l");
        if(opc==22) return "sv_fcvtn(c, "+n(sz)+", "+n(Q)+", 0, "+n(d)+", "+n(nn)+");";
        return "sv_fcvtl(c, "+n(sz)+", "+n(Q)+", "+n(d)+", "+n(nn)+");";
    }
    if(!scalar && sz && !Q) throw std::runtime_error("fp misc 2d Q");
    auto x=vq(VT,Q,nn);
    if(opc>=12 && opc<=15) {
        if(!a) throw std::runtime_error("fp16 misc");
        if(opc==15) { if(scalar) throw std::runtime_error("scalar fabs"); std::string mask=sz?"0x8000000000000000ull":"0x80000000u"; return vwr(VU,Q,d,vr(VU,nn)+(U_?" ^ ":" & ~")+mask); }
        int cop=-1; bool swap=false; if(opc==12) cop=U_?1:2; else if(opc==13) { cop=U_?1:0; swap=U_; } else if(opc==14 && !U_) { cop=2; swap=true; }
        if(cop<0) throw std::runtime_error("fcm zero");
        auto A=scalar?"("+VT+"){"+SF+"("+n(nn)+")}":x,B="("+VT+"){}"; if(swap) std::swap(A,B);
        auto expr="vcmp_"+F+"(c, "+n(cop)+", "+A+", "+B+")";
        return scalar?"c->v["+n(d)+"] = (uint"+n(32u<<sz)+"_t)"+expr+"[0];":vwr(VU,Q,d,expr);
    }
    if(opc==31) { if(!a) throw std::runtime_error("fp misc 11111"); if(U_) { if(scalar) throw std::runtime_error("scalar fsqrt"); return std::string("SV_FOP_")+(sz?"D":"S")+"(FOP_SQRT, "+n(Q)+", "+n(d)+", "+x+", "+x+", ("+VT+"){});"; } if(!scalar) throw std::runtime_error("vector frecpx"); return SET+"("+n(d)+", recpx_"+F+"(c, "+SF+"("+n(nn)+")));"; }
    if(opc==24 || opc==25) { if(scalar) throw std::runtime_error("scalar frint"); int rm=-1; unsigned k=opc&1; if(!U_) rm=a?(k?3:1):(k?2:0); else if(!a) rm=k?5:4; else if(k) rm=6; if(rm<0) throw std::runtime_error("frint"); return vwr(VT,Q,d,"vfrint_"+F+"(c, "+x+", "+n(rm)+")"); }
    if(opc==26 || opc==27 || (opc==28 && !a)) { unsigned rm=opc==26?a:opc==27?2+a:4; if(scalar) return "c->v["+n(d)+"] = (uint"+n(32u<<sz)+"_t)toint_"+F+"(c, "+SF+"("+n(nn)+"), "+n(rm)+", 0, "+n(32u<<sz)+", "+n(U_)+");"; return vwr(VU,Q,d,"vtoint_"+F+"(c, "+x+", "+n(rm)+", 0, "+n(U_)+")"); }
    if(opc==28) { if(scalar || sz) throw std::runtime_error("urecpe"); return same(lop(U_?"URSQRTE":"URECPE"),2,(8u<<Q)>>2,d,nn); }
    if(opc==29) {
        if(!a) { if(scalar) return SET+"("+n(d)+", fromint_"+F+"(c, "+SU+"("+n(nn)+"), "+n(32u<<sz)+", "+n(U_)+", 0));"; return vwr(VT,Q,d,"vfromint_"+F+"(c, "+vr(VU,nn)+", 0, "+n(U_)+")"); }
        if(scalar) return SET+"("+n(d)+", "+(U_?"rsqrte":"recpe")+"_"+F+"(c, "+SF+"("+n(nn)+")));";
        return vwr(VT,Q,d,"v"+std::string(U_?"rsqrte":"recpe")+"_"+F+"(c, "+x+")");
    }
    throw std::runtime_error("fp misc opcode");
}
inline std::string lift(uint32_t w,int type) {
    bool scalar=type==2 || type==4; unsigned Q=bits(w,30,1),U_=bits(w,29,1),sz=bits(w,22,2),opc=bits(w,12,5),nn=bits(w,5,5),d=bits(w,0,5);
    if(type==3 || type==4) {
        if(scalar) {
            if(!U_) { if(opc==27 && sz==3) return "c->v["+n(d)+"] = (uint64_t)(VS64("+n(nn)+") + VHI64("+n(nn)+"));"; throw std::runtime_error("scalar pairwise"); }
            std::string f; if(opc==12) f=(sz>>1)?"FOP_MINNM":"FOP_MAXNM"; else if(opc==13 && !(sz>>1)) f="FOP_ADD"; else if(opc==15) f=(sz>>1)?"FOP_MIN":"FOP_MAX";
            if(f.empty()) throw std::runtime_error("scalar fp pairwise");
            bool s=sz&1; auto F=s?"d":"s",VT=s?"vf64":"vf32",SET=s?"FP_SETD":"FP_SETS";
            return std::string(SET)+"("+n(d)+", pair_"+F+"(c, "+f+", "+vr(VT,nn)+"[0], "+vr(VT,nn)+"[1]));";
        }
        if(opc==12 || opc==15) { if(!U_) throw std::runtime_error("fp16 across"); if(!Q || (sz&1)) throw std::runtime_error("fp across size"); auto f=opc==12?(sz>>1?"FOP_MINNM":"FOP_MAXNM"):(sz>>1?"FOP_MIN":"FOP_MAX"); return "sv_fred_s(c, "+std::string(f)+", "+n(d)+", "+n(nn)+");"; }
        int op=opc==3?1:opc==10?2:opc==26?3:opc==27?0:-1;
        if(op<0 || (!op && U_) || sz==3 || (sz==2 && !Q)) throw std::runtime_error("across");
        return "sl_across(c, "+n(op)+", "+n(U_)+", "+n(Q)+", "+n(sz)+", "+n(d)+", "+n(nn)+");";
    }
    if(opc>=24 || opc==22 || opc==23 || (opc>=12 && opc<=15)) return fp(w,scalar);
    if(scalar) {
        if(opc==3 || opc==7) return same(lop(opc==3?(U_?"USQADD":"SUQADD"):(U_?"SQNEG":"SQABS")),sz,1,d,nn);
        if(opc==18 || opc==20) { if(sz==3 || (opc==18 && !U_)) throw std::runtime_error("scalar xtn"); return gen(lop(opc==18?"SQXTUN":U_?"UQXTN":"SQXTN"),sz,1,d,sz,0,0,nn,sz+1,0); }
        if(sz!=3) throw std::runtime_error("scalar misc size");
        auto a="VS64("+n(nn)+")",sa="(int64_t)"+a, dest="c->v["+n(d)+"] = ";
        if(opc==8 || opc==9) return dest+"("+sa+(opc==8?(U_?" >= ":" > "):(U_?" <= ":" == "))+"0) ? ~0ull : 0;";
        if(opc==10 && !U_) return dest+"("+sa+" < 0) ? ~0ull : 0;";
        if(opc==11) return U_?dest+"(uint64_t)-"+a+";":dest+"("+sa+" < 0) ? (uint64_t)-"+a+" : "+a+";";
        throw std::runtime_error("scalar misc opcode");
    }
    unsigned ne=(8u<<Q)>>sz; auto T=vu(sz),S="vs"+n(8u<<sz);
    if(opc==0 || (opc==1 && !U_)) { unsigned gsz=!U_&&!opc?3:U_&&!opc?2:1; if(sz>=gsz) throw std::runtime_error("rev"); unsigned es=1u<<sz,gb=1u<<gsz; std::vector<unsigned> idx; for(unsigned i=0;i<16;i++) { unsigned g=i/gb*gb,o=i%gb; idx.push_back(g+(gb/es-1-o/es)*es+o%es); } return vwr("vu8",Q,d,shuf(vr("vu8",nn),vr("vu8",nn),idx)); }
    if(sz==3 && !Q && opc!=18 && opc!=20 && opc!=19) throw std::runtime_error("misc 1d");
    if(opc==2 || opc==6) { if(sz==3) throw std::runtime_error("addlp size"); return "sl_addlp(c, "+n(U_)+", "+n(opc==6)+", "+n(Q)+", "+n(sz)+", "+n(d)+", "+n(nn)+");"; }
    if(opc==3) return same(lop(U_?"USQADD":"SUQADD"),sz,ne,d,nn);
    if(opc==4) { if(sz==3) throw std::runtime_error("clz size"); return same(lop(U_?"CLZ":"CLS"),sz,ne,d,nn); }
    if(opc==5) { if(!U_) { if(sz) throw std::runtime_error("cnt size"); return same(lop("CNT"),0,ne,d,nn); } if(sz==0) return vwr("vu64",Q,d,"~"+vr("vu64",nn)); if(sz==1) return same(lop("RBIT"),0,8u<<Q,d,nn); throw std::runtime_error("not/rbit size"); }
    if(opc==7) return same(lop(U_?"SQNEG":"SQABS"),sz,ne,d,nn);
    auto a=vr(T,nn),sa=vr(S,nn);
    if(opc==8 || opc==9) return vwr(T,Q,d,"("+T+")("+sa+(opc==8?(U_?" >= ":" > "):(U_?" <= ":" == "))+"0)");
    if(opc==10 && !U_) return vwr(T,Q,d,"("+T+")("+sa+" < 0)");
    if(opc==11) { if(U_) return vwr(T,Q,d,"-"+a); auto sign=n((8u<<sz)-1); return vwr(T,Q,d,"(("+T+")"+sa+" ^ ("+T+")("+sa+" >> "+sign+")) - ("+T+")("+sa+" >> "+sign+")"); }
    if(opc==18 || opc==20) { if(sz==3) throw std::runtime_error("xtn size"); ne=8u>>sz; return gen(lop(opc==18?(U_?"SQXTUN":"XTN"):(U_?"UQXTN":"SQXTN")),sz,ne,d,sz,Q*ne,Q,nn,sz+1,0); }
    if(opc==19 && U_) { if(sz==3) throw std::runtime_error("shll size"); ne=8u>>sz; return gen(lop("SHLL"),sz,ne,d,sz+1,0,0,nn,sz,Q*ne); }
    throw std::runtime_error("misc2 opcode");
}
}
