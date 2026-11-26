; ============================================================================
; 04-qwen38-27b-os/pci.asm — PCI config mechanism #1 + ivshmem discovery
;
; Finds the QEMU ivshmem-plain device (1AF4:1110), sizes its 64-bit BAR2
; (the shared-memory window backed by the host .asmbin file), enables
; memory decoding, and identity-maps the BAR Write-Back.
; ============================================================================

bits 64
default rel

%define PCI_ADDR        0xCF8
%define PCI_DATA        0xCFC
%define IVSHMEM_ID      0x11101AF4          ; device << 16 | vendor

extern map_range_wb

section .text

global pci_find_ivshmem
global pci_read32
global g_ivshmem_bdf

; uint32 pci_read32(uint32 bdf_off in edi)   (edi = bus<<16|dev<<11|fn<<8|off)
pci_read32:
    mov     eax, edi
    or      eax, 0x80000000
    and     eax, ~3
    mov     dx, PCI_ADDR
    out     dx, eax
    mov     dx, PCI_DATA
    in      eax, dx
    ret

; void pci_write32(uint32 bdf_off in edi, uint32 val in esi)
pci_write32:
    mov     eax, edi
    or      eax, 0x80000000
    and     eax, ~3
    mov     dx, PCI_ADDR
    out     dx, eax
    mov     eax, esi
    mov     dx, PCI_DATA
    out     dx, eax
    ret

; ----------------------------------------------------------------------------
; uint64 pci_find_ivshmem(uint64 *size_out in rdi)
; Returns BAR2 physical base (0 if not found / not a 64-bit memory BAR or
; unassigned). Writes BAR size to *size_out. Maps the BAR WB.
; ----------------------------------------------------------------------------
pci_find_ivshmem:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    mov     r14, rdi
    xor     ebx, ebx                    ; bdf = bus<<16 | dev<<11 | fn<<8
.scan:
    mov     edi, ebx
    call    pci_read32
    cmp     eax, IVSHMEM_ID
    je      .found
    add     ebx, 0x100                  ; next function
    cmp     ebx, 0x80000                ; buses 0..7
    jb      .scan
    xor     eax, eax
    jmp     .ret

.found:
    mov     [rel g_ivshmem_bdf], ebx
    ; Read command, disable decode while sizing
    lea     edi, [ebx + 0x04]
    call    pci_read32
    mov     r13d, eax                   ; saved command/status
    lea     edi, [ebx + 0x04]
    mov     esi, r13d
    and     esi, 0xFFFF & ~0x3
    call    pci_write32

    lea     edi, [ebx + 0x18]
    call    pci_read32
    mov     r12d, eax                   ; BAR2 low
    mov     ecx, eax
    and     ecx, 0x7
    cmp     ecx, 0x4                    ; memory, 64-bit
    jne     .bad
    lea     edi, [ebx + 0x1C]
    call    pci_read32
    mov     ebp, eax                    ; BAR3 (high)

    ; Size: write all ones, read back, restore
    lea     edi, [ebx + 0x18]
    mov     esi, 0xFFFFFFFF
    call    pci_write32
    lea     edi, [ebx + 0x1C]
    mov     esi, 0xFFFFFFFF
    call    pci_write32
    lea     edi, [ebx + 0x18]
    call    pci_read32
    mov     ecx, eax
    push    rcx
    lea     edi, [ebx + 0x1C]
    call    pci_read32
    pop     rcx
    shl     rax, 32
    and     ecx, 0xFFFFFFF0
    or      rax, rcx
    not     rax
    inc     rax                         ; size
    mov     [r14], rax

    lea     edi, [ebx + 0x18]
    mov     esi, r12d
    call    pci_write32
    lea     edi, [ebx + 0x1C]
    mov     esi, ebp
    call    pci_write32

    ; Enable memory decoding (bit 1)
    lea     edi, [ebx + 0x04]
    mov     esi, r13d
    and     esi, 0xFFFF
    or      esi, 0x2
    call    pci_write32

    mov     rax, rbp
    shl     rax, 32
    and     r12d, 0xFFFFFFF0
    or      rax, r12
    test    rax, rax
    jz      .ret
    mov     r12, rax
    mov     rdi, rax
    mov     rsi, [r14]
    call    map_range_wb
    test    rax, rax
    js      .bad
    mov     rax, r12
    jmp     .ret
.bad:
    xor     eax, eax
.ret:
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

section .bss
g_ivshmem_bdf:      resd 1
