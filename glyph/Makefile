# Glyph — build, test and portability checks.
#
#   make                  build/glyph (native Linux CLI)
#   make test             unit tests + conformance corpus
#   make sanitize         ASan+UBSan build, then unit tests + corpus
#   make valgrind         unit tests + corpus under valgrind memcheck
#   make fuzz             libFuzzer run (clang), FUZZ_SECONDS=60 by default
#   make freestanding     compile the core with kernel flags, prove no external symbols
#   make baremetal-link   link core + bare-metal adapter against the fork's Phase 2/4 drivers.asm
#   make kernel-link      full link of the fork's Phase 2/4 kernels with Glyph (no boot)
#   make check-all        everything above except fuzz
#
# Tools: cc (gcc or clang), GNU make, ld, nm. Optional: clang (fuzz),
# valgrind, nasm (baremetal-link).

CC       ?= cc
BUILD    := build
CORE_SRC := src/core/util.c src/core/lex.c src/core/parse.c src/core/exec.c \
            src/core/turtle.c src/core/ui.c src/core/runner.c src/core/script.c
HOST_SRC := src/host/main.c src/host/term.c
INC      := -Iinclude -Isrc/core

WARN     := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wno-sign-conversion
CFLAGS   ?= -O2 -g
HOSTDEFS := -D_POSIX_C_SOURCE=200809L

# Flags that match the bare-metal kernel environment (x86-64, linked at 1 MiB,
# no libc, no red zone, no SSE/x87 state touched, no stack protector, no PIC).
FREESTANDING := -std=c99 -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie \
                -mno-red-zone -mgeneral-regs-only -mcmodel=small \
                -fno-stack-protector -fno-asynchronous-unwind-tables \
                -fno-tree-loop-distribute-patterns -fno-common \
                -Wall -Wextra -Werror

FORK     := ..
P2_DIR   := $(FORK)/02-baremetal-os
P4_DIR   := $(FORK)/04-qwen38-27b-os

.PHONY: all test unit corpus sanitize valgrind fuzz freestanding baremetal-link kernel-link check-all clean reference screenshots glyph-os qemu-run qemu-test vm-screenshots

all: $(BUILD)/glyph

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/glyph: $(CORE_SRC) $(HOST_SRC) include/*.h src/core/*.h src/host/*.h | $(BUILD)
	$(CC) $(WARN) $(CFLAGS) $(HOSTDEFS) $(INC) $(CORE_SRC) $(HOST_SRC) -o $@

$(BUILD)/unit: $(CORE_SRC) tests/unit.c include/*.h src/core/*.h | $(BUILD)
	$(CC) $(WARN) $(CFLAGS) $(HOSTDEFS) $(INC) $(CORE_SRC) tests/unit.c -o $@

unit: $(BUILD)/unit
	./$(BUILD)/unit

corpus: $(BUILD)/glyph
	./$(BUILD)/glyph test tests/corpus
	./$(BUILD)/glyph test examples

test: unit corpus

# ---- sanitizers -------------------------------------------------------------------
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
$(BUILD)/san: | $(BUILD)
	mkdir -p $(BUILD)/san
$(BUILD)/san/glyph: $(CORE_SRC) $(HOST_SRC) include/*.h src/core/*.h src/host/*.h | $(BUILD)/san
	$(CC) $(WARN) -O1 -g $(SAN) $(HOSTDEFS) $(INC) $(CORE_SRC) $(HOST_SRC) -o $@
$(BUILD)/san/unit: $(CORE_SRC) tests/unit.c include/*.h src/core/*.h | $(BUILD)/san
	$(CC) $(WARN) -O1 -g $(SAN) $(HOSTDEFS) $(INC) $(CORE_SRC) tests/unit.c -o $@

sanitize: $(BUILD)/san/glyph $(BUILD)/san/unit
	./$(BUILD)/san/unit
	./$(BUILD)/san/glyph test tests/corpus
	./$(BUILD)/san/glyph test examples

valgrind: $(BUILD)/glyph $(BUILD)/unit
	valgrind -q --error-exitcode=1 --leak-check=full ./$(BUILD)/unit
	valgrind -q --error-exitcode=1 --leak-check=full ./$(BUILD)/glyph test tests/corpus
	valgrind -q --error-exitcode=1 --leak-check=full ./$(BUILD)/glyph test examples

# ---- fuzzing (clang libFuzzer) ---------------------------------------------------------
FUZZ_SECONDS ?= 60
$(BUILD)/fuzz: $(CORE_SRC) tests/fuzz.c include/*.h src/core/*.h | $(BUILD)
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(INC) $(CORE_SRC) tests/fuzz.c -o $@

fuzz: $(BUILD)/fuzz
	mkdir -p $(BUILD)/fuzz-corpus
	sh tests/fuzz_seeds.sh $(BUILD)/fuzz-corpus
	./$(BUILD)/fuzz -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -timeout=5 -rss_limit_mb=1024 \
		-print_final_stats=1 $(BUILD)/fuzz-corpus

# ---- portability checks -----------------------------------------------------------
# 1. The core compiles with kernel flags and needs no symbol from anywhere else.
freestanding: | $(BUILD)
	mkdir -p $(BUILD)/fs
	for f in $(CORE_SRC); do \
		$(CC) $(FREESTANDING) $(INC) -c $$f -o $(BUILD)/fs/$$(basename $$f .c).o || exit 1; done
	ld -r $(BUILD)/fs/*.o -o $(BUILD)/glyph_core.o
	@echo "undefined symbols in glyph_core.o (must be empty):"
	@nm -u $(BUILD)/glyph_core.o | tee $(BUILD)/fs/undefined.txt
	@test ! -s $(BUILD)/fs/undefined.txt && echo "OK: core is self-contained"
	@size $(BUILD)/glyph_core.o

# 2. Core + bare-metal adapter link against the fork's own driver objects.
#    Nothing in the fork is modified; its drivers.asm is assembled read-only.
baremetal-link: freestanding
	mkdir -p $(BUILD)/bm
	$(CC) $(FREESTANDING) $(INC) -c src/baremetal/glyph_bm.c -o $(BUILD)/bm/glyph_bm.o
	nasm -f elf64 -DBARE_METAL -I$(FORK)/shared/ -I$(P2_DIR)/ src/baremetal/glyph_bm_entry.asm -o $(BUILD)/bm/glyph_bm_entry.o
	for p in 02:$(P2_DIR) 04:$(P4_DIR); do \
		tag=$${p%%:*}; dir=$${p#*:}; \
		nasm -f elf64 -DBARE_METAL -DFAST_Q8=1 -I$(FORK)/shared/ -I$$dir/ $$dir/drivers.asm -o $(BUILD)/bm/drivers_$$tag.o || exit 1; \
		ld -r -z noexecstack $(BUILD)/glyph_core.o $(BUILD)/bm/glyph_bm.o $(BUILD)/bm/glyph_bm_entry.o $(BUILD)/bm/drivers_$$tag.o \
			-o $(BUILD)/bm/glyph_phase$$tag.o || exit 1; \
		echo "phase $$tag: unresolved after linking with drivers.asm (kernel provides these):"; \
		nm -u $(BUILD)/bm/glyph_phase$$tag.o | tee $(BUILD)/bm/undefined_$$tag.txt; \
		if grep -v -E ' (g_boot_tsc)$$' $(BUILD)/bm/undefined_$$tag.txt | grep -q .; then \
			echo "FAIL: unexpected unresolved symbols"; exit 1; fi; \
		echo "OK: phase $$tag driver symbols resolve"; \
	done

# 3. Full link of the fork's Phase 2 and Phase 4 kernels (unmodified sources,
#    their own linker scripts) with Glyph kept in; compares against a baseline.
kernel-link: baremetal-link
	sh tests/kernel_link.sh $(BUILD) $(FORK)

check-all: test sanitize valgrind freestanding baremetal-link kernel-link reference

clean:
	rm -rf $(BUILD)

# ---- independent reference (python3) ------------------------------------------
# Busy beavers, Rule 110, recursion and the sieve checked against Python.
reference: $(BUILD)/glyph
	python3 -I tests/reference/check_tc.py $(BUILD)/glyph

# ---- screenshots (Xvfb, xterm, xdotool, ImageMagick) ------------------------
screenshots: $(BUILD)/glyph
	tools/screenshots.sh docs/screenshots

# ---- bootable Glyph kernel (QEMU) ---------------------------------------------
# A standalone Multiboot kernel: the fork's unmodified Phase 2 boot.asm,
# drivers.asm, runtime.asm and linker.ld, plus Glyph and src/baremetal/glyph_os.c
# (kernel_main). Embeds the examples and the conformance corpus.
OSK          := $(BUILD)/os
GLYPH_OS     := $(BUILD)/glyph-os.elf
APP          ?= demo
QEMU_SYSTEM  ?= qemu-system-x86_64 -machine pc -cpu max -m 256M -no-reboot
QEMU_DISPLAY ?=

$(GLYPH_OS): freestanding src/baremetal/glyph_os.c src/baremetal/glyph_bm.c tools/mkpayload.sh \
             examples/*.glyph tests/corpus/*.glyph
	mkdir -p $(OSK)
	$(CC) $(FREESTANDING) $(INC) -c src/baremetal/glyph_bm.c -o $(OSK)/glyph_bm.o
	$(CC) $(FREESTANDING) $(INC) -c src/baremetal/glyph_os.c -o $(OSK)/glyph_os.o
	sh tools/mkpayload.sh $(OSK)/payload.asm
	nasm -f elf64 $(OSK)/payload.asm -o $(OSK)/payload.o
	for m in boot drivers runtime; do \
		nasm -f elf64 -DBARE_METAL -DFAST_Q8=1 -I$(FORK)/shared/ -I$(P2_DIR)/ $(P2_DIR)/$$m.asm -o $(OSK)/$$m.o || exit 1; done
	ld -m elf_x86_64 -T $(P2_DIR)/linker.ld --gc-sections -s -z max-page-size=0x1000 -z noexecstack \
		$(OSK)/boot.o $(OSK)/drivers.o $(OSK)/runtime.o $(BUILD)/glyph_core.o \
		$(OSK)/glyph_bm.o $(OSK)/glyph_os.o $(OSK)/payload.o -o $(GLYPH_OS).64
	objcopy -O elf32-i386 $(GLYPH_OS).64 $(GLYPH_OS)

glyph-os: $(GLYPH_OS)
	ls -la $(GLYPH_OS)

# Boot interactively: VGA window (or QEMU_DISPLAY='-display curses' in a terminal).
qemu-run: $(GLYPH_OS)
	$(QEMU_SYSTEM) $(QEMU_DISPLAY) -kernel $(GLYPH_OS) -append 'app=$(APP)'

# Boot with glyph=test and compare every transcript on COM1 with the host .expect.
qemu-test: $(GLYPH_OS)
	python3 -I tests/qemu_conformance.py $(GLYPH_OS)

# VGA screenshots through the emulated PS/2 keyboard (QEMU monitor sendkey/screendump).
vm-screenshots: $(GLYPH_OS)
	python3 -I tools/qemu_shots.py docs/screenshots
