#!/bin/sh
# kernel_link.sh — link the fork's Phase 2 and Phase 4 kernels, unmodified,
# with and without the Glyph objects, using the fork's own NASM flags,
# linker scripts and ld flags (copied from the root Makefile at 6c43dec).
#
# Nothing in the fork is changed and no call site is added: the Glyph entry
# points are kept with `ld -u`, so this proves that the core + adapter link
# into the real kernel image (symbols, relocations, sections, Multiboot
# header placement). It does NOT boot anything.
#
# usage: tests/kernel_link.sh BUILD_DIR FORK_DIR
set -eu
B=${1:?build dir}; F=${2:?fork dir}
O="$B/kernel"
mkdir -p "$O/p2" "$O/p4"
KEEP="-u glyph_shell_run -u glyph_shell_check -u glyph_shell_transcript"
GLYPH="$B/glyph_core.o $B/bm/glyph_bm.o $B/bm/glyph_bm_entry.o"

asm() { nasm "$@"; }

# ---- Phase 2 (02-baremetal-os) -------------------------------------------
A2="-f elf64 -DBARE_METAL -DFAST_Q8=1 -I$F/shared/ -I$F/02-baremetal-os/"
for m in boot drivers runtime smp kernel; do asm $A2 "$F/02-baremetal-os/$m.asm" -o "$O/p2/$m.o"; done
for m in math matmul tokenizer transformer; do asm $A2 "$F/shared/$m.asm" -o "$O/p2/$m.o"; done
P2OBJS="$O/p2/boot.o $O/p2/drivers.o $O/p2/runtime.o $O/p2/smp.o $O/p2/kernel.o $O/p2/math.o $O/p2/matmul.o $O/p2/tokenizer.o $O/p2/transformer.o"
L2="-m elf_x86_64 -T $F/02-baremetal-os/linker.ld --gc-sections -s -z max-page-size=0x1000 -z noexecstack"
ld $L2 $P2OBJS -o "$O/p2-base.elf64"
ld $L2 $KEEP $P2OBJS $GLYPH -o "$O/p2-glyph.elf64"

# ---- Phase 4 (04-qwen38-27b-os) ---------------------------------------------
A4="-f elf64 -DBARE_METAL -DFAST_Q8=1 -DQ8_MAX_K=32768 -I$F/shared/ -I$F/04-qwen38-27b-os/"
for m in boot drivers runtime smp paging pci kernel; do asm $A4 "$F/04-qwen38-27b-os/$m.asm" -o "$O/p4/$m.o"; done
for m in math matmul tokenizer transformer qwen35; do asm $A4 "$F/shared/$m.asm" -o "$O/p4/$m.o"; done
P4OBJS="$O/p4/boot.o $O/p4/drivers.o $O/p4/runtime.o $O/p4/smp.o $O/p4/paging.o $O/p4/pci.o $O/p4/math.o $O/p4/matmul.o $O/p4/tokenizer.o $O/p4/transformer.o $O/p4/qwen35.o $O/p4/kernel.o"
L4="-m elf_x86_64 -T $F/04-qwen38-27b-os/linker.ld --gc-sections -s -z max-page-size=0x1000 -z noexecstack"
ld $L4 $P4OBJS -o "$O/p4-base.elf64"
ld $L4 $KEEP $P4OBJS $GLYPH -o "$O/p4-glyph.elf64"

# ---- checks ---------------------------------------------------------------------
fail=0
for k in p2-base p2-glyph p4-base p4-glyph; do
    objcopy -O elf32-i386 "$O/$k.elf64" "$O/$k.elf"
    size=$(stat -c %s "$O/$k.elf")
    # Multiboot 1: magic 0x1BADB002 must sit 4-byte aligned in the first 8 KiB
    off=$(od -A d -t x4 -w4 -v -N 8192 "$O/$k.elf" | awk '$2=="1badb002"{print $1+0; exit}')
    text=$(size -A "$O/$k.elf64" | awk '$1==".text"{print $2}')
    bss=$(size -A "$O/$k.elf64" | awk '$1==".bss"{print $2}')
    printf '%-9s file=%6s bytes  .text=%6s  .bss=%8s  multiboot_magic_offset=%s\n' "$k" "$size" "$text" "$bss" "${off:-MISSING}"
    if [ -z "$off" ] || [ "$off" -ge 8192 ] || [ $((off % 4)) -ne 0 ]; then fail=1; fi
done
# Symbols are stripped (-s); confirm the Glyph code is present by its strings.
for p in p2 p4; do
    if grep -q "session faulted during start" "$O/$p-glyph.elf"; then echo "$p: Glyph core present in kernel image"; else echo "$p: Glyph core MISSING"; fail=1; fi
    if grep -q "session faulted during start" "$O/$p-base.elf"; then echo "$p: baseline unexpectedly contains Glyph"; fail=1; fi
done
[ $fail -eq 0 ] && echo "OK: Phase 2 and Phase 4 kernels link with Glyph; Multiboot headers intact"
exit $fail
