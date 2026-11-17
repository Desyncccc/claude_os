/* RadiaNT -- mm.h : memory manager (PFN database, paging, pool). */
#pragma once
#include "ntdef.h"

#define PAGE_SIZE   4096ull
#define PAGE_SHIFT  12

/* Kernel virtual layout */
#define MM_PHYSMAP_BASE     0xFFFF800000000000ull  /* all RAM, 2 MB pages   */
#define MM_KERNEL_BASE      0xFFFFFFFF80000000ull  /* kernel image (-2 GB)  */
#define MM_USER_LIMIT       0x0000800000000000ull
#define MM_USER_STACK_TOP   0x00007FFFFFFE0000ull
#define MM_USER_STACK_SIZE  0x10000ull
#define MM_USER_HEAP_BASE   0x0000000010000000ull

#define PTE_PRESENT 0x001ull
#define PTE_WRITE   0x002ull
#define PTE_USER    0x004ull
#define PTE_LARGE   0x080ull
#define PTE_NX      0x8000000000000000ull
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

static inline PVOID   MmPhysToVirt(ULONG64 pa) { return (PVOID)(pa + MM_PHYSMAP_BASE); }
static inline ULONG64 MmVirtToPhys(PVOID va)
{
    ULONG64 v = (ULONG64)va;
    return v >= MM_KERNEL_BASE ? v - MM_KERNEL_BASE : v - MM_PHYSMAP_BASE;
}

void    MmInitSystem(void);
ULONG64 MmAllocatePages(ULONG64 Count);     /* contiguous, returns physical or 0 */
void    MmFreePages(ULONG64 PhysicalAddress, ULONG64 Count);
PVOID   MmAllocateContiguousMemory(SIZE_T Bytes);    /* zeroed, virtual */
void    MmFreeContiguousMemory(PVOID Va, SIZE_T Bytes);
PVOID   MmMapIoSpace(ULONG64 PhysicalAddress, SIZE_T Bytes);

extern ULONG64 MmTotalPages, MmFreePagesCount, MmHighestPhysicalPage;
extern ULONG64 MmKernelImagePages;

/* Address spaces */
ULONG64  MmCreateAddressSpace(void);                 /* returns PML4 physical */
void     MmDestroyAddressSpace(ULONG64 Pml4, ULONG64 *PagesFreed);
NTSTATUS MmMapUserPage(ULONG64 Pml4, ULONG64 Va, ULONG64 Pa, ULONG64 Flags);
ULONG64 *MmGetPte(ULONG64 Pml4, ULONG64 Va);         /* NULL if not mapped */
BOOLEAN  MmIsUserRangeValid(ULONG64 Pml4, ULONG64 Va, SIZE_T Length, BOOLEAN Write);
NTSTATUS MmAllocateUserMemory(ULONG64 Pml4, ULONG64 Va, SIZE_T Length, ULONG64 Flags,
                              ULONG64 *PagesCommitted);
NTSTATUS MmCopyToUserSpace(ULONG64 Pml4, ULONG64 Va, const void *Src, SIZE_T Length);
extern ULONG64 MmSystemPml4;

/* Executive pool */
#define TAG(a,b,c,d) ((ULONG)(a) | ((ULONG)(b) << 8) | ((ULONG)(c) << 16) | ((ULONG)(d) << 24))
PVOID ExAllocatePoolWithTag(SIZE_T Bytes, ULONG Tag);    /* zeroed */
void  ExFreePool(PVOID P);
typedef struct _POOL_TAG_INFO { ULONG Tag; ULONG Allocs; ULONG Frees; ULONG64 Bytes; } POOL_TAG_INFO;
ULONG ExQueryPoolTags(POOL_TAG_INFO *Out, ULONG Max);
extern ULONG64 ExPoolBytesInUse, ExPoolBytesTotal;
