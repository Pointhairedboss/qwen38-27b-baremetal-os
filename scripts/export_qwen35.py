#!/usr/bin/env python3
"""
Export Qwen3.5 / Qwen3.8 hybrid (Gated DeltaNet + gated full attention)
HF safetensors into .asmbin v3 for 04-qwen38-27b-os.

Handles sharded checkpoints (model.safetensors.index.json), BF16/F16/F32
inputs, tied (0.8B/4B) and untied (27B) embeddings. Vision tower and MTP
head are skipped.

.asmbin v3 layout (all offsets 64-byte aligned):
  [0, 256)        header (see HEADER_FMT below)
  tokenizer       same blob format as v1/v2 (byte-level BPE + merge hash table)
  embed           Q8_0 [vocab, dim]
  lm_head         Q8_0 [vocab, dim]   (absent when tied: off_lm_head == off_embed)
  layer table     u64[n_layers] absolute layer offsets
  layers          per-layer blocks (size depends on layer type)
  final_norm      fp32 [dim] (+1 baked)

Q8_0 matrices: per-row split [K int8][K/32 fp16 scales] (row stride K + K/16),
identical to v2, so matmul_q8_0 (FAST_Q8) works unchanged.

Full-attention layer (bit set in layer_bitmap):
  f32 in_norm[dim] (+1)        f32 post_norm[dim] (+1)
  f32 q_norm[hd] (+1)          f32 k_norm[hd] (+1)
  Q8  wqkv  [2*nh*hd + 2*nkv*hd, dim]  rows: q(all heads) | gate(all heads) | k | v
            (HF q_proj per-head interleave [q|gate] is undone at export)
  Q8  wo    [dim, nh*hd]
  Q8  wgu   [2*inter, dim]  rows: gate | up
  Q8  wdown [dim, inter]

Linear (Gated DeltaNet) layer:
  f32 in_norm[dim] (+1)        f32 post_norm[dim] (+1)
  f32 A[nv]        (= -exp(A_log))
  f32 dt_bias[nv]
  f32 conv_w[conv_dim * conv_k]  (row-major [channel][tap], conv_dim = 2*nk*dk + nv*dv)
  f32 lin_norm[dv]  (plain weight, NO +1)
  Q8  win   [conv_dim + nv*dv + 2*nv, dim]  rows: qkv | z | b | a
  Q8  wout  [dim, nv*dv]
  Q8  wgu   [2*inter, dim]
  Q8  wdown [dim, inter]
"""

import argparse
import json
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_weights import (  # noqa: E402
    HT_MASK,
    align64,
    build_tokenizer_blob,
    pad64,
    quantize_matrix_q8_0_row_split,
)

MAGIC = b"ASML"
VERSION = 3
ARCH_QWEN35 = 1
HEADER_SIZE = 256

FLAG_TIED_EMB = 1
FLAG_QK_NORM = 2
FLAG_FP16_SCALES = 4
FLAG_ATTN_OUT_GATE = 8

# Header layout (little-endian). Offsets are referenced by asm (os_constants / qwen35.asm)
# and by scripts/ref_qwen35.py -- keep in sync.
HEADER_FIELDS = [
    ("magic", "4s"),          # 0
    ("version", "I"),         # 4
    ("arch", "I"),            # 8
    ("flags", "I"),           # 12
    ("dim", "I"),             # 16
    ("hidden_dim", "I"),      # 20
    ("n_layers", "I"),        # 24
    ("vocab_size", "I"),      # 28
    ("n_heads", "I"),         # 32
    ("n_kv_heads", "I"),      # 36
    ("head_dim", "I"),        # 40
    ("rot_dims", "I"),        # 44
    ("rope_theta", "f"),      # 48
    ("rms_eps", "f"),         # 52
    ("ht_mask", "I"),         # 56  (same offset as v1/v2 HDR_HT_MASK)
    ("max_seq_len", "I"),     # 60
    ("lin_nk", "I"),          # 64
    ("lin_nv", "I"),          # 68
    ("lin_dk", "I"),          # 72
    ("lin_dv", "I"),          # 76
    ("conv_k", "I"),          # 80
    ("n_full_layers", "I"),   # 84
    ("layer_bitmap", "Q"),    # 88  bit i = 1 -> layer i is full attention
    ("off_tokenizer", "Q"),   # 96
    ("off_embed", "Q"),       # 104
    ("off_lm_head", "Q"),     # 112
    ("off_final_norm", "Q"),  # 120
    ("off_layer_table", "Q"), # 128
    ("file_size", "Q"),       # 136 (data end, before pow2 padding)
    ("eos", "4I"),            # 144 (0xFFFFFFFF = unused)
    # Chat template special IDs                                           160..
    ("id_im_start", "I"),     # 160
    ("id_im_end", "I"),       # 164
    ("id_endoftext", "I"),    # 168
    ("id_think", "I"),        # 172
    ("id_end_think", "I"),    # 176
    ("id_user", "I"),         # 180
    ("id_assistant", "I"),    # 184
    ("id_system", "I"),       # 188
    ("id_nl", "I"),           # 192
    ("id_nlnl", "I"),         # 196
    ("layer_bytes_full", "Q"),   # 200
    ("layer_bytes_linear", "Q"), # 208
]
HEADER_FMT = "<" + "".join(f for _, f in HEADER_FIELDS)
assert struct.calcsize(HEADER_FMT) <= HEADER_SIZE, struct.calcsize(HEADER_FMT)


def header_offsets():
    offs, pos = {}, 0
    for name, f in HEADER_FIELDS:
        offs[name] = pos
        pos += struct.calcsize("<" + f)
    return offs


class ShardedST:
    """Minimal raw safetensors reader over one or many shards (no torch needed)."""

    def __init__(self, model_dir: str):
        idx = os.path.join(model_dir, "model.safetensors.index.json")
        if os.path.exists(idx):
            with open(idx) as f:
                wmap = json.load(f)["weight_map"]
            files = sorted(set(wmap.values()))
        else:
            files = ["model.safetensors"]
        self.meta = {}
        self.files = {}
        for fn in files:
            p = os.path.join(model_dir, fn)
            fh = open(p, "rb")
            n = struct.unpack("<Q", fh.read(8))[0]
            hdr = json.loads(fh.read(n))
            hdr.pop("__metadata__", None)
            self.files[fn] = fh
            for k, v in hdr.items():
                self.meta[k] = (fn, 8 + n, v)

    def __contains__(self, name):
        return name in self.meta

    def shape(self, name):
        return tuple(self.meta[name][2]["shape"])

    def get(self, name, rows=None) -> np.ndarray:
        """Return float32 tensor. rows=(r0, r1) reads a row slice of a 2-D tensor."""
        fn, base, m = self.meta[name]
        dtype, shape = m["dtype"], tuple(m["shape"])
        start, end = m["data_offsets"]
        esz = {"BF16": 2, "F16": 2, "F32": 4}[dtype]
        if rows is not None:
            r0, r1 = rows
            rowlen = int(np.prod(shape[1:])) * esz
            start, end = start + r0 * rowlen, start + r1 * rowlen
            shape = (r1 - r0,) + shape[1:]
        fh = self.files[fn]
        fh.seek(base + start)
        raw = fh.read(end - start)
        if dtype == "BF16":
            a = (np.frombuffer(raw, dtype="<u2").astype(np.uint32) << 16).view(np.float32)
        elif dtype == "F16":
            a = np.frombuffer(raw, dtype="<f2").astype(np.float32)
        else:
            a = np.frombuffer(raw, dtype="<f4").copy()
        return a.reshape(shape)


def write_f32(out, arr):
    pad64(out)
    out.write(np.ascontiguousarray(arr, dtype="<f4").tobytes())


def write_q8(out, mats, chunk_rows=8192):
    """Quantize the row-concatenation of `mats` (list of [Mi, K] arrays or (st, name) refs)."""
    pad64(out)
    for m in mats:
        if isinstance(m, tuple):  # (ShardedST, name) -> stream in row chunks
            st, name = m
            M = st.shape(name)[0]
            for r0 in range(0, M, chunk_rows):
                blk = st.get(name, rows=(r0, min(M, r0 + chunk_rows)))
                out.write(quantize_matrix_q8_0_row_split(blk, "fp16"))
        else:
            for r0 in range(0, m.shape[0], chunk_rows):
                out.write(quantize_matrix_q8_0_row_split(m[r0 : r0 + chunk_rows], "fp16"))


def special_ids(tok_json_path):
    with open(tok_json_path, encoding="utf-8") as f:
        tj = json.load(f)
    vocab = tj["model"]["vocab"]
    added = {a["content"]: a["id"] for a in tj.get("added_tokens", [])}

    def tok(s):
        if s in added:
            return added[s]
        return vocab[s]

    return {
        "id_im_start": tok("<|im_start|>"),
        "id_im_end": tok("<|im_end|>"),
        "id_endoftext": tok("<|endoftext|>"),
        "id_think": tok("<think>"),
        "id_end_think": tok("</think>"),
        "id_user": vocab["user"],
        "id_assistant": vocab["assistant"],
        "id_system": vocab["system"],
        "id_nl": vocab["Ċ"],          # byte-level "\n"
        "id_nlnl": vocab["ĊĊ"],  # "\n\n"
    }


def export(model_dir, out_path, max_seq_len, pad_pow2):
    t0 = time.time()
    with open(os.path.join(model_dir, "config.json")) as f:
        cfg = json.load(f)
    tc = cfg.get("text_config", cfg)
    dim = tc["hidden_size"]
    inter = tc["intermediate_size"]
    n_layers = tc["num_hidden_layers"]
    vocab = tc["vocab_size"]
    nh = tc["num_attention_heads"]
    nkv = tc["num_key_value_heads"]
    hd = tc["head_dim"]
    nk = tc["linear_num_key_heads"]
    nv = tc["linear_num_value_heads"]
    dk = tc["linear_key_head_dim"]
    dv = tc["linear_value_head_dim"]
    conv_k = tc["linear_conv_kernel_dim"]
    eps = tc["rms_norm_eps"]
    rp = tc.get("rope_parameters", {})
    theta = rp.get("rope_theta", tc.get("rope_theta"))
    prf = rp.get("partial_rotary_factor", tc.get("partial_rotary_factor", 1.0))
    rot_dims = int(hd * prf)
    assert tc.get("attn_output_gate", True)
    tied = bool(cfg.get("tie_word_embeddings", tc.get("tie_word_embeddings", False)))

    layer_types = tc.get("layer_types")
    if layer_types is None:
        iv = tc["full_attention_interval"]
        layer_types = ["full_attention" if (i + 1) % iv == 0 else "linear_attention" for i in range(n_layers)]
    assert n_layers <= 64
    bitmap = 0
    for i, t in enumerate(layer_types):
        if t == "full_attention":
            bitmap |= 1 << i
        else:
            assert t == "linear_attention", t
    n_full = bin(bitmap).count("1")
    conv_dim = 2 * nk * dk + nv * dv
    assert nv % nk == 0 and dim % 32 == 0 and inter % 32 == 0 and (nh * hd) % 32 == 0 and (nv * dv) % 32 == 0

    st = ShardedST(model_dir)
    P = "model.language_model."
    if P + "embed_tokens.weight" not in st:
        P = "model."
    if "lm_head.weight" in st and not tied:
        lm_name = "lm_head.weight"
    else:
        tied = True
        lm_name = None

    tok_path = os.path.join(model_dir, "tokenizer.json")
    sids = special_ids(tok_path)
    gpath = os.path.join(model_dir, "generation_config.json")
    gen = json.load(open(gpath)) if os.path.exists(gpath) else {}
    eos = gen.get("eos_token_id", tc.get("eos_token_id"))
    eos = eos if isinstance(eos, list) else [eos]
    for e in (sids["id_im_end"], sids["id_endoftext"]):
        if e not in eos:
            eos.append(e)
    eos = (eos + [0xFFFFFFFF] * 4)[:4]

    print(
        f"Qwen3.5 hybrid: dim={dim} inter={inter} L={n_layers} (full={n_full}) vocab={vocab} "
        f"attn {nh}/{nkv}x{hd} rot={rot_dims} theta={theta:g} | GDN nk={nk} nv={nv} dk={dk} dv={dv} "
        f"conv={conv_k} | tied={tied} eos={eos[:2]}"
    )
    tok_blob = build_tokenizer_blob(tok_path, vocab)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as out:
        out.write(b"\x00" * HEADER_SIZE)
        off_tok = pad64(out)
        out.write(tok_blob)

        off_embed = pad64(out)
        assert st.shape(P + "embed_tokens.weight") == (vocab, dim)
        print("Quantizing embed_tokens ...")
        write_q8(out, [(st, P + "embed_tokens.weight")])
        off_lm = off_embed
        if lm_name:
            assert st.shape(lm_name) == (vocab, dim)
            print("Quantizing lm_head ...")
            off_lm = pad64(out)
            write_q8(out, [(st, lm_name)])

        off_table = pad64(out)
        out.write(b"\x00" * (8 * n_layers))
        layer_offs = []
        sizes = {}
        for l in range(n_layers):
            start = pad64(out)
            layer_offs.append(start)
            p = f"{P}layers.{l}."
            write_f32(out, st.get(p + "input_layernorm.weight") + 1.0)
            write_f32(out, st.get(p + "post_attention_layernorm.weight") + 1.0)
            if bitmap >> l & 1:
                a = p + "self_attn."
                write_f32(out, st.get(a + "q_norm.weight") + 1.0)
                write_f32(out, st.get(a + "k_norm.weight") + 1.0)
                wq = st.get(a + "q_proj.weight")
                assert wq.shape == (2 * nh * hd, dim), wq.shape
                wq = wq.reshape(nh, 2, hd, dim)
                q_rows = wq[:, 0].reshape(nh * hd, dim)
                g_rows = wq[:, 1].reshape(nh * hd, dim)
                wk = st.get(a + "k_proj.weight")
                wv = st.get(a + "v_proj.weight")
                assert wk.shape == (nkv * hd, dim) and wv.shape == (nkv * hd, dim)
                write_q8(out, [q_rows, g_rows, wk, wv])
                del wq, q_rows, g_rows, wk, wv
                assert st.shape(a + "o_proj.weight") == (dim, nh * hd)
                write_q8(out, [(st, a + "o_proj.weight")])
                kind = "full"
            else:
                a = p + "linear_attn."
                A = -np.exp(st.get(a + "A_log").astype(np.float64)).astype(np.float32)
                assert A.shape == (nv,)
                write_f32(out, A)
                write_f32(out, st.get(a + "dt_bias"))
                cw = st.get(a + "conv1d.weight")
                assert cw.shape == (conv_dim, 1, conv_k), cw.shape
                assert a + "conv1d.bias" not in st
                write_f32(out, cw.reshape(conv_dim, conv_k))
                write_f32(out, st.get(a + "norm.weight"))
                shapes = [(conv_dim, dim), (nv * dv, dim), (nv, dim), (nv, dim)]
                names = ["in_proj_qkv", "in_proj_z", "in_proj_b", "in_proj_a"]
                for n_, s_ in zip(names, shapes):
                    assert st.shape(a + n_ + ".weight") == s_, (n_, st.shape(a + n_ + ".weight"))
                write_q8(out, [(st, a + n_ + ".weight") for n_ in names])
                assert st.shape(a + "out_proj.weight") == (dim, nv * dv)
                write_q8(out, [(st, a + "out_proj.weight")])
                kind = "linear"
            m = p + "mlp."
            write_q8(out, [(st, m + "gate_proj.weight"), (st, m + "up_proj.weight")])
            assert st.shape(m + "down_proj.weight") == (dim, inter)
            write_q8(out, [(st, m + "down_proj.weight")])
            end = pad64(out)
            sz = end - start
            assert sizes.setdefault(kind, sz) == sz, f"layer {l} {kind} size {sz} != {sizes[kind]}"
            if (l + 1) % 4 == 0 or l == n_layers - 1:
                print(f"  layer {l + 1}/{n_layers} ({time.time() - t0:.0f}s)", flush=True)

        off_fnorm = pad64(out)
        out.write((st.get(P + "norm.weight") + 1.0).astype("<f4").tobytes())
        file_size = pad64(out)

        out.seek(off_table)
        out.write(np.array(layer_offs, dtype="<u8").tobytes())

        flags = FLAG_QK_NORM | FLAG_FP16_SCALES | FLAG_ATTN_OUT_GATE | (FLAG_TIED_EMB if tied else 0)
        vals = dict(
            magic=MAGIC, version=VERSION, arch=ARCH_QWEN35, flags=flags, dim=dim, hidden_dim=inter,
            n_layers=n_layers, vocab_size=vocab, n_heads=nh, n_kv_heads=nkv, head_dim=hd,
            rot_dims=rot_dims, rope_theta=float(theta), rms_eps=float(eps), ht_mask=HT_MASK,
            max_seq_len=max_seq_len, lin_nk=nk, lin_nv=nv, lin_dk=dk, lin_dv=dv, conv_k=conv_k,
            n_full_layers=n_full, layer_bitmap=bitmap, off_tokenizer=off_tok, off_embed=off_embed,
            off_lm_head=off_lm, off_final_norm=off_fnorm, off_layer_table=off_table,
            file_size=file_size, layer_bytes_full=sizes.get("full", 0),
            layer_bytes_linear=sizes.get("linear", 0), **sids,
        )
        args = []
        for name, f in HEADER_FIELDS:
            if name == "eos":
                args.extend(eos)
            else:
                args.append(vals[name])
        hdr = struct.pack(HEADER_FMT, *args)
        out.seek(0)
        out.write(hdr + b"\x00" * (HEADER_SIZE - len(hdr)))

    final = file_size
    if pad_pow2:
        final = 1 << (file_size - 1).bit_length()
        os.truncate(out_path, final)  # sparse; ivshmem BAR size must be a power of two

    print(f"\nExport complete: {out_path}")
    print(f"  data size  : {file_size:,} B ({file_size / 2**30:.2f} GiB), file size {final:,} B")
    print(f"  layer bytes: full={sizes.get('full', 0):,} linear={sizes.get('linear', 0):,}")
    print(f"  offsets    : tok={off_tok} embed={off_embed} lm_head={off_lm} table={off_table} fnorm={off_fnorm}")
    print(f"  special    : {sids}")
    print(f"  elapsed    : {time.time() - t0:.1f}s")


def main():
    ap = argparse.ArgumentParser(description="Export Qwen3.5/3.8 hybrid HF checkpoint to .asmbin v3 (Q8_0)")
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-seq-len", type=int, default=16384)
    ap.add_argument("--pad-pow2", action="store_true", help="sparse-pad file to a power of two (ivshmem BAR)")
    a = ap.parse_args()
    export(a.model_dir, a.out, a.max_seq_len, a.pad_pow2)


if __name__ == "__main__":
    main()
