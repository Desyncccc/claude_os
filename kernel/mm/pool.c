/*
 * RadiaNT -- mm/pool.c
 * Executive pool: a first-fit heap with boundary tags and coalescing, grown
 * in contiguous segments from the PFN allocator. Every allocation carries a
 * four character tag so leaks can be attributed, just like on NT.
 */
#include "mm.h"
#include "ke.h"
#include "rtl.h"

#define POOL_MAGIC_USED 0x4C4F4F50u     /* 'POOL' */
#define POOL_MAGIC_FREE 0x45455246u     /* 'FREE' */
#define POOL_FLAG_LAST  1u
#define POOL_SEGMENT    (1ull << 20)
#define POOL_MIN_SPLIT  64

typedef struct _POOL_HEADER {
    ULONG   Magic;
    ULONG   Tag;
    ULONG64 Size;           /* whole block, header included */
    ULONG64 PrevSize;       /* 0 for the first block of a segment */
    ULONG64 Flags;
} POOL_HEADER;              /* 32 bytes; a free block stores a LIST_ENTRY behind it */

static LIST_ENTRY ExpFreeList = { &ExpFreeList, &ExpFreeList };
static KSPIN_LOCK ExpPoolLock;
static POOL_TAG_INFO ExpTagTable[96];
ULONG64 ExPoolBytesInUse, ExPoolBytesTotal;

#define FREE_LINK(h)   ((PLIST_ENTRY)((POOL_HEADER *)(h) + 1))
#define LINK_HEADER(l) ((POOL_HEADER *)(l) - 1)
#define NEXT_BLOCK(h)  ((POOL_HEADER *)((UCHAR *)(h) + (h)->Size))
#define PREV_BLOCK(h)  ((POOL_HEADER *)((UCHAR *)(h) - (h)->PrevSize))

static POOL_TAG_INFO *ExpFindTag(ULONG Tag)
{
    POOL_TAG_INFO *spare = NULL;
    for (ULONG i = 0; i < ARRAY_SIZE(ExpTagTable); i++) {
        if (ExpTagTable[i].Tag == Tag) return &ExpTagTable[i];
        if (!ExpTagTable[i].Tag && !spare) spare = &ExpTagTable[i];
    }
    if (spare) spare->Tag = Tag;
    return spare;
}

static BOOLEAN ExpGrowPool(SIZE_T Need)
{
    SIZE_T bytes = ALIGN_UP(MAX(Need, POOL_SEGMENT), PAGE_SIZE);
    ULONG64 pa = MmAllocatePages(bytes >> PAGE_SHIFT);
    if (!pa) return FALSE;
    POOL_HEADER *h = MmPhysToVirt(pa);
    h->Magic = POOL_MAGIC_FREE; h->Tag = 0; h->Size = bytes; h->PrevSize = 0; h->Flags = POOL_FLAG_LAST;
    InsertTailList(&ExpFreeList, FREE_LINK(h));
    ExPoolBytesTotal += bytes;
    return TRUE;
}

PVOID ExAllocatePoolWithTag(SIZE_T Bytes, ULONG Tag)
{
    SIZE_T need = ALIGN_UP(Bytes + sizeof(POOL_HEADER), 16);
    if (need < sizeof(POOL_HEADER) + sizeof(LIST_ENTRY)) need = sizeof(POOL_HEADER) + sizeof(LIST_ENTRY);
    KIRQL irql; KeAcquireSpinLock(&ExpPoolLock, &irql);
    POOL_HEADER *h = NULL;
    for (int attempt = 0; attempt < 2 && !h; attempt++) {
        for (PLIST_ENTRY e = ExpFreeList.Flink; e != &ExpFreeList; e = e->Flink) {
            if (LINK_HEADER(e)->Size >= need) { h = LINK_HEADER(e); break; }
        }
        if (!h && !ExpGrowPool(need)) break;
    }
    if (!h) { KeReleaseSpinLock(&ExpPoolLock, irql); return NULL; }
    if (h->Magic != POOL_MAGIC_FREE) KeBugCheckEx(BAD_POOL_HEADER, 1, (ULONG64)h, h->Magic, 0);

    RemoveEntryList(FREE_LINK(h));
    if (h->Size - need >= POOL_MIN_SPLIT) {
        POOL_HEADER *rest = (POOL_HEADER *)((UCHAR *)h + need);
        rest->Magic = POOL_MAGIC_FREE; rest->Tag = 0;
        rest->Size = h->Size - need; rest->PrevSize = need;
        rest->Flags = h->Flags & POOL_FLAG_LAST;
        if (!(rest->Flags & POOL_FLAG_LAST)) NEXT_BLOCK(rest)->PrevSize = rest->Size;
        h->Size = need; h->Flags &= ~(ULONG64)POOL_FLAG_LAST;
        InsertHeadList(&ExpFreeList, FREE_LINK(rest));
    }
    h->Magic = POOL_MAGIC_USED;
    h->Tag = Tag;
    ExPoolBytesInUse += h->Size;
    POOL_TAG_INFO *ti = ExpFindTag(Tag);
    if (ti) { ti->Allocs++; ti->Bytes += h->Size; }
    KeReleaseSpinLock(&ExpPoolLock, irql);
    memset(h + 1, 0, h->Size - sizeof(POOL_HEADER));
    return h + 1;
}

void ExFreePool(PVOID P)
{
    if (!P) return;
    POOL_HEADER *h = (POOL_HEADER *)P - 1;
    KIRQL irql; KeAcquireSpinLock(&ExpPoolLock, &irql);
    if (h->Magic != POOL_MAGIC_USED) KeBugCheckEx(BAD_POOL_HEADER, 2, (ULONG64)h, h->Magic, 0);
    POOL_TAG_INFO *ti = ExpFindTag(h->Tag);
    if (ti) { ti->Frees++; ti->Bytes -= h->Size; }
    ExPoolBytesInUse -= h->Size;
    h->Magic = POOL_MAGIC_FREE;

    if (!(h->Flags & POOL_FLAG_LAST)) {             /* merge with the next block */
        POOL_HEADER *n = NEXT_BLOCK(h);
        if (n->Magic == POOL_MAGIC_FREE) {
            RemoveEntryList(FREE_LINK(n));
            h->Size += n->Size; h->Flags |= n->Flags & POOL_FLAG_LAST;
            n->Magic = 0;
        }
    }
    if (h->PrevSize) {                              /* merge with the previous block */
        POOL_HEADER *p = PREV_BLOCK(h);
        if (p->Magic == POOL_MAGIC_FREE) {
            RemoveEntryList(FREE_LINK(p));
            p->Size += h->Size; p->Flags |= h->Flags & POOL_FLAG_LAST;
            h->Magic = 0;
            h = p;
        }
    }
    if (!(h->Flags & POOL_FLAG_LAST)) NEXT_BLOCK(h)->PrevSize = h->Size;
    InsertHeadList(&ExpFreeList, FREE_LINK(h));
    KeReleaseSpinLock(&ExpPoolLock, irql);
}

ULONG ExQueryPoolTags(POOL_TAG_INFO *Out, ULONG Max)
{
    ULONG n = 0;
    KIRQL irql; KeAcquireSpinLock(&ExpPoolLock, &irql);
    for (ULONG i = 0; i < ARRAY_SIZE(ExpTagTable) && n < Max; i++)
        if (ExpTagTable[i].Tag) Out[n++] = ExpTagTable[i];
    KeReleaseSpinLock(&ExpPoolLock, irql);
    return n;
}
