# Phase 4: Bare-Metal Qwen3.8-27B OS (Qwen3.5 hybrid) on KVM

A standalone Multiboot 1 x86-64 kernel in pure assembly that runs **Qwen/Qwen3.8-27B** (`model_type: qwen3_5`) with no OS, no libc and no C. The same kernel also runs the Qwen3.5-0.8B and 4B dev models.

- **Hybrid forward pass** (`shared/qwen35.asm`):
  - 48 **Gated DeltaNet** layers: causal conv1d + SiLU, L2-normed q/k, a 128×128 fp32 delta-rule state per V head, and a gated RMSNorm.
  - 16 **gated full-attention** layers: fused q|gate|k|v, per-head QK-norm, partial NeoX RoPE (64/256 dims), and `·sigmoid(gate)`.
  - A SwiGLU MLP everywhere, an untied lm_head and a 248k vocabulary.
- **Model delivery: zero-copy via ivshmem.** The 28.6 GB Q8_0 `.asmbin` v3 is a read-only `memory-backend-file` on the host, exposed to the guest as a 64-bit PCI BAR (`ivshmem-plain`). The kernel maps it write-back with 1 GiB pages. The weights never occupy guest RAM; they are shared with the host page cache.
- **Guest RAM holds only state.** That is the KV cache for the 16 full-attention layers (128 KiB/token), the DeltaNet + conv state (~150 MiB) and scratch. `-m 8G` covers ctx=16384: that's a 2.2 GiB arena, and boot to ready takes 164 ms.
- **Byte-exact parity** with the Linux user-space build `build/asm-qwen35` (same `shared/` code) at 1–32 vCPUs. This covers prompt tokens, generated tokens and UTF-8 text, plus multi-turn REPL transcripts including `/clear`.

## What changed versus Phase 2 (`02-baremetal-os`)

| Limit in 02 | 04 |
|---|---|
| Identity map only covers 0–4 GiB | `paging.asm` maps every RAM region from the 64-bit Multiboot mmap plus the BAR (1 GiB pages when `pdpe1gb` is available, else 2 MiB) |
| Heap clamped to 0xF0000000 | Regions above 4 GiB are used whole |
| Model is a Multiboot module (32-bit, < 4 GiB) | `pci.asm` finds ivshmem (1AF4:1110) and maps 64-bit BAR2 |
| Qwen3 dense block, tied embeddings, full RoPE | Qwen3.5 hybrid; header-driven layer bitmap, dims and special IDs (`.asmbin` v3) |
| `Q8_MAX_K` = 16384 | `-DQ8_MAX_K=32768` (MLP down-proj K = 17408) |
| `max_tokens` unclamped | Clamped to `MAX_GEN_TOKENS` |
| `smp.asm` matmul/attention jobs | Generic `tp_run(fn, arg)` pool (`tp_matmul_q8_0`, `tp_gqa_attn`) |

DeltaNet state is recurrent, not position-indexed. Any sequence that restarts at position 0 (batch prompt, `/clear`, `/bench`, context full) therefore calls `qwen35_reset`.

## Build

```bash
sudo apt install nasm binutils qemu-system-x86      # + xorriso grub-efi-amd64-bin for the ISO
make os27            # build/asm-llm-os27.elf
make asm-qwen35      # Linux reference build (same shared code)
```

## Export the model

```bash
# needs numpy + safetensors (e.g. a venv: uv venv && uv pip install numpy safetensors)
# HF safetensors (bf16) -> .asmbin v3 Q8_0 with fp16 scales, sparse-padded to 2^n for ivshmem
.venv/bin/python -I scripts/export_qwen35.py --model-dir <Qwen3.8-27B dir> \
    --out models/qwen38-27b-q8_0.asmbin --pad-pow2
```

This writes 28.6 GB of data, padded sparsely to a 32 GiB file, in about 2.5 minutes. ivshmem needs the backing size to be a power of two. If an older export isn't padded, run `truncate -s 32G file` (it's sparse and the header records the real size).

## Run on QEMU/KVM

```bash
make os27-run MODEL27=models/qwen38-27b-q8_0.asmbin SMP27=16 \
     CMDLINE='prompt="Explain the Gated DeltaNet in two sentences." max_tokens=128'
# no prompt= -> interactive asm-llm> REPL on the serial console (-nographic)
```

This is equivalent to:

```bash
qemu-system-x86_64 -enable-kvm -machine q35 -cpu host -m 8G -smp 16 \
  -global q35-pcihost.pci-hole64-size=128G \
  -object memory-backend-file,id=mdl,mem-path=models/qwen38-27b-q8_0.asmbin,size=32G,share=on,readonly=on \
  -device ivshmem-plain,memdev=mdl \
  -kernel build/asm-llm-os27.elf -append 'max_tokens=256' -nographic
```

`-machine q35` and a large `pci-hole64-size` are required so the firmware can place a 32 GiB BAR.

**Kernel command line options** (same as 02):
- `prompt="..."`, `max_tokens=N`, `ctx=N` (default 4096)
- `threads=N`, `think=0|1`, `raw=1`, `temp=`, `topp=`, `seed=`
- `print_tokens=1`, `single_turn=1`, `shutdown=1`

**REPL commands:** `/think` `/nothink` `/clear` (resets the KV cache and DeltaNet/conv state) `/bench` `/help` `/halt`.

## Verify

```bash
python3 scripts/verify_os27.py --model models/qwen38-27b-q8_0.asmbin --smp 1,8,16,32
```

The script runs:
1. Batch parity between the OS and `build/asm-qwen35` at each vCPU count.
2. Multi-turn REPL parity against `asm-qwen35 --chat`: remember a name and number, recall them, `/clear`, then the recall must fail. `--chat-ctx 80 --chat-script 'a|b|c'` forces repeated context-full resets.
3. A performance table.

The Linux build itself is validated against an independent numpy reference (`scripts/ref_qwen35.py`, which reads the same `.asmbin`). The gate is teacher-forced top-1 agreement:

| Model | Steps | top-1 mismatches | Notes |
|---|---|---|---|
| 0.8B | 87 | 2 | near-ties |
| 4B | — | 1 | margin 0.018 |
| 27B | 47 | 0 | worst logits cos 0.9969 |

Exact equality isn't achievable because the int8 activation rounding amplifies ~1e-7 float differences. The numpy reference with f32 activations matches HF transformers greedy decoding 32/32 on 0.8B and 4B.

## Performance (Ryzen AI Max+ 395, 128 GB LPDDR5X, QEMU 10.2 KVM)

Qwen3.8-27B, `verify_os27.py`, ~19-token prompt plus 24 generated tokens, one fresh VM per run. The host page cache was warm, but guest EPT mappings start cold on every boot.

| vCPUs | boot ms | boot → 1st token | prefill tok/s | decode tok/s |
|---:|---:|---:|---:|---:|
| 1 | 62 | 41.6 s | 0.47 | 1.11 |
| 8 | 79 | 11.6 s | 1.69 | 2.07 |
| 16 | 77 | 6.6 s | 3.01 | 3.63 |
| 32 | 81 | 8.4 s | 2.36 | 2.25 |

Dev models on the same kernel: Qwen3.5-0.8B decodes at 70 tok/s on 8 vCPUs, and Qwen3.5-4B at 22 tok/s on 16 vCPUs.

Decode is memory-bandwidth bound: every token streams all 28.6 GB of weights. Guest-side BAR read bandwidth measured ~108 GB/s (M0), the same as guest RAM, so decode tops out near 4 tok/s. 16 vCPUs saturates bandwidth; 32 (SMT siblings) is slightly slower. The first pass after boot is slower while EPT entries for the BAR fault in. Prefill is still token-by-token (batched prefill is future work), so TTFT ≈ prompt_len / decode rate.

## Proxmox VE (untested sketch)

> The ISO + OVMF + ivshmem combination below has been tested on plain QEMU 10.2 (the BAR landed at 0x380000000000 under OVMF). The `qm` commands have **not** been run on a real Proxmox host, so adapt the storage names and paths to your setup.

The kernel-only ISO (`make os27-iso`) contains just GRUB and the 33 KB kernel. The model can't go in an ISO9660 image and always arrives through ivshmem.

```bash
# on the Proxmox host: model file on local storage, ISO uploaded to an ISO store
qm create 270 --name qwen27b --machine q35 --cpu host --cores 16 --memory 8192 --balloon 0 \
  --bios ovmf --efidisk0 local-lvm:0 --cdrom local:iso/asm-llm-os27.iso --serial0 socket --vga std
qm set 270 --args '-global q35-pcihost.pci-hole64-size=128G \
  -object memory-backend-file,id=mdl,mem-path=/var/lib/models/qwen38-27b-q8_0.asmbin,size=32G,share=on,readonly=on \
  -device ivshmem-plain,memdev=mdl'
qm start 270 && qm terminal 270
```

Notes:
- The ISO is UEFI-only unless `grub-pc-bin` is installed on the build host, in which case it's a BIOS+UEFI hybrid. Use `--bios ovmf` for the UEFI-only one.
- `--balloon 0`: the bump allocator assumes RAM never shrinks.
- The kernel command line is baked into `grub.cfg` (`make os27-iso CMDLINE='...'`). It can also be edited at the GRUB menu.
- With OVMF, make sure the 64-bit MMIO window is large enough. The `pci-hole64-size` global above does this; on older OVMF builds, also add `-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=131072`.
