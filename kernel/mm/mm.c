/*
 * RadiaNT -- mm/mm.c
 * Memory manager: the PFN bitmap, the physical map, per-process address
 * spaces and user-range probing.
 */
#include "mm.h"
#include "ke.h"
#include "hal.h"
#include "rtl.h"

extern char KiImageStart[], KiImageEnd[];
extern ULONG64 KiBootPml4, KiBootPdptLow;
extern BOOLEAN KiNxEnabled;

ULONG64 MmTotalPages, MmFreePagesCount, MmHighestPhysicalPage, MmKernelImagePages;
ULONG64 MmSystemPml4;

static ULONG64 *MmPfnBitmap;        /* 1 bit per page, 1 == in use */
static ULONG64  MmPfnCount;
static ULONG64  MmSearchHint;
static KSPIN_LOCK MmPfnLock;

#define BIT_TEST(i)  (MmPfnBitmap[(i) >> 6] &  (1ull << ((i) & 63)))
#define BIT_SET(i)   (MmPfnBitmap[(i) >> 6] |= (1ull << ((i) & 63)))
#define BIT_CLEAR(i) (MmPfnBitmap[(i) >> 6] &= ~(1ull << ((i) & 63)))

static void MmMarkRange(ULONG64 Base, ULONG64 End, BOOLEAN Used)
{
    for (ULONG64 p = Base >> PAGE_SHIFT; p < (End + PAGE_SIZE - 1) >> PAGE_SHIFT && p < MmPfnCount; p++) {
        if (Used) { if (!BIT_TEST(p)) { BIT_SET(p); MmFreePagesCount--; } }
        else      { if (BIT_TEST(p))  { BIT_CLEAR(p); MmFreePagesCount++; } }
    }
}

void MmInitSystem(void)
{
    KE_LOADER_BLOCK *lb = &KeLoaderBlock;
    ULONG64 kernelStart = 0x100000;
    ULONG64 kernelEnd = (ULONG64)KiImageEnd - MM_KERNEL_BASE;
    MmKernelImagePages = (kernelEnd - kernelStart) >> PAGE_SHIFT;

    ULONG64 highest = 0;
    for (ULONG i = 0; i < lb->MemoryRunCount; i++) {
        KE_MEMORY_RUN *r = &lb->MemoryRuns[i];
        if (r->Type == 1 && r->Base + r->Length > highest) highest = r->Base + r->Length;
    }
    if (highest > (64ull << 30)) highest = 64ull << 30;
    MmHighestPhysicalPage = highest >> PAGE_SHIFT;
    MmPfnCount = ALIGN_UP(MmHighestPhysicalPage, 64);
    ULONG64 bitmapBytes = ALIGN_UP(MmPfnCount / 8, PAGE_SIZE);

    /* Find a home for the bitmap: usable RAM above the kernel, below 4 GiB,
     * not overlapping the image or any boot module. */
    ULONG64 bitmapPhys = 0;
    for (ULONG i = 0; i < lb->MemoryRunCount && !bitmapPhys; i++) {
        KE_MEMORY_RUN *r = &lb->MemoryRuns[i];
        if (r->Type != 1) continue;
        ULONG64 cand = ALIGN_UP(MAX(r->Base, kernelEnd), PAGE_SIZE);
        ULONG64 end = MIN(r->Base + r->Length, 0x100000000ull);
        for (BOOLEAN moved = TRUE; moved; ) {
            moved = FALSE;
            for (ULONG m = 0; m < lb->ModuleCount; m++) {
                if (cand < lb->Modules[m].End && cand + bitmapBytes > lb->Modules[m].Start) {
                    cand = ALIGN_UP(lb->Modules[m].End, PAGE_SIZE);
                    moved = TRUE;
                }
            }
        }
        if (cand + bitmapBytes <= end) bitmapPhys = cand;
    }
    if (!bitmapPhys) KeBugCheckEx(PHASE0_INITIALIZATION_FAILED, 0x4D4D, bitmapBytes, highest, 0);

    MmPfnBitmap = MmPhysToVirt(bitmapPhys);
    memset(MmPfnBitmap, 0xFF, bitmapBytes);
    MmFreePagesCount = 0;
    for (ULONG i = 0; i < lb->MemoryRunCount; i++) {
        KE_MEMORY_RUN *r = &lb->MemoryRuns[i];
        if (r->Type != 1) continue;
        ULONG64 base = ALIGN_UP(MAX(r->Base, 0x100000), PAGE_SIZE);
        ULONG64 end = ALIGN_DOWN(MIN(r->Base + r->Length, highest), PAGE_SIZE);
        if (end > base) { MmMarkRange(base, end, FALSE); MmTotalPages += (end - base) >> PAGE_SHIFT; }
    }
    MmMarkRange(kernelStart, kernelEnd, TRUE);
    MmMarkRange(bitmapPhys, bitmapPhys + bitmapBytes, TRUE);
    for (ULONG m = 0; m < lb->ModuleCount; m++)
        MmMarkRange(ALIGN_DOWN(lb->Modules[m].Start, PAGE_SIZE), lb->Modules[m].End, TRUE);
    MmSearchHint = kernelEnd >> PAGE_SHIFT;

    /* Extend the physical map past the 4 GiB the boot stub provided. */
    ULONG64 *pdpt = MmPhysToVirt(KiBootPdptLow);
    ULONG64 need = MAX(highest, HalFramebuffer.PhysicalAddress +
                       (ULONG64)HalFramebuffer.Pitch * HalFramebuffer.Height);
    for (ULONG64 gb = 4; gb < ((need + (1ull << 30) - 1) >> 30) && gb < 512; gb++) {
        ULONG64 pdPhys = MmAllocatePages(1);
        if (!pdPhys || pdPhys >= 0x100000000ull)
            KeBugCheckEx(PHASE0_INITIALIZATION_FAILED, 0x4D4D, 2, gb, pdPhys);
        ULONG64 *pd = MmPhysToVirt(pdPhys);
        for (ULONG64 i = 0; i < 512; i++)
            pd[i] = ((gb << 30) + (i << 21)) | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
        pdpt[gb] = pdPhys | PTE_PRESENT | PTE_WRITE;
    }

    /* Drop the identity mapping: the lower half now belongs to user mode. */
    MmSystemPml4 = KiBootPml4;
    ULONG64 *pml4 = MmPhysToVirt(MmSystemPml4);
    pml4[0] = 0;
    HalWriteCr3(MmSystemPml4);
}

ULONG64 MmAllocatePages(ULONG64 Count)
{
    if (!Count) return 0;
    KIRQL irql; KeAcquireSpinLock(&MmPfnLock, &irql);
    ULONG64 result = 0;
    for (int pass = 0; pass < 2 && !result; pass++) {
        ULONG64 run = 0, start = 0;
        for (ULONG64 i = pass ? 0 : MmSearchHint; i < MmPfnCount; i++) {
            if (!run && !(i & 63) && MmPfnBitmap[i >> 6] == ~0ull) { i += 63; continue; }
            if (BIT_TEST(i)) { run = 0; continue; }
            if (!run) start = i;
            if (++run == Count) {
                for (ULONG64 p = start; p < start + Count; p++) BIT_SET(p);
                MmFreePagesCount -= Count;
                if (start == MmSearchHint) MmSearchHint = start + Count;
                result = start << PAGE_SHIFT;
                break;
            }
        }
    }
    KeReleaseSpinLock(&MmPfnLock, irql);
    return result;
}

void MmFreePages(ULONG64 PhysicalAddress, ULONG64 Count)
{
    KIRQL irql; KeAcquireSpinLock(&MmPfnLock, &irql);
    ULONG64 first = PhysicalAddress >> PAGE_SHIFT;
    for (ULONG64 p = first; p < first + Count && p < MmPfnCount; p++) {
        if (!BIT_TEST(p)) KeBugCheckEx(0x0000004E /* PFN_LIST_CORRUPT */, 0x99, p, 0, 0);
        BIT_CLEAR(p);
        MmFreePagesCount++;
    }
    if (first < MmSearchHint) MmSearchHint = first;
    KeReleaseSpinLock(&MmPfnLock, irql);
}

PVOID MmAllocateContiguousMemory(SIZE_T Bytes)
{
    ULONG64 pages = (Bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    ULONG64 pa = MmAllocatePages(pages);
    if (!pa) return NULL;
    PVOID va = MmPhysToVirt(pa);
    memset(va, 0, pages << PAGE_SHIFT);
    return va;
}

void MmFreeContiguousMemory(PVOID Va, SIZE_T Bytes)
{
    if (Va) MmFreePages(MmVirtToPhys(Va), (Bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

/* Every physical address is already reachable through the physical map. */
PVOID MmMapIoSpace(ULONG64 PhysicalAddress, SIZE_T Bytes)
{
    (void)Bytes;
    return MmPhysToVirt(PhysicalAddress);
}

/* ---- Address spaces ---------------------------------------------------------------- */
ULONG64 MmCreateAddressSpace(void)
{
    ULONG64 pa = MmAllocatePages(1);
    if (!pa) return 0;
    ULONG64 *pml4 = MmPhysToVirt(pa), *sys = MmPhysToVirt(MmSystemPml4);
    memset(pml4, 0, PAGE_SIZE / 2);
    memcpy(pml4 + 256, sys + 256, PAGE_SIZE / 2);      /* share the kernel half */
    return pa;
}

static ULONG64 *MmWalk(ULONG64 Pml4, ULONG64 Va, BOOLEAN Create)
{
    ULONG64 *table = MmPhysToVirt(Pml4);
    for (int level = 3; level > 0; level--) {
        ULONG64 *e = &table[(Va >> (12 + 9 * level)) & 511];
        if (!(*e & PTE_PRESENT)) {
            if (!Create) return NULL;
            ULONG64 pa = MmAllocatePages(1);
            if (!pa) return NULL;
            memset(MmPhysToVirt(pa), 0, PAGE_SIZE);
            *e = pa | PTE_PRESENT | PTE_WRITE | PTE_USER;
        } else if (*e & PTE_LARGE) {
            return NULL;
        }
        table = MmPhysToVirt(*e & PTE_ADDR_MASK);
    }
    return &table[(Va >> 12) & 511];
}

ULONG64 *MmGetPte(ULONG64 Pml4, ULONG64 Va)
{
    ULONG64 *pte = MmWalk(Pml4, Va, FALSE);
    return (pte && (*pte & PTE_PRESENT)) ? pte : NULL;
}

NTSTATUS MmMapUserPage(ULONG64 Pml4, ULONG64 Va, ULONG64 Pa, ULONG64 Flags)
{
    if (Va >= MM_USER_LIMIT || Va < 0x10000) return STATUS_INVALID_PARAMETER;
    ULONG64 *pte = MmWalk(Pml4, Va, TRUE);
    if (!pte) return STATUS_NO_MEMORY;
    if (!KiNxEnabled) Flags &= ~PTE_NX;
    *pte = (Pa & PTE_ADDR_MASK) | Flags | PTE_PRESENT | PTE_USER;
    if (Pml4 == (HalReadCr3() & PTE_ADDR_MASK)) HalInvalidatePage(Va);
    return STATUS_SUCCESS;
}

NTSTATUS MmAllocateUserMemory(ULONG64 Pml4, ULONG64 Va, SIZE_T Length, ULONG64 Flags,
                              ULONG64 *PagesCommitted)
{
    ULONG64 start = ALIGN_DOWN(Va, PAGE_SIZE), end = ALIGN_UP(Va + Length, PAGE_SIZE);
    if (end > MM_USER_LIMIT || end < start) return STATUS_INVALID_PARAMETER;
    for (ULONG64 v = start; v < end; v += PAGE_SIZE) {
        if (MmGetPte(Pml4, v)) continue;
        ULONG64 pa = MmAllocatePages(1);
        if (!pa) return STATUS_NO_MEMORY;
        memset(MmPhysToVirt(pa), 0, PAGE_SIZE);
        NTSTATUS st = MmMapUserPage(Pml4, v, pa, Flags);
        if (!NT_SUCCESS(st)) { MmFreePages(pa, 1); return st; }
        if (PagesCommitted) (*PagesCommitted)++;
    }
    return STATUS_SUCCESS;
}

NTSTATUS MmCopyToUserSpace(ULONG64 Pml4, ULONG64 Va, const void *Src, SIZE_T Length)
{
    const UCHAR *s = Src;
    while (Length) {
        ULONG64 *pte = MmGetPte(Pml4, Va);
        if (!pte) return STATUS_ACCESS_VIOLATION;
        SIZE_T off = Va & (PAGE_SIZE - 1), n = MIN(Length, PAGE_SIZE - off);
        memcpy((UCHAR *)MmPhysToVirt(*pte & PTE_ADDR_MASK) + off, s, n);
        Va += n; s += n; Length -= n;
    }
    return STATUS_SUCCESS;
}

/* Probe a user buffer before a system service touches it. */
BOOLEAN MmIsUserRangeValid(ULONG64 Pml4, ULONG64 Va, SIZE_T Length, BOOLEAN Write)
{
    if (!Length) return TRUE;
    ULONG64 end = Va + Length;
    if (end < Va || end > MM_USER_LIMIT || Va < 0x10000) return FALSE;
    for (ULONG64 v = ALIGN_DOWN(Va, PAGE_SIZE); v < end; v += PAGE_SIZE) {
        ULONG64 *pte = MmGetPte(Pml4, v);
        if (!pte || !(*pte & PTE_USER) || (Write && !(*pte & PTE_WRITE))) return FALSE;
    }
    return TRUE;
}

/* Tear down the user half: every leaf page is private to the process. */
void MmDestroyAddressSpace(ULONG64 Pml4, ULONG64 *PagesFreed)
{
    ULONG64 freed = 0;
    ULONG64 *l4 = MmPhysToVirt(Pml4);
    for (int a = 0; a < 256; a++) {
        if (!(l4[a] & PTE_PRESENT)) continue;
        ULONG64 *l3 = MmPhysToVirt(l4[a] & PTE_ADDR_MASK);
        for (int b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_PRESENT)) continue;
            ULONG64 *l2 = MmPhysToVirt(l3[b] & PTE_ADDR_MASK);
            for (int c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_PRESENT)) continue;
                ULONG64 *l1 = MmPhysToVirt(l2[c] & PTE_ADDR_MASK);
                for (int d = 0; d < 512; d++) {
                    if (l1[d] & PTE_PRESENT) { MmFreePages(l1[d] & PTE_ADDR_MASK, 1); freed++; }
                }
                MmFreePages(l2[c] & PTE_ADDR_MASK, 1); freed++;
            }
            MmFreePages(l3[b] & PTE_ADDR_MASK, 1); freed++;
        }
        MmFreePages(l4[a] & PTE_ADDR_MASK, 1); freed++;
    }
    MmFreePages(Pml4, 1); freed++;
    if (PagesFreed) *PagesFreed = freed;
}
