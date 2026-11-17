/*
 * RadiaNT -- ke.h : the kernel proper.
 * IRQL, DPCs, dispatcher objects, the thread scheduler, traps and bugchecks.
 */
#pragma once
#include "ntdef.h"

/* ---- Interrupt request levels ---- */
typedef UCHAR KIRQL;
#define PASSIVE_LEVEL   0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2
#define DEVICE_LEVEL    5       /* all device interrupts in this build */
#define CLOCK_LEVEL     13
#define HIGH_LEVEL      15

extern volatile KIRQL KiIrql;
static inline KIRQL KeGetCurrentIrql(void) { return KiIrql; }
KIRQL KeRaiseIrqlToDpcLevel(void);
void  KeLowerIrql(KIRQL NewIrql);

/* Uniprocessor spin lock: acquiring raises to DISPATCH_LEVEL. */
typedef struct _KSPIN_LOCK { volatile ULONG Held; } KSPIN_LOCK;
static inline void KeInitializeSpinLock(KSPIN_LOCK *l) { l->Held = 0; }
void KeAcquireSpinLock(KSPIN_LOCK *Lock, KIRQL *OldIrql);
void KeReleaseSpinLock(KSPIN_LOCK *Lock, KIRQL OldIrql);

/* ---- Deferred procedure calls ---- */
struct _KDPC;
typedef void (*PKDEFERRED_ROUTINE)(struct _KDPC *Dpc, PVOID Context);
typedef struct _KDPC {
    LIST_ENTRY          DpcListEntry;
    PKDEFERRED_ROUTINE  DeferredRoutine;
    PVOID               DeferredContext;
    BOOLEAN             Inserted;
} KDPC;
void    KeInitializeDpc(KDPC *Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context);
BOOLEAN KeInsertQueueDpc(KDPC *Dpc);
extern ULONG64 KeDpcCount;

/* ---- Dispatcher objects ---- */
typedef enum _KOBJECTS {
    EventNotificationObject = 0,
    EventSynchronizationObject = 1,
    MutantObject = 2,
    ProcessObject = 3,
    SemaphoreObject = 5,
    ThreadObject = 6,
} KOBJECTS;

typedef struct _DISPATCHER_HEADER {
    UCHAR      Type;
    LONG       SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct _KEVENT     { DISPATCHER_HEADER Header; } KEVENT;
typedef struct _KSEMAPHORE { DISPATCHER_HEADER Header; LONG Limit; } KSEMAPHORE;
struct _KTHREAD;
typedef struct _KMUTANT {
    DISPATCHER_HEADER Header;
    struct _KTHREAD  *OwnerThread;
    ULONG             RecursionCount;
} KMUTANT;

void KeInitializeEvent(KEVENT *Event, KOBJECTS Type, BOOLEAN State);
LONG KeSetEvent(KEVENT *Event);
LONG KeResetEvent(KEVENT *Event);
void KeInitializeSemaphore(KSEMAPHORE *Sem, LONG Count, LONG Limit);
LONG KeReleaseSemaphore(KSEMAPHORE *Sem, LONG Adjustment);
void KeInitializeMutant(KMUTANT *Mutant);
LONG KeReleaseMutant(KMUTANT *Mutant);
NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG TimeoutMs);
NTSTATUS KeDelayExecutionThread(ULONG Milliseconds);
void     KeSignalDispatcherObject(DISPATCHER_HEADER *Header);   /* at DISPATCH */

/* ---- Threads ---- */
typedef enum _KTHREAD_STATE {
    Initialized, Ready, Running, Waiting, Terminated
} KTHREAD_STATE;

#define MAXIMUM_PRIORITY    32
#define LOW_REALTIME_PRIORITY 16
#define KERNEL_STACK_SIZE   0x8000
#define THREAD_QUANTUM      20          /* clock ticks (ms) */

struct _KPROCESS;
typedef struct _KTHREAD {
    ULONG64             KernelStack;    /* saved RSP -- must stay at offset 0 */
    DISPATCHER_HEADER   Header;         /* signalled when the thread exits */
    PVOID               StackBase;
    ULONG64             InitialStack;   /* top of kernel stack (TSS.RSP0) */
    UCHAR               State;
    UCHAR               Priority;
    LONG                Quantum;
    LIST_ENTRY          QueueLink;      /* ready queue linkage */
    LIST_ENTRY          WaitLink;       /* dispatcher object wait list */
    DISPATCHER_HEADER  *WaitObject;
    NTSTATUS            WaitStatus;
    LIST_ENTRY          TimerLink;
    ULONG64             DueTime;
    BOOLEAN             TimerActive;
    struct _KPROCESS   *Process;
    ULONG64             KernelTime;     /* ticks */
    ULONG64             UserTime;
    ULONG64             ContextSwitches;
} KTHREAD;

typedef struct _KPROCESS {
    DISPATCHER_HEADER   Header;         /* signalled when the process exits */
    ULONG64             DirectoryTableBase;
    LIST_ENTRY          ThreadListHead;
    ULONG               ThreadCount;
} KPROCESS;

extern KTHREAD *KiCurrentThread;
static inline KTHREAD *KeGetCurrentThread(void) { return KiCurrentThread; }

void KiInitializeScheduler(void);
void KiInitializeThread(KTHREAD *Thread, struct _KPROCESS *Process, UCHAR Priority);
void KiReadyThread(KTHREAD *Thread);            /* at DISPATCH_LEVEL */
void KiSwapThread(void);                        /* at DISPATCH_LEVEL */
void KiUnwaitThread(KTHREAD *Thread, NTSTATUS Status);
void KiProcessSoftwareInterrupts(KIRQL TargetIrql);
void KeYieldExecution(void);
NORETURN void KiIdleLoop(void);

extern volatile ULONG64 KeTickCount;            /* 1 tick == 1 ms */
extern volatile ULONG64 KeIdleTicks;
extern ULONG64 KeContextSwitches;
extern ULONG64 KeInterruptCount;
extern ULONG64 KeSystemCalls;
extern ULONG64 KeExceptionCount;

/* ---- Traps ---- */
typedef struct _KTRAP_FRAME {
    ULONG64 R15, R14, R13, R12, R11, R10, R9, R8;
    ULONG64 Rbp, Rdi, Rsi, Rdx, Rcx, Rbx, Rax;
    ULONG64 Vector, ErrorCode;
    ULONG64 Rip, SegCs, EFlags, Rsp, SegSs;
} KTRAP_FRAME;

void KiInitializeProcessor(void);       /* GDT, IDT, TSS, SYSCALL MSRs */
void KiSetKernelStack(ULONG64 Top);     /* TSS.RSP0 + syscall stack */
void KiTrapDispatch(KTRAP_FRAME *Frame);

/* ---- Bugchecks ---- */
#define IRQL_NOT_LESS_OR_EQUAL          0x0000000A
#define KMODE_EXCEPTION_NOT_HANDLED     0x0000001E
#define PAGE_FAULT_IN_NONPAGED_AREA     0x00000050
#define UNEXPECTED_KERNEL_MODE_TRAP     0x0000007F
#define BAD_POOL_HEADER                 0x00000019
#define NO_MORE_SYSTEM_MEMORY           0x0000002D
#define MANUALLY_INITIATED_CRASH        0x000000E2
#define PHASE0_INITIALIZATION_FAILED    0x00000031
#define ASSERTION_FAILURE               0x000000A0
NORETURN void KeBugCheckEx(ULONG Code, ULONG64 P1, ULONG64 P2, ULONG64 P3, ULONG64 P4);
NORETURN void KiBugCheckWithFrame(ULONG Code, ULONG64 P1, ULONG64 P2, ULONG64 P3,
                                  ULONG64 P4, KTRAP_FRAME *Frame);

/* ---- Boot information (copied out of the Multiboot2 structure) ---- */
#define KE_MAX_MEMORY_RUNS  64
#define KE_MAX_BOOT_MODULES 16
typedef struct _KE_MEMORY_RUN { ULONG64 Base, Length; ULONG Type; } KE_MEMORY_RUN;
typedef struct _KE_BOOT_MODULE { ULONG64 Start, End; CHAR Name[32]; } KE_BOOT_MODULE;
typedef struct _KE_LOADER_BLOCK {
    CHAR            LoaderName[48];
    CHAR            CommandLine[96];
    ULONG           MemoryRunCount;
    KE_MEMORY_RUN   MemoryRuns[KE_MAX_MEMORY_RUNS];
    ULONG           ModuleCount;
    KE_BOOT_MODULE  Modules[KE_MAX_BOOT_MODULES];
    BOOLEAN         FirmwareIsEfi;
} KE_LOADER_BLOCK;
extern KE_LOADER_BLOCK KeLoaderBlock;

/* Assembly entry points (ke/amd64.asm) */
void KiSwapContext(KTHREAD *Old, KTHREAD *New);
void KiThreadTrampoline(void);
NORETURN void KiEnterUserMode(ULONG64 Rip, ULONG64 Rsp);
void KiSystemCall64(void);
extern ULONG64 KiIsrStubTable[256];
