#ifndef PPU_STORE_THUNK_H
#define PPU_STORE_THUNK_H

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
#else
#define PPU_ST_GPRS(op) op("rax", "0") op("rcx", "8") op("rsi", "16") op("rdi", "24") op("r8", "32") op("r9", "40") op("r10", "48") op("r11", "56")
#endif
#define PPU_ST_GSAVE(r, o) "    movq %" r ", " o "(%rsp)\n"
#define PPU_ST_GLOAD(r, o) "    movq " o "(%rsp), %" r "\n"
#ifdef _WIN32
__asm__(".att_syntax\n.text\n.globl ppu_st_thunk\n.def ppu_st_thunk; .scl 2; .type 32; .endef\n.p2align 4\nppu_st_thunk:\n"
        ".seh_proc ppu_st_thunk\n    pushq %rbp\n.seh_pushreg %rbp\n    movq %rsp, %rbp\n.seh_setframe %rbp, 0\n.seh_endprologue\n"
        "    andq $-64, %rsp\n    subq $" PPU_ST_FRAME ", %rsp\n" PPU_ST_GPRS(PPU_ST_GSAVE) PPU_ST_VSAVE
        "    movq 8(%rbp), %r9\n    callq ppu_store_slow\n" PPU_ST_VLOAD PPU_ST_GPRS(PPU_ST_GLOAD)
        "    leaq ppu_st_sink(%rip), %rdx\n    leaq (%rbp), %rsp\n    popq %rbp\n    retq\n.seh_endproc\n"
        ".bss\n.p2align 4\nppu_st_sink:\n    .zero 16\n.text\n");
#else
__asm__(".att_syntax\n.text\n.globl ppu_st_thunk\n.type ppu_st_thunk, @function\n.p2align 4\nppu_st_thunk:\n"
        ".cfi_startproc\n.cfi_def_cfa_offset 136\n.cfi_offset %rip, -136\n    pushq %rbp\n.cfi_def_cfa_offset 144\n"
        ".cfi_offset %rbp, -144\n    movq %rsp, %rbp\n.cfi_def_cfa_register %rbp\n"
        "    andq $-64, %rsp\n    subq $" PPU_ST_FRAME ", %rsp\n" PPU_ST_GPRS(PPU_ST_GSAVE) PPU_ST_VSAVE
        "    movq %rcx, %rdi\n    movq %rdx, %rsi\n    movq %r8, %rdx\n    movq 8(%rbp), %rcx\n    callq ppu_store_slow\n"
        PPU_ST_VLOAD PPU_ST_GPRS(PPU_ST_GLOAD)
        "    leaq ppu_st_sink(%rip), %rdx\n    leave\n.cfi_def_cfa %rsp, 136\n    retq\n.cfi_endproc\n"
        ".size ppu_st_thunk, .-ppu_st_thunk\n.bss\n.p2align 4\nppu_st_sink:\n    .zero 16\n.text\n");
#endif

#endif
