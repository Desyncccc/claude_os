/*
 * RadiaNT -- ke/sched.c
 * IRQL management, DPCs, dispatcher objects and the thread scheduler.
 *
 * The model is NT's, reduced to one processor:
 *   - Code at IRQL >= DISPATCH_LEVEL is never preempted by another thread.
 *   - ISRs do the minimum and queue a DPC; DPCs run at DISPATCH_LEVEL with
 *     interrupts enabled, when the IRQL is about to drop below DISPATCH.
 *   - The dispatcher database (ready queues, wait lists, timers) is only
 *     touched at DISPATCH_LEVEL, so it needs no further locking.
 *   - 32 priority levels, round robin within a level, preemptive.
 */
#include "ke.h"
#include "hal.h"
#include "mm.h"
#include "ps.h"
#include "rtl.h"

volatile KIRQL KiIrql = PASSIVE_LEVEL;
KTHREAD *KiCurrentThread;
static KTHREAD *KiIdleThread;

static LIST_ENTRY KiReadyQueues[MAXIMUM_PRIORITY];
static ULONG      KiReadySummary;
static LIST_ENTRY KiTimerList;
static LIST_ENTRY KiDpcList;
static volatile BOOLEAN KiReschedPending;
static ULONG64    KiCurrentCr3;

volatile ULONG64 KeTickCount, KeIdleTicks;
ULONG64 KeContextSwitches, KeDpcCount;

/* ---- IRQL --------------------------------------------------------------------- */
KIRQL KeRaiseIrqlToDpcLevel(void)
{
    KIRQL old = KiIrql;
    if (old < DISPATCH_LEVEL) KiIrql = DISPATCH_LEVEL;
    return old;
}

void KeLowerIrql(KIRQL NewIrql)
{
    KiIrql = NewIrql;
    if (NewIrql < DISPATCH_LEVEL && (KiReschedPending || !IsListEmpty(&KiDpcList)))
        KiProcessSoftwareInterrupts(NewIrql);
}

void KeAcquireSpinLock(KSPIN_LOCK *Lock, KIRQL *OldIrql)
{
    *OldIrql = KeRaiseIrqlToDpcLevel();
    if (Lock->Held) KeBugCheckEx(0x0000000F /* SPIN_LOCK_ALREADY_OWNED */, (ULONG64)Lock, 0, 0, 0);
    Lock->Held = 1;
}

void KeReleaseSpinLock(KSPIN_LOCK *Lock, KIRQL OldIrql)
{
    Lock->Held = 0;
    KeLowerIrql(OldIrql);
}

/* ---- DPCs ---------------------------------------------------------------------- */
void KeInitializeDpc(KDPC *Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
{
    Dpc->DeferredRoutine = Routine;
    Dpc->DeferredContext = Context;
    Dpc->Inserted = FALSE;
}

BOOLEAN KeInsertQueueDpc(KDPC *Dpc)
{
    ULONG64 f = HalSaveFlagsAndCli();
    BOOLEAN inserted = FALSE;
    if (!Dpc->Inserted) {
        Dpc->Inserted = TRUE;
        InsertTailList(&KiDpcList, &Dpc->DpcListEntry);
        inserted = TRUE;
    }
    HalRestoreFlags(f);
    return inserted;
}

/* Entered and left with interrupts disabled, IRQL == DISPATCH_LEVEL. */
static void KiRetireDpcList(void)
{
    while (!IsListEmpty(&KiDpcList)) {
        KDPC *dpc = CONTAINING_RECORD(RemoveHeadList(&KiDpcList), KDPC, DpcListEntry);
        dpc->Inserted = FALSE;
        HalEnableInterrupts();
        dpc->DeferredRoutine(dpc, dpc->DeferredContext);
        KeDpcCount++;
        HalDisableInterrupts();
    }
}

/*
 * The software interrupt dispatcher. Called whenever the IRQL is about to
 * drop below DISPATCH_LEVEL (from KeLowerIrql or from the tail of a hardware
 * interrupt): drains the DPC queue, then honours a pending reschedule.
 */
void KiProcessSoftwareInterrupts(KIRQL TargetIrql)
{
    ULONG64 f = HalSaveFlagsAndCli();
    for (;;) {
        if (!IsListEmpty(&KiDpcList)) {
            KiIrql = DISPATCH_LEVEL;
            KiRetireDpcList();
            continue;
        }
        if (KiReschedPending && TargetIrql < DISPATCH_LEVEL) {
            KiIrql = DISPATCH_LEVEL;
            KiSwapThread();
            continue;
        }
        break;
    }
    KiIrql = TargetIrql;
    HalRestoreFlags(f);
}

/* ---- Scheduler ------------------------------------------------------------------ */
void KiReadyThread(KTHREAD *Thread)
{
    Thread->State = Ready;
    InsertTailList(&KiReadyQueues[Thread->Priority], &Thread->QueueLink);
    KiReadySummary |= 1u << Thread->Priority;
    if (KiCurrentThread == KiIdleThread || Thread->Priority > KiCurrentThread->Priority)
        KiReschedPending = TRUE;
}

static KTHREAD *KiSelectNextThread(void)
{
    if (!KiReadySummary) return KiIdleThread;
    int pri = 31 - __builtin_clz(KiReadySummary);
    KTHREAD *t = CONTAINING_RECORD(RemoveHeadList(&KiReadyQueues[pri]), KTHREAD, QueueLink);
    if (IsListEmpty(&KiReadyQueues[pri])) KiReadySummary &= ~(1u << pri);
    return t;
}

/*
 * Give up the processor. The caller is at DISPATCH_LEVEL and has already put
 * the current thread into its new state (Waiting / Terminated), or left it
 * Running for a preemption or quantum-end check.
 */
void KiSwapThread(void)
{
    ULONG64 f = HalSaveFlagsAndCli();
    KTHREAD *old = KiCurrentThread;
    KiReschedPending = FALSE;

    if (old->State == Running) {
        if (old != KiIdleThread) {
            int top = KiReadySummary ? 31 - __builtin_clz(KiReadySummary) : -1;
            if (top < (int)old->Priority || (top == (int)old->Priority && old->Quantum > 0)) {
                if (old->Quantum <= 0) old->Quantum = THREAD_QUANTUM;
                HalRestoreFlags(f);
                return;
            }
            old->State = Ready;
            InsertTailList(&KiReadyQueues[old->Priority], &old->QueueLink);
            KiReadySummary |= 1u << old->Priority;
        } else if (!KiReadySummary) {
            HalRestoreFlags(f);
            return;
        } else {
            old->State = Ready;             /* the idle thread is never queued */
        }
    }

    KTHREAD *next = KiSelectNextThread();
    next->State = Running;
    next->Quantum = THREAD_QUANTUM;
    if (next != old) {
        KiCurrentThread = next;
        next->ContextSwitches++;
        KeContextSwitches++;
        if (next->Process->DirectoryTableBase != KiCurrentCr3) {
            KiCurrentCr3 = next->Process->DirectoryTableBase;
            HalWriteCr3(KiCurrentCr3);
        }
        KiSetKernelStack(next->InitialStack);
        KiSwapContext(old, next);
    }
    HalRestoreFlags(f);
}

void KeYieldExecution(void)
{
    KIRQL old = KeRaiseIrqlToDpcLevel();
    KiCurrentThread->Quantum = 0;
    KiSwapThread();
    KeLowerIrql(old);
}

/* First C code of every new thread (reached through KiThreadTrampoline). */
void KiThreadStartup(void)
{
    PETHREAD thread = PsGetCurrentThread();
    KiIrql = PASSIVE_LEVEL;
    HalEnableInterrupts();
    thread->StartRoutine(thread->StartContext);
    PsTerminateCurrentThread(STATUS_SUCCESS);
}

void KiInitializeThread(KTHREAD *Thread, KPROCESS *Process, UCHAR Priority)
{
    Thread->Header.Type = ThreadObject;
    Thread->Header.SignalState = 0;
    InitializeListHead(&Thread->Header.WaitListHead);
    Thread->Process = Process;
    Thread->Priority = Priority;
    Thread->State = Initialized;
    Thread->Quantum = THREAD_QUANTUM;
    Thread->StackBase = MmAllocateContiguousMemory(KERNEL_STACK_SIZE);
    if (!Thread->StackBase) KeBugCheckEx(NO_MORE_SYSTEM_MEMORY, KERNEL_STACK_SIZE, 0, 0, 0);
    Thread->InitialStack = (ULONG64)Thread->StackBase + KERNEL_STACK_SIZE;

    /* Frame consumed by KiSwapContext: r15 r14 r13 r12 rbx rbp, return address. */
    ULONG64 *sp = (ULONG64 *)Thread->InitialStack;
    *--sp = 0;
    *--sp = (ULONG64)KiThreadTrampoline;
    for (int i = 0; i < 6; i++) *--sp = 0;
    Thread->KernelStack = (ULONG64)sp;
}

/* ---- Waits ------------------------------------------------------------------------ */
void KiUnwaitThread(KTHREAD *Thread, NTSTATUS Status)
{
    if (Thread->State != Waiting) return;
    if (Thread->WaitObject) {
        RemoveEntryList(&Thread->WaitLink);
        Thread->WaitObject = NULL;
    }
    if (Thread->TimerActive) {
        RemoveEntryList(&Thread->TimerLink);
        Thread->TimerActive = FALSE;
    }
    Thread->WaitStatus = Status;
    KiReadyThread(Thread);
}

static void KiSatisfyWait(DISPATCHER_HEADER *h, KTHREAD *Thread)
{
    switch (h->Type) {
    case EventSynchronizationObject: h->SignalState = 0; break;
    case SemaphoreObject:            h->SignalState--; break;
    case MutantObject: {
        KMUTANT *m = (KMUTANT *)h;
        h->SignalState = 0; m->OwnerThread = Thread; m->RecursionCount = 1;
        break;
    }
    default: break;                 /* notification events, processes, threads */
    }
}

void KeSignalDispatcherObject(DISPATCHER_HEADER *h)
{
    while (h->SignalState > 0 && !IsListEmpty(&h->WaitListHead)) {
        KTHREAD *t = CONTAINING_RECORD(h->WaitListHead.Flink, KTHREAD, WaitLink);
        KiSatisfyWait(h, t);
        KiUnwaitThread(t, STATUS_SUCCESS);
    }
}

NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG TimeoutMs)
{
    DISPATCHER_HEADER *h = Object;
    KTHREAD *t = KiCurrentThread;
    KIRQL old = KeRaiseIrqlToDpcLevel();

    if (h->Type == MutantObject && ((KMUTANT *)h)->OwnerThread == t) {
        ((KMUTANT *)h)->RecursionCount++;
        KeLowerIrql(old);
        return STATUS_SUCCESS;
    }
    if (h->SignalState > 0) {
        KiSatisfyWait(h, t);
        KeLowerIrql(old);
        return STATUS_SUCCESS;
    }
    if (TimeoutMs == 0) { KeLowerIrql(old); return STATUS_TIMEOUT; }
    if (old >= DISPATCH_LEVEL)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, (ULONG64)Object, old, 0, (ULONG64)__builtin_return_address(0));

    t->State = Waiting;
    t->WaitObject = h;
    t->WaitStatus = STATUS_SUCCESS;
    InsertTailList(&h->WaitListHead, &t->WaitLink);
    if (TimeoutMs != INFINITE) {
        t->DueTime = KeTickCount + TimeoutMs;
        t->TimerActive = TRUE;
        InsertTailList(&KiTimerList, &t->TimerLink);
    }
    KiSwapThread();
    NTSTATUS status = t->WaitStatus;
    KeLowerIrql(old);
    return status;
}

NTSTATUS KeDelayExecutionThread(ULONG Milliseconds)
{
    if (Milliseconds == 0) { KeYieldExecution(); return STATUS_SUCCESS; }
    KTHREAD *t = KiCurrentThread;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (old >= DISPATCH_LEVEL)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, 0, old, 1, (ULONG64)__builtin_return_address(0));
    t->State = Waiting;
    t->WaitObject = NULL;
    t->WaitStatus = STATUS_SUCCESS;
    t->DueTime = KeTickCount + Milliseconds;
    t->TimerActive = TRUE;
    InsertTailList(&KiTimerList, &t->TimerLink);
    KiSwapThread();
    NTSTATUS status = t->WaitStatus;
    KeLowerIrql(old);
    return status;
}

/* ---- Events, semaphores, mutants ---------------------------------------------------- */
void KeInitializeEvent(KEVENT *Event, KOBJECTS Type, BOOLEAN State)
{
    Event->Header.Type = (UCHAR)Type;
    Event->Header.SignalState = State ? 1 : 0;
    InitializeListHead(&Event->Header.WaitListHead);
}

LONG KeSetEvent(KEVENT *Event)
{
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG prev = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    KeSignalDispatcherObject(&Event->Header);
    KeLowerIrql(old);
    return prev;
}

LONG KeResetEvent(KEVENT *Event)
{
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG prev = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    KeLowerIrql(old);
    return prev;
}

void KeInitializeSemaphore(KSEMAPHORE *Sem, LONG Count, LONG Limit)
{
    Sem->Header.Type = SemaphoreObject;
    Sem->Header.SignalState = Count;
    Sem->Limit = Limit;
    InitializeListHead(&Sem->Header.WaitListHead);
}

LONG KeReleaseSemaphore(KSEMAPHORE *Sem, LONG Adjustment)
{
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG prev = Sem->Header.SignalState;
    Sem->Header.SignalState = MIN(prev + Adjustment, Sem->Limit);
    KeSignalDispatcherObject(&Sem->Header);
    KeLowerIrql(old);
    return prev;
}

void KeInitializeMutant(KMUTANT *Mutant)
{
    Mutant->Header.Type = MutantObject;
    Mutant->Header.SignalState = 1;
    Mutant->OwnerThread = NULL;
    Mutant->RecursionCount = 0;
    InitializeListHead(&Mutant->Header.WaitListHead);
}

LONG KeReleaseMutant(KMUTANT *Mutant)
{
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (Mutant->OwnerThread == KiCurrentThread && --Mutant->RecursionCount == 0) {
        Mutant->OwnerThread = NULL;
        Mutant->Header.SignalState = 1;
        KeSignalDispatcherObject(&Mutant->Header);
    }
    KeLowerIrql(old);
    return 0;
}

/* ---- Clock ------------------------------------------------------------------------------ */
static KDPC KiClockDpc;
BOOLEAN KiInterruptedUserMode;       /* set by KiTrapDispatch for the clock ISR */

/* DISPATCH_LEVEL half of the clock: expire timed waits, account the quantum. */
static void KiClockDpcRoutine(KDPC *Dpc, PVOID Context)
{
    (void)Dpc; (void)Context;
    ULONG64 now = KeTickCount;
    for (PLIST_ENTRY e = KiTimerList.Flink; e != &KiTimerList; ) {
        KTHREAD *t = CONTAINING_RECORD(e, KTHREAD, TimerLink);
        e = e->Flink;
        if (t->DueTime <= now) KiUnwaitThread(t, t->WaitObject ? STATUS_TIMEOUT : STATUS_SUCCESS);
    }
    KTHREAD *cur = KiCurrentThread;
    if (cur != KiIdleThread && --cur->Quantum <= 0) KiReschedPending = TRUE;
}

/* CLOCK_LEVEL half: runs 1000 times a second straight from IRQ0. */
static void KiClockInterrupt(PVOID Context)
{
    (void)Context;
    KeTickCount++;
    KTHREAD *cur = KiCurrentThread;
    if (cur == KiIdleThread) KeIdleTicks++;
    else if (KiInterruptedUserMode) cur->UserTime++;
    else cur->KernelTime++;
    KeInsertQueueDpc(&KiClockDpc);
}

void KiInitializeScheduler(void)
{
    for (int i = 0; i < MAXIMUM_PRIORITY; i++) InitializeListHead(&KiReadyQueues[i]);
    InitializeListHead(&KiTimerList);
    InitializeListHead(&KiDpcList);
    KeInitializeDpc(&KiClockDpc, KiClockDpcRoutine, NULL);
}

/* Called once Ps has built the idle thread around the boot context. */
void KiStartClock(KTHREAD *IdleThread)
{
    KiIdleThread = IdleThread;
    KiCurrentCr3 = HalReadCr3();
    HalInitializeClock(1000);
    HalConnectInterrupt(0, KiClockInterrupt, NULL, "Clock (i8254)");
}

NORETURN void KiIdleLoop(void)
{
    for (;;) {
        HalEnableInterrupts();
        HalHalt();
    }
}
