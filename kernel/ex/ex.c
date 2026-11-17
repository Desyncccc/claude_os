/*
 * RadiaNT -- ex/ex.c
 * Executive support: the object types that wrap kernel dispatcher objects,
 * and the system service table (the "SSDT") behind the SYSCALL instruction.
 */
#include "ke.h"
#include "ps.h"
#include "mm.h"
#include "ob.h"
#include "hal.h"
#include "rtl.h"
#include "win32k.h"
#include "ntapi.h"

POBJECT_TYPE ExEventObjectType, ExMutantObjectType, ExSemaphoreObjectType;
ULONG64 KeSystemCalls;

void ExInitSystem(void)
{
    ObCreateObjectType("Event", TAG('E', 'v', 'e', 'n'), NULL, &ExEventObjectType);
    ObCreateObjectType("Mutant", TAG('M', 'u', 't', 'a'), NULL, &ExMutantObjectType);
    ObCreateObjectType("Semaphore", TAG('S', 'e', 'm', 'a'), NULL, &ExSemaphoreObjectType);
}

#define PROBE(ptr, len, write) \
    MmIsUserRangeValid(PsGetCurrentProcess()->Pcb.DirectoryTableBase, (ULONG64)(ptr), (len), (write))

/* ---- Native services --------------------------------------------------------------- */
static ULONG64 NtDisplayString(ULONG64 Buffer, ULONG64 Length, ULONG64 a3, ULONG64 a4)
{
    (void)a3; (void)a4;
    if (Length > 4096) Length = 4096;
    if (!PROBE(Buffer, Length, FALSE)) return (ULONG)STATUS_ACCESS_VIOLATION;
    ConWriteForProcess(PsGetCurrentProcess(), (const char *)Buffer, Length);
    return STATUS_SUCCESS;
}

static ULONG64 NtTerminateProcess(ULONG64 ExitStatus, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a2; (void)a3; (void)a4;
    PsTerminateProcess(PsGetCurrentProcess(), (NTSTATUS)ExitStatus);
    return STATUS_SUCCESS;
}

static ULONG64 NtDelayExecution(ULONG64 Milliseconds, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a2; (void)a3; (void)a4;
    if (Milliseconds > 3600000) Milliseconds = 3600000;
    return (ULONG)KeDelayExecutionThread((ULONG)Milliseconds);
}

static ULONG64 NtQuerySystemInformation(ULONG64 Class, ULONG64 Buffer, ULONG64 Length, ULONG64 a4)
{
    (void)a4;
    if (Class != SystemBasicInformation) return (ULONG)STATUS_INVALID_PARAMETER;
    if (Length < sizeof(SYSTEM_BASIC_INFORMATION)) return (ULONG)STATUS_BUFFER_TOO_SMALL;
    if (!PROBE(Buffer, sizeof(SYSTEM_BASIC_INFORMATION), TRUE)) return (ULONG)STATUS_ACCESS_VIOLATION;
    SYSTEM_BASIC_INFORMATION info;
    memset(&info, 0, sizeof(info));
    info.MajorVersion = RADIANT_VER_MAJOR; info.MinorVersion = RADIANT_VER_MINOR;
    info.BuildNumber = RADIANT_BUILD;
    info.NumberOfProcessors = 1;
    info.PageSize = PAGE_SIZE;
    info.ProcessCount = PsProcessCount; info.ThreadCount = PsThreadCount;
    info.TotalPhysicalPages = MmTotalPages; info.FreePhysicalPages = MmFreePagesCount;
    info.TickCount = KeTickCount;
    RtlStringCopy(info.ProcessorName, sizeof(info.ProcessorName), HalCpuInfo.Brand);
    memcpy((PVOID)Buffer, &info, sizeof(info));
    return STATUS_SUCCESS;
}

static ULONG64 NtGetTickCount(ULONG64 a1, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return KeTickCount;
}

static ULONG64 NtYieldExecution(ULONG64 a1, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    KeYieldExecution();
    return STATUS_SUCCESS;
}

static ULONG64 NtAllocateVirtualMemory(ULONG64 BaseAddress, ULONG64 Size, ULONG64 a3, ULONG64 a4)
{
    (void)a3; (void)a4;
    PEPROCESS p = PsGetCurrentProcess();
    if (!PROBE(BaseAddress, sizeof(ULONG64), TRUE)) return (ULONG)STATUS_ACCESS_VIOLATION;
    Size = ALIGN_UP(Size, PAGE_SIZE);
    if (!Size || Size > (64ull << 20) || p->PrivatePages + (Size >> PAGE_SHIFT) > 32768)
        return (ULONG)STATUS_NO_MEMORY;
    ULONG64 va = p->HeapNext;
    NTSTATUS st = MmAllocateUserMemory(p->Pcb.DirectoryTableBase, va, Size, PTE_WRITE | PTE_NX,
                                       &p->PrivatePages);
    if (!NT_SUCCESS(st)) return (ULONG)st;
    p->HeapNext = va + Size + PAGE_SIZE;            /* leave a guard page */
    *(ULONG64 *)BaseAddress = va;
    return STATUS_SUCCESS;
}

static ULONG64 NtCreateEvent(ULONG64 HandleOut, ULONG64 ManualReset, ULONG64 InitialState, ULONG64 a4)
{
    (void)a4;
    if (!PROBE(HandleOut, sizeof(HANDLE), TRUE)) return (ULONG)STATUS_ACCESS_VIOLATION;
    PVOID body;
    NTSTATUS st = ObCreateObject(ExEventObjectType, NULL, sizeof(KEVENT), &body);
    if (!NT_SUCCESS(st)) return (ULONG)st;
    KeInitializeEvent(body, ManualReset ? EventNotificationObject : EventSynchronizationObject,
                      InitialState != 0);
    HANDLE h;
    st = ObCreateHandle(PsGetCurrentProcess()->ObjectTable, body, &h);
    ObDereferenceObject(body);                      /* the handle keeps it alive */
    if (NT_SUCCESS(st)) *(HANDLE *)HandleOut = h;
    return (ULONG)st;
}

static ULONG64 NtSetEvent(ULONG64 Handle, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a2; (void)a3; (void)a4;
    PVOID ev;
    NTSTATUS st = ObReferenceObjectByHandle(PsGetCurrentProcess()->ObjectTable, Handle, ExEventObjectType, &ev);
    if (!NT_SUCCESS(st)) return (ULONG)st;
    KeSetEvent(ev);
    ObDereferenceObject(ev);
    return STATUS_SUCCESS;
}

static ULONG64 NtWaitForSingleObject(ULONG64 Handle, ULONG64 TimeoutMs, ULONG64 a3, ULONG64 a4)
{
    (void)a3; (void)a4;
    PVOID obj;
    NTSTATUS st = ObReferenceObjectByHandle(PsGetCurrentProcess()->ObjectTable, Handle, NULL, &obj);
    if (!NT_SUCCESS(st)) return (ULONG)st;
    POBJECT_TYPE type = OBJECT_TO_HEADER(obj)->Type;
    if (type != ExEventObjectType && type != ExMutantObjectType && type != ExSemaphoreObjectType)
        st = STATUS_OBJECT_TYPE_MISMATCH;
    else
        st = KeWaitForSingleObject(obj, (ULONG)TimeoutMs);
    ObDereferenceObject(obj);
    return (ULONG)st;
}

static ULONG64 NtClose(ULONG64 Handle, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return (ULONG)ObCloseHandle(PsGetCurrentProcess()->ObjectTable, Handle);
}

static ULONG64 NtQuerySystemTime(ULONG64 Buffer, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a2; (void)a3; (void)a4;
    if (!PROBE(Buffer, sizeof(SYSTEM_TIME_INFORMATION), TRUE)) return (ULONG)STATUS_ACCESS_VIOLATION;
    TIME_FIELDS t; KeQuerySystemTimeFields(&t);
    SYSTEM_TIME_INFORMATION *o = (PVOID)Buffer;
    o->Year = t.Year; o->Month = t.Month; o->Day = t.Day; o->Hour = t.Hour;
    o->Minute = t.Minute; o->Second = t.Second; o->Weekday = t.Weekday; o->Pad = 0;
    return STATUS_SUCCESS;
}

static ULONG64 NtGetCurrentProcessId(ULONG64 a1, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    return PsGetCurrentProcess()->UniqueProcessId;
}

static ULONG64 NtSetConsoleColor(ULONG64 Color, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    (void)a2; (void)a3; (void)a4;
    ConSetColorForProcess(PsGetCurrentProcess(), (ULONG)Color);
    return STATUS_SUCCESS;
}

/* ---- The service descriptor table ------------------------------------------------------ */
typedef ULONG64 (*KSERVICE_ROUTINE)(ULONG64, ULONG64, ULONG64, ULONG64);
static const KSERVICE_ROUTINE KiServiceTable[SVC_COUNT] = {
    [SVC_NtDisplayString]          = NtDisplayString,
    [SVC_NtTerminateProcess]       = NtTerminateProcess,
    [SVC_NtDelayExecution]         = NtDelayExecution,
    [SVC_NtQuerySystemInformation] = NtQuerySystemInformation,
    [SVC_NtGetTickCount]           = NtGetTickCount,
    [SVC_NtYieldExecution]         = NtYieldExecution,
    [SVC_NtAllocateVirtualMemory]  = NtAllocateVirtualMemory,
    [SVC_NtCreateEvent]            = NtCreateEvent,
    [SVC_NtSetEvent]               = NtSetEvent,
    [SVC_NtWaitForSingleObject]    = NtWaitForSingleObject,
    [SVC_NtClose]                  = NtClose,
    [SVC_NtQuerySystemTime]        = NtQuerySystemTime,
    [SVC_NtGetCurrentProcessId]    = NtGetCurrentProcessId,
    [SVC_NtSetConsoleColor]        = NtSetConsoleColor,
};
const char *const KiServiceNames[SVC_COUNT] = {
    "NtDisplayString", "NtTerminateProcess", "NtDelayExecution", "NtQuerySystemInformation",
    "NtGetTickCount", "NtYieldExecution", "NtAllocateVirtualMemory", "NtCreateEvent",
    "NtSetEvent", "NtWaitForSingleObject", "NtClose", "NtQuerySystemTime",
    "NtGetCurrentProcessId", "NtSetConsoleColor",
};
const ULONG KiServiceLimit = SVC_COUNT;
ULONG64 KiServiceCounts[SVC_COUNT];

/* Reached from KiSystemCall64 at PASSIVE_LEVEL on the thread's kernel stack. */
ULONG64 KiSystemServiceDispatch(ULONG64 A1, ULONG64 A2, ULONG64 A3, ULONG64 A4, ULONG64 Number)
{
    ULONG64 result;
    KeSystemCalls++;
    if (Number >= SVC_COUNT) {
        result = (ULONG)STATUS_INVALID_SYSTEM_SERVICE;
    } else {
        KiServiceCounts[Number]++;
        result = KiServiceTable[Number](A1, A2, A3, A4);
    }
    PEPROCESS p = PsGetCurrentProcess();
    if (p->Terminating) PsTerminateCurrentThread(p->ExitStatus);
    return result;
}
