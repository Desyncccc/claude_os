/*
 * RadiaNT user mode -- ntdll.h
 * The native API as seen from ring 3: thin SYSCALL stubs plus a tiny printf.
 * Programs are freestanding PE32+ images with no imports.
 */
#pragma once
#include "../kernel/include/ntapi.h"

typedef unsigned long long ULONG64;
typedef unsigned int ULONG;
typedef int NTSTATUS;
typedef ULONG64 HANDLE;
#define STATUS_SUCCESS 0
#define STATUS_TIMEOUT 0x102
#define INFINITE 0xFFFFFFFFu

/* RAX = service, R10/RDX/R8/R9 = arguments. The kernel clobbers RCX and R11. */
static inline ULONG64 NtSyscall(ULONG64 n, ULONG64 a1, ULONG64 a2, ULONG64 a3, ULONG64 a4)
{
    register ULONG64 r10 __asm__("r10") = a1;
    register ULONG64 r8 __asm__("r8") = a3;
    register ULONG64 r9 __asm__("r9") = a4;
    __asm__ volatile("syscall"
                     : "+a"(n), "+r"(r10), "+d"(a2), "+r"(r8), "+r"(r9)
                     : : "rcx", "r11", "memory", "cc");
    return n;
}

static inline NTSTATUS NtDisplayString(const char *s, ULONG64 len) { return (NTSTATUS)NtSyscall(SVC_NtDisplayString, (ULONG64)s, len, 0, 0); }
static inline __attribute__((noreturn)) void NtTerminateProcess(NTSTATUS status)
{
    NtSyscall(SVC_NtTerminateProcess, (ULONG64)status, 0, 0, 0);
    for (;;) { }
}
static inline NTSTATUS NtDelayExecution(ULONG ms) { return (NTSTATUS)NtSyscall(SVC_NtDelayExecution, ms, 0, 0, 0); }
static inline NTSTATUS NtQuerySystemInformation(ULONG cls, void *buf, ULONG64 len) { return (NTSTATUS)NtSyscall(SVC_NtQuerySystemInformation, cls, (ULONG64)buf, len, 0); }
static inline ULONG64  NtGetTickCount(void) { return NtSyscall(SVC_NtGetTickCount, 0, 0, 0, 0); }
static inline NTSTATUS NtYieldExecution(void) { return (NTSTATUS)NtSyscall(SVC_NtYieldExecution, 0, 0, 0, 0); }
static inline NTSTATUS NtAllocateVirtualMemory(void **base, ULONG64 size) { return (NTSTATUS)NtSyscall(SVC_NtAllocateVirtualMemory, (ULONG64)base, size, 0, 0); }
static inline NTSTATUS NtCreateEvent(HANDLE *h, ULONG manual, ULONG initial) { return (NTSTATUS)NtSyscall(SVC_NtCreateEvent, (ULONG64)h, manual, initial, 0); }
static inline NTSTATUS NtSetEvent(HANDLE h) { return (NTSTATUS)NtSyscall(SVC_NtSetEvent, h, 0, 0, 0); }
static inline NTSTATUS NtWaitForSingleObject(HANDLE h, ULONG ms) { return (NTSTATUS)NtSyscall(SVC_NtWaitForSingleObject, h, ms, 0, 0); }
static inline NTSTATUS NtClose(HANDLE h) { return (NTSTATUS)NtSyscall(SVC_NtClose, h, 0, 0, 0); }
static inline NTSTATUS NtQuerySystemTime(SYSTEM_TIME_INFORMATION *t) { return (NTSTATUS)NtSyscall(SVC_NtQuerySystemTime, (ULONG64)t, 0, 0, 0); }
static inline ULONG    NtGetCurrentProcessId(void) { return (ULONG)NtSyscall(SVC_NtGetCurrentProcessId, 0, 0, 0, 0); }
static inline NTSTATUS NtSetConsoleColor(ULONG c) { return (NTSTATUS)NtSyscall(SVC_NtSetConsoleColor, c, 0, 0, 0); }

/* crt.c */
void  Print(const char *fmt, ...);
void  PrintColor(ULONG color, const char *fmt, ...);
int   main(void);
