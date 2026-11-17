/*
 * RadiaNT -- ntapi.h : the native system service interface.
 * Shared verbatim between the kernel (GCC, SysV ABI) and user-mode PE images
 * (clang, Microsoft x64 ABI), so it only uses plain C scalar types.
 *
 * Calling convention (same register protocol NT uses on x64):
 *     RAX = service number, R10/RDX/R8/R9 = arguments, SYSCALL, RAX = NTSTATUS
 */
#pragma once

#define SVC_NtDisplayString             0
#define SVC_NtTerminateProcess          1
#define SVC_NtDelayExecution            2
#define SVC_NtQuerySystemInformation    3
#define SVC_NtGetTickCount              4
#define SVC_NtYieldExecution            5
#define SVC_NtAllocateVirtualMemory     6
#define SVC_NtCreateEvent               7
#define SVC_NtSetEvent                  8
#define SVC_NtWaitForSingleObject       9
#define SVC_NtClose                     10
#define SVC_NtQuerySystemTime           11
#define SVC_NtGetCurrentProcessId       12
#define SVC_NtSetConsoleColor           13
#define SVC_COUNT                       14

#define SystemBasicInformation 0
typedef struct _SYSTEM_BASIC_INFORMATION {
    unsigned int       MajorVersion, MinorVersion, BuildNumber;
    unsigned int       NumberOfProcessors;
    unsigned int       PageSize;
    unsigned int       ProcessCount, ThreadCount;
    unsigned int       Reserved;
    unsigned long long TotalPhysicalPages, FreePhysicalPages;
    unsigned long long TickCount;
    char               ProcessorName[64];
} SYSTEM_BASIC_INFORMATION;

typedef struct _SYSTEM_TIME_INFORMATION {
    unsigned short Year, Month, Day, Hour, Minute, Second, Weekday, Pad;
} SYSTEM_TIME_INFORMATION;

/* Console colour indices accepted by NtSetConsoleColor */
#define CON_DEFAULT 0
#define CON_DIM     1
#define CON_PINK    2
#define CON_ORANGE  3
#define CON_CYAN    4
#define CON_GREEN   5
#define CON_RED     6
#define CON_VIOLET  7
#define CON_WHITE   8
#define CON_YELLOW  9
