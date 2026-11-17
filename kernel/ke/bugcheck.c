/* RadiaNT -- ke/bugcheck.c : the end of the road. */
#include "ke.h"
#include "hal.h"
#include "ps.h"
#include "rtl.h"
#include "win32k.h"

static const struct { ULONG Code; PCSTR Name; } KiBugCheckNames[] = {
    { IRQL_NOT_LESS_OR_EQUAL,       "IRQL_NOT_LESS_OR_EQUAL" },
    { 0x0000000F,                   "SPIN_LOCK_ALREADY_OWNED" },
    { 0x00000018,                   "REFERENCE_BY_POINTER" },
    { BAD_POOL_HEADER,              "BAD_POOL_HEADER" },
    { KMODE_EXCEPTION_NOT_HANDLED,  "KMODE_EXCEPTION_NOT_HANDLED" },
    { NO_MORE_SYSTEM_MEMORY,        "NO_MORE_SYSTEM_MEMORY" },
    { PHASE0_INITIALIZATION_FAILED, "PHASE0_INITIALIZATION_FAILED" },
    { 0x0000004E,                   "PFN_LIST_CORRUPT" },
    { PAGE_FAULT_IN_NONPAGED_AREA,  "PAGE_FAULT_IN_NONPAGED_AREA" },
    { UNEXPECTED_KERNEL_MODE_TRAP,  "UNEXPECTED_KERNEL_MODE_TRAP" },
    { ASSERTION_FAILURE,            "ASSERTION_FAILURE" },
    { MANUALLY_INITIATED_CRASH,     "MANUALLY_INITIATED_CRASH" },
};

static BOOLEAN KiBugCheckActive;

NORETURN void KiBugCheckWithFrame(ULONG Code, ULONG64 P1, ULONG64 P2, ULONG64 P3, ULONG64 P4,
                                  KTRAP_FRAME *Frame)
{
    HalDisableInterrupts();
    if (KiBugCheckActive) for (;;) HalHalt();
    KiBugCheckActive = TRUE;
    KiIrql = HIGH_LEVEL;

    PCSTR name = "UNKNOWN_BUGCHECK";
    for (ULONG i = 0; i < ARRAY_SIZE(KiBugCheckNames); i++)
        if (KiBugCheckNames[i].Code == Code) name = KiBugCheckNames[i].Name;

    char detail[512];
    PETHREAD t = PsGetCurrentThread();
    int n = RtlStringPrintf(detail, sizeof(detail), "Thread: %s (tid %u)   Process: %s   Uptime: %llu ms\n",
                            t ? t->Name : "?", t ? t->UniqueThreadId : 0,
                            t ? PsGetCurrentProcess()->ImageFileName : "?", KeTickCount);
    if (Frame && n < (int)sizeof(detail)) {
        RtlStringPrintf(detail + n, sizeof(detail) - n,
            "RIP=%016llX  RSP=%016llX  RFLAGS=%08llX\n"
            "RAX=%016llX  RBX=%016llX  RCX=%016llX\n"
            "RDX=%016llX  RSI=%016llX  RDI=%016llX\n"
            "RBP=%016llX  R8 =%016llX  R9 =%016llX\n"
            "CR2=%016llX  CR3=%016llX  ERR=%04llX",
            Frame->Rip, Frame->Rsp, Frame->EFlags, Frame->Rax, Frame->Rbx, Frame->Rcx,
            Frame->Rdx, Frame->Rsi, Frame->Rdi, Frame->Rbp, Frame->R8, Frame->R9,
            HalReadCr2(), HalReadCr3(), Frame->ErrorCode);
    }

    char line[160];
    n = RtlStringPrintf(line, sizeof(line),
        "\n*** STOP: 0x%08X (0x%016llX, 0x%016llX, 0x%016llX, 0x%016llX)\n*** %s\n",
        Code, P1, P2, P3, P4, name);
    HalSerialWrite(line, n);
    HalSerialWrite(detail, strlen(detail));
    HalSerialWrite("\n", 1);

    ULONG64 params[4] = { P1, P2, P3, P4 };
    Win32kDrawBugCheck(Code, name, params, detail);
    for (;;) HalHalt();
}

NORETURN void KeBugCheckEx(ULONG Code, ULONG64 P1, ULONG64 P2, ULONG64 P3, ULONG64 P4)
{
    KiBugCheckWithFrame(Code, P1, P2, P3, P4, NULL);
}
