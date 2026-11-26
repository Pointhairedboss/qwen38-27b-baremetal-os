; ============================================================================
; qwen35.inc — .asmbin v3 header offsets + g_q35 hybrid-model state layout
; (Qwen3.5 / Qwen3.8: Gated DeltaNet + gated full attention)
; ============================================================================

%ifndef QWEN35_INC
%define QWEN35_INC

%define ASMBIN_VERSION_V3   3
%define ASMBIN_ARCH_QWEN35  1

; --- .asmbin v3 header (256 B, see scripts/export_qwen35.py HEADER_FIELDS) ---
%define HDR3_MAGIC          0
%define HDR3_VERSION        4
%define HDR3_ARCH           8
%define HDR3_FLAGS          12
%define HDR3_DIM            16
%define HDR3_HIDDEN         20
%define HDR3_N_LAYERS       24
%define HDR3_VOCAB          28
%define HDR3_N_HEADS        32
%define HDR3_N_KV           36
%define HDR3_HEAD_DIM       40
%define HDR3_ROT_DIMS       44
%define HDR3_THETA          48          ; f32
%define HDR3_EPS            52          ; f32
%define HDR3_HT_MASK        56
%define HDR3_MAX_SEQ        60
%define HDR3_NK             64
%define HDR3_NV             68
%define HDR3_DK             72
%define HDR3_DV             76
%define HDR3_CONV_K         80
%define HDR3_N_FULL         84
%define HDR3_BITMAP         88          ; u64, bit l = 1 -> full attention
%define HDR3_OFF_TOK        96
%define HDR3_OFF_EMBED      104
%define HDR3_OFF_LM_HEAD    112
%define HDR3_OFF_FNORM      120
%define HDR3_OFF_LTABLE     128         ; u64[n_layers] absolute layer offsets
%define HDR3_FILE_SIZE      136
%define HDR3_EOS            144         ; 4 x u32 (0xFFFFFFFF = unused)
%define HDR3_IDS            160         ; 10 x u32 special IDs (same order as Q_ID_*)
%define HDR3_LB_FULL        200         ; u64
%define HDR3_LB_LIN         208         ; u64

; --- g_q35 global state (qword fields unless noted) ---
%define Q_NK                0
%define Q_NV                8
%define Q_DK                16
%define Q_DV                24
%define Q_CONV_K            32
%define Q_CONV_DIM          40
%define Q_ROT               48
%define Q_WIN_M             56
%define Q_RATIO             64          ; nv / nk
%define Q_N_FULL            72
%define Q_N_LIN             80
%define Q_LM_HEAD           88
; arena pointers (relocated together, keep contiguous!)
%define Q_S_BASE            96          ; DeltaNet state [n_lin][nv][dk][dv] f32
%define Q_CONV_BASE         104         ; conv state [n_lin][conv_dim/8][3][8] f32 (raw qkv, oldest first)
%define Q_CONVT_BASE        112         ; conv weights [n_lin][conv_dim/8][4][8] (interleaved)
%define Q_BETA              120         ; [nv]
%define Q_EG                128         ; [nv] exp(g)
%define Q_ONES              136         ; [conv_dim] 1.0f (silu via silu_mul_avx2)
%define Q_N_ARENA_PTRS      6
%define Q_S_BYTES           152         ; per linear layer
%define Q_CONV_BYTES        160         ; per linear layer
%define Q_ATT_SCALE         168         ; f32
%define Q_GQA               172         ; u32
%define Q_KV_LAYER_BYTES    176
%define Q_IDS               184         ; 10 x u32, order below (= header order)
%define Q_ID_IM_START       184
%define Q_ID_IM_END         188
%define Q_ID_ENDOFTEXT      192
%define Q_ID_THINK          196
%define Q_ID_END_THINK      200
%define Q_ID_USER           204
%define Q_ID_ASSISTANT      208
%define Q_ID_SYSTEM         212
%define Q_ID_NL             216
%define Q_ID_NLNL           220
%define Q_WQKV_M            224
%define Q_STATE_BYTES       232         ; S + conv state bytes (zeroed by qwen35_reset)
%define Q_SCALE_Q           240         ; f32 1/sqrt(dk)
%define Q_L2_EPS            244         ; f32 1e-6
%define Q_LAYERS            256         ; Q_MAX_LAYERS x LR_SIZE records
%define Q_MAX_LAYERS        64

; --- per-layer record ---
%define LR_FULL             0
%define LR_IDX              8           ; index among full (or linear) layers
%define LR_IN_NORM          16
%define LR_POST_NORM        24
%define LR_P0               32          ; full: q_norm   | linear: A (= -exp(A_log))
%define LR_P1               40          ; full: k_norm   | linear: dt_bias
%define LR_CONVW            48          ; linear: conv_w [conv_dim][conv_k]
%define LR_LINNORM          56          ; linear: gated-norm weight [dv]
%define LR_W1               64          ; full: wqkv     | linear: win
%define LR_W2               72          ; full: wo       | linear: wout
%define LR_WGU              80
%define LR_WDOWN            88
%define LR_CONVT            96          ; linear: arena transposed conv_w
%define LR_S                104         ; linear: arena S state
%define LR_CST              112         ; linear: arena conv state
%define LR_KVOFF            120         ; full: byte offset into K/V cache
%define LR_SIZE             128

%define Q35_SIZE            (Q_LAYERS + Q_MAX_LAYERS * LR_SIZE)

%endif
