#ifndef PPU_STORE_THUNK_H
#define PPU_STORE_THUNK_H

#include "../memory/vm_page_map.h"
#include "../spu/spu_coh_bitmap.h"

#define PPU_ST_V(op) ".irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n    " op "\n.endr\n"
#if defined(__AVX512F__)
#define PPU_ST_VSAVE PPU_ST_V("vmovdqu64 %zmm\\r, (128+64*\\r)(%rsp)")
#define PPU_ST_VLOAD PPU_ST_V("vmovdqu64 (128+64*\\r)(%rsp), %zmm\\r")
#define PPU_ST_FRAME "1152"
#elif defined(__AVX__)
#define PPU_ST_VSAVE PPU_ST_V("vmovdqu %ymm\\r, (128+32*\\r)(%rsp)")
#define PPU_ST_VLOAD PPU_ST_V("vmovdqu (128+32*\\r)(%rsp), %ymm\\r")
#define PPU_ST_FRAME "640"
#else
#define PPU_ST_VSAVE PPU_ST_V("movdqu %xmm\\r, (128+16*\\r)(%rsp)")
#define PPU_ST_VLOAD PPU_ST_V("movdqu (128+16*\\r)(%rsp), %xmm\\r")
#define PPU_ST_FRAME "384"
#endif
#ifdef _WIN32
#define PPU_ST_GPRS(op) op("rax", "32") op("rcx", "40") op("r8", "48") op("r9", "56") op("r10", "64") op("r11", "72")
#define PPU_ST_ARGS "    movq 8(%rbp), %r9\n"
#define PPU_ST_RET "    leaq ppu_st_sink(%rip), %rdx\n    leaq (%rbp), %rsp\n    popq %rbp\n    retq\n"
#define PPU_ST_HEAD ".def ppu_st_thunk; .scl 2; .type 32; .endef\n.p2align 4\nppu_st_thunk:\n" \
    ".seh_proc ppu_st_thunk\n    pushq %rbp\n.seh_pushreg %rbp\n    movq %rsp, %rbp\n.seh_setframe %rbp, 0\n.seh_endprologue\n"
#define PPU_ST_TAIL ".seh_endproc\n"
#else
#define PPU_ST_GPRS(op) op("rax", "0") op("rcx", "8") op("rsi", "16") op("rdi", "24") op("r8", "32") op("r9", "40") op("r10", "48") op("r11", "56")
#define PPU_ST_ARGS "    movq %rcx, %rdi\n    movq %rdx, %rsi\n    movq %r8, %rdx\n    movq 8(%rbp), %rcx\n"
#define PPU_ST_RET ".cfi_remember_state\n    leaq ppu_st_sink(%rip), %rdx\n    leave\n.cfi_def_cfa %rsp, 136\n    retq\n.cfi_restore_state\n"
#define PPU_ST_HEAD ".type ppu_st_thunk, @function\n.p2align 4\nppu_st_thunk:\n" \
    ".cfi_startproc\n.cfi_def_cfa_offset 136\n.cfi_offset %rip, -136\n    pushq %rbp\n.cfi_def_cfa_offset 144\n" \
    ".cfi_offset %rbp, -144\n    movq %rsp, %rbp\n.cfi_def_cfa_register %rbp\n"
#define PPU_ST_TAIL ".cfi_endproc\n.size ppu_st_thunk, .-ppu_st_thunk\n"
#endif
#define PPU_ST_GSAVE(r, o) "    movq %" r ", " o "(%rsp)\n"
#define PPU_ST_GLOAD(r, o) "    movq " o "(%rsp), %" r "\n"
#define PPU_ST_SCRATCH_POP "    popq %r11\n    popq %r10\n    popq %r9\n    popq %rax\n"
#define PPU_ST_FULL(prep) "    andq $-64, %rsp\n    subq $" PPU_ST_FRAME ", %rsp\n" PPU_ST_GPRS(PPU_ST_GSAVE) PPU_ST_VSAVE prep \
    PPU_ST_ARGS "    callq ppu_store_slow\n" PPU_ST_VLOAD PPU_ST_GPRS(PPU_ST_GLOAD) PPU_ST_RET
__asm__(".att_syntax\n.text\n.globl ppu_st_thunk\n" PPU_ST_HEAD
        "    pushq %rax\n    pushq %r9\n    pushq %r10\n    pushq %r11\n"
        "    movq %rcx, %r10\n    shrq $32, %r10\n    movl %r10d, %r11d\n    andl $0xF0, %r11d\n    cmpl $0x40, %r11d\n"
        "    movl %r10d, %r11d\n    jne .Lppu_st_n\n    andl $0x0F, %r11d\n"
        ".Lppu_st_n:\n    leal -1(%r11), %eax\n    testl %eax, %r11d\n    jnz .Lppu_st_full\n"
        "    testl %r11d, %r11d\n    jz .Lppu_st_full\n    cmpl $16, %r11d\n    ja .Lppu_st_full\n"
        "    movl %ecx, %eax\n    andl $0xFFF, %eax\n    addl %r11d, %eax\n    cmpl $0x1000, %eax\n    ja .Lppu_st_full\n"
        "    movl %ecx, %eax\n    shrl $12, %eax\n    leaq ppu_slow_page(%rip), %r9\n    movzbl (%r9,%rax), %r10d\n"
        "    testl $1, %r10d\n    jnz .Lppu_st_full\n    testl $8, %r10d\n    jz .Lppu_st_go\n"
        "    movq g_spu_coh_bits(%rip), %rax\n    testq %rax, %rax\n    jz .Lppu_st_go\n"
        "    movl %ecx, %r10d\n    shrl $7, %r10d\n    btl %r10d, (%rax)\n    jc .Lppu_st_full\n"
        ".Lppu_st_go:\n    movl %ecx, %r10d\n    addq vm_base(%rip), %r10\n"
        "    cmpl $8, %r11d\n    ja .Lppu_st_16\n    je .Lppu_st_8\n    cmpl $2, %r11d\n    ja .Lppu_st_4\n    je .Lppu_st_2\n"
        "    movb %dl, (%r10)\n    jmp .Lppu_st_check\n"
        ".Lppu_st_2:\n    movw %dx, (%r10)\n    jmp .Lppu_st_check\n"
        ".Lppu_st_4:\n    movl %edx, (%r10)\n    jmp .Lppu_st_check\n"
        ".Lppu_st_8:\n    movq %rdx, (%r10)\n    jmp .Lppu_st_check\n"
        ".Lppu_st_16:\n    movq %rdx, (%r10)\n    movq %r8, 8(%r10)\n"
        ".Lppu_st_check:\n    movl %ecx, %eax\n    shrl $12, %eax\n    movzbl (%r9,%rax), %eax\n    testl $5, %eax\n    jnz .Lppu_st_recheck\n"
        "    testl $8, %eax\n    jz .Lppu_st_watch\n    movq g_spu_coh_bits(%rip), %rax\n    testq %rax, %rax\n    jz .Lppu_st_watch\n"
        "    movl %ecx, %r10d\n    shrl $7, %r10d\n    btl %r10d, (%rax)\n    jc .Lppu_st_recheck\n"
        ".Lppu_st_watch:\n"
        "    movl vm_watch_ea(%rip), %eax\n    testl %eax, %eax\n    jz .Lppu_st_done\n"
        "    subl %ecx, %eax\n    cmpl %r11d, %eax\n    jb .Lppu_st_recheck\n"
        ".Lppu_st_done:\n" PPU_ST_SCRATCH_POP PPU_ST_RET
        ".Lppu_st_recheck:\n" PPU_ST_SCRATCH_POP PPU_ST_FULL("    movl %ecx, %ecx\n")
        ".Lppu_st_full:\n" PPU_ST_SCRATCH_POP PPU_ST_FULL("")
        PPU_ST_TAIL ".bss\n.p2align 4\nppu_st_sink:\n    .zero 16\n.text\n");

#endif
