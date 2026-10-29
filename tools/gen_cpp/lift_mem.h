// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_reg.h"
namespace lift_mem {
using namespace lift_imm;
inline std::string signed_offset(int64_t off) { return off>=0 ? "+ "+std::to_string(off) : "- "+std::to_string(-off); }
inline std::string lift(uint32_t w,U pc) {
    if(bits(w,24,6)==8) {
        unsigned size=bits(w,30,2),o2=bits(w,23,1),L=bits(w,22,1),o1=bits(w,21,1),rs=bits(w,16,5),rn=bits(w,5,5),rt=bits(w,0,5); auto n=std::to_string(8u<<size), addr=reg(1,rn,true);
        if(o1) throw std::runtime_error("exclusive pair / CAS");
        if(!o2) { if(L) return set(1,rt,"ldx"+n+"(c, "+addr+")"); return "{ uint32_t st = stx"+n+"(c, "+addr+", "+x(rt)+"); "+set(0,rs,"st")+" }"; }
        if(L) return set(1,rt,"lda"+n+"("+addr+")");
        return "stl"+n+"("+addr+", "+x(rt)+");";
    }
    if(bits(w,27,3)==3 && !bits(w,24,2)) {
        unsigned opc=bits(w,30,2),rt=bits(w,0,5); U addr=pc+U(sext(bits(w,5,19),19)*4);
        if(opc==0) return set(0,rt,"LD32("+hex(addr)+")");
        if(opc==1) return set(1,rt,"LD64("+hex(addr)+")");
        if(opc==2) return set(1,rt,"(int64_t)(int32_t)LD32("+hex(addr)+")");
        return "/* prfm */";
    }
    if(bits(w,27,3)==5) {
        unsigned opc=bits(w,30,2),mode=bits(w,23,2),L=bits(w,22,1),rt2=bits(w,10,5),rn=bits(w,5,5),rt=bits(w,0,5);
        if(opc==3) throw std::runtime_error("pair opc 3");
        int64_t off=sext(bits(w,15,7),7)*(opc==2?8:4); auto base=reg(1,rn,true); bool post=mode==1, wb=mode==1 || mode==3;
        std::string out="{ uint64_t ea = "+(post?base:base+" "+signed_offset(off))+"; ";
        if(L) {
            if(opc==2) out+="uint64_t a = LD64(ea), b = LD64(ea + 8); ";
            else if(opc==1) out+="uint64_t a = (int64_t)(int32_t)LD32(ea), b = (int64_t)(int32_t)LD32(ea + 4); ";
            else out+="uint64_t a = LD32(ea), b = LD32(ea + 4); ";
        } else if(opc==2) out+="ST64(ea, "+x(rt)+"); ST64(ea + 8, "+x(rt2)+"); ";
        else out+="ST32(ea, "+reg(0,rt)+"); ST32(ea + 4, "+reg(0,rt2)+"); ";
        if(wb) out+=base+" = "+(post?"ea "+signed_offset(off):"ea")+"; ";
        if(L) out+=set(1,rt,"a")+" "+set(1,rt2,"b")+" ";
        return out+"}";
    }
    if(bits(w,27,3)==7) {
        unsigned size=bits(w,30,2),opc=bits(w,22,2),rn=bits(w,5,5),rt=bits(w,0,5); auto base=reg(1,rn,true); std::string ea,wb;
        if(bits(w,24,2)==1) ea=base+" + "+hex(U(bits(w,10,12))<<size);
        else {
            unsigned kind=bits(w,10,2);
            if(bits(w,21,1)) {
                if(kind!=2) throw std::runtime_error("ldst reg atomic/pac");
                unsigned rm=bits(w,16,5),option=bits(w,13,3),S=bits(w,12,1);
                if(option!=2 && option!=3 && option!=6 && option!=7) throw std::runtime_error("ldst reg option");
                ea=base+" + "+lift_reg::extend(rm,option,S?size:0);
            } else {
                auto sign=signed_offset(sext(bits(w,12,9),9));
                if(kind==0 || kind==2) ea=base+" "+sign;
                else if(kind==1) { ea=base; wb=base+" "+sign; }
                else { ea=base+" "+sign; wb="ea"; }
            }
        }
        auto n=std::to_string(8u<<size); std::string part,load;
        if(opc==0) part="ST"+n+"(ea, "+(size==3?x(rt):reg(0,rt))+");";
        else if(opc==1) { part="uint64_t v = LD"+n+"(ea);"; load=set(1,rt,"v"); }
        else if(size==3) return "/* prfm */";
        else if(size==2 && opc==3) throw std::runtime_error("ldst size2 opc3");
        else { part="uint64_t v = (uint64_t)(int64_t)(int"+n+"_t)LD"+n+"(ea);"; load=set(opc==2,rt,"v"); }
        std::string out="{ uint64_t ea = "+ea+"; "+part;
        if(!wb.empty()) out+=" "+base+" = "+wb+";";
        if(!load.empty()) out+=" "+load;
        return out+" }";
    }
    throw std::runtime_error("ldst "+lift_branch::numberword(w));
}
}
