/*
 * RadiaNT -- ntdef.h
 * Fundamental types, status codes and list primitives shared by every
 * executive component. Naming follows the NT conventions (ULONG, NTSTATUS,
 * LIST_ENTRY, ...) but every line here is original code.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define RADIANT_NAME        "RadiaNT"
#define RADIANT_VER_MAJOR   0
#define RADIANT_VER_MINOR   1
#define RADIANT_BUILD       2611
#define RADIANT_VERSION_STR "0.1.2611"
#define RADIANT_CODENAME    "First Light"

typedef void            VOID, *PVOID;
typedef char            CHAR, *PCHAR;
typedef const char     *PCSTR;
typedef uint8_t         UCHAR, BOOLEAN;
typedef uint16_t        USHORT;
typedef int32_t         LONG;
typedef uint32_t        ULONG;
typedef long long           LONG64, LONGLONG, LONG_PTR;
typedef unsigned long long  ULONG64, ULONGLONG, ULONG_PTR, SIZE_T;
typedef ULONG_PTR       HANDLE;
typedef int32_t         NTSTATUS;

#define TRUE  1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif

#define NT_SUCCESS(s)                   ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000)
#define STATUS_ALERTED                  ((NTSTATUS)0x00000101)
#define STATUS_TIMEOUT                  ((NTSTATUS)0x00000102)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002)
#define STATUS_ACCESS_VIOLATION         ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_HANDLE           ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017)
#define STATUS_INVALID_SYSTEM_SERVICE   ((NTSTATUS)0xC000001C)
#define STATUS_ILLEGAL_INSTRUCTION      ((NTSTATUS)0xC000001D)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)0xC0000024)
#define STATUS_OBJECT_NAME_INVALID      ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)0xC0000035)
#define STATUS_INVALID_IMAGE_FORMAT     ((NTSTATUS)0xC000007B)
#define STATUS_INTEGER_DIVIDE_BY_ZERO   ((NTSTATUS)0xC0000094)
#define STATUS_PRIVILEGED_INSTRUCTION   ((NTSTATUS)0xC0000096)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009A)
#define STATUS_PROCESS_IS_TERMINATING   ((NTSTATUS)0xC000010A)
#define STATUS_CONTROL_C_EXIT           ((NTSTATUS)0xC000013A)

#define INFINITE 0xFFFFFFFFu

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)  (((x) + ((a) - 1)) & ~((ULONG64)(a) - 1))
#define ALIGN_DOWN(x,a) ((x) & ~((ULONG64)(a) - 1))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define NORETURN        __attribute__((noreturn))
#define PACKED          __attribute__((packed))

/* ---- Doubly linked lists (the backbone of every NT-style structure) ---- */
typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

#define CONTAINING_RECORD(addr, type, field) \
    ((type *)((char *)(addr) - offsetof(type, field)))

static inline void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
static inline BOOLEAN IsListEmpty(const LIST_ENTRY *h) { return h->Flink == h; }
static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e)
{
    e->Flink = h; e->Blink = h->Blink; h->Blink->Flink = e; h->Blink = e;
}
static inline void InsertHeadList(PLIST_ENTRY h, PLIST_ENTRY e)
{
    e->Blink = h; e->Flink = h->Flink; h->Flink->Blink = e; h->Flink = e;
}
static inline void RemoveEntryList(PLIST_ENTRY e)
{
    e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink;
    e->Flink = e->Blink = e;
}
static inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h)
{
    PLIST_ENTRY e = h->Flink; RemoveEntryList(e); return e;
}
