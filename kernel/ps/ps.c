/*
 * RadiaNT -- ps/ps.c
 * Process and thread manager: EPROCESS/ETHREAD lifetime, system threads,
 * user-mode process creation and the reaper that recycles dead threads.
 */
#include "ps.h"
#include "mm.h"
#include "hal.h"
#include "rtl.h"

POBJECT_TYPE PsProcessType, PsThreadType;
PEPROCESS PsInitialSystemProcess, PsIdleProcess;
LIST_ENTRY PsActiveProcessHead = { &PsActiveProcessHead, &PsActiveProcessHead };
ULONG PsProcessCount, PsThreadCount;

static ULONG      PspNextClientId = 4;      /* PIDs and TIDs share one id space */
static LIST_ENTRY PspReaperList = { &PspReaperList, &PspReaperList };
static KEVENT     PspReaperEvent;

extern char KiBootStack[], KiBootStackTop[];
void KiStartClock(KTHREAD *IdleThread);

static ULONG PspAllocateClientId(void)
{
    ULONG id = PspNextClientId;
    PspNextClientId += 4;
    return id;
}

static PEPROCESS PspCreateProcessObject(PCSTR Name, ULONG Pid, ULONG64 Cr3)
{
    PVOID body;
    if (!NT_SUCCESS(ObCreateObject(PsProcessType, NULL, sizeof(EPROCESS), &body))) return NULL;
    PEPROCESS p = body;
    p->Pcb.Header.Type = ProcessObject;
    InitializeListHead(&p->Pcb.Header.WaitListHead);
    InitializeListHead(&p->Pcb.ThreadListHead);
    p->Pcb.DirectoryTableBase = Cr3;
    p->UniqueProcessId = Pid;
    p->CreateTime = KeTickCount;
    RtlStringCopy(p->ImageFileName, sizeof(p->ImageFileName), Name);
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    InsertTailList(&PsActiveProcessHead, &p->ActiveProcessLinks);
    PsProcessCount++;
    KeLowerIrql(irql);
    return p;
}

void PsInitSystem(void)
{
    ObCreateObjectType("Process", TAG('P', 'r', 'o', 'c'), NULL, &PsProcessType);
    ObCreateObjectType("Thread", TAG('T', 'h', 'r', 'e'), NULL, &PsThreadType);
    KeInitializeEvent(&PspReaperEvent, EventSynchronizationObject, FALSE);

    /* The boot context becomes the idle thread of the Idle process (PID 0). */
    PsIdleProcess = PspCreateProcessObject("Idle", 0, MmSystemPml4);
    PVOID body;
    ObCreateObject(PsThreadType, NULL, sizeof(ETHREAD), &body);
    PETHREAD idle = body;
    idle->Tcb.Header.Type = ThreadObject;
    InitializeListHead(&idle->Tcb.Header.WaitListHead);
    idle->Tcb.Process = &PsIdleProcess->Pcb;
    idle->Tcb.State = Running;
    idle->Tcb.Priority = 0;
    idle->Tcb.StackBase = KiBootStack;
    idle->Tcb.InitialStack = (ULONG64)KiBootStackTop;
    RtlStringCopy(idle->Name, sizeof(idle->Name), "Idle");
    InsertTailList(&PsIdleProcess->Pcb.ThreadListHead, &idle->ThreadListEntry);
    PsIdleProcess->Pcb.ThreadCount = 1;
    PsThreadCount = 1;
    KiCurrentThread = &idle->Tcb;
    KiSetKernelStack(idle->Tcb.InitialStack);

    PsInitialSystemProcess = PspCreateProcessObject("System", PspAllocateClientId(), MmSystemPml4);
    PsInitialSystemProcess->ObjectTable = ObCreateHandleTable();
    KiStartClock(&idle->Tcb);
}

static NTSTATUS PsCreateThread(PEPROCESS Process, PKSTART_ROUTINE Routine, PVOID Context,
                        UCHAR Priority, PCSTR Name, PETHREAD *Thread)
{
    PVOID body;
    NTSTATUS st = ObCreateObject(PsThreadType, NULL, sizeof(ETHREAD), &body);
    if (!NT_SUCCESS(st)) return st;
    PETHREAD t = body;
    KiInitializeThread(&t->Tcb, &Process->Pcb, Priority);
    t->StartRoutine = Routine;
    t->StartContext = Context;
    RtlStringCopy(t->Name, sizeof(t->Name), Name);

    KIRQL irql = KeRaiseIrqlToDpcLevel();
    t->UniqueThreadId = PspAllocateClientId();
    InsertTailList(&Process->Pcb.ThreadListHead, &t->ThreadListEntry);
    Process->Pcb.ThreadCount++;
    PsThreadCount++;
    if (Thread) { OBJECT_TO_HEADER(t)->PointerCount++; *Thread = t; }
    KiReadyThread(&t->Tcb);
    KeLowerIrql(irql);
    return STATUS_SUCCESS;
}

NTSTATUS PsCreateSystemThread(PKSTART_ROUTINE Routine, PVOID Context, UCHAR Priority,
                              PCSTR Name, PETHREAD *Thread)
{
    return PsCreateThread(PsInitialSystemProcess, Routine, Context, Priority, Name, Thread);
}

/* ---- Termination --------------------------------------------------------------------- */
NORETURN void PsTerminateCurrentThread(NTSTATUS Status)
{
    PETHREAD t = PsGetCurrentThread();
    PEPROCESS p = PsGetCurrentProcess();
    KeRaiseIrqlToDpcLevel();
    t->ExitStatus = Status;
    RemoveEntryList(&t->ThreadListEntry);
    t->Tcb.Header.SignalState = 1;
    KeSignalDispatcherObject(&t->Tcb.Header);
    if (--p->Pcb.ThreadCount == 0) {
        if (!p->Terminating) p->ExitStatus = Status;
        p->Exited = TRUE;
        p->Pcb.Header.SignalState = 1;
        KeSignalDispatcherObject(&p->Pcb.Header);
    }
    t->Tcb.State = Terminated;
    InsertTailList(&PspReaperList, &t->ReaperLink);
    PspReaperEvent.Header.SignalState = 1;
    KeSignalDispatcherObject(&PspReaperEvent.Header);
    KiSwapThread();
    KeBugCheckEx(0x00000048 /* a terminated thread was resumed */, (ULONG64)t, 0, 0, 0);
}

NTSTATUS PsTerminateProcess(PEPROCESS Process, NTSTATUS Status)
{
    if (Process == PsInitialSystemProcess || Process == PsIdleProcess) return STATUS_INVALID_PARAMETER;
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    if (!Process->Exited && !Process->Terminating) {
        Process->Terminating = TRUE;
        Process->ExitStatus = Status;
        /* Kick threads out of any wait; they exit on their way back to user mode. */
        for (PLIST_ENTRY e = Process->Pcb.ThreadListHead.Flink; e != &Process->Pcb.ThreadListHead; e = e->Flink) {
            PETHREAD t = CONTAINING_RECORD(e, ETHREAD, ThreadListEntry);
            if (&t->Tcb != KiCurrentThread) KiUnwaitThread(&t->Tcb, STATUS_ALERTED);
        }
    }
    KeLowerIrql(irql);
    if (Process == PsGetCurrentProcess()) PsTerminateCurrentThread(Status);
    return STATUS_SUCCESS;
}

NTSTATUS PsTerminateProcessById(ULONG Pid, NTSTATUS Status)
{
    PEPROCESS target = NULL;
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    for (PLIST_ENTRY e = PsActiveProcessHead.Flink; e != &PsActiveProcessHead; e = e->Flink) {
        PEPROCESS p = CONTAINING_RECORD(e, EPROCESS, ActiveProcessLinks);
        if (p->UniqueProcessId == Pid) { target = p; OBJECT_TO_HEADER(p)->PointerCount++; break; }
    }
    KeLowerIrql(irql);
    if (!target) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = PsTerminateProcess(target, Status);
    ObDereferenceObject(target);
    return st;
}

/* A console window is going away: cut every process loose from it. */
void PsDetachConsole(PVOID Console, BOOLEAN Kill)
{
    for (;;) {
        PEPROCESS victim = NULL;
        KIRQL irql = KeRaiseIrqlToDpcLevel();
        for (PLIST_ENTRY e = PsActiveProcessHead.Flink; e != &PsActiveProcessHead; e = e->Flink) {
            PEPROCESS p = CONTAINING_RECORD(e, EPROCESS, ActiveProcessLinks);
            if (p->Console == Console) {
                p->Console = NULL;
                victim = p; OBJECT_TO_HEADER(p)->PointerCount++;
                break;
            }
        }
        KeLowerIrql(irql);
        if (!victim) break;
        if (Kill) PsTerminateProcess(victim, STATUS_CONTROL_C_EXIT);
        ObDereferenceObject(victim);
    }
}

/* ---- Reaper: frees what a thread cannot free for itself --------------------------------- */
static void PspReaperThread(PVOID Context)
{
    (void)Context;
    for (;;) {
        KeWaitForSingleObject(&PspReaperEvent, INFINITE);
        for (;;) {
            KIRQL irql = KeRaiseIrqlToDpcLevel();
            PETHREAD t = NULL;
            if (!IsListEmpty(&PspReaperList))
                t = CONTAINING_RECORD(RemoveHeadList(&PspReaperList), ETHREAD, ReaperLink);
            KeLowerIrql(irql);
            if (!t) break;

            PEPROCESS p = (PEPROCESS)t->Tcb.Process;
            MmFreeContiguousMemory(t->Tcb.StackBase, KERNEL_STACK_SIZE);
            PsThreadCount--;
            ObDereferenceObject(t);

            if (p->Exited && p->Pcb.ThreadCount == 0) {
                irql = KeRaiseIrqlToDpcLevel();
                RemoveEntryList(&p->ActiveProcessLinks);
                PsProcessCount--;
                KeLowerIrql(irql);
                ObKillHandleTable(p->ObjectTable);
                p->ObjectTable = NULL;
                if (p->UserMode) {
                    MmDestroyAddressSpace(p->Pcb.DirectoryTableBase, NULL);
                    p->Pcb.DirectoryTableBase = MmSystemPml4;
                }
                DbgPrint("PS: process %u (%s) exited with status 0x%08X\n",
                         p->UniqueProcessId, p->ImageFileName, (ULONG)p->ExitStatus);
                ObDereferenceObject(p);
            }
        }
    }
}

void PsInitPhase1(void)
{
    PsCreateSystemThread(PspReaperThread, NULL, 12, "Reaper", NULL);
}

/* ---- User-mode processes ------------------------------------------------------------------ */
static void PspUserThreadStartup(PVOID Context)
{
    (void)Context;
    PETHREAD t = PsGetCurrentThread();
    KiEnterUserMode(t->UserEntry, t->UserStack);
}

static KE_BOOT_MODULE *PspFindImage(PCSTR Name)
{
    char withExt[40];
    RtlStringPrintf(withExt, sizeof(withExt), "%s.exe", Name);
    for (ULONG i = 0; i < KeLoaderBlock.ModuleCount; i++) {
        KE_BOOT_MODULE *m = &KeLoaderBlock.Modules[i];
        if (!RtlCompareStringI(m->Name, Name) || !RtlCompareStringI(m->Name, withExt)) return m;
    }
    return NULL;
}

NTSTATUS PsCreateUserProcess(PCSTR ImageName, PVOID Console, PEPROCESS *Process)
{
    KE_BOOT_MODULE *image = PspFindImage(ImageName);
    if (!image) return STATUS_OBJECT_NAME_NOT_FOUND;

    ULONG64 cr3 = MmCreateAddressSpace();
    if (!cr3) return STATUS_NO_MEMORY;
    PEPROCESS p = PspCreateProcessObject(image->Name, PspAllocateClientId(), cr3);
    if (!p) { MmDestroyAddressSpace(cr3, NULL); return STATUS_NO_MEMORY; }
    p->UserMode = TRUE;
    p->Console = Console;
    p->HeapNext = MM_USER_HEAP_BASE;
    p->ObjectTable = ObCreateHandleTable();

    NTSTATUS st = PspLoadImage(p, MmPhysToVirt(image->Start), image->End - image->Start);
    if (NT_SUCCESS(st))
        st = MmAllocateUserMemory(cr3, MM_USER_STACK_TOP - MM_USER_STACK_SIZE, MM_USER_STACK_SIZE,
                                  PTE_WRITE | PTE_NX, &p->PrivatePages);
    PETHREAD t = NULL;
    if (NT_SUCCESS(st)) {
        /* Microsoft x64 ABI: 32 bytes of home space + a (null) return address. */
        PVOID body;
        st = ObCreateObject(PsThreadType, NULL, sizeof(ETHREAD), &body);
        if (NT_SUCCESS(st)) {
            t = body;
            KiInitializeThread(&t->Tcb, &p->Pcb, 8);
            t->StartRoutine = PspUserThreadStartup;
            t->UserEntry = p->EntryPoint;
            t->UserStack = MM_USER_STACK_TOP - 0x28;
            RtlStringCopy(t->Name, sizeof(t->Name), "Main");
        }
    }
    if (!NT_SUCCESS(st)) {
        KIRQL irql = KeRaiseIrqlToDpcLevel();
        RemoveEntryList(&p->ActiveProcessLinks);
        PsProcessCount--;
        KeLowerIrql(irql);
        ObKillHandleTable(p->ObjectTable);
        MmDestroyAddressSpace(cr3, NULL);
        ObDereferenceObject(p);
        return st;
    }

    KIRQL irql = KeRaiseIrqlToDpcLevel();
    t->UniqueThreadId = PspAllocateClientId();
    InsertTailList(&p->Pcb.ThreadListHead, &t->ThreadListEntry);
    p->Pcb.ThreadCount++;
    PsThreadCount++;
    if (Process) { OBJECT_TO_HEADER(p)->PointerCount++; *Process = p; }
    KiReadyThread(&t->Tcb);
    KeLowerIrql(irql);
    return STATUS_SUCCESS;
}

/* ---- Snapshot for the shell / system monitor ------------------------------------------------- */
ULONG PsSnapshot(PS_SNAPSHOT_ENTRY *Out, ULONG Max)
{
    ULONG n = 0;
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    for (PLIST_ENTRY pe = PsActiveProcessHead.Flink; pe != &PsActiveProcessHead; pe = pe->Flink) {
        PEPROCESS p = CONTAINING_RECORD(pe, EPROCESS, ActiveProcessLinks);
        for (PLIST_ENTRY te = p->Pcb.ThreadListHead.Flink; te != &p->Pcb.ThreadListHead && n < Max; te = te->Flink) {
            PETHREAD t = CONTAINING_RECORD(te, ETHREAD, ThreadListEntry);
            PS_SNAPSHOT_ENTRY *o = &Out[n++];
            o->Pid = p->UniqueProcessId; o->Tid = t->UniqueThreadId;
            RtlStringCopy(o->ProcessName, sizeof(o->ProcessName), p->ImageFileName);
            RtlStringCopy(o->ThreadName, sizeof(o->ThreadName), t->Name);
            o->State = t->Tcb.State; o->Priority = t->Tcb.Priority;
            o->UserMode = p->UserMode;
            o->KernelTime = t->Tcb.KernelTime; o->UserTime = t->Tcb.UserTime;
            o->ContextSwitches = t->Tcb.ContextSwitches;
            o->PrivatePages = p->PrivatePages;
            o->Handles = p->ObjectTable ? p->ObjectTable->HandleCount : 0;
        }
    }
    KeLowerIrql(irql);
    return n;
}
