/*
 * RadiaNT -- ke/trap.c
 * Common trap handler: hardware interrupts, CPU exceptions and the
 * user-mode exception path that turns faults into process termination.
 */
#include "ke.h"
#include "hal.h"
#include "ps.h"
#include "rtl.h"
#include "win32k.h"
#include "ntapi.h"

ULONG64 KeInterruptCount, KeExceptionCount;
extern BOOLEAN KiInterruptedUserMode;

static const char *const KiExceptionNames[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound Range",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Overrun",
    "Invalid TSS", "Segment Not Present", "Stack Fault", "General Protection",
    "Page Fault", "Reserved", "x87 FP Error", "Alignment Check", "Machine Check",
    "SIMD FP Error", "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security", "Reserved",
};

static NTSTATUS KiExceptionToStatus(ULONG64 Vector)
{
    switch (Vector) {
    case 0:  return STATUS_INTEGER_DIVIDE_BY_ZERO;
    case 6:  return STATUS_ILLEGAL_INSTRUCTION;
    case 13: return STATUS_PRIVILEGED_INSTRUCTION;
    default: return STATUS_ACCESS_VIOLATION;
    }
}

static void KiDispatchException(KTRAP_FRAME *f)
{
    KeExceptionCount++;
    if (f->SegCs & 3) {
        /* Fault in user mode: report it on the process console and kill it. */
        PEPROCESS p = PsGetCurrentProcess();
        NTSTATUS st = KiExceptionToStatus(f->Vector);
        char msg[200];
        ULONG64 cr2 = HalReadCr2();
        KiIrql = PASSIVE_LEVEL;
        HalEnableInterrupts();
        if (f->Vector == 14)
            RtlStringPrintf(msg, sizeof(msg),
                "\n*** %s (pid %u): unhandled exception 0x%08X (%s)\n"
                "    %s address 0x%016llX from RIP 0x%016llX\n",
                p->ImageFileName, p->UniqueProcessId, (ULONG)st, KiExceptionNames[f->Vector],
                (f->ErrorCode & 2) ? "writing" : ((f->ErrorCode & 16) ? "executing" : "reading"),
                cr2, f->Rip);
        else
            RtlStringPrintf(msg, sizeof(msg),
                "\n*** %s (pid %u): unhandled exception 0x%08X (%s) at RIP 0x%016llX\n",
                p->ImageFileName, p->UniqueProcessId, (ULONG)st, KiExceptionNames[f->Vector], f->Rip);
        DbgPrint("%s", msg);
        ConSetColorForProcess(p, CON_RED);
        ConWriteForProcess(p, msg, strlen(msg));
        ConSetColorForProcess(p, CON_DEFAULT);
        PsTerminateProcess(p, st);          /* does not return for the current process */
    }

    switch (f->Vector) {
    case 14:
        KiBugCheckWithFrame(PAGE_FAULT_IN_NONPAGED_AREA, HalReadCr2(), f->ErrorCode, f->Rip, 0, f);
    case 8:
        KiBugCheckWithFrame(UNEXPECTED_KERNEL_MODE_TRAP, 8, f->Rip, f->Rsp, 0, f);
    default:
        KiBugCheckWithFrame(KMODE_EXCEPTION_NOT_HANDLED, f->Vector, f->Rip, f->ErrorCode, 0, f);
    }
}

void KiTrapDispatch(KTRAP_FRAME *f)
{
    if (f->Vector < 32) {
        KiDispatchException(f);
        return;
    }
    if (f->Vector >= 0x20 && f->Vector < 0x30) {
        ULONG irq = (ULONG)f->Vector - 0x20;
        KIRQL old = KiIrql;
        KiIrql = irq == 0 ? CLOCK_LEVEL : DEVICE_LEVEL;
        KiInterruptedUserMode = (f->SegCs & 3) != 0;
        KeInterruptCount++;
        HalDispatchIrq(irq);
        KiIrql = old;
        if (old < DISPATCH_LEVEL) KiProcessSoftwareInterrupts(old);
    }
    /* Returning to user mode: honour a pending termination request. */
    if ((f->SegCs & 3) && PsGetCurrentProcess()->Terminating) {
        KiIrql = PASSIVE_LEVEL;
        HalEnableInterrupts();
        PsTerminateCurrentThread(PsGetCurrentProcess()->ExitStatus);
    }
}
