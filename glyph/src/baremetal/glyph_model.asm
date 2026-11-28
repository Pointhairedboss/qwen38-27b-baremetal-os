; ============================================================================
; glyph_model.asm — what /draw needs from the inference kernel's model state.
; Linked only into the inference kernel (build/glyph-kernel.o).
;
;   uint64 glyph_model_text(uint64 n in rdi, char *out in rsi, uint64 cap in rdx)
;       Decode the first n generated token ids at [g_ctx + CTX_BUF_GEN] into
;       out as the raw bytes tok_write_stdout prints, truncated to cap - 1 and
;       NUL-terminated. Returns the number of bytes written. Ids outside the
;       vocabulary are skipped, as tok_write_stdout does. Makes no calls.
;
;   glyph_draw_primer / glyph_draw_repair (+ _len): the prompt templates
;       tools/draw_primer.txt and tools/draw_repair.txt, as tuned with
;       tools/draw_eval.py on the Linux build.
; ============================================================================
bits 64
default rel

%include "constants.inc"                ; CTX_* offsets (fork shared/)

extern g_ctx

global glyph_model_text
global glyph_draw_primer
global glyph_draw_primer_len
global glyph_draw_repair
global glyph_draw_repair_len

section .text

glyph_model_text:
    push    rbx
    push    r12
    push    r13
    xor     eax, eax                    ; bytes written
    test    rdx, rdx
    jz      .ret
    dec     rdx                         ; room for the NUL
    lea     r8, [rel g_ctx]
    mov     r9, [r8 + CTX_TOK_OFFS_PTR] ; uint32 offsets, vocab + 1
    mov     r10, [r8 + CTX_TOK_BYTES_PTR]
    mov     r11, [r8 + CTX_VOCAB_SIZE]
    mov     r8, [r8 + CTX_BUF_GEN]      ; uint32 token ids
    xor     ecx, ecx                    ; i
.tok:
    cmp     rcx, rdi
    jae     .end
    mov     ebx, [r8 + rcx*4]           ; t
    inc     rcx
    cmp     rbx, r11
    jae     .tok
    mov     r12d, [r9 + rbx*4 + 4]      ; end
    mov     ebx, [r9 + rbx*4]           ; start
.byte:
    cmp     rbx, r12
    jae     .tok
    cmp     rax, rdx
    jae     .end
    mov     r13b, [r10 + rbx]
    mov     [rsi + rax], r13b
    inc     rax
    inc     rbx
    jmp     .byte
.end:
    mov     byte [rsi + rax], 0
.ret:
    pop     r13
    pop     r12
    pop     rbx
    ret

section .rodata

glyph_draw_primer:      incbin "tools/draw_primer.txt"
glyph_draw_primer_end:
                        db 0
glyph_draw_repair:      incbin "tools/draw_repair.txt"
glyph_draw_repair_end:
                        db 0
align 4
glyph_draw_primer_len:  dd glyph_draw_primer_end - glyph_draw_primer
glyph_draw_repair_len:  dd glyph_draw_repair_end - glyph_draw_repair
