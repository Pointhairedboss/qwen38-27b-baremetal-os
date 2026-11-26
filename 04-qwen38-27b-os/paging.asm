; ============================================================================
; 04-qwen38-27b-os/paging.asm — 64-bit identity mapper above 4 GiB
;
; boot.asm identity-maps 0..4 GiB with 2 MiB pages (0xFE000000.. UC).
; This module extends the identity map to arbitrary physical ranges above
; 4 GiB (all usable RAM from the Multiboot mmap + PCI BARs such as the
; ivshmem model window), using 1 GiB pages when CPUID.pdpe1gb is present
; and 2 MiB pages otherwise. Page-table pages come from a .bss pool.
; All new mappings are Write-Back (P | RW | PS = 0x83).
; ============================================================================

bits 64
default rel

%include "os_constants.inc"

%define PG_POOL_PAGES   256                 ; 1 MiB of page-table pages
%define ADDR_MASK       0x000FFFFFFFFFF000
%define GIB             0x40000000
%define MIB2            0x00200000
%define FOUR_GIB        0x100000000

extern pml4_table

section .text

global paging_init
global map_range_wb
global g_has_1g_pages
global g_mapped_top

; ----------------------------------------------------------------------------
; void paging_init(const void *mbi in rdi)
; Detects 1 GiB page support and maps every usable (type 1) Multiboot mmap
; region above 4 GiB. Reads the mmap with full 64-bit addr/len.
; ----------------------------------------------------------------------------
paging_init:
    push    rbx
    push    rbp
    push    r12
    mov     r12, rdi

    ; CPUID 0x80000001 EDX bit 26 = Page1GB
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      .no1g
    mov     eax, 0x80000001
    cpuid
    bt      edx, 26
    jnc     .no1g
    mov     dword [rel g_has_1g_pages], 1
.no1g:
    mov     eax, [r12 + MBI_FLAGS]
    test    eax, 0x40
    jz      .done
    mov     ebx, [r12 + MBI_MMAP_ADDR]
    mov     ebp, [r12 + MBI_MMAP_LENGTH]
    add     rbp, rbx
.lp:
    cmp     rbx, rbp
    jae     .done
    cmp     dword [rbx + 20], 1
    jne     .next
    mov     rdi, [rbx + 4]
    mov     rsi, [rbx + 12]
    call    map_range_wb
.next:
    mov     eax, [rbx]
    lea     rbx, [rbx + rax + 4]
    jmp     .lp
.done:
    pop     r12
    pop     rbp
    pop     rbx
    ret

; ----------------------------------------------------------------------------
; int64 map_range_wb(uint64 phys in rdi, uint64 size in rsi)
; Identity-maps [phys, phys+size) rounded out to 2 MiB, skipping anything
; below 4 GiB (already mapped by boot.asm). Returns 0, or -1 if the
; page-table pool is exhausted.
; ----------------------------------------------------------------------------
map_range_wb:
    push    rbx
    push    r12
    push    r13
    lea     r12, [rdi + rsi + MIB2 - 1]
    and     r12, -MIB2                  ; end (exclusive)
    mov     rbx, rdi
    and     rbx, -MIB2                  ; addr
    mov     rax, FOUR_GIB
    cmp     rbx, rax
    jae     .loop
    mov     rbx, rax
.loop:
    cmp     rbx, r12
    jae     .ok
    cmp     r12, [rel g_mapped_top]
    jbe     .top_ok
    mov     [rel g_mapped_top], r12
.top_ok:
    ; --- PML4 level ---
    mov     rax, rbx
    shr     rax, 39
    and     eax, 511
    lea     rcx, [rel pml4_table]
    lea     r13, [rcx + rax*8]          ; &pml4e
    mov     rax, [r13]
    test    rax, rax
    jnz     .have_pdpt
    call    pg_alloc
    test    rax, rax
    jz      .oom
    or      rax, 0x03
    mov     [r13], rax
.have_pdpt:
    mov     rcx, ADDR_MASK
    and     rax, rcx                    ; pdpt base
    mov     rdx, rbx
    shr     rdx, 30
    and     edx, 511
    lea     r13, [rax + rdx*8]          ; &pdpte
    mov     rax, [r13]
    test    rax, 0x80                   ; already a 1 GiB page?
    jz      .not_1g
    add     rbx, GIB
    and     rbx, -GIB
    jmp     .loop
.not_1g:
    test    rax, rax
    jnz     .have_pd
    ; Empty PDPTE: try a 1 GiB page
    cmp     dword [rel g_has_1g_pages], 0
    je      .alloc_pd
    test    rbx, GIB - 1
    jnz     .alloc_pd
    mov     rdx, r12
    sub     rdx, rbx
    cmp     rdx, GIB
    jb      .alloc_pd
    mov     rax, rbx
    or      rax, 0x83
    mov     [r13], rax
    add     rbx, GIB
    jmp     .loop
.alloc_pd:
    call    pg_alloc
    test    rax, rax
    jz      .oom
    or      rax, 0x03
    mov     [r13], rax
.have_pd:
    mov     rcx, ADDR_MASK
    and     rax, rcx                    ; pd base
    mov     rdx, rbx
    shr     rdx, 21
    and     edx, 511
    lea     r13, [rax + rdx*8]
    cmp     qword [r13], 0
    jne     .next2m
    mov     rax, rbx
    or      rax, 0x83
    mov     [r13], rax
.next2m:
    add     rbx, MIB2
    jmp     .loop
.ok:
    mov     rax, cr3                    ; flush paging-structure caches
    mov     cr3, rax
    xor     eax, eax
    jmp     .ret
.oom:
    mov     rax, -1
.ret:
    pop     r13
    pop     r12
    pop     rbx
    ret

; Returns a zeroed 4 KiB page from the pool in rax (0 if exhausted)
pg_alloc:
    mov     eax, [rel pg_pool_used]
    cmp     eax, PG_POOL_PAGES
    jae     .none
    inc     dword [rel pg_pool_used]
    shl     rax, 12
    lea     rcx, [rel pg_pool]
    add     rax, rcx                    ; pool is zeroed by boot .bss clear
    ret
.none:
    xor     eax, eax
    ret

section .bss
alignb 4096
pg_pool:            resb PG_POOL_PAGES * 4096
g_mapped_top:       resq 1
g_has_1g_pages:     resd 1
pg_pool_used:       resd 1
