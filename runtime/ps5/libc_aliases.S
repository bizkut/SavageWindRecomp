// FreeBSD libc names that pacbrew's archives (FFmpeg's link: runtime/hle/movie.c, ps5/tools/setup-ffmpeg.sh) reference
// and no system module exports, as tail jumps to what the system's libc does export under another name:
//   _setjmp / _longjmp (OpenSSL's ASYNC fibres): setjmp / longjmp, which also save and restore the signal mask (the
//     same jump; a tail jump keeps the caller's frame, so setjmp records the caller);
//   ___mb_cur_max (libiconv's MB_CUR_MAX): the platform layer's ps5____mb_cur_max_l, which reads __mb_cur_max.
// SPDX-License-Identifier: GPL-3.0-or-later
    .text
    .globl _setjmp
    .type _setjmp, @function
_setjmp:
    jmp setjmp@PLT
    .size _setjmp, . - _setjmp

    .globl _longjmp
    .type _longjmp, @function
_longjmp:
    jmp longjmp@PLT
    .size _longjmp, . - _longjmp

    .globl ___mb_cur_max
    .type ___mb_cur_max, @function
___mb_cur_max:
    jmp ps5____mb_cur_max_l
    .size ___mb_cur_max, . - ___mb_cur_max

    .section .note.GNU-stack, "", @progbits
