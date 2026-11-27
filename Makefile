NASM    := nasm
LD      := ld
OBJCOPY := objcopy

LDFLAGS := --gc-sections -s -z noexecstack -z noseparate-code
BUILD   := build

# Portable defaults (override on CLI, e.g. make inference FAST_Q8=0)
FAST_Q8 ?= 1
MODEL   ?= models/qwen3-0.6b-q8_0.asmbin
DATA    ?= data/tinyshakespeare.txt
SMP     ?= 16
RAM     ?= 2G
CMDLINE ?=

# Stamp file tracks FAST_Q8 compile-time toggle so toggling rebuilds cleanly
FAST_Q8_STAMP := $(BUILD)/.fast_q8_$(FAST_Q8)

# --- Shared x86-64 NASM modules ---
SHARED_DIR := shared
AFLAGS     := -f elf64 -DFAST_Q8=$(FAST_Q8) -I$(SHARED_DIR)/

# --- Phase 1: Qwen3-0.6B Linux Inference Engine (12 KB static ELF) ---
P1_DIR  := 01-qwen3-inference
TARGET  := $(BUILD)/asm-llm
TEST    := $(BUILD)/test_math

P1_SHARED_OBJS := \
	$(BUILD)/syscalls.o \
	$(BUILD)/math.o \
	$(BUILD)/matmul.o \
	$(BUILD)/threads.o \
	$(BUILD)/tokenizer.o \
	$(BUILD)/transformer.o

P1_OBJS := $(P1_SHARED_OBJS) $(BUILD)/main.o

TEST_OBJS := \
	$(BUILD)/syscalls.o \
	$(BUILD)/math.o \
	$(BUILD)/matmul.o \
	$(BUILD)/test_math.o

# --- Phase 2: Bare-Metal Bootable LLM OS (22 KB Multiboot 1 kernel) ---
P2_DIR     := 02-baremetal-os
OS_BUILD   := $(BUILD)/os
OS_ELF64   := $(OS_BUILD)/asm-llm-os.elf64
OS_ELF     := $(BUILD)/asm-llm-os.elf
OS_ISO     := $(BUILD)/asm-llm-os.iso
OS_AFLAGS  := -f elf64 -DBARE_METAL -DFAST_Q8=$(FAST_Q8) -I$(SHARED_DIR)/ -I$(P2_DIR)/
OS_LDFLAGS := -m elf_x86_64 -T $(P2_DIR)/linker.ld --gc-sections -s -z max-page-size=0x1000 -z noexecstack

OS_OBJS := \
	$(OS_BUILD)/boot.o \
	$(OS_BUILD)/drivers.o \
	$(OS_BUILD)/runtime.o \
	$(OS_BUILD)/smp.o \
	$(OS_BUILD)/kernel.o \
	$(OS_BUILD)/math.o \
	$(OS_BUILD)/matmul.o \
	$(OS_BUILD)/tokenizer.o \
	$(OS_BUILD)/transformer.o

# Glyph language runtime (glyph/): one relocatable object, built by glyph/Makefile
# with freestanding flags; kernel.asm's REPL hands /glyph lines to it.
GLYPH_KOBJ := glyph/build/glyph-kernel.o
OS_OBJS += $(GLYPH_KOBJ)

# --- Phase 3: Pure x86-64 Mini-GPT Training & Inference Engine (32 KB ELF) ---
P3_DIR       := 03-minigpt-train
TRAIN_BUILD  := $(BUILD)/train
TRAIN_TARGET := $(BUILD)/asm-gpt-train
TRAIN_AFLAGS := -f elf64 -I$(SHARED_DIR)/ -I$(P3_DIR)/

TRAIN_OBJS := \
	$(BUILD)/syscalls.o \
	$(BUILD)/math.o \
	$(TRAIN_BUILD)/sgemm.o \
	$(TRAIN_BUILD)/kernels.o \
	$(TRAIN_BUILD)/model.o \
	$(TRAIN_BUILD)/tui.o \
	$(TRAIN_BUILD)/main.o

.PHONY: FORCE asm-qwen35 all inference test-math check os os-check os-iso os-run os27-bartest os27-bartest-run os27 os27-run os27-iso train train-check check-all clean

all: inference $(TEST) os train

# --- Phase 1 Targets ---
inference: $(TARGET)

test-math: $(TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(FAST_Q8_STAMP): | $(BUILD)
	rm -f $(BUILD)/.fast_q8_* && touch $@

$(BUILD)/%.o: $(SHARED_DIR)/%.asm $(SHARED_DIR)/constants.inc $(FAST_Q8_STAMP) | $(BUILD)
	$(NASM) $(AFLAGS) $< -o $@

$(BUILD)/main.o: $(P1_DIR)/main.asm $(SHARED_DIR)/constants.inc | $(BUILD)
	$(NASM) $(AFLAGS) $< -o $@

$(BUILD)/test_math.o: $(P1_DIR)/test_math.asm $(SHARED_DIR)/constants.inc $(FAST_Q8_STAMP) | $(BUILD)
	$(NASM) $(AFLAGS) $< -o $@

$(TARGET): $(P1_OBJS)
	$(LD) $(LDFLAGS) $(P1_OBJS) -o $@

$(TEST): $(TEST_OBJS)
	$(LD) $(LDFLAGS) $(TEST_OBJS) -o $@

check: $(TARGET)
	ls -la $(TARGET)
	file $(TARGET)
	readelf -d $(TARGET) || true
	wc -l $(SHARED_DIR)/*.asm $(SHARED_DIR)/*.inc $(P1_DIR)/*.asm

# --- Phase 2 Targets ---
$(OS_BUILD):
	mkdir -p $(OS_BUILD)

$(OS_BUILD)/%.o: $(P2_DIR)/%.asm $(P2_DIR)/os_constants.inc $(SHARED_DIR)/constants.inc $(FAST_Q8_STAMP) | $(OS_BUILD)
	$(NASM) $(OS_AFLAGS) $< -o $@

$(OS_BUILD)/math.o: $(SHARED_DIR)/math.asm | $(OS_BUILD)
	$(NASM) $(OS_AFLAGS) $< -o $@

$(OS_BUILD)/matmul.o: $(SHARED_DIR)/matmul.asm $(FAST_Q8_STAMP) | $(OS_BUILD)
	$(NASM) $(OS_AFLAGS) $< -o $@

$(OS_BUILD)/tokenizer.o: $(SHARED_DIR)/tokenizer.asm $(SHARED_DIR)/constants.inc | $(OS_BUILD)
	$(NASM) $(OS_AFLAGS) $< -o $@

$(OS_BUILD)/transformer.o: $(SHARED_DIR)/transformer.asm $(SHARED_DIR)/constants.inc $(FAST_Q8_STAMP) | $(OS_BUILD)
	$(NASM) $(OS_AFLAGS) $< -o $@

$(OS_ELF64): $(OS_OBJS) $(P2_DIR)/linker.ld
	$(LD) $(OS_LDFLAGS) $(OS_OBJS) -o $@

$(OS_ELF): $(OS_ELF64)
	$(OBJCOPY) -O elf32-i386 $< $@

$(GLYPH_KOBJ): FORCE
	$(MAKE) -C glyph kernel-obj

FORCE:

os: $(OS_ELF)

os-check: $(OS_ELF)
	ls -la $(OS_ELF)
	file $(OS_ELF)
	readelf -S $(OS_ELF)
	readelf -l $(OS_ELF)
	readelf -d $(OS_ELF) || true
	wc -l $(P2_DIR)/*.asm $(P2_DIR)/*.inc $(P2_DIR)/linker.ld

os-iso: $(OS_ELF)
	bash scripts/mkiso.sh "$(OS_ELF)" "$(MODEL)" "$(OS_ISO)" '$(CMDLINE)'

os-run: $(OS_ELF)
	qemu-system-x86_64 -enable-kvm -cpu host -m $(RAM) -smp $(SMP) \
		-kernel $(OS_ELF) -initrd "$(MODEL)" \
		-append '$(CMDLINE)' -nographic

# --- Qwen3.5/3.8 hybrid: Linux user-space driver (M2/M3 validation) ---
Q35_BUILD  := $(BUILD)/q35
Q35_TARGET := $(BUILD)/asm-qwen35
Q35_AFLAGS := -f elf64 -DFAST_Q8=$(FAST_Q8) -DQ8_MAX_K=32768 -I$(SHARED_DIR)/
Q35_OBJS := $(addprefix $(Q35_BUILD)/,syscalls.o math.o matmul.o threads.o tokenizer.o transformer.o qwen35.o linux_main.o)

$(Q35_BUILD):
	mkdir -p $(Q35_BUILD)

$(Q35_BUILD)/%.o: $(SHARED_DIR)/%.asm $(SHARED_DIR)/constants.inc $(SHARED_DIR)/qwen35.inc $(FAST_Q8_STAMP) | $(Q35_BUILD)
	$(NASM) $(Q35_AFLAGS) $< -o $@

$(Q35_BUILD)/linux_main.o: 04-qwen38-27b-os/linux_main.asm $(SHARED_DIR)/constants.inc $(SHARED_DIR)/qwen35.inc | $(Q35_BUILD)
	$(NASM) $(Q35_AFLAGS) $< -o $@

$(Q35_TARGET): $(Q35_OBJS)
	$(LD) $(LDFLAGS) $(Q35_OBJS) -o $@

asm-qwen35: $(Q35_TARGET)

# --- Phase 4: Qwen3.8-27B bare-metal OS (model via ivshmem 64-bit PCI BAR) ---
P4_DIR      := 04-qwen38-27b-os
OS27_BUILD  := $(BUILD)/os27
OS27_AFLAGS := -f elf64 -DBARE_METAL -DFAST_Q8=$(FAST_Q8) -DQ8_MAX_K=32768 -I$(SHARED_DIR)/ -I$(P4_DIR)/
OS27_LDFLAGS := -m elf_x86_64 -T $(P4_DIR)/linker.ld --gc-sections -s -z max-page-size=0x1000 -z noexecstack
MODEL27     ?= models/qwen38-27b-q8_0.asmbin
SMP27       ?= 32
RAM27       ?= 8G
BAR27       ?= $(shell f="$(MODEL27)"; [ -f "$$f" ] && stat -c %s "$$f")
HOLE64      ?= 128G

OS27_BASE_OBJS := $(OS27_BUILD)/boot.o $(OS27_BUILD)/drivers.o $(OS27_BUILD)/runtime.o \
	$(OS27_BUILD)/smp.o $(OS27_BUILD)/paging.o $(OS27_BUILD)/pci.o
BARTEST_ELF := $(BUILD)/asm-llm-os27-bartest.elf

OS27_QEMU = qemu-system-x86_64 -enable-kvm -machine q35 -cpu host -m $(RAM27) -smp $(SMP27) \
	-global q35-pcihost.pci-hole64-size=$(HOLE64) \
	-object memory-backend-file,id=mdl,mem-path=$(MODEL27),size=$(BAR27),share=on,readonly=on \
	-device ivshmem-plain,memdev=mdl -nographic

$(OS27_BUILD):
	mkdir -p $(OS27_BUILD)

$(OS27_BUILD)/%.o: $(P4_DIR)/%.asm $(P4_DIR)/os_constants.inc $(SHARED_DIR)/constants.inc | $(OS27_BUILD)
	$(NASM) $(OS27_AFLAGS) $< -o $@

$(BARTEST_ELF): $(OS27_BASE_OBJS) $(OS27_BUILD)/bartest.o $(P4_DIR)/linker.ld
	$(LD) $(OS27_LDFLAGS) $(OS27_BASE_OBJS) $(OS27_BUILD)/bartest.o -o $@.64
	$(OBJCOPY) -O elf32-i386 $@.64 $@

os27-bartest: $(BARTEST_ELF)

os27-bartest-run: $(BARTEST_ELF)
	$(OS27_QEMU) -kernel $(BARTEST_ELF) -append '$(CMDLINE)'

OS27_ELF  := $(BUILD)/asm-llm-os27.elf
OS27_ISO  := $(BUILD)/asm-llm-os27.iso
OS27_OBJS := $(OS27_BASE_OBJS) $(addprefix $(OS27_BUILD)/,math.o matmul.o tokenizer.o transformer.o qwen35.o kernel.o)

$(OS27_BUILD)/%.o: $(SHARED_DIR)/%.asm $(SHARED_DIR)/constants.inc $(SHARED_DIR)/qwen35.inc $(FAST_Q8_STAMP) | $(OS27_BUILD)
	$(NASM) $(OS27_AFLAGS) $< -o $@

$(OS27_BUILD)/kernel.o: $(SHARED_DIR)/qwen35.inc

$(OS27_ELF): $(OS27_OBJS) $(P4_DIR)/linker.ld
	$(LD) $(OS27_LDFLAGS) $(OS27_OBJS) -o $@.64
	$(OBJCOPY) -O elf32-i386 $@.64 $@

os27: $(OS27_ELF)

os27-run: $(OS27_ELF)
	$(OS27_QEMU) -kernel $(OS27_ELF) -append '$(CMDLINE)'

# Kernel-only ISO (the model never fits ISO9660; it always arrives via ivshmem)
os27-iso: $(OS27_ELF)
	ISO_TITLE="asm-llm OS v4 (Qwen3.5/3.8 hybrid, model via ivshmem)" \
		bash scripts/mkiso.sh "$(OS27_ELF)" "" "$(OS27_ISO)" '$(CMDLINE)'

# --- Phase 3 Targets ---
$(TRAIN_BUILD):
	mkdir -p $(TRAIN_BUILD)

$(TRAIN_BUILD)/%.o: $(P3_DIR)/%.asm $(P3_DIR)/train_constants.inc $(SHARED_DIR)/constants.inc | $(TRAIN_BUILD)
	$(NASM) $(TRAIN_AFLAGS) $< -o $@

$(TRAIN_TARGET): $(TRAIN_OBJS)
	$(LD) $(LDFLAGS) $(TRAIN_OBJS) -o $@

train: $(TRAIN_TARGET)

train-check: $(TRAIN_TARGET)
	ls -la $(TRAIN_TARGET)
	file $(TRAIN_TARGET)
	readelf -d $(TRAIN_TARGET) || true
	readelf -S $(TRAIN_TARGET)
	wc -l $(P3_DIR)/*.asm $(P3_DIR)/*.inc

check-all: check os-check train-check

clean:
	rm -rf $(BUILD)
