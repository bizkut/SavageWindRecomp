// SPDX-License-Identifier: GPL-3.0-or-later
#include "lift_simd_misc.h"
#include <iostream>
#include <string>
int main() {
    std::string line;
    while (std::getline(std::cin,line)) {
        try {
            uint32_t word=static_cast<uint32_t>(std::stoul(line,nullptr,16));
            unsigned op=(word>>25)&15;
            std::cout<<line<<'\t';
            if((op&14)==8) {
                try { auto result=lift_imm::lift(word,0x7000000000ULL); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&14)==10) {
                try { std::cout<<lift_branch::lift(word,0x7000000000ULL); }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&5)==4 && !((word>>26)&1)) {
                try { auto result=lift_mem::lift(word,0x7000000000ULL); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&5)==4) {
                try { auto result=lift_simd_mem::lift(word,0x7000000000ULL); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&5)!=4 && (op&7)==5) {
                try { auto result=lift_reg::lift(word); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_simd_basic::type(word)) {
                try { auto result=lift_simd_basic::lift(word,lift_simd_basic::type(word)); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_simd_diff::type(word)) {
                try { auto result=lift_simd_diff::lift(word,lift_simd_diff::type(word)); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_simd_same::type(word)) {
                try { auto result=lift_simd_same::lift(word,lift_simd_same::type(word)); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_simd_indexed::type(word)) {
                try { auto result=lift_simd_indexed::lift(word,lift_simd_indexed::type(word)); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_simd_shift::type(word)) {
                try { auto result=lift_simd_shift::lift(word,lift_simd_shift::type(word)); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_simd_misc::type(word)) {
                try { auto result=lift_simd_misc::lift(word,lift_simd_misc::type(word)); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&7)==7 && lift_scalar_fp::handles(word)) {
                try { auto result=lift_scalar_fp::lift(word); std::cout<<"op\t"<<result; }
                catch(const std::runtime_error &e) { std::cout<<"unsupported\t"<<e.what(); }
            } else if((op&14)!=10 && (op&5)!=4 && (op&7)!=5 && (op&7)!=7) {
                std::cout<<"trap\trt_udf(c, 0x7000000000ull);";
            } else {
                auto error=lift_simd_reserved::error(word);
                if(error.empty()) std::cout<<"unported";
                else std::cout<<"unsupported\t"<<error;
            }
            std::cout<<'\n';
        } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
    }
}
