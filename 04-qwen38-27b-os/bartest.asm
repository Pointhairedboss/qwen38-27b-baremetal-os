; ============================================================================
; 04-qwen38-27b-os/bartest.asm — M0 gate: ivshmem BAR vs guest RAM bandwidth
;
; Boots (Multiboot 1, no module), maps all RAM above 4 GiB, brings up SMP,
; finds the ivshmem-plain BAR (host .asmbin file), prints its address, size
; and first dword, then measures sequential AVX2 read bandwidth:
;   BAR  1-core cold (first touch -> EPT faults), 1-core warm, N-core warm
;   RAM  N-core fill (fault-in), 1-core warm, N-core warm
; Kernel cmdline: gib=<GiB to read, default 4> threads=<N, default all>
; ============================================================================

bits 64
default rel

%include "os_constants.inc"

extern g_mb_magic
extern g_mb_info
extern g_has_avx2_fma
extern g_has_1g_pages
extern g_mapped_top
extern serial_init
extern vga_clear
extern timer_init
extern timer_ns
extern qemu_shutdown
extern paging_init
extern smp_init
extern tp_run
extern tp_shutdown
extern pci_find_ivshmem
extern parse_u64
extern write_cstr
extern write_u64
extern write_f64_2dp
extern write_hex64_raw

section .text
global kernel_main

%macro PRINT 1
    mov     edi, 1
    lea     rsi, [rel %1]
    call    write_cstr
%endmacro
%macro PRINTU 1
    mov     edi, 1
    mov     rsi, %1
    call    write_u64
%endmacro
%macro PRINTH 1
    mov     rdi, %1
    call    write_hex64_raw
%endmacro

kernel_main:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8

    call    serial_init
    mov     dil, VGA_ATTR_GREY
    call    vga_clear
    cmp     dword [rel g_has_avx2_fma], 0
    je      .halt
    call    timer_init
    cmp     dword [rel g_mb_magic], MB_BOOTLOADER_MAGIC
    jne     .halt
    mov     r12d, [rel g_mb_info]

    PRINT   msg_banner

    ; --- cmdline: gib=, threads= ---
    mov     qword [rel opt_gib], 4
    mov     qword [rel opt_threads], 0
    mov     eax, [r12 + MBI_FLAGS]
    test    eax, 0x04
    jz      .no_cmd
    mov     edi, [r12 + MBI_CMDLINE]
    lea     rsi, [rel key_gib]
    call    find_key
    test    rax, rax
    jz      .no_gib
    mov     rdi, rax
    call    parse_u64
    test    rax, rax
    jz      .no_gib
    mov     [rel opt_gib], rax
.no_gib:
    mov     edi, [r12 + MBI_CMDLINE]
    lea     rsi, [rel key_threads]
    call    find_key
    test    rax, rax
    jz      .no_cmd
    mov     rdi, rax
    call    parse_u64
    mov     [rel opt_threads], rax
.no_cmd:

    ; --- paging above 4 GiB ---
    mov     rdi, r12
    call    paging_init
    PRINT   msg_1g
    mov     eax, [rel g_has_1g_pages]
    PRINTU  rax
    PRINT   msg_top
    PRINTH  [rel g_mapped_top]
    PRINT   msg_nl

    ; --- SMP ---
    mov     rdi, [rel opt_threads]
    call    smp_init
    mov     [rel n_cores], rax
    PRINT   msg_cores
    PRINTU  [rel n_cores]
    PRINT   msg_nl

    ; --- ivshmem ---
    lea     rdi, [rel bar_size]
    call    pci_find_ivshmem
    test    rax, rax
    jz      .no_bar
    mov     [rel bar_base], rax
    PRINT   msg_bar
    PRINTH  [rel bar_base]
    PRINT   msg_size
    mov     rax, [rel bar_size]
    shr     rax, 20
    PRINTU  rax
    PRINT   msg_mib
    mov     rax, [rel bar_base]
    mov     eax, [rax]
    PRINTH  rax
    PRINT   msg_nl

    ; test length = min(gib, bar_size)
    mov     rax, [rel opt_gib]
    shl     rax, 30
    mov     rcx, [rel bar_size]
    cmp     rax, rcx
    jbe     .len_ok
    mov     rax, rcx
.len_ok:
    mov     [rel test_len], rax

    ; BAR 1-core cold / warm, then all-core warm
    mov     rdi, [rel bar_base]
    mov     rsi, [rel test_len]
    lea     rdx, [rel lbl_bar_cold1]
    xor     ecx, ecx
    call    bench
    mov     rdi, [rel bar_base]
    mov     rsi, [rel test_len]
    lea     rdx, [rel lbl_bar_warm1]
    xor     ecx, ecx
    call    bench
    mov     rdi, [rel bar_base]
    mov     rsi, [rel test_len]
    lea     rdx, [rel lbl_bar_warmN]
    mov     ecx, 1
    call    bench
    jmp     .ram

.no_bar:
    PRINT   msg_nobar

.ram:
    ; --- RAM: largest usable region at/above 4 GiB ---
    call    find_high_ram               ; rax = base, rdx = len
    test    rax, rax
    jz      .noram
    mov     [rel ram_base], rax
    mov     rcx, [rel opt_gib]
    shl     rcx, 30
    cmp     rdx, rcx
    jbe     .rl
    mov     rdx, rcx
.rl:
    and     rdx, -4096
    mov     [rel ram_len], rdx
    PRINT   msg_ram
    PRINTH  [rel ram_base]
    PRINT   msg_size
    mov     rax, [rel ram_len]
    shr     rax, 20
    PRINTU  rax
    PRINT   msg_mib2

    mov     rax, [rel ram_base]
    mov     [rel job_base], rax
    mov     rax, [rel ram_len]
    mov     [rel job_len], rax
    call    timer_ns
    mov     rbx, rax
    lea     rdi, [rel job_fill]
    lea     rsi, [rel job_base]
    call    tp_run
    call    timer_ns
    sub     rax, rbx
    mov     rdi, [rel ram_len]
    mov     rsi, rax
    lea     rdx, [rel lbl_ram_fill]
    call    report

    mov     rdi, [rel ram_base]
    mov     rsi, [rel ram_len]
    lea     rdx, [rel lbl_ram_warm1]
    xor     ecx, ecx
    call    bench
    mov     rdi, [rel ram_base]
    mov     rsi, [rel ram_len]
    lea     rdx, [rel lbl_ram_warmN]
    mov     ecx, 1
    call    bench
    jmp     .done
.noram:
    PRINT   msg_noram
.done:
    PRINT   msg_done
    call    tp_shutdown
    call    qemu_shutdown
.halt:
    cli
    hlt
    jmp     .halt

; ----------------------------------------------------------------------------
; bench(base rdi, len rsi, label rdx, parallel ecx)
; ----------------------------------------------------------------------------
bench:
    push    rbx
    push    r12
    push    r13
    mov     [rel job_base], rdi
    mov     [rel job_len], rsi
    mov     r12, rdx
    mov     r13d, ecx
    call    timer_ns
    mov     rbx, rax
    test    r13d, r13d
    jz      .one
    lea     rdi, [rel job_read]
    lea     rsi, [rel job_base]
    call    tp_run
    jmp     .t
.one:
    xor     edi, edi
    mov     esi, 1
    lea     rdx, [rel job_base]
    call    job_read
.t:
    call    timer_ns
    sub     rax, rbx
    mov     rdi, [rel job_len]
    mov     rsi, rax
    mov     rdx, r12
    call    report
    pop     r13
    pop     r12
    pop     rbx
    ret

; report(bytes rdi, ns rsi, label rdx): "label  <MiB> MiB in <ms> ms = <GB/s> GB/s"
report:
    push    rbx
    push    r12
    push    r13
    mov     rbx, rdi
    mov     r12, rsi
    mov     rsi, rdx
    mov     edi, 1
    call    write_cstr
    mov     rax, r12
    xor     edx, edx
    mov     ecx, 1000000
    div     rcx
    PRINTU  rax
    PRINT   msg_ms
    vcvtsi2sd xmm0, xmm0, rbx
    vcvtsi2sd xmm1, xmm1, r12
    vdivsd  xmm0, xmm0, xmm1            ; bytes/ns = GB/s
    mov     edi, 1
    call    write_f64_2dp
    PRINT   msg_gbs
    pop     r13
    pop     r12
    pop     rbx
    ret

; slice(tid rdi, n rsi, arg rdx) -> rax = start, rdx = len (4 KiB granular)
slice:
    mov     r8, [rdx]                   ; base
    mov     r9, [rdx + 8]               ; len
    mov     rax, r9
    xor     edx, edx
    mov     rcx, rsi
    div     rcx
    and     rax, -4096                  ; chunk
    mov     r10, rax
    imul    rax, rdi                    ; off = tid * chunk
    lea     rcx, [rsi - 1]
    cmp     rdi, rcx
    jne     .mid
    mov     rdx, r9
    sub     rdx, rax                    ; last slice takes remainder
    jmp     .o
.mid:
    mov     rdx, r10
.o:
    add     rax, r8
    ret

; job_read(tid, n, arg): AVX2 sequential read of the thread's slice
job_read:
    call    slice
    vpxor   ymm0, ymm0, ymm0
    vpxor   ymm1, ymm1, ymm1
    vpxor   ymm2, ymm2, ymm2
    vpxor   ymm3, ymm3, ymm3
    shr     rdx, 8                      ; 256 B per iteration
    jz      .end
.lp:
    vpaddq  ymm0, ymm0, [rax]
    vpaddq  ymm1, ymm1, [rax + 32]
    vpaddq  ymm2, ymm2, [rax + 64]
    vpaddq  ymm3, ymm3, [rax + 96]
    vpaddq  ymm0, ymm0, [rax + 128]
    vpaddq  ymm1, ymm1, [rax + 160]
    vpaddq  ymm2, ymm2, [rax + 192]
    vpaddq  ymm3, ymm3, [rax + 224]
    add     rax, 256
    dec     rdx
    jnz     .lp
.end:
    vpaddq  ymm0, ymm0, ymm1
    vpaddq  ymm2, ymm2, ymm3
    vpaddq  ymm0, ymm0, ymm2
    vmovdqu [rel sink], ymm0
    vzeroupper
    ret

; job_fill(tid, n, arg): write the slice (faults guest RAM in)
job_fill:
    call    slice
    vpcmpeqd ymm0, ymm0, ymm0
    shr     rdx, 7
    jz      .end
.lp:
    vmovdqa [rax], ymm0
    vmovdqa [rax + 32], ymm0
    vmovdqa [rax + 64], ymm0
    vmovdqa [rax + 96], ymm0
    sub     rax, -128
    dec     rdx
    jnz     .lp
.end:
    vzeroupper
    ret

; rax = base, rdx = len of the largest type-1 mmap region with base >= 4 GiB
find_high_ram:
    xor     eax, eax
    xor     edx, edx
    mov     r8d, [rel g_mb_info]
    test    dword [r8 + MBI_FLAGS], 0x40
    jz      .ret
    mov     ecx, [r8 + MBI_MMAP_ADDR]
    mov     r9d, [r8 + MBI_MMAP_LENGTH]
    add     r9, rcx
    mov     r11, 0x100000000
.lp:
    cmp     rcx, r9
    jae     .ret
    cmp     dword [rcx + 20], 1
    jne     .next
    mov     r10, [rcx + 4]
    cmp     r10, r11
    jb      .next
    cmp     [rcx + 12], rdx
    jbe     .next
    mov     rax, r10
    mov     rdx, [rcx + 12]
.next:
    mov     r10d, [rcx]
    lea     rcx, [rcx + r10 + 4]
    jmp     .lp
.ret:
    ret

; find_key(cmdline rdi, key rsi) -> rax = ptr after key, or 0
find_key:
.outer:
    cmp     byte [rdi], 0
    je      .none
    xor     ecx, ecx
.cmp:
    mov     al, [rsi + rcx]
    test    al, al
    jz      .hit
    cmp     al, [rdi + rcx]
    jne     .adv
    inc     ecx
    jmp     .cmp
.adv:
    inc     rdi
    jmp     .outer
.hit:
    lea     rax, [rdi + rcx]
    ret
.none:
    xor     eax, eax
    ret

section .rodata
key_gib:        db "gib=", 0
key_threads:    db "threads=", 0
msg_banner:     db 10, "=== asm-llm OS27 M0: ivshmem BAR bandwidth gate ===", 10, 0
msg_1g:         db "1GiB pages: ", 0
msg_top:        db "  mapped top: 0x", 0
msg_cores:      db "cores: ", 0
msg_bar:        db "ivshmem BAR2: 0x", 0
msg_size:       db "  size: ", 0
msg_mib:        db " MiB  magic: 0x", 0
msg_mib2:       db " MiB", 10, 0
msg_nobar:      db "ivshmem device NOT found / BAR unassigned", 10, 0
msg_ram:        db "RAM test region: 0x", 0
msg_noram:      db "no RAM region above 4 GiB (use -m >= 8G)", 10, 0
msg_ms:         db " ms  ", 0
msg_gbs:        db " GB/s", 10, 0
msg_nl:         db 10, 0
msg_done:       db "M0 DONE", 10, 0
lbl_bar_cold1:  db "BAR read  1-core cold : ", 0
lbl_bar_warm1:  db "BAR read  1-core warm : ", 0
lbl_bar_warmN:  db "BAR read  N-core warm : ", 0
lbl_ram_fill:   db "RAM fill  N-core      : ", 0
lbl_ram_warm1:  db "RAM read  1-core warm : ", 0
lbl_ram_warmN:  db "RAM read  N-core warm : ", 0

section .bss
alignb 32
sink:           resb 32
job_base:       resq 1
job_len:        resq 1
opt_gib:        resq 1
opt_threads:    resq 1
n_cores:        resq 1
bar_base:       resq 1
bar_size:       resq 1
test_len:       resq 1
ram_base:       resq 1
ram_len:        resq 1
