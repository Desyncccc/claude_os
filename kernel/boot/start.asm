; ============================================================================
;  RadiaNT -- boot/start.asm
;  Multiboot2 entry. The loader leaves us in 32-bit protected mode; this stub
;  builds the initial page tables, switches to long mode and jumps to the
;  kernel in the higher half.
;
;      PML4[0]    -> identity map of the first 4 GiB (dropped by Mm later)
;      PML4[256]  -> the same tables: the physical map at 0xFFFF8000'00000000
;      PML4[511]  -> kernel image at 0xFFFFFFFF'80000000
; ============================================================================

KERNEL_VMA equ 0xFFFFFFFF80000000

; Preferred video mode. GRUB takes the mode from this header (it overrides any
; gfxpayload setting) and falls back to the closest mode the firmware offers.
; Override at build time:  make FB_WIDTH=1920 FB_HEIGHT=1080
%ifndef FB_WIDTH
%define FB_WIDTH  1280
%define FB_HEIGHT 720
%endif

section .multiboot progbits alloc noexec nowrite align=8
mb_header:
        dd 0xE85250D6                       ; Multiboot2 magic
        dd 0                                ; architecture: i386 protected mode
        dd mb_header_end - mb_header
        dd -(0xE85250D6 + (mb_header_end - mb_header))
        align 8
        dw 5, 0                             ; framebuffer request
        dd 20
        dd FB_WIDTH, FB_HEIGHT, 32
        align 8
        dw 0, 0                             ; end tag
        dd 8
mb_header_end:

section .boot.bss nobits alloc noexec write align=4096
boot_pml4:      resb 4096
boot_pdpt_low:  resb 4096
boot_pdpt_high: resb 4096
boot_pd:        resb 4096 * 4               ; 4 GiB of 2 MiB pages
boot_stack:     resb 4096
boot_stack_top:

section .boot.data progbits alloc noexec write align=16
boot_gdt:
        dq 0
        dq 0x00209A0000000000               ; 0x08 64-bit code
        dq 0x0000920000000000               ; 0x10 data
boot_gdt_ptr:
        dw $ - boot_gdt - 1
        dd boot_gdt
mb_magic: dd 0
mb_info:  dd 0
nx_flag:  dd 0

section .boot.text progbits alloc exec nowrite align=16
bits 32
global _start
_start:
        cli
        cld
        mov     esp, boot_stack_top
        mov     [mb_magic], eax
        mov     [mb_info], ebx

        ; ---- require long mode ------------------------------------------------
        mov     eax, 0x80000000
        cpuid
        cmp     eax, 0x80000001
        jb      .no_long_mode
        mov     eax, 0x80000001
        cpuid
        test    edx, 1 << 29
        jz      .no_long_mode
        mov     eax, edx
        shr     eax, 20
        and     eax, 1
        mov     [nx_flag], eax

        ; ---- clear and build the page tables ----------------------------------
        mov     edi, boot_pml4
        mov     ecx, (7 * 4096) / 4
        xor     eax, eax
        rep     stosd

        mov     eax, boot_pdpt_low
        or      eax, 3
        mov     [boot_pml4], eax
        mov     [boot_pml4 + 256 * 8], eax
        mov     eax, boot_pdpt_high
        or      eax, 3
        mov     [boot_pml4 + 511 * 8], eax

        mov     edi, boot_pdpt_low
        mov     eax, boot_pd
        or      eax, 3
        mov     ecx, 4
.pdpt:  mov     [edi], eax
        add     eax, 0x1000
        add     edi, 8
        loop    .pdpt

        mov     eax, boot_pd
        or      eax, 3
        mov     [boot_pdpt_high + 510 * 8], eax
        add     eax, 0x1000
        mov     [boot_pdpt_high + 511 * 8], eax

        xor     ebx, ebx
.pd:    mov     eax, ebx
        shl     eax, 21
        or      eax, 0x83                   ; present | write | 2 MiB
        mov     [boot_pd + ebx * 8], eax
        mov     eax, ebx
        shr     eax, 11
        mov     [boot_pd + ebx * 8 + 4], eax
        inc     ebx
        cmp     ebx, 2048
        jne     .pd

        ; ---- PAE + SSE enable, load CR3, set EFER.LME, enable paging ----------
        mov     eax, cr4
        or      eax, (1 << 5) | (1 << 9) | (1 << 10)
        mov     cr4, eax
        mov     eax, boot_pml4
        mov     cr3, eax

        mov     ecx, 0xC0000080
        rdmsr
        or      eax, (1 << 8) | (1 << 0)    ; LME | SCE
        cmp     dword [nx_flag], 0
        je      .no_nx
        or      eax, (1 << 11)              ; NXE
.no_nx: wrmsr

        mov     eax, cr0
        and     eax, ~(1 << 2)              ; clear EM
        or      eax, (1 << 31) | (1 << 16) | (1 << 1)
        mov     cr0, eax

        lgdt    [boot_gdt_ptr]
        jmp     0x08:.long_mode

.no_long_mode:
        mov     dword [0xB8000], 0x4F344F36 ; "64" in white on red
        mov     dword [0xB8004], 0x4F3F4F20
.halt32:
        hlt
        jmp     .halt32

bits 64
.long_mode:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        xor     eax, eax
        mov     fs, ax
        mov     gs, ax

        mov     rsp, KiBootStackTop
        xor     ebp, ebp
        mov     edi, [mb_magic]
        mov     esi, [mb_info]
        mov     edx, [nx_flag]
        mov     rax, KiSystemStartup
        call    rax
.halt64:
        cli
        hlt
        jmp     .halt64

extern KiSystemStartup

; ---- data the C kernel needs from the stub ---------------------------------
section .data
global KiBootPml4, KiBootPdptLow
KiBootPml4:     dq boot_pml4
KiBootPdptLow:  dq boot_pdpt_low

section .bss
align 16
global KiBootStack, KiBootStackTop
KiBootStack:    resb 0x8000
KiBootStackTop:
