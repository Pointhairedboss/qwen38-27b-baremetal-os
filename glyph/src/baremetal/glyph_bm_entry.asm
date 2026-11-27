; ============================================================================
; glyph_bm_entry.asm — call Glyph (C, SysV AMD64 ABI) from kernel assembly.
;
; The kernel's own routines do not keep RSP 16-byte aligned at every call
; site; compiled C may rely on that alignment. These shims realign the stack,
; clear DF, and forward the SysV arguments unchanged. Callee-saved registers
; (rbx, rbp, r12-r15) are preserved by the C code.
;
;   int64 glyph_shell_run(const char *src  in rdi, uint64 len in rsi)
;   int64 glyph_shell_check(const char *src in rdi, uint64 len in rsi)
;   int64 glyph_shell_transcript(src rdi, len rsi, script rdx, script_len rcx)
;
; Return: rax = 0 (GLYPH_OK) or a Glyph status code (sign-extended).
; ============================================================================
bits 64
default rel

section .text

extern glyph_bm_run
extern glyph_bm_check
extern glyph_bm_transcript

global glyph_shell_run
global glyph_shell_check
global glyph_shell_transcript

%macro ALIGNED_CALL 1
    push    rbp
    mov     rbp, rsp
    and     rsp, -16
    cld
    call    %1
    mov     rsp, rbp
    pop     rbp
    movsxd  rax, eax
    ret
%endmacro

glyph_shell_run:
    ALIGNED_CALL glyph_bm_run

glyph_shell_check:
    ALIGNED_CALL glyph_bm_check

glyph_shell_transcript:
    ALIGNED_CALL glyph_bm_transcript
