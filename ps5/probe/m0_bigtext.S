/* Savage Wind PS5 - M0 spike S3: a .text block the size of the generated game code (PS5_BIGTEXT_MB, default 256),
 * with callable code at both ends. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_BIGTEXT_MB
#define PS5_BIGTEXT_MB 256
#endif
    .text
    .globl m0_bigtext_entry
    .type m0_bigtext_entry, @function
m0_bigtext_entry:
    leal 1(%rdi), %eax
    ret
    .skip PS5_BIGTEXT_MB * 1024 * 1024, 0xcc
    .globl m0_bigtext_tail
    .type m0_bigtext_tail, @function
m0_bigtext_tail:
    leal 2(%rdi), %eax
    ret
    .globl m0_bigtext_end
m0_bigtext_end:
    .section .note.GNU-stack, "", @progbits
