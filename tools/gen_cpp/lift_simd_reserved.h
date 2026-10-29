// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lift_simd_same.h"
namespace lift_simd_reserved {
using namespace lift_imm;
// Only claim the terminal error when no not-yet-ported group would have been entered.
inline std::string error(uint32_t w) {
    unsigned o = bits(w, 28, 4), p = bits(w, 23, 2), q = bits(w, 19, 4), r = bits(w, 10, 9);
    if ((o & 13) == 5) {
        if ((!(p & 2) && (q & 7) == 4 && (r & 0x183) == 2) || (!(p & 2) && (q & 7) == 6 && (r & 0x183) == 2) ||
            (p == 2 && (r & 1)) || ((p & 2) && !(r & 1))) {
            return {};
        }
        return "simd scalar " + lift_branch::numberword(w);
    }
    if (!(o & 9)) {
        if ((!(p & 2) && (q & 7) == 4 && (r & 0x183) == 2) || (!(p & 2) && (q & 7) == 6 && (r & 0x183) == 2) ||
            (p == 2 && (r & 1)) || ((p & 2) && !(r & 1))) {
            return {};
        }
        return "simd vector " + lift_branch::numberword(w);
    }
    return "fp/simd " + lift_branch::numberword(w);
}
} // namespace lift_simd_reserved
