// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_scalar_fp.h"
#include <vector>
namespace lift_simd_basic {
using namespace lift_imm;
using lift_scalar_fp::n;
inline std::string vu(unsigned s) { return "vu"+n(8u<<s); }
inline std::string vr(const std::string &t,unsigned r) { return "VREG("+t+", "+n(r)+")"; }
inline std::string vwr(const std::string &t,unsigned Q,unsigned d,const std::string &e) { return "VW("+t+", "+n(Q)+", "+n(d)+", "+e+");"; }
inline std::string shuf(const std::string &a,const std::string &b,const std::vector<unsigned> &idx) { std::string x="SHUF("+a+", "+b; for(auto i:idx) x+=", "+n(i); return x+")"; }
inline std::pair<unsigned,unsigned> imm5(unsigned v) { if(!(v&15)) throw std::runtime_error("copy imm5"); unsigned sz=0; while(!((v>>sz)&1)) ++sz; return {sz,v>>(sz+1)}; }
inline int type(uint32_t w) {
    unsigned op0=bits(w,28,4),op1=bits(w,23,2),op2=bits(w,19,4),op3=bits(w,10,9);
    if(op0==4 && !(op1&2) && (op2&7)==5 && (op3&0x183)==2) return 1;
    if(op0==5 && !(op1&2) && !(op2&4) && !(op3&35)) return 2;
    if(op0==5 && !(op1&2) && (op2&7)==5 && (op3&0x183)==2) return 3;
    if((op0&13)==5) { if(!op1 && !(op2&12) && (op3&33)==1) return 4; return 0; }
    if(!(op0&11) && !(op1&2) && !(op2&4) && !(op3&35)) return 5;
    if(!(op0&11) && !(op1&2) && !(op2&4) && (op3&35)==2) return 6;
    if((op0&11)==2 && !(op1&2) && !(op2&4) && !(op3&33)) return 7;
    if(!(op0&9)) { if(!op1 && !(op2&12) && (op3&33)==1) return 8; }
    return 0;
}
inline std::string lift(uint32_t w,int type) {
    unsigned d=bits(w,0,5),nn=bits(w,5,5),m=bits(w,16,5),Q=bits(w,30,1),sz=bits(w,22,2);
    if(type==1) { if(sz) throw std::runtime_error("aes size"); unsigned opc=bits(w,12,5); if(opc<4 || opc>7) throw std::runtime_error("aes opcode"); return "sv_aes(c, "+n(opc-4)+", "+n(d)+", "+n(nn)+");"; }
    if(type==2) { if(sz) throw std::runtime_error("sha size"); unsigned opc=bits(w,12,3); std::string f; unsigned v;
        if(opc<3) { f="sv_sha1hash"; v=opc; } else if(opc==3 || opc==6) { f="sv_sha_misc"; v=opc==3?0:3; } else if(opc==4 || opc==5) { f="sv_sha256hash"; v=opc==4; } else throw std::runtime_error("sha3 opcode");
        return f+"(c, "+n(v)+", "+n(d)+", "+n(nn)+", "+n(m)+");";
    }
    if(type==3) { if(sz) throw std::runtime_error("sha size"); unsigned opc=bits(w,12,5); if(opc>2) throw std::runtime_error("sha2 opcode"); return "sv_sha_misc(c, "+n(opc?opc:4)+", "+n(d)+", "+n(nn)+", 0);"; }
    if(type==4 || type==8) {
        if(type==4 && (bits(w,29,1) || bits(w,11,4))) throw std::runtime_error("scalar copy");
        auto [z,idx]=imm5(m); if(z>3) throw std::runtime_error(type==4?"scalar dup size":"copy size");
        const char *P[]={"uint8_t","sa16","sa32","sa64"}; std::string p=P[z], src="(("+p+" *)&c->v["+n(nn)+"])["+n(idx)+"]";
        if(type==4) return "c->v["+n(d)+"] = "+src+";";
        unsigned op=bits(w,29,1),i4=bits(w,11,4); auto T=vu(z),dest="(("+p+" *)&c->v["+n(d)+"])["+n(idx)+"]";
        if(op) { if(!Q) throw std::runtime_error("ins elem Q"); return dest+" = (("+p+" *)&c->v["+n(nn)+"])["+n(i4>>z)+"];"; }
        if(i4==0) { if(z==3 && !Q) throw std::runtime_error("dup 1d"); return vwr(T,Q,d,shuf(vr(T,nn),vr(T,nn),std::vector<unsigned>(16u>>z,idx))); }
        if(i4==1) { if(z==3 && !Q) throw std::runtime_error("dup 1d"); return vwr(T,Q,d,"("+T+"){} + (uint"+n(8u<<z)+"_t)"+x(nn)); }
        if(i4==3) { if(!Q) throw std::runtime_error("ins Q"); return dest+" = (uint"+n(8u<<z)+"_t)"+x(nn)+";"; }
        if(i4==5) { if(z>=2+Q) throw std::runtime_error("smov size"); return set(Q,d,"(uint64_t)(int64_t)(int"+n(8u<<z)+"_t)"+src); }
        if(i4==7) { if((Q && z!=3) || (!Q && z==3)) throw std::runtime_error("umov size"); return set(Q,d,src); }
        throw std::runtime_error("copy imm4");
    }
    if(type==5) { if(sz) throw std::runtime_error("tbl op2"); return "sv_tbl(c, "+n(Q)+", "+n(bits(w,13,2))+", "+n(bits(w,12,1))+", "+n(d)+", "+n(nn)+", "+n(m)+");"; }
    if(type==6) { unsigned opc=bits(w,12,3); if(opc==0 || opc==4 || (sz==3 && !Q)) throw std::runtime_error("permute"); unsigned ne=(8u<<Q)>>sz,NL=16u>>sz,part=opc>>2,kind=opc&3; std::vector<unsigned> idx;
        for(unsigned i=0;i<ne;i++) { if(kind==1) { unsigned j=2*i+part; idx.push_back(j<ne?j:NL+j-ne); } else if(kind==2) { unsigned p=i&~1u; idx.push_back((i&1)?NL+p+part:p+part); } else { unsigned base=part*ne/2; idx.push_back((i&1)?NL+base+i/2:base+i/2); } }
        idx.resize(NL); auto T=vu(sz); return vwr(T,Q,d,shuf(vr(T,nn),vr(T,m),idx));
    }
    if(type==7) { unsigned imm=bits(w,11,4); if(sz || (!Q && imm>=8)) throw std::runtime_error("ext"); std::vector<unsigned> idx; for(unsigned i=0;i<(Q?16u:8u);i++) idx.push_back(Q?imm+i:imm+i<8?imm+i:16+imm+i-8); idx.resize(16); return vwr("vu8",Q,d,shuf(vr("vu8",nn),vr("vu8",m),idx)); }
    throw std::runtime_error("invalid SIMD basic group");
}
}
