; ============================================================================
; qwen35.asm — Qwen3.5 / Qwen3.8 hybrid forward pass (.asmbin v3)
;   Gated DeltaNet linear-attention layers + gated full-attention layers
;   (partial NeoX RoPE, QK-norm, sigmoid output gate), SwiGLU MLP everywhere.
; Pure x86-64 NASM, AVX2/FMA. Reuses g_ctx buffers so the shared kernels
; (tp_matmul_q8_0, gqa_attn_range, tokenizer) work unchanged.
;
; Semantics mirror scripts/ref_qwen35.py (forward, --act q8).
; ============================================================================

%include "constants.inc"
%include "qwen35.inc"

%ifndef Q8_MAX_K
%define Q8_MAX_K 16384
%endif

default rel

extern g_ctx
%ifndef BARE_METAL
extern sys_openat_ro
extern sys_close
extern sys_fstat
extern sys_mmap_file
%endif
extern sys_mmap_anon
extern sys_write_all
extern str_len
extern matmul_set_fp16
extern dequant_row_q8_0
extern dot_f32_avx2
extern tp_matmul_q8_0
extern tp_gqa_attn
extern rmsnorm_avx2
extern silu_mul_avx2
extern vec_add_avx2
extern init_rope_freqs
extern compute_rope_cs
extern rope_apply_avx2
extern fast_expf_avx2
extern exp_f64
extern log_f64
extern tok_encode_bpe

global g_q35
global q35_dump_fd
global qwen35_init
global qwen35_forward
global qwen35_reset
global qwen35_build_chat_prompt

%macro ALIGN64 1
    add     %1, 63
    and     %1, -64
%endmacro

; walk helpers (rsi = cursor, rdi = layer record)
%macro TAKE_F32 2                       ; field, n_floats (register)
    ALIGN64 rsi
    mov     [rdi + %1], rsi
    lea     rsi, [rsi + %2 * 4]
%endmacro

%macro TAKE_Q8 3                        ; field, M, K  (not rax/rdx)
    ALIGN64 rsi
    mov     [rdi + %1], rsi
    mov     rax, %3
    mov     rdx, rax
    shr     rdx, 4
    add     rax, rdx                    ; row stride = K + K/16 (fp16 scales)
    imul    rax, %2
    add     rsi, rax
%endmacro

; arena helper (rbp = running offset)
%macro ALLOC 3                          ; base reg, field, size
    ALIGN64 rbp
    mov     [%1 + %2], rbp
    add     rbp, %3
%endmacro

section .text

; ----------------------------------------------------------------------------
; int64 qwen35_init(const char *path, uint64 max_seq_override)
; Returns 0 on success, <0 on error:
;   -1 open/mmap, -2 bad header, -3 arena, -4 layer layout, -5 unsupported dims
; ----------------------------------------------------------------------------
qwen35_init:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, STAT_SIZE + 8

    lea     rbx, [rel g_ctx]
    lea     r12, [rel g_q35]
%ifdef BARE_METAL
    ; bare metal: (base rdi, size rsi, max_seq_override rdx), weights already mapped
    mov     r15, rdi
    mov     rbp, rsi
    mov     r13, rdx
%else
    mov     r13, rsi                    ; max_seq_override

    call    sys_openat_ro
    test    rax, rax
    js      .err_open
    mov     r14, rax                    ; fd
    mov     rdi, r14
    mov     rsi, rsp
    call    sys_fstat
    test    rax, rax
    js      .err_open_close
    mov     rbp, [rsp + STAT_ST_SIZE]   ; file size
    mov     rdi, rbp
    mov     rsi, r14
    call    sys_mmap_file
    mov     r15, rax
    mov     rdi, r14
    call    sys_close
    test    r15, r15
    js      .err_open
%endif
    mov     [rbx + CTX_MMAP_BASE], r15
    mov     [rbx + CTX_MMAP_SIZE], rbp

    ; ---- header checks ----
    cmp     dword [r15 + HDR3_MAGIC], ASMBIN_MAGIC
    jne     .err_hdr
    cmp     dword [r15 + HDR3_VERSION], ASMBIN_VERSION_V3
    jne     .err_hdr
    cmp     dword [r15 + HDR3_ARCH], ASMBIN_ARCH_QWEN35
    jne     .err_hdr
    mov     rax, [r15 + HDR3_FILE_SIZE]
    cmp     rax, rbp
    ja      .err_hdr

    mov     dword [rbx + CTX_FP16_SCALES], 1
    mov     dword [rbx + CTX_SCALE_SHIFT], 4
    mov     edi, 1
    call    matmul_set_fp16

    ; ---- dims -> g_ctx ----
    mov     eax, [r15 + HDR3_DIM]
    mov     [rbx + CTX_DIM], rax
    mov     eax, [r15 + HDR3_HIDDEN]
    mov     [rbx + CTX_HIDDEN_DIM], rax
    mov     eax, [r15 + HDR3_N_LAYERS]
    mov     [rbx + CTX_N_LAYERS], rax
    mov     eax, [r15 + HDR3_VOCAB]
    mov     [rbx + CTX_VOCAB_SIZE], rax
    mov     eax, [r15 + HDR3_N_HEADS]
    mov     [rbx + CTX_N_HEADS], rax
    mov     eax, [r15 + HDR3_N_KV]
    mov     [rbx + CTX_N_KV_HEADS], rax
    mov     eax, [r15 + HDR3_HEAD_DIM]
    mov     [rbx + CTX_HEAD_DIM], rax
    mov     rax, [rbx + CTX_N_HEADS]
    imul    rax, [rbx + CTX_HEAD_DIM]
    mov     [rbx + CTX_Q_DIM], rax
    mov     rax, [rbx + CTX_N_KV_HEADS]
    imul    rax, [rbx + CTX_HEAD_DIM]
    mov     [rbx + CTX_KV_DIM], rax
    mov     eax, [r15 + HDR3_EPS]
    mov     [rbx + CTX_RMS_EPS], eax
    mov     eax, [r15 + HDR3_THETA]
    mov     [rbx + CTX_ROPE_THETA], eax
    mov     eax, [r15 + HDR3_HT_MASK]
    mov     [rbx + CTX_HT_MASK], rax
    mov     eax, [r15 + HDR3_MAX_SEQ]
    test    r13, r13
    jz      .seq_ok
    cmp     r13, rax
    ja      .seq_ok
    mov     rax, r13
.seq_ok:
    mov     [rbx + CTX_MAX_SEQ], rax
    mov     eax, [r15 + HDR3_EOS]
    mov     [rbx + CTX_EOS_1], rax
    mov     ecx, [r15 + HDR3_EOS + 4]
    cmp     ecx, 0xFFFFFFFF
    cmove   ecx, eax
    mov     [rbx + CTX_EOS_2], rcx

    mov     eax, [r15 + HDR3_NK]
    mov     [r12 + Q_NK], rax
    mov     eax, [r15 + HDR3_NV]
    mov     [r12 + Q_NV], rax
    mov     eax, [r15 + HDR3_DK]
    mov     [r12 + Q_DK], rax
    mov     eax, [r15 + HDR3_DV]
    mov     [r12 + Q_DV], rax
    mov     eax, [r15 + HDR3_CONV_K]
    mov     [r12 + Q_CONV_K], rax
    mov     eax, [r15 + HDR3_ROT_DIMS]
    mov     [r12 + Q_ROT], rax

    ; special IDs (10 x u32, same order)
    xor     ecx, ecx
.ids_lp:
    mov     eax, [r15 + HDR3_IDS + rcx * 4]
    mov     [r12 + Q_IDS + rcx * 4], eax
    inc     ecx
    cmp     ecx, 10
    jb      .ids_lp

    ; ---- dimension sanity (kernel requirements) ----
    cmp     qword [rbx + CTX_N_LAYERS], Q_MAX_LAYERS
    ja      .err_dims
    cmp     qword [r12 + Q_CONV_K], 4
    jne     .err_dims
    test    qword [rbx + CTX_DIM], 31
    jnz     .err_dims
    test    qword [rbx + CTX_HEAD_DIM], 31
    jnz     .err_dims
    test    qword [r12 + Q_DK], 31
    jnz     .err_dims
    test    qword [r12 + Q_DV], 31
    jnz     .err_dims
    test    qword [rbx + CTX_HIDDEN_DIM], 31
    jnz     .err_dims
    mov     rax, [r12 + Q_ROT]
    test    rax, 15                     ; half = rot/2 must be a multiple of 8
    jnz     .err_dims
    test    rax, rax
    jz      .err_dims
    cmp     rax, [rbx + CTX_HEAD_DIM]
    ja      .err_dims
    mov     rax, [r12 + Q_NV]
    xor     edx, edx
    div     qword [r12 + Q_NK]
    test    rdx, rdx
    jnz     .err_dims
    mov     [r12 + Q_RATIO], rax
    mov     rax, [rbx + CTX_N_HEADS]
    xor     edx, edx
    div     qword [rbx + CTX_N_KV_HEADS]
    test    rdx, rdx
    jnz     .err_dims
    mov     [r12 + Q_GQA], eax

    ; derived dims
    mov     rax, [r12 + Q_NK]
    imul    rax, [r12 + Q_DK]
    add     rax, rax
    mov     rcx, [r12 + Q_NV]
    imul    rcx, [r12 + Q_DV]           ; nv*dv
    mov     [rsp + 8], rcx              ; local: nv*dv
    add     rax, rcx
    mov     [r12 + Q_CONV_DIM], rax     ; conv_dim = 2*nk*dk + nv*dv
    add     rax, rcx
    mov     rdx, [r12 + Q_NV]
    lea     rax, [rax + rdx * 2]
    mov     [r12 + Q_WIN_M], rax        ; conv_dim + nv*dv + 2*nv
    mov     rax, [rbx + CTX_Q_DIM]
    add     rax, [rbx + CTX_KV_DIM]
    add     rax, rax
    mov     [r12 + Q_WQKV_M], rax       ; 2*nh*hd + 2*nkv*hd
    mov     rax, [rbx + CTX_HIDDEN_DIM]
    add     rax, rax
    mov     [rsp + 0], rax              ; local: 2*I

    ; matmul K limit (q8 activation scratch)
    mov     rax, [rbx + CTX_DIM]
    cmp     rax, Q8_MAX_K
    ja      .err_dims
    mov     rax, [rbx + CTX_Q_DIM]
    cmp     rax, Q8_MAX_K
    ja      .err_dims
    mov     rax, [rsp + 8]
    cmp     rax, Q8_MAX_K
    ja      .err_dims
    mov     rax, [rbx + CTX_HIDDEN_DIM]
    cmp     rax, Q8_MAX_K
    ja      .err_dims

    ; ---- tokenizer / embed / lm_head / final_norm pointers ----
    mov     rax, [r15 + HDR3_OFF_TOK]
    add     rax, r15
    mov     rcx, [rax + TOKH_OFF_B2T]
    add     rcx, rax
    mov     [rbx + CTX_B2T_PTR], rcx
    mov     rcx, [rax + TOKH_OFF_OFFSETS]
    add     rcx, rax
    mov     [rbx + CTX_TOK_OFFS_PTR], rcx
    mov     rcx, [rax + TOKH_OFF_BYTES]
    add     rcx, rax
    mov     [rbx + CTX_TOK_BYTES_PTR], rcx
    mov     rcx, [rax + TOKH_OFF_HT]
    add     rcx, rax
    mov     [rbx + CTX_HT_PTR], rcx
    mov     rax, [r15 + HDR3_OFF_EMBED]
    add     rax, r15
    mov     [rbx + CTX_EMB_PTR], rax
    mov     rax, [r15 + HDR3_OFF_LM_HEAD]
    add     rax, r15
    mov     [r12 + Q_LM_HEAD], rax
    mov     rax, [r15 + HDR3_OFF_FNORM]
    add     rax, r15
    mov     [rbx + CTX_FINAL_NORM_PTR], rax
    mov     rax, [r15 + HDR3_OFF_LTABLE]
    add     rax, r15
    mov     [rbx + CTX_LAYERS_PTR], rax

    ; ---- walk layers, fill records, verify sizes ----
    mov     qword [rsp + 16], 0         ; full count
    mov     qword [rsp + 24], 0         ; linear count
    xor     r14d, r14d                  ; l
.lw:
    cmp     r14, [rbx + CTX_N_LAYERS]
    jae     .lw_done
    mov     rdi, r14
    shl     rdi, 7                      ; * LR_SIZE
    lea     rdi, [r12 + Q_LAYERS + rdi]
    mov     rsi, [rbx + CTX_LAYERS_PTR]
    mov     rsi, [rsi + r14 * 8]
    add     rsi, r15                    ; cursor
    mov     r8, rsi                     ; layer start
    mov     rcx, [r15 + HDR3_BITMAP]
    bt      rcx, r14
    jnc     .lw_lin

    mov     qword [rdi + LR_FULL], 1
    mov     rax, [rsp + 16]
    mov     [rdi + LR_IDX], rax
    inc     qword [rsp + 16]
    mov     rcx, [rbx + CTX_DIM]
    TAKE_F32 LR_IN_NORM, rcx
    TAKE_F32 LR_POST_NORM, rcx
    mov     rcx, [rbx + CTX_HEAD_DIM]
    TAKE_F32 LR_P0, rcx
    TAKE_F32 LR_P1, rcx
    TAKE_Q8 LR_W1, [r12 + Q_WQKV_M], [rbx + CTX_DIM]
    TAKE_Q8 LR_W2, [rbx + CTX_DIM], [rbx + CTX_Q_DIM]
    jmp     .lw_mlp

.lw_lin:
    mov     qword [rdi + LR_FULL], 0
    mov     rax, [rsp + 24]
    mov     [rdi + LR_IDX], rax
    inc     qword [rsp + 24]
    mov     rcx, [rbx + CTX_DIM]
    TAKE_F32 LR_IN_NORM, rcx
    TAKE_F32 LR_POST_NORM, rcx
    mov     rcx, [r12 + Q_NV]
    TAKE_F32 LR_P0, rcx
    TAKE_F32 LR_P1, rcx
    mov     rcx, [r12 + Q_CONV_DIM]
    shl     rcx, 2                      ; conv_dim * conv_k floats
    TAKE_F32 LR_CONVW, rcx
    mov     rcx, [r12 + Q_DV]
    TAKE_F32 LR_LINNORM, rcx
    TAKE_Q8 LR_W1, [r12 + Q_WIN_M], [rbx + CTX_DIM]
    TAKE_Q8 LR_W2, [rbx + CTX_DIM], [rsp + 8]

.lw_mlp:
    TAKE_Q8 LR_WGU, [rsp + 0], [rbx + CTX_DIM]
    TAKE_Q8 LR_WDOWN, [rbx + CTX_DIM], [rbx + CTX_HIDDEN_DIM]
    ALIGN64 rsi
    sub     rsi, r8
    mov     rax, [r15 + HDR3_LB_LIN]
    cmp     qword [rdi + LR_FULL], 0
    je      .lw_cmp
    mov     rax, [r15 + HDR3_LB_FULL]
.lw_cmp:
    cmp     rsi, rax
    jne     .err_layout
    inc     r14
    jmp     .lw

.lw_done:
    mov     rax, [rsp + 16]
    mov     [r12 + Q_N_FULL], rax
    cmp     eax, [r15 + HDR3_N_FULL]
    jne     .err_layout
    mov     rax, [rsp + 24]
    mov     [r12 + Q_N_LIN], rax

    ; ---- per-layer state sizes ----
    mov     rax, [r12 + Q_NV]
    imul    rax, [r12 + Q_DK]
    imul    rax, [r12 + Q_DV]
    shl     rax, 2
    mov     [r12 + Q_S_BYTES], rax
    mov     rax, [r12 + Q_CONV_DIM]
    imul    rax, 12                     ; 3 rows * 4 B
    mov     [r12 + Q_CONV_BYTES], rax
    mov     rax, [rbx + CTX_N_KV_HEADS]
    imul    rax, [rbx + CTX_MAX_SEQ]
    imul    rax, [rbx + CTX_HEAD_DIM]
    shl     rax, 2
    mov     [r12 + Q_KV_LAYER_BYTES], rax

    ; ---- arena layout (offsets, relocated below) ----
    xor     ebp, ebp
    mov     rax, [rbx + CTX_DIM]
    shl     rax, 2
    ALLOC   rbx, CTX_BUF_X, rax
    ALLOC   rbx, CTX_BUF_XB, rax

    mov     rax, [r12 + Q_WQKV_M]
    cmp     rax, [r12 + Q_WIN_M]
    jae     .proj_ok
    mov     rax, [r12 + Q_WIN_M]
.proj_ok:
    shl     rax, 2
    ALLOC   rbx, CTX_BUF_Q, rax
    mov     rax, [rbx + CTX_BUF_Q]
    mov     rcx, [rbx + CTX_Q_DIM]
    lea     rax, [rax + rcx * 8]        ; + 2*nh*hd*4 (skip q and gate)
    mov     [rbx + CTX_BUF_K], rax
    mov     rcx, [rbx + CTX_KV_DIM]
    lea     rax, [rax + rcx * 4]
    mov     [rbx + CTX_BUF_V], rax

    mov     rax, [rbx + CTX_Q_DIM]
    cmp     rax, [rsp + 8]
    jae     .ao_ok
    mov     rax, [rsp + 8]
.ao_ok:
    shl     rax, 2
    ALLOC   rbx, CTX_BUF_ATT_OUT, rax

    mov     rax, [rbx + CTX_N_HEADS]
    imul    rax, [rbx + CTX_MAX_SEQ]
    shl     rax, 2
    ALLOC   rbx, CTX_BUF_ATT_SCR, rax

    mov     rax, [rsp + 0]              ; 2*I
    shl     rax, 2
    ALLOC   rbx, CTX_BUF_GATE, rax
    mov     rax, [rbx + CTX_BUF_GATE]
    mov     rcx, [rbx + CTX_HIDDEN_DIM]
    lea     rax, [rax + rcx * 4]
    mov     [rbx + CTX_BUF_UP], rax

    mov     rax, [rbx + CTX_VOCAB_SIZE]
    shl     rax, 2
    ALLOC   rbx, CTX_BUF_LOGITS, rax
    ALLOC   rbx, CTX_BUF_PROBS, rax
    ALLOC   rbx, CTX_BUF_XQ, 64         ; unused (bss q8 scratch is used)
    ALLOC   rbx, CTX_BUF_XS, 64

    mov     rax, [r12 + Q_ROT]
    shl     rax, 2                      ; (rot/2) doubles
    ALLOC   rbx, CTX_BUF_INV_FREQ, rax
    shr     rax, 1                      ; (rot/2) floats
    ALLOC   rbx, CTX_BUF_COS, rax
    ALLOC   rbx, CTX_BUF_SIN, rax
    ALLOC   rbx, CTX_BUF_PROMPT, MAX_PROMPT_TOKENS * 4
    ALLOC   rbx, CTX_BUF_GEN, MAX_GEN_TOKENS * 4

    mov     rax, [r12 + Q_KV_LAYER_BYTES]
    imul    rax, [r12 + Q_N_FULL]
    ALLOC   rbx, CTX_BUF_K_CACHE, rax
    ALLOC   rbx, CTX_BUF_V_CACHE, rax

    ; recurrent state: S then conv state, contiguous (zeroed by qwen35_reset)
    mov     rax, [r12 + Q_S_BYTES]
    imul    rax, [r12 + Q_N_LIN]
    ALLOC   r12, Q_S_BASE, rax
    mov     rax, [r12 + Q_CONV_BYTES]
    imul    rax, [r12 + Q_N_LIN]
    ALLOC   r12, Q_CONV_BASE, rax
    ALIGN64 rbp
    mov     rax, rbp
    sub     rax, [r12 + Q_S_BASE]
    mov     [r12 + Q_STATE_BYTES], rax

    mov     rax, [r12 + Q_CONV_DIM]
    shl     rax, 4                      ; conv_dim * 4 taps * 4 B
    imul    rax, [r12 + Q_N_LIN]
    ALLOC   r12, Q_CONVT_BASE, rax
    mov     rax, [r12 + Q_NV]
    shl     rax, 2
    ALLOC   r12, Q_BETA, rax
    ALLOC   r12, Q_EG, rax
    mov     rax, [r12 + Q_CONV_DIM]
    shl     rax, 2
    ALLOC   r12, Q_ONES, rax
    ALIGN64 rbp
    mov     [rbx + CTX_ARENA_SIZE], rbp

    mov     rdi, rbp
    call    sys_mmap_anon
    test    rax, rax
    js      .err_arena
    mov     [rbx + CTX_ARENA_BASE], rax

    lea     rcx, [rbx + CTX_BUF_X]
    mov     edx, 20
.reloc1:
    add     [rcx], rax
    add     rcx, 8
    dec     edx
    jnz     .reloc1
    lea     rcx, [r12 + Q_S_BASE]
    mov     edx, Q_N_ARENA_PTRS
.reloc2:
    add     [rcx], rax
    add     rcx, 8
    dec     edx
    jnz     .reloc2

    ; ---- ones[] ----
    mov     rdi, [r12 + Q_ONES]
    mov     rcx, [r12 + Q_CONV_DIM]
    mov     eax, 0x3F800000
    rep stosd

    ; ---- per-layer arena pointers + interleaved conv weights ----
    xor     r14d, r14d
.lp2:
    cmp     r14, [rbx + CTX_N_LAYERS]
    jae     .lp2_done
    mov     rdi, r14
    shl     rdi, 7
    lea     rdi, [r12 + Q_LAYERS + rdi]
    mov     rax, [rdi + LR_IDX]
    cmp     qword [rdi + LR_FULL], 0
    je      .lp2_lin
    imul    rax, [r12 + Q_KV_LAYER_BYTES]
    mov     [rdi + LR_KVOFF], rax
    jmp     .lp2_next
.lp2_lin:
    mov     rcx, rax
    imul    rcx, [r12 + Q_S_BYTES]
    add     rcx, [r12 + Q_S_BASE]
    mov     [rdi + LR_S], rcx
    mov     rcx, rax
    imul    rcx, [r12 + Q_CONV_BYTES]
    add     rcx, [r12 + Q_CONV_BASE]
    mov     [rdi + LR_CST], rcx
    mov     rcx, [r12 + Q_CONV_DIM]
    shl     rcx, 4
    imul    rcx, rax
    add     rcx, [r12 + Q_CONVT_BASE]
    mov     [rdi + LR_CONVT], rcx
    ; convT[(c>>3)*32 + t*8 + (c&7)] = conv_w[c*4 + t]
    mov     rsi, [rdi + LR_CONVW]
    xor     edx, edx                    ; c
.tr_lp:
    cmp     rdx, [r12 + Q_CONV_DIM]
    jae     .lp2_next
    mov     r8, rdx
    shr     r8, 3
    shl     r8, 5
    mov     r9, rdx
    and     r9, 7
    add     r8, r9                      ; dst float index for t = 0
    mov     r9, rdx
    shl     r9, 4                       ; src byte offset c*16
    mov     eax, [rsi + r9 + 0]
    mov     [rcx + r8 * 4 + 0], eax
    mov     eax, [rsi + r9 + 4]
    mov     [rcx + r8 * 4 + 32], eax
    mov     eax, [rsi + r9 + 8]
    mov     [rcx + r8 * 4 + 64], eax
    mov     eax, [rsi + r9 + 12]
    mov     [rcx + r8 * 4 + 96], eax
    inc     rdx
    jmp     .tr_lp
.lp2_next:
    inc     r14
    jmp     .lp2
.lp2_done:

    ; ---- scalars ----
    vcvtsi2sd xmm0, xmm0, qword [rbx + CTX_HEAD_DIM]
    vsqrtsd xmm0, xmm0, xmm0
    vmovsd  xmm1, [rel q_one_d]
    vdivsd  xmm0, xmm1, xmm0
    vcvtsd2ss xmm0, xmm0, xmm0
    vmovss  [r12 + Q_ATT_SCALE], xmm0
    vcvtsi2sd xmm0, xmm0, qword [r12 + Q_DK]
    vsqrtsd xmm0, xmm0, xmm0
    vmovsd  xmm1, [rel q_one_d]
    vdivsd  xmm0, xmm1, xmm0
    vcvtsd2ss xmm0, xmm0, xmm0
    vmovss  [r12 + Q_SCALE_Q], xmm0
    mov     dword [r12 + Q_L2_EPS], 0x358637BD  ; 1e-6f

    mov     rdi, [rbx + CTX_BUF_INV_FREQ]
    mov     rsi, [r12 + Q_ROT]
    vmovss  xmm0, [rbx + CTX_ROPE_THETA]
    call    init_rope_freqs

    xor     eax, eax
    jmp     .ret

%ifndef BARE_METAL
.err_open_close:
    mov     rdi, r14
    call    sys_close
.err_open:
    mov     rax, -1
    jmp     .ret
%endif
.err_hdr:
    mov     rax, -2
    jmp     .ret
.err_arena:
    mov     rax, -3
    jmp     .ret
.err_layout:
    mov     rax, -4
    jmp     .ret
.err_dims:
    mov     rax, -5
.ret:
    add     rsp, STAT_SIZE + 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; ----------------------------------------------------------------------------
; void qwen35_reset(void) — zero DeltaNet S + conv states (KV cache is
; position-indexed and simply overwritten).
; ----------------------------------------------------------------------------
qwen35_reset:
    lea     rax, [rel g_q35]
    mov     rdi, [rax + Q_S_BASE]
    mov     rcx, [rax + Q_STATE_BYTES]
    test    rdi, rdi
    jz      .done
    xor     eax, eax
    rep stosb
.done:
    ret

; ----------------------------------------------------------------------------
; void qwen35_forward(uint64 token, uint64 pos, uint64 compute_logits)
; Logits (if requested) -> g_ctx.BUF_LOGITS.
; ----------------------------------------------------------------------------
qwen35_forward:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 56
    ; [rsp+0] pos, [rsp+8] compute_logits, [rsp+16] l
    mov     [rsp + 0], rsi
    mov     [rsp + 8], rdx
    lea     rbx, [rel g_ctx]
    lea     r15, [rel g_q35]

    ; embedding row -> X
    mov     rax, [rbx + CTX_DIM]
    mov     rdx, rax
    shr     rdx, 4
    add     rdx, rax
    imul    rdi, rdx
    add     rdi, [rbx + CTX_EMB_PTR]
    mov     rsi, [rbx + CTX_BUF_X]
    mov     rdx, rax
    call    dequant_row_q8_0

    ; partial-RoPE cos/sin for this position
    mov     rdi, [rbx + CTX_BUF_COS]
    mov     rsi, [rbx + CTX_BUF_SIN]
    mov     rdx, [rbx + CTX_BUF_INV_FREQ]
    mov     rcx, [r15 + Q_ROT]
    shr     rcx, 1
    mov     r8,  [rsp + 0]
    call    compute_rope_cs

    mov     qword [rsp + 16], 0
.layer_lp:
    mov     rax, [rsp + 16]
    cmp     rax, [rbx + CTX_N_LAYERS]
    jae     .layers_done
    shl     rax, 7
    lea     r14, [r15 + Q_LAYERS + rax]

    ; h = rmsnorm(x, in_norm) -> XB
    mov     rdi, [rbx + CTX_BUF_XB]
    mov     rsi, [rbx + CTX_BUF_X]
    mov     rdx, [r14 + LR_IN_NORM]
    mov     rcx, [rbx + CTX_DIM]
    vmovss  xmm0, [rbx + CTX_RMS_EPS]
    call    rmsnorm_avx2

    cmp     qword [r14 + LR_FULL], 0
    je      .linear

    ; ======================= full attention =======================
    mov     rdi, [r14 + LR_W1]
    mov     rsi, [rbx + CTX_BUF_XB]
    mov     rdx, [rbx + CTX_BUF_Q]
    mov     rcx, [r15 + Q_WQKV_M]
    mov     r8,  [rbx + CTX_DIM]
    call    tp_matmul_q8_0

    ; q heads: rmsnorm(q_norm) + partial rope
    mov     r12, [rbx + CTX_BUF_Q]
    mov     r13, [rbx + CTX_N_HEADS]
.qh_lp:
    mov     rdi, r12
    mov     rsi, r12
    mov     rdx, [r14 + LR_P0]
    mov     rcx, [rbx + CTX_HEAD_DIM]
    vmovss  xmm0, [rbx + CTX_RMS_EPS]
    call    rmsnorm_avx2
    mov     rdi, r12
    mov     rsi, [rbx + CTX_BUF_COS]
    mov     rdx, [rbx + CTX_BUF_SIN]
    mov     ecx, 1
    mov     r8,  [r15 + Q_ROT]
    call    rope_apply_avx2
    mov     rax, [rbx + CTX_HEAD_DIM]
    lea     r12, [r12 + rax * 4]
    dec     r13
    jnz     .qh_lp

    mov     r12, [rbx + CTX_BUF_K]
    mov     r13, [rbx + CTX_N_KV_HEADS]
.kh_lp:
    mov     rdi, r12
    mov     rsi, r12
    mov     rdx, [r14 + LR_P1]
    mov     rcx, [rbx + CTX_HEAD_DIM]
    vmovss  xmm0, [rbx + CTX_RMS_EPS]
    call    rmsnorm_avx2
    mov     rdi, r12
    mov     rsi, [rbx + CTX_BUF_COS]
    mov     rdx, [rbx + CTX_BUF_SIN]
    mov     ecx, 1
    mov     r8,  [r15 + Q_ROT]
    call    rope_apply_avx2
    mov     rax, [rbx + CTX_HEAD_DIM]
    lea     r12, [r12 + rax * 4]
    dec     r13
    jnz     .kh_lp

    ; store k, v into cache[full_idx][kv_h][pos][hd]
    mov     r9, [rbx + CTX_HEAD_DIM]
    shl     r9, 2                       ; head bytes
    mov     r10, [rbx + CTX_MAX_SEQ]
    imul    r10, r9                     ; kv head stride
    mov     rax, [rsp + 0]
    imul    rax, r9
    add     rax, [r14 + LR_KVOFF]
    mov     rdi, [rbx + CTX_BUF_K_CACHE]
    add     rdi, rax
    mov     rdx, [rbx + CTX_BUF_V_CACHE]
    add     rdx, rax
    mov     rsi, [rbx + CTX_BUF_K]
    mov     r8,  [rbx + CTX_BUF_V]
    mov     r12, [rbx + CTX_N_KV_HEADS]
.kv_head_lp:
    xor     ecx, ecx
.kv_copy_lp:
    vmovups ymm0, [rsi + rcx]
    vmovups ymm1, [r8 + rcx]
    vmovups [rdi + rcx], ymm0
    vmovups [rdx + rcx], ymm1
    add     rcx, 32
    cmp     rcx, r9
    jb      .kv_copy_lp
    add     rsi, r9
    add     r8,  r9
    add     rdi, r10
    add     rdx, r10
    dec     r12
    jnz     .kv_head_lp

    ; attention over heads -> ATT_OUT
    mov     rdi, [r14 + LR_KVOFF]
    mov     rsi, [rsp + 0]
    mov     edx, [r15 + Q_GQA]
    shl     rdx, 32
    mov     eax, [r15 + Q_ATT_SCALE]
    or      rdx, rax
    mov     rcx, [rbx + CTX_N_HEADS]
    call    tp_gqa_attn

    ; att *= sigmoid(gate)
    mov     rdi, [rbx + CTX_BUF_ATT_OUT]
    mov     rax, [rbx + CTX_Q_DIM]
    mov     rsi, [rbx + CTX_BUF_Q]
    lea     rsi, [rsi + rax * 4]
    mov     rdx, rax
    call    sigmoid_mul_avx2

    ; x += wo @ att
    mov     rdi, [r14 + LR_W2]
    mov     rsi, [rbx + CTX_BUF_ATT_OUT]
    mov     rdx, [rbx + CTX_BUF_XB]
    mov     rcx, [rbx + CTX_DIM]
    mov     r8,  [rbx + CTX_Q_DIM]
    call    tp_matmul_q8_0
    jmp     .resid_mlp

    ; ===================== Gated DeltaNet layer =====================
.linear:
    mov     rdi, [r14 + LR_W1]
    mov     rsi, [rbx + CTX_BUF_XB]
    mov     rdx, [rbx + CTX_BUF_Q]
    mov     rcx, [r15 + Q_WIN_M]
    mov     r8,  [rbx + CTX_DIM]
    call    tp_matmul_q8_0

    ; causal depthwise conv (k=4) over raw qkv, state shift, in place
    mov     rsi, [r14 + LR_CST]
    mov     rdi, [r14 + LR_CONVT]
    mov     rdx, [rbx + CTX_BUF_Q]
    mov     rcx, [r15 + Q_CONV_DIM]
    shr     rcx, 3
.conv_lp:
    vmovups ymm0, [rsi]
    vmovups ymm1, [rsi + 32]
    vmovups ymm2, [rsi + 64]
    vmovups ymm3, [rdx]
    vmulps  ymm4, ymm0, [rdi]
    vmulps  ymm5, ymm1, [rdi + 32]
    vaddps  ymm4, ymm4, ymm5
    vmulps  ymm5, ymm2, [rdi + 64]
    vaddps  ymm4, ymm4, ymm5
    vmulps  ymm5, ymm3, [rdi + 96]
    vaddps  ymm4, ymm4, ymm5
    vmovups [rsi], ymm1
    vmovups [rsi + 32], ymm2
    vmovups [rsi + 64], ymm3
    vmovups [rdx], ymm4
    add     rsi, 96
    add     rdi, 128
    add     rdx, 32
    dec     rcx
    jnz     .conv_lp

    ; silu over conv output
    mov     rdi, [rbx + CTX_BUF_Q]
    mov     rsi, [r15 + Q_ONES]
    mov     rdx, [r15 + Q_CONV_DIM]
    call    silu_mul_avx2

    ; L2-normalise q and k heads (2*nk heads of dk); q *= 1/sqrt(dk)
    mov     r12, [rbx + CTX_BUF_Q]
    xor     r13d, r13d
.l2_lp:
    mov     rax, [r15 + Q_NK]
    add     rax, rax
    cmp     r13, rax
    jae     .l2_done
    mov     rdi, r12
    mov     rsi, r12
    mov     rdx, [r15 + Q_DK]
    call    dot_f32_avx2
    vaddss  xmm0, xmm0, [r15 + Q_L2_EPS]
    vsqrtss xmm0, xmm0, xmm0
    vbroadcastss ymm1, xmm0
    vbroadcastss ymm2, [r15 + Q_SCALE_Q]
    mov     rcx, [r15 + Q_DK]
    shl     rcx, 2
    xor     eax, eax
    cmp     r13, [r15 + Q_NK]
    jae     .l2k_lp
.l2q_lp:
    vmovups ymm0, [r12 + rax]
    vdivps  ymm0, ymm0, ymm1
    vmulps  ymm0, ymm0, ymm2
    vmovups [r12 + rax], ymm0
    add     rax, 32
    cmp     rax, rcx
    jb      .l2q_lp
    jmp     .l2_next
.l2k_lp:
    vmovups ymm0, [r12 + rax]
    vdivps  ymm0, ymm0, ymm1
    vmovups [r12 + rax], ymm0
    add     rax, 32
    cmp     rax, rcx
    jb      .l2k_lp
.l2_next:
    add     r12, rcx
    inc     r13
    jmp     .l2_lp
.l2_done:

    ; beta = sigmoid(b), eg = exp(A * softplus(a + dt_bias))
    mov     rax, [r15 + Q_NV]
    imul    rax, [r15 + Q_DV]
    add     rax, [r15 + Q_CONV_DIM]
    mov     r13, [rbx + CTX_BUF_Q]
    lea     r13, [r13 + rax * 4]        ; b[]
    mov     rax, [r15 + Q_NV]
    lea     rbp, [r13 + rax * 4]        ; a[]
    xor     r12d, r12d
.bg_lp:
    cmp     r12, [r15 + Q_NV]
    jae     .bg_done
    vmovss  xmm0, [r13 + r12 * 4]
    vcvtss2sd xmm0, xmm0, xmm0
    vxorpd  xmm0, xmm0, [rel q_sign_d]
    call    q_exp_f64
    vaddsd  xmm0, xmm0, [rel q_one_d]
    vmovsd  xmm1, [rel q_one_d]
    vdivsd  xmm0, xmm1, xmm0
    vcvtsd2ss xmm0, xmm0, xmm0
    mov     rax, [r15 + Q_BETA]
    vmovss  [rax + r12 * 4], xmm0

    vmovss  xmm0, [rbp + r12 * 4]
    mov     rax, [r14 + LR_P1]          ; dt_bias
    vaddss  xmm0, xmm0, [rax + r12 * 4]
    vucomiss xmm0, [rel q_f20]
    ja      .sp_lin
    vcvtss2sd xmm0, xmm0, xmm0
    call    q_exp_f64
    vaddsd  xmm0, xmm0, [rel q_one_d]
    call    log_f64
    vcvtsd2ss xmm0, xmm0, xmm0
.sp_lin:
    mov     rax, [r14 + LR_P0]          ; A = -exp(A_log)
    vmulss  xmm0, xmm0, [rax + r12 * 4]
    vcvtss2sd xmm0, xmm0, xmm0
    call    q_exp_f64
    vcvtsd2ss xmm0, xmm0, xmm0
    mov     rax, [r15 + Q_EG]
    vmovss  [rax + r12 * 4], xmm0
    inc     r12
    jmp     .bg_lp
.bg_done:

    ; delta-rule update + readout per v head -> ATT_OUT[hv]
    xor     r12d, r12d
.gdn_lp:
    cmp     r12, [r15 + Q_NV]
    jae     .gdn_done
    mov     rax, r12
    xor     edx, edx
    div     qword [r15 + Q_RATIO]       ; hk
    imul    rax, [r15 + Q_DK]
    mov     rsi, [rbx + CTX_BUF_Q]
    lea     rsi, [rsi + rax * 4]        ; q[hk]
    mov     rax, [r15 + Q_NK]
    imul    rax, [r15 + Q_DK]
    lea     rdx, [rsi + rax * 4]        ; k[hk]
    mov     rcx, [rbx + CTX_BUF_Q]
    lea     rcx, [rcx + rax * 8]        ; v base (after q, k)
    mov     rax, r12
    imul    rax, [r15 + Q_DV]
    lea     rcx, [rcx + rax * 4]        ; v[hv]
    mov     r8, [rbx + CTX_BUF_ATT_OUT]
    lea     r8, [r8 + rax * 4]          ; out[hv]
    mov     rax, [r15 + Q_DK]
    imul    rax, [r15 + Q_DV]
    imul    rax, r12
    mov     rdi, [r14 + LR_S]
    lea     rdi, [rdi + rax * 4]        ; S[hv]
    mov     r9,  [r15 + Q_DK]
    mov     r10, [r15 + Q_DV]
    mov     rax, [r15 + Q_EG]
    vmovss  xmm0, [rax + r12 * 4]
    mov     rax, [r15 + Q_BETA]
    vmovss  xmm1, [rax + r12 * 4]
    call    gdn_head
    inc     r12
    jmp     .gdn_lp
.gdn_done:

    ; gated RMSNorm: out = rmsnorm(out, lin_norm) * silu(z)  (result in z)
    mov     r12, [rbx + CTX_BUF_ATT_OUT]
    mov     r13, [r15 + Q_NV]
.gn_lp:
    mov     rdi, r12
    mov     rsi, r12
    mov     rdx, [r14 + LR_LINNORM]
    mov     rcx, [r15 + Q_DV]
    vmovss  xmm0, [rbx + CTX_RMS_EPS]
    call    rmsnorm_avx2
    mov     rax, [r15 + Q_DV]
    lea     r12, [r12 + rax * 4]
    dec     r13
    jnz     .gn_lp

    mov     rax, [r15 + Q_CONV_DIM]
    mov     r12, [rbx + CTX_BUF_Q]
    lea     r12, [r12 + rax * 4]        ; z
    mov     r13, [r15 + Q_NV]
    imul    r13, [r15 + Q_DV]           ; nv*dv
    mov     rdi, r12
    mov     rsi, [rbx + CTX_BUF_ATT_OUT]
    mov     rdx, r13
    call    silu_mul_avx2

    mov     rdi, [r14 + LR_W2]
    mov     rsi, r12
    mov     rdx, [rbx + CTX_BUF_XB]
    mov     rcx, [rbx + CTX_DIM]
    mov     r8,  r13
    call    tp_matmul_q8_0

    ; ====================== residual + MLP ======================
.resid_mlp:
    mov     rdi, [rbx + CTX_BUF_X]
    mov     rsi, [rbx + CTX_BUF_XB]
    mov     rdx, [rbx + CTX_DIM]
    call    vec_add_avx2

    mov     rdi, [rbx + CTX_BUF_XB]
    mov     rsi, [rbx + CTX_BUF_X]
    mov     rdx, [r14 + LR_POST_NORM]
    mov     rcx, [rbx + CTX_DIM]
    vmovss  xmm0, [rbx + CTX_RMS_EPS]
    call    rmsnorm_avx2

    mov     rdi, [r14 + LR_WGU]
    mov     rsi, [rbx + CTX_BUF_XB]
    mov     rdx, [rbx + CTX_BUF_GATE]
    mov     rcx, [rbx + CTX_HIDDEN_DIM]
    add     rcx, rcx
    mov     r8,  [rbx + CTX_DIM]
    call    tp_matmul_q8_0

    mov     rdi, [rbx + CTX_BUF_GATE]
    mov     rsi, [rbx + CTX_BUF_UP]
    mov     rdx, [rbx + CTX_HIDDEN_DIM]
    call    silu_mul_avx2

    mov     rdi, [r14 + LR_WDOWN]
    mov     rsi, [rbx + CTX_BUF_GATE]
    mov     rdx, [rbx + CTX_BUF_XB]
    mov     rcx, [rbx + CTX_DIM]
    mov     r8,  [rbx + CTX_HIDDEN_DIM]
    call    tp_matmul_q8_0

    mov     rdi, [rbx + CTX_BUF_X]
    mov     rsi, [rbx + CTX_BUF_XB]
    mov     rdx, [rbx + CTX_DIM]
    call    vec_add_avx2

    ; optional hidden-state dump
    mov     rdi, [rel q35_dump_fd]
    test    rdi, rdi
    js      .no_dump
    mov     rsi, [rbx + CTX_BUF_X]
    mov     rdx, [rbx + CTX_DIM]
    shl     rdx, 2
    call    sys_write_all
.no_dump:
    inc     qword [rsp + 16]
    jmp     .layer_lp

.layers_done:
    cmp     qword [rsp + 8], 0
    je      .fwd_ret
    mov     rdi, [rbx + CTX_BUF_X]
    mov     rsi, [rbx + CTX_BUF_X]
    mov     rdx, [rbx + CTX_FINAL_NORM_PTR]
    mov     rcx, [rbx + CTX_DIM]
    vmovss  xmm0, [rbx + CTX_RMS_EPS]
    call    rmsnorm_avx2
    mov     rdi, [r15 + Q_LM_HEAD]
    mov     rsi, [rbx + CTX_BUF_X]
    mov     rdx, [rbx + CTX_BUF_LOGITS]
    mov     rcx, [rbx + CTX_VOCAB_SIZE]
    mov     r8,  [rbx + CTX_DIM]
    call    tp_matmul_q8_0
.fwd_ret:
    vzeroupper
    add     rsp, 56
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; ----------------------------------------------------------------------------
; gdn_head: one Gated-DeltaNet step for a single v head.
; rdi = S [dk][dv] (in/out), rsi = q[dk], rdx = k[dk], rcx = v[dv],
; r8 = out[dv], r9 = dk, r10 = dv (multiple of 32), xmm0 = exp(g), xmm1 = beta
;   S *= eg; kv = k@S; delta = (v - kv)*beta; S += k (x) delta; out = q@S
; Clobbers rax, r10, r11, ymm0-15. Preserves rbx.
; ----------------------------------------------------------------------------
gdn_head:
    push    rbx
    vbroadcastss ymm14, xmm0
    vbroadcastss ymm15, xmm1
    lea     r11, [r10 * 4]              ; row bytes
    xor     eax, eax                    ; column chunk byte offset
.col_lp:
    cmp     rax, r11
    jae     .done
    ; pass 1: decay + kv accumulate
    lea     r10, [rdi + rax]
    xor     ebx, ebx
    vxorps  ymm0, ymm0, ymm0
    vxorps  ymm1, ymm1, ymm1
    vxorps  ymm2, ymm2, ymm2
    vxorps  ymm3, ymm3, ymm3
.p1:
    vbroadcastss ymm8, [rdx + rbx * 4]
    vmulps  ymm4, ymm14, [r10]
    vmulps  ymm5, ymm14, [r10 + 32]
    vmulps  ymm6, ymm14, [r10 + 64]
    vmulps  ymm7, ymm14, [r10 + 96]
    vmovups [r10], ymm4
    vmovups [r10 + 32], ymm5
    vmovups [r10 + 64], ymm6
    vmovups [r10 + 96], ymm7
    vfmadd231ps ymm0, ymm8, ymm4
    vfmadd231ps ymm1, ymm8, ymm5
    vfmadd231ps ymm2, ymm8, ymm6
    vfmadd231ps ymm3, ymm8, ymm7
    add     r10, r11
    inc     rbx
    cmp     rbx, r9
    jb      .p1
    ; delta = (v - kv) * beta
    vmovups ymm9,  [rcx + rax]
    vmovups ymm10, [rcx + rax + 32]
    vmovups ymm11, [rcx + rax + 64]
    vmovups ymm12, [rcx + rax + 96]
    vsubps  ymm9,  ymm9,  ymm0
    vsubps  ymm10, ymm10, ymm1
    vsubps  ymm11, ymm11, ymm2
    vsubps  ymm12, ymm12, ymm3
    vmulps  ymm9,  ymm9,  ymm15
    vmulps  ymm10, ymm10, ymm15
    vmulps  ymm11, ymm11, ymm15
    vmulps  ymm12, ymm12, ymm15
    ; pass 2: rank-1 update + readout
    lea     r10, [rdi + rax]
    xor     ebx, ebx
    vxorps  ymm0, ymm0, ymm0
    vxorps  ymm1, ymm1, ymm1
    vxorps  ymm2, ymm2, ymm2
    vxorps  ymm3, ymm3, ymm3
.p2:
    vbroadcastss ymm8,  [rdx + rbx * 4]     ; k[i]
    vbroadcastss ymm13, [rsi + rbx * 4]     ; q[i]
    vmulps  ymm4, ymm8, ymm9
    vmulps  ymm5, ymm8, ymm10
    vmulps  ymm6, ymm8, ymm11
    vmulps  ymm7, ymm8, ymm12
    vaddps  ymm4, ymm4, [r10]
    vaddps  ymm5, ymm5, [r10 + 32]
    vaddps  ymm6, ymm6, [r10 + 64]
    vaddps  ymm7, ymm7, [r10 + 96]
    vmovups [r10], ymm4
    vmovups [r10 + 32], ymm5
    vmovups [r10 + 64], ymm6
    vmovups [r10 + 96], ymm7
    vfmadd231ps ymm0, ymm13, ymm4
    vfmadd231ps ymm1, ymm13, ymm5
    vfmadd231ps ymm2, ymm13, ymm6
    vfmadd231ps ymm3, ymm13, ymm7
    add     r10, r11
    inc     rbx
    cmp     rbx, r9
    jb      .p2
    vmovups [r8 + rax], ymm0
    vmovups [r8 + rax + 32], ymm1
    vmovups [r8 + rax + 64], ymm2
    vmovups [r8 + rax + 96], ymm3
    add     rax, 128
    jmp     .col_lp
.done:
    pop     rbx
    ret

; ----------------------------------------------------------------------------
; sigmoid_mul_avx2(float *dst, const float *gate, uint64 n%8)
; dst[i] *= 1 / (1 + exp(-gate[i]))
; ----------------------------------------------------------------------------
sigmoid_mul_avx2:
    vbroadcastss ymm8, [rel q_sign_f]
    vbroadcastss ymm10, [rel q_one_f]
    shl     rdx, 2
    xor     ecx, ecx
.lp:
    cmp     rcx, rdx
    jae     .done
    vmovups ymm0, [rsi + rcx]
    vxorps  ymm0, ymm0, ymm8
    call    fast_expf_avx2              ; clobbers ymm0-5 only
    vaddps  ymm0, ymm0, ymm10
    vdivps  ymm1, ymm10, ymm0
    vmulps  ymm1, ymm1, [rdi + rcx]
    vmovups [rdi + rcx], ymm1
    add     rcx, 32
    jmp     .lp
.done:
    ret

; xmm0 = exp(clamp(xmm0, -700, 700)) in f64
q_exp_f64:
    vmaxsd  xmm0, xmm0, [rel q_exp_lo]
    vminsd  xmm0, xmm0, [rel q_exp_hi]
    jmp     exp_f64

; ----------------------------------------------------------------------------
; uint64 qwen35_build_chat_prompt(const char *prompt, uint32 *out,
;                                 uint64 max, int raw, int think)
; <|im_start|>user\n{bpe}<|im_end|>\n<|im_start|>assistant\n<think>
;   + think ? "\n" : "\n\n</think>\n\n"
; ----------------------------------------------------------------------------
qwen35_build_chat_prompt:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     r12, rdi
    mov     r13, rsi
    mov     r14, rdx
    mov     r15, rcx
    mov     rbx, r8
    mov     rdi, r12
    call    str_len
    test    r15, r15
    jz      .chat
    mov     rdi, r12
    mov     rsi, rax
    mov     rdx, r13
    mov     rcx, r14
    call    tok_encode_bpe
    jmp     .out
.chat:
    lea     r8, [rel g_q35]
    mov     ecx, [r8 + Q_ID_IM_START]
    mov     [r13 + 0], ecx
    mov     ecx, [r8 + Q_ID_USER]
    mov     [r13 + 4], ecx
    mov     ecx, [r8 + Q_ID_NL]
    mov     [r13 + 8], ecx
    mov     rdi, r12
    mov     rsi, rax
    lea     rdx, [r13 + 12]
    lea     rcx, [r14 - 16]
    call    tok_encode_bpe
    lea     rcx, [rax + 3]
    lea     r8, [rel g_q35]
    mov     eax, [r8 + Q_ID_IM_END]
    mov     [r13 + rcx * 4 + 0], eax
    mov     eax, [r8 + Q_ID_NL]
    mov     [r13 + rcx * 4 + 4], eax
    mov     eax, [r8 + Q_ID_IM_START]
    mov     [r13 + rcx * 4 + 8], eax
    mov     eax, [r8 + Q_ID_ASSISTANT]
    mov     [r13 + rcx * 4 + 12], eax
    mov     eax, [r8 + Q_ID_NL]
    mov     [r13 + rcx * 4 + 16], eax
    mov     eax, [r8 + Q_ID_THINK]
    mov     [r13 + rcx * 4 + 20], eax
    add     rcx, 6
    test    rbx, rbx
    jz      .nothink
    mov     eax, [r8 + Q_ID_NL]
    mov     [r13 + rcx * 4], eax
    inc     rcx
    jmp     .cnt
.nothink:
    mov     eax, [r8 + Q_ID_NLNL]
    mov     [r13 + rcx * 4 + 0], eax
    mov     eax, [r8 + Q_ID_END_THINK]
    mov     [r13 + rcx * 4 + 4], eax
    mov     eax, [r8 + Q_ID_NLNL]
    mov     [r13 + rcx * 4 + 8], eax
    add     rcx, 3
.cnt:
    mov     rax, rcx
.out:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

section .data
align 8
q35_dump_fd: dq -1

section .rodata
align 16
q_sign_d:   dq 0x8000000000000000, 0
q_one_d:    dq 1.0
q_exp_lo:   dq -700.0
q_exp_hi:   dq 700.0
q_f20:      dd 20.0
q_one_f:    dd 1.0
q_sign_f:   dd 0x80000000

section .bss
align 64
g_q35:      resb Q35_SIZE
