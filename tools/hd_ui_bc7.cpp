// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter for the separately downloaded MIT bc7enc_rdo; RGBA pixels on stdin, BC7 blocks on stdout.
#include "bc7enc.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned w=std::strtoul(argv[1],nullptr,10), h=std::strtoul(argv[2],nullptr,10);
    if (!w || !h || w>4096 || h>4096) return 2;
    std::vector<unsigned char> pixels((size_t)w*h*4);
    if (std::fread(pixels.data(),1,pixels.size(),stdin)!=pixels.size()) return 1;
    bc7enc_compress_block_init(); bc7enc_compress_block_params params{};
    bc7enc_compress_block_params_init(&params); bc7enc_compress_block_params_init_linear_weights(&params);
    for (unsigned by=0;by<(h+3)/4;by++) for (unsigned bx=0;bx<(w+3)/4;bx++) {
        unsigned char rgba[64], encoded[16];
        for (unsigned i=0;i<16;i++) {
            unsigned x=std::min(bx*4+i%4,w-1), y=std::min(by*4+i/4,h-1);
            for (unsigned c=0;c<4;c++) rgba[i*4+c]=pixels[((size_t)y*w+x)*4+c];
        }
        bc7enc_compress_block(encoded,rgba,&params);
        if (std::fwrite(encoded,1,16,stdout)!=16) return 1;
    }
    return std::fflush(stdout) ? 1 : 0;
}
