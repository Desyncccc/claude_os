/* RadiaNT -- ps.h : process and thread manager. */
#pragma once
#include "ntdef.h"
#include "ke.h"
#include "ob.h"

typedef void (*PKSTART_ROUTINE)(PVOID Context);

typedef struct _EPROCESS {
    KPROCESS        Pcb;
    ULONG           UniqueProcessId;
    CHAR            ImageFileName[32];
    LIST_ENTRY      ActiveProcessLinks;
    HANDLE_TABLE   *ObjectTable;
    PVOID           Console;            /* win32k console the process writes to */
    NTSTATUS        ExitStatus;
    BOOLEAN         Terminating;
    BOOLEAN         Exited;
    BOOLEAN         UserMode;
    ULONG64         CreateTime;         /* tick count */
    ULONG64         ImageBase, ImageSize, EntryPoint;
    ULONG64         HeapNext;
    ULONG64         PrivatePages;
} EPROCESS, *PEPROCESS;

typedef struct _ETHREAD {
    KTHREAD         Tcb;
    ULONG           UniqueThreadId;
    CHAR            Name[32];
    LIST_ENTRY      ThreadListEntry;    /* EPROCESS.Pcb.ThreadListHead */
    LIST_ENTRY      ReaperLink;
    PKSTART_ROUTINE StartRoutine;
    PVOID           StartContext;
    ULONG64         UserEntry, UserStack;
    NTSTATUS        ExitStatus;
} ETHREAD, *PETHREAD;

extern POBJECT_TYPE PsProcessType, PsThreadType;
extern PEPROCESS PsInitialSystemProcess, PsIdleProcess;
extern LIST_ENTRY PsActiveProcessHead;
extern ULONG PsProcessCount, PsThreadCount;

static inline PETHREAD  PsGetCurrentThread(void)  { return (PETHREAD)KiCurrentThread; }
static inline PEPROCESS PsGetCurrentProcess(void) { return (PEPROCESS)KiCurrentThread->Process; }

void     PsInitSystem(void);            /* phase 0: idle + system process */
void     PsInitPhase1(void);            /* reaper thread */
NTSTATUS PsCreateSystemThread(PKSTART_ROUTINE Routine, PVOID Context, UCHAR Priority,
                              PCSTR Name, PETHREAD *Thread);
NTSTATUS PsCreateUserProcess(PCSTR ImageName, PVOID Console, PEPROCESS *Process);
NORETURN void PsTerminateCurrentThread(NTSTATUS Status);
NTSTATUS PsTerminateProcess(PEPROCESS Process, NTSTATUS Status);
NTSTATUS PsTerminateProcessById(ULONG Pid, NTSTATUS Status);
void     PsDetachConsole(PVOID Console, BOOLEAN Kill);
NTSTATUS PspLoadImage(PEPROCESS Process, const UCHAR *File, SIZE_T Size);

typedef struct _PS_SNAPSHOT_ENTRY {
    ULONG   Pid, Tid;
    CHAR    ProcessName[32], ThreadName[32];
    UCHAR   State, Priority;
    BOOLEAN UserMode;
    ULONG64 KernelTime, UserTime, ContextSwitches, PrivatePages;
    ULONG   Handles;
} PS_SNAPSHOT_ENTRY;
ULONG PsSnapshot(PS_SNAPSHOT_ENTRY *Out, ULONG Max);

/* System service dispatch (ke/service.c) */
ULONG64 KiSystemServiceDispatch(ULONG64 A1, ULONG64 A2, ULONG64 A3, ULONG64 A4, ULONG64 Number);
extern const char *const KiServiceNames[];
extern const ULONG KiServiceLimit;
extern ULONG64 KiServiceCounts[];
