// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_basic.h"
namespace lift_simd_diff {
using namespace lift_imm; using lift_scalar_fp::n;
inline std::string lop(const std::string &op,unsigned u=0) { return "LOP_"+op+(u?" | LOP_U":""); }
inline std::string gen(const std::string &op,unsigned sz,unsigned ne,unsigned d,unsigned szd,unsigned offd,unsigned keep,unsigned a,unsigned sza,unsigned offa,int b=-1,unsigned szb=0,unsigned offb=0,int bidx=-1,U bimm=0) {
    return "sl_gen(c, "+op+", "+n(sz)+", "+n(ne)+", "+n(d)+", "+n(szd)+", "+n(offd)+", "+n(keep)+", "+n(a)+", "+n(sza)+", "+n(offa)+", "+std::to_string(b)+", "+n(szb)+", "+n(offb)+", "+std::to_string(bidx)+", "+hex(bimm)+");";
}
inline int type(uint32_t w) {
    unsigned op0=bits(w,28,4),op1=bits(w,23,2),op2=bits(w,19,4),op3=bits(w,10,9);
    if(lift_simd_basic::type(w)) return 0;
    if((op0&13)==5 && !(op1&2) && (op2&4) && !(op3&3)) return 2;
    if(!(op0&9) && !(op1&2) && (op2&4) && !(op3&3)) return 1;
    return 0;
}
inline std::string lift(uint32_t w,int type) {
    unsigned Q=bits(w,30,1),U_=bits(w,29,1),sz=bits(w,22,2),m=bits(w,16,5),opc=bits(w,12,4),a=bits(w,5,5),d=bits(w,0,5);
    if(type==2) {
        if(U_ || (opc!=9 && opc!=11 && opc!=13) || (sz!=1 && sz!=2)) throw std::runtime_error("scalar 3diff");
        std::string op=opc==9?"SQDMLAL":opc==11?"SQDMLSL":"SQDMULL"; return gen(lop(op),sz,1,d,sz+1,0,0,a,sz,0,m,sz,0);
    }
    if(opc==14) { if(U_) throw std::runtime_error("pmull U"); if(sz==3) return "sl_pmull64(c, "+n(Q)+", "+n(d)+", "+n(a)+", "+n(m)+");"; if(sz) throw std::runtime_error("pmull size"); }
    if(sz==3 || opc==15) throw std::runtime_error("3diff size");
    unsigned ne=8u>>sz, off=Q*ne;
    if(opc==4 || opc==6) { const char *ops[]={"ADDHN","RADDHN","SUBHN","RSUBHN"}; return gen(lop(ops[(opc==6)*2+U_]),sz,ne,d,sz,off,Q,a,sz+1,0,m,sz+1,0); }
    if(opc==1 || opc==3) return gen(lop(opc==1?"ADDW":"SUBW",U_),sz,ne,d,sz+1,0,0,a,sz+1,0,m,sz,off);
    const char *ops[]={"ADDL","ADDW","SUBL","SUBW","ADDHN","ABAL","SUBHN","ABDL","MLAL","SQDMLAL","MLSL","SQDMLSL","MULL","SQDMULL","PMULL"}; std::string op=ops[opc];
    bool sq=op.compare(0,3,"SQD")==0, pm=op.compare(0,2,"PM")==0;
    if(sq && (U_ || (sz!=1 && sz!=2))) throw std::runtime_error("sqdmull");
    return gen(lop(op,(sq||pm)?0:U_),sz,ne,d,sz+1,0,0,a,sz,off,m,sz,off);
}
}
