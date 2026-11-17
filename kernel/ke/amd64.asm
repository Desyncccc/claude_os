; ============================================================================
;  RadiaNT -- ke/amd64.asm
;  Interrupt/exception stubs, the context switcher, the SYSCALL entry point
;  and the ring-3 transition.
; ============================================================================
bits 64
default rel

extern KiTrapDispatch
extern KiThreadStartup
extern KiSystemServiceDispatch
extern KiSyscallStackTop
extern KiUserRspScratch

section .text

; ---- 256 vector stubs: normalise the frame, then join the common path -------
%macro ISR_STUB 1
isr_stub_%1:
%if (%1 == 8) || (%1 >= 10 && %1 <= 14) || (%1 == 17) || (%1 == 21) || (%1 == 29) || (%1 == 30)
        ; CPU pushed an error code
%else
        push    0
%endif
        push    %1
        jmp     KiTrapCommon
%endmacro

%assign vec 0
%rep 256
        ISR_STUB vec
%assign vec vec + 1
%endrep

KiTrapCommon:
        push    rax
        push    rbx
        push    rcx
        push    rdx
        push    rsi
        push    rdi
        push    rbp
        push    r8
        push    r9
        push    r10
        push    r11
        push    r12
        push    r13
        push    r14
        push    r15
        cld
        mov     rdi, rsp                    ; KTRAP_FRAME *
        call    KiTrapDispatch
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rbp
        pop     rdi
        pop     rsi
        pop     rdx
        pop     rcx
        pop     rbx
        pop     rax
        add     rsp, 16                     ; vector + error code
        iretq

; ---- void KiSwapContext(KTHREAD *Old, KTHREAD *New) -------------------------
; KTHREAD.KernelStack is at offset 0.
global KiSwapContext
KiSwapContext:
        push    rbp
        push    rbx
        push    r12
        push    r13
        push    r14
        push    r15
        mov     [rdi], rsp
        mov     rsp, [rsi]
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbx
        pop     rbp
        ret

; First code a brand-new thread runs (KiInitializeThread points its saved
; return address here).
global KiThreadTrampoline
KiThreadTrampoline:
        and     rsp, -16
        xor     ebp, ebp
        call    KiThreadStartup
.dead:  hlt
        jmp     .dead

; ---- NORETURN void KiEnterUserMode(ULONG64 Rip, ULONG64 Rsp) ---------------
global KiEnterUserMode
KiEnterUserMode:
        cli
        mov     ax, 0x1B
        mov     ds, ax
        mov     es, ax
        push    0x1B                        ; SS  (user data | RPL 3)
        push    rsi                         ; RSP
        push    0x202                       ; RFLAGS: IF
        push    0x23                        ; CS  (user code | RPL 3)
        push    rdi                         ; RIP
        xor     eax, eax
        xor     ebx, ebx
        xor     ecx, ecx
        xor     edx, edx
        xor     esi, esi
        xor     edi, edi
        xor     ebp, ebp
        xor     r8d, r8d
        xor     r9d, r9d
        xor     r10d, r10d
        xor     r11d, r11d
        xor     r12d, r12d
        xor     r13d, r13d
        xor     r14d, r14d
        xor     r15d, r15d
        iretq

; ---- SYSCALL entry ----------------------------------------------------------
; In:  RAX = service number, R10/RDX/R8/R9 = arguments (the NT x64 protocol)
;      RCX = user RIP, R11 = user RFLAGS (set by the CPU)
; Callers use the Microsoft ABI, so RDI/RSI must be preserved for them.
global KiSystemCall64
KiSystemCall64:
        mov     [KiUserRspScratch], rsp     ; IF is masked by SFMASK
        mov     rsp, [KiSyscallStackTop]
        push    qword [KiUserRspScratch]
        push    r11
        push    rcx
        push    rdi
        push    rsi
        sub     rsp, 8                      ; keep the stack 16-byte aligned
        sti
        cld
        mov     rdi, r10
        mov     rsi, rdx
        mov     rdx, r8
        mov     rcx, r9
        mov     r8, rax
        call    KiSystemServiceDispatch
        cli
        add     rsp, 8
        pop     rsi
        pop     rdi
        pop     rcx
        pop     r11
        xor     edx, edx                    ; do not leak kernel values
        xor     r8d, r8d
        xor     r9d, r9d
        xor     r10d, r10d
        pop     rsp
        o64 sysret

; ---- vector table consumed by KiInitializeProcessor -------------------------
section .data
global KiIsrStubTable
KiIsrStubTable:
%assign vec 0
%rep 256
        dq isr_stub_ %+ vec
%assign vec vec + 1
%endrep
