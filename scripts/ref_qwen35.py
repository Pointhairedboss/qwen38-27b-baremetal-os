#!/usr/bin/env python3
"""
Independent NumPy reference for .asmbin v3 (Qwen3.5 / Qwen3.8 hybrid:
Gated DeltaNet + gated full attention), reading the .asmbin itself.

Two tiers of verification:
  --act f32 : FP32 activations x dequantized Q8_0 weights. Compare against
              HF transformers FP32 (--compare-hf DIR): top-1 / cosine / greedy prefix.
  --act q8  : emulates FAST_Q8=1 matmul.asm exactly at the quantization level
              (per-32 int8 activation blocks, scale = max|x| * (1/127f),
              q = rint(x * (127f / max|x|)) clamped to [-127, 127]; integer block
              dots are exact). This is the target the asm implementation must hit.

Usage:
  ref_qwen35.py --model models/qwen35-0.8b-q8_0.asmbin --prompt "Hi" --compare-hf models/hf/qwen35-0.8b
  ref_qwen35.py --model M --tokens 248045,846,198,... --max-tokens 64 --act q8 --print-tokens
"""

import argparse
import math
import mmap
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_qwen35 import HEADER_FIELDS, HEADER_FMT  # noqa: E402

FIB_MULT = 0x9E3779B97F4A7C15
HT_BITS = 19
F32 = np.float32


def align64(n):
    return (n + 63) & ~63


class Q8Mat:
    """Q8_0 matrix view [M, K] (int8 + fp16 per-32 scales); dequant caches built lazily."""

    def __init__(self, buf, off, M, K):
        self.M, self.K, self.nb = M, K, K // 32
        stride = K + self.nb * 2
        raw = np.frombuffer(buf, dtype=np.uint8, count=M * stride, offset=off).reshape(M, stride)
        self.q = raw[:, :K].view(np.int8)
        self.s = np.ascontiguousarray(raw[:, K:]).view("<f2").reshape(M, self.nb).astype(F32)
        self.nbytes = M * stride
        self._deq = None
        self._qf = None

    def row_f32(self, r):
        return (self.q[r].reshape(-1, 32).astype(F32) * self.s[r][:, None]).reshape(-1)

    def matvec(self, x, mode, cache=True):
        if mode == "f32":
            W = self._deq
            if W is None:
                W = (self.q.reshape(self.M, self.nb, 32).astype(F32) * self.s[:, :, None]).reshape(self.M, self.K)
                if cache:
                    self._deq = W
            return (W @ x.astype(F32)).astype(F32)
        # q8 activation emulation
        xq, xs = quantize_act(x)
        Wq = self._qf
        if Wq is None:
            Wq = np.ascontiguousarray(self.q.reshape(self.M, self.nb, 32).transpose(1, 0, 2)).astype(F32)
            if cache:
                self._qf = Wq
        # exact integer per-block dots (|sum| <= 32*127*127 < 2^24 so f32 is exact)
        dots = np.matmul(Wq, xq.reshape(self.nb, 32, 1).astype(F32))[:, :, 0].T  # [M, nb]
        sc = (self.s * xs[None, :]).astype(F32)  # fp32 (scale_w * scale_x) as in asm
        return np.sum(dots.astype(np.float64) * sc, axis=1).astype(F32)


def quantize_act(x):
    xb = x.astype(F32).reshape(-1, 32)
    amax = np.max(np.abs(xb), axis=1).astype(F32)
    xs = (amax * F32(0.007874015748031496)).astype(F32)
    with np.errstate(divide="ignore"):
        inv = np.where(amax > 0, F32(127.0) / np.where(amax > 0, amax, F32(1)), F32(0)).astype(F32)
    q = np.clip(np.rint((xb * inv[:, None]).astype(F32)), -127, 127).astype(np.int8)
    return q.reshape(-1), xs


class AsmBin35:
    def __init__(self, path):
        self._f = open(path, "rb")
        self._mm = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        self.buf = memoryview(self._mm)
        vals = struct.unpack_from(HEADER_FMT, self.buf, 0)
        h, i = {}, 0
        for name, f in HEADER_FIELDS:
            if name == "eos":
                h[name] = [v for v in vals[i : i + 4] if v != 0xFFFFFFFF]
                i += 4
            else:
                h[name] = vals[i]
                i += 1
        assert h["magic"] == b"ASML" and h["version"] == 3 and h["arch"] == 1, (h["magic"], h["version"])
        self.h = h
        for k, v in h.items():
            setattr(self, k, v)
        self.dump = None
        self._load_tokenizer()
        self._load_weights()

    # ---------------- tokenizer (same algorithm as shared/tokenizer.asm) ----------------
    def _load_tokenizer(self):
        base = self.off_tokenizer
        o_b2t, o_offs, o_bytes, o_ht = struct.unpack_from("<QQQQ", self.buf, base)
        self.byte_to_tok = np.frombuffer(self.buf, "<u4", 256, base + o_b2t)
        self.tok_offsets = np.frombuffer(self.buf, "<u4", self.vocab_size + 1, base + o_offs)
        self.tok_bytes = bytes(self.buf[base + o_bytes : base + o_bytes + int(self.tok_offsets[-1])])
        dt = np.dtype([("key", "<u8"), ("merged_id", "<u4"), ("rank", "<u4")])
        ht = np.frombuffer(self.buf, dt, self.ht_mask + 1, base + o_ht)
        self.ht_keys, self.ht_merged, self.ht_rank = ht["key"], ht["merged_id"], ht["rank"]

    def ht_lookup(self, a, b):
        key = (a << 32) | b
        slot = ((key * FIB_MULT) & 0xFFFFFFFFFFFFFFFF) >> (64 - HT_BITS)
        while True:
            r = int(self.ht_rank[slot])
            if r == 0xFFFFFFFF:
                return None
            if int(self.ht_keys[slot]) == key:
                return int(self.ht_merged[slot]), r
            slot = (slot + 1) & self.ht_mask

    def encode_bpe(self, text):
        toks = [int(self.byte_to_tok[b]) for b in text.encode("utf-8")]
        while len(toks) > 1:
            best = (0xFFFFFFFF, -1, -1)
            for i in range(len(toks) - 1):
                r = self.ht_lookup(toks[i], toks[i + 1])
                if r is not None and r[1] < best[0]:
                    best = (r[1], i, r[0])
            if best[1] < 0:
                break
            toks[best[1]] = best[2]
            del toks[best[1] + 1]
        return toks

    def chat_prompt(self, user, think=True):
        ids = [self.id_im_start, self.id_user, self.id_nl] + self.encode_bpe(user)
        ids += [self.id_im_end, self.id_nl, self.id_im_start, self.id_assistant, self.id_nl, self.id_think]
        ids += [self.id_nl] if think else [self.id_nlnl, self.id_end_think, self.id_nlnl]
        return ids

    def decode(self, ids):
        out = bytearray()
        for t in ids:
            if 0 <= t < self.vocab_size:
                out += self.tok_bytes[int(self.tok_offsets[t]) : int(self.tok_offsets[t + 1])]
        return out.decode("utf-8", errors="replace")

    # ---------------- weights ----------------
    def _load_weights(self):
        D, I = self.dim, self.hidden_dim
        nh, nkv, hd = self.n_heads, self.n_kv_heads, self.head_dim
        nk, nv, dk, dv, ck = self.lin_nk, self.lin_nv, self.lin_dk, self.lin_dv, self.conv_k
        self.conv_dim = 2 * nk * dk + nv * dv
        self.embed = Q8Mat(self.buf, self.off_embed, self.vocab_size, D)
        self.lm_head = Q8Mat(self.buf, self.off_lm_head, self.vocab_size, D)
        self.final_norm = np.frombuffer(self.buf, "<f4", D, self.off_final_norm)
        table = np.frombuffer(self.buf, "<u8", self.n_layers, self.off_layer_table)
        self.layers = []
        for l in range(self.n_layers):
            pos = int(table[l])
            start = pos

            def f32(n):
                nonlocal pos
                pos = align64(pos)
                a = np.frombuffer(self.buf, "<f4", n, pos)
                pos += 4 * n
                return a

            def q8(M, K):
                nonlocal pos
                pos = align64(pos)
                m = Q8Mat(self.buf, pos, M, K)
                pos += m.nbytes
                return m

            L = {"full": bool(self.layer_bitmap >> l & 1), "in_norm": f32(D), "post_norm": f32(D)}
            if L["full"]:
                L["q_norm"] = f32(hd)
                L["k_norm"] = f32(hd)
                L["wqkv"] = q8(2 * nh * hd + 2 * nkv * hd, D)
                L["wo"] = q8(D, nh * hd)
            else:
                L["A"] = f32(nv)
                L["dt_bias"] = f32(nv)
                L["conv_w"] = f32(self.conv_dim * ck).reshape(self.conv_dim, ck)
                L["lin_norm"] = f32(dv)
                L["win"] = q8(self.conv_dim + nv * dv + 2 * nv, D)
                L["wout"] = q8(D, nv * dv)
            L["wgu"] = q8(2 * I, D)
            L["wdown"] = q8(D, I)
            size = align64(pos) - start
            exp = self.layer_bytes_full if L["full"] else self.layer_bytes_linear
            assert size == exp, f"layer {l}: parsed {size} != header {exp}"
            self.layers.append(L)

    # ---------------- math ----------------
    def rmsnorm(self, x, w):
        ss = np.mean(x * x, axis=-1, keepdims=True, dtype=F32)
        return (x * (F32(1.0) / np.sqrt(ss + F32(self.rms_eps))) * w).astype(F32)

    @staticmethod
    def sigmoid(x):
        return (F32(1.0) / (F32(1.0) + np.exp(-x.astype(F32)))).astype(F32)

    @staticmethod
    def silu(x):
        e = np.exp(-np.abs(x)).astype(F32)
        return (x * np.where(x < 0, e, F32(1.0)) / (F32(1.0) + e)).astype(F32)

    @staticmethod
    def softplus(x):
        return np.where(x > 20, x, np.log1p(np.exp(np.minimum(x, 20)))).astype(F32)

    def new_state(self, max_len):
        nkv, hd = self.n_kv_heads, self.head_dim
        st = {"k": {}, "v": {}, "conv": {}, "S": {}}
        for l, L in enumerate(self.layers):
            if L["full"]:
                st["k"][l] = np.zeros((max_len, nkv, hd), F32)
                st["v"][l] = np.zeros((max_len, nkv, hd), F32)
            else:
                st["conv"][l] = np.zeros((self.conv_k - 1, self.conv_dim), F32)
                st["S"][l] = np.zeros((self.lin_nv, self.lin_dk, self.lin_dv), F32)
        return st

    def forward(self, tok, pos, st, mode="f32", logits=True):
        D = self.dim
        nh, nkv, hd, rot = self.n_heads, self.n_kv_heads, self.head_dim, self.rot_dims
        nk, nv, dk, dv = self.lin_nk, self.lin_nv, self.lin_dk, self.lin_dv
        x = self.embed.row_f32(tok)

        half = rot // 2
        inv_freq = 1.0 / (self.rope_theta ** (np.arange(0, rot, 2, dtype=np.float64) / rot))
        ang = pos * inv_freq
        cos, sin = np.cos(ang).astype(F32), np.sin(ang).astype(F32)

        def rope(t):  # t [H, hd]; NeoX rotate_half over the first `rot` dims
            a, b = t[:, :half], t[:, half:rot]
            return np.concatenate([a * cos - b * sin, b * cos + a * sin, t[:, rot:]], axis=1).astype(F32)

        for l, L in enumerate(self.layers):
            h = self.rmsnorm(x, L["in_norm"])
            if L["full"]:
                o = L["wqkv"].matvec(h, mode)
                q = o[: nh * hd].reshape(nh, hd)
                gate = o[nh * hd : 2 * nh * hd]
                k = o[2 * nh * hd : 2 * nh * hd + nkv * hd].reshape(nkv, hd)
                v = o[2 * nh * hd + nkv * hd :].reshape(nkv, hd)
                q = rope(self.rmsnorm(q, L["q_norm"]))
                k = rope(self.rmsnorm(k, L["k_norm"]))
                st["k"][l][pos] = k
                st["v"][l][pos] = v
                grp = nh // nkv
                att = np.zeros((nh, hd), F32)
                sc = F32(1.0 / math.sqrt(hd))
                for hh in range(nh):
                    kh = hh // grp
                    s = (st["k"][l][: pos + 1, kh] @ q[hh]) * sc
                    p = np.exp(s - s.max()).astype(F32)
                    p /= p.sum(dtype=F32)
                    att[hh] = p @ st["v"][l][: pos + 1, kh]
                att = att.reshape(-1) * self.sigmoid(gate)
                x = (x + L["wo"].matvec(att, mode)).astype(F32)
            else:
                cd = self.conv_dim
                o = L["win"].matvec(h, mode)
                qkv = o[:cd]
                z = o[cd : cd + nv * dv].reshape(nv, dv)
                b = o[cd + nv * dv : cd + nv * dv + nv]
                a = o[cd + nv * dv + nv :]
                cs = st["conv"][l]
                win = np.concatenate([cs, qkv[None, :]], axis=0)  # [ck, cd], oldest first
                conv = np.einsum("tc,ct->c", win, L["conv_w"]).astype(F32)
                cs[:] = win[1:]
                conv = self.silu(conv)
                q = conv[: nk * dk].reshape(nk, dk)
                k = conv[nk * dk : 2 * nk * dk].reshape(nk, dk)
                v = conv[2 * nk * dk :].reshape(nv, dv)
                q = q / np.sqrt(np.sum(q * q, axis=1, keepdims=True, dtype=F32) + F32(1e-6))
                k = k / np.sqrt(np.sum(k * k, axis=1, keepdims=True, dtype=F32) + F32(1e-6))
                q = (q * F32(1.0 / math.sqrt(dk))).astype(F32)
                beta = self.sigmoid(b)
                g = (L["A"] * self.softplus(a + L["dt_bias"])).astype(F32)
                S = st["S"][l]
                r = nv // nk
                out = np.empty((nv, dv), F32)
                for hv in range(nv):
                    hk = hv // r
                    Sh = S[hv]
                    Sh *= np.exp(g[hv]).astype(F32)
                    kv_mem = k[hk] @ Sh
                    delta = (v[hv] - kv_mem) * beta[hv]
                    Sh += np.outer(k[hk], delta).astype(F32)
                    out[hv] = q[hk] @ Sh
                out = self.rmsnorm(out, L["lin_norm"]) * self.silu(z)
                x = (x + L["wout"].matvec(out.reshape(-1).astype(F32), mode)).astype(F32)
            h2 = self.rmsnorm(x, L["post_norm"])
            gu = L["wgu"].matvec(h2, mode)
            act = (self.silu(gu[: self.hidden_dim]) * gu[self.hidden_dim :]).astype(F32)
            x = (x + L["wdown"].matvec(act, mode)).astype(F32)
            if self.dump is not None:
                self.dump.append(x.copy())
        if not logits:
            return None
        x = self.rmsnorm(x, self.final_norm)
        return self.lm_head.matvec(x, mode, cache=self.vocab_size * D < 2**29)

    def generate(self, prompt, n, mode="f32", stop_eos=True):
        st = self.new_state(len(prompt) + n + 1)
        lg = None
        for p, t in enumerate(prompt):
            lg = self.forward(t, p, st, mode, logits=(p == len(prompt) - 1))
        first = lg
        gen = []
        for step in range(n):
            nt = int(np.argmax(lg))
            if stop_eos and nt in self.eos:
                break
            gen.append(nt)
            if step == n - 1:
                break
            lg = self.forward(nt, len(prompt) + step, st, mode)
        return first, gen


def metrics(a, b):
    a64, b64 = a.astype(np.float64), b.astype(np.float64)
    cos = float(a64 @ b64 / (np.linalg.norm(a64) * np.linalg.norm(b64)))
    return float(np.max(np.abs(a64 - b64))), cos


def prefix_len(a, b):
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


def run_hf(hf_dir, ids, n, eos):
    import torch
    from transformers import AutoModelForCausalLM

    torch.set_grad_enabled(False)
    m = AutoModelForCausalLM.from_pretrained(hf_dir, dtype=torch.float32)
    m.eval()
    inp = torch.tensor([ids])
    lg = m(inp).logits[0, -1].float().numpy()
    out = m.generate(inp, max_new_tokens=n, do_sample=False, eos_token_id=list(eos), pad_token_id=eos[0])
    gen = []
    for t in out[0, len(ids) :].tolist():
        if t in eos:
            break
        gen.append(t)
    return lg, gen


def main():
    ap = argparse.ArgumentParser(description="NumPy reference for .asmbin v3 (Qwen3.5 hybrid)")
    ap.add_argument("--model", required=True)
    ap.add_argument("--prompt", default="Explain how a CPU cache works in 3 concise sentences.")
    ap.add_argument("--tokens", default=None, help="comma-separated prompt token IDs (overrides --prompt)")
    ap.add_argument("--max-tokens", type=int, default=32)
    ap.add_argument("--think", type=int, default=1)
    ap.add_argument("--act", choices=["f32", "q8", "both"], default="f32")
    ap.add_argument("--compare-hf", default=None, help="HF model dir: FP32 transformers oracle")
    ap.add_argument("--dump-logits", default=None, help="write first-step logits (f32) here")
    ap.add_argument("--print-tokens", action="store_true")
    ap.add_argument("--dump-hidden", default=None,
                    help="prefill the prompt (first --act mode) and write x after every layer, f32 [pos][layer][dim]")
    a = ap.parse_args()

    m = AsmBin35(a.model)
    print(
        f"asmbin v3: dim={m.dim} L={m.n_layers} full={m.n_full_layers} bitmap=0x{m.layer_bitmap:x} "
        f"vocab={m.vocab_size} eos={m.eos} tied={bool(m.flags & 1)}"
    )
    if a.tokens:
        ids = [int(t) for t in a.tokens.split(",") if t.strip()]
    else:
        ids = m.chat_prompt(a.prompt, think=bool(a.think))
    print(f"prompt ({len(ids)} tok): {ids}")

    if a.compare_hf and not a.tokens:
        from transformers import AutoTokenizer

        tok = AutoTokenizer.from_pretrained(a.compare_hf)
        s = tok.apply_chat_template(
            [{"role": "user", "content": a.prompt}], tokenize=False, add_generation_prompt=True,
            enable_thinking=bool(a.think),
        )
        hf_ids = tok.encode(s, add_special_tokens=False)
        print(f"[tokenizer vs HF] exact={hf_ids == ids}")
        if hf_ids != ids:
            print(f"  HF : {hf_ids}\n  ASM: {ids}")

    modes = ["f32", "q8"] if a.act == "both" else [a.act]
    if a.dump_hidden:
        st = m.new_state(len(ids) + 1)
        m.dump = []
        for p, t in enumerate(ids):
            m.forward(t, p, st, modes[0], logits=False)
        np.stack(m.dump).astype("<f4").tofile(a.dump_hidden)
        m.dump = None
        print(f"dumped {len(ids)}x{m.n_layers} hidden states to {a.dump_hidden}")
    res = {}
    for mode in modes:
        t0 = time.time()
        lg, gen = m.generate(ids, a.max_tokens, mode)
        dt = time.time() - t0
        res[mode] = (lg, gen)
        print(f"\n[ref act={mode}] {dt:.1f}s, {len(gen)} tok")
        print(f"  top5: {np.argsort(lg)[-5:][::-1].tolist()} {[round(float(v), 4) for v in np.sort(lg)[-5:][::-1]]}")
        print(f"  text: {m.decode(gen)!r}")
        if a.print_tokens:
            print(f"GEN_TOKENS_{mode.upper()}:" + ",".join(map(str, gen)))
    if len(modes) == 2:
        (l1, g1), (l2, g2) = res["f32"], res["q8"]
        mx, cos = metrics(l1, l2)
        print(f"\n[f32 vs q8] max_abs={mx:.5f} cos={cos:.7f} prefix={prefix_len(g1, g2)}/{min(len(g1), len(g2))}")
    if a.dump_logits:
        res[modes[0]][0].astype("<f4").tofile(a.dump_logits)

    if a.compare_hf:
        t0 = time.time()
        hl, hg = run_hf(a.compare_hf, ids, a.max_tokens, m.eos)
        print(f"\n[HF transformers fp32] {time.time() - t0:.1f}s")
        print(f"  top5: {np.argsort(hl)[-5:][::-1].tolist()} {[round(float(v), 4) for v in np.sort(hl)[-5:][::-1]]}")
        for mode, (lg, gen) in res.items():
            mx, cos = metrics(lg, hl)
            print(
                f"  ref[{mode}] vs HF: max_abs={mx:.4f} cos={cos:.6f} top1={int(np.argmax(lg)) == int(np.argmax(hl))} "
                f"greedy_prefix={prefix_len(gen, hg)}/{min(len(gen), len(hg))}"
            )
        print(f"  HF text: {m.decode(hg)!r}")
        if a.print_tokens:
            print("GEN_TOKENS_HF:" + ",".join(map(str, hg)))


if __name__ == "__main__":
    main()
