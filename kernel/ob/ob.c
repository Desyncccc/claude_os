/*
 * RadiaNT -- ob/ob.c
 * The object manager. Every kernel resource (process, thread, event, device,
 * driver, ...) is a typed, reference counted object that may live in a
 * hierarchical namespace rooted at "\" and be referred to by handle.
 */
#include "ob.h"
#include "ke.h"
#include "mm.h"
#include "rtl.h"

#define OB_MAGIC 0x544A424F5444524Full     /* "ORDTOBJT" */

POBJECT_TYPE ObTypeObjectType, ObDirectoryObjectType, ObSymbolicLinkObjectType;
OBJECT_DIRECTORY *ObRootDirectory;
static KSPIN_LOCK ObpLock;

/* ---- Allocation --------------------------------------------------------------- */
static PVOID ObpAllocate(POBJECT_TYPE Type, PCSTR Name, SIZE_T BodySize, ULONG Tag)
{
    POBJECT_HEADER h = ExAllocatePoolWithTag(sizeof(OBJECT_HEADER) + BodySize, Tag);
    if (!h) return NULL;
    h->Magic = OB_MAGIC;
    h->PointerCount = 1;
    h->Type = Type;
    InitializeListHead(&h->DirectoryLink);
    if (Name) RtlStringCopy(h->Name, sizeof(h->Name), Name);
    if (Type) {
        Type->TotalNumberOfObjects++;
        if (Type->TotalNumberOfObjects > Type->HighWaterNumberOfObjects)
            Type->HighWaterNumberOfObjects = Type->TotalNumberOfObjects;
    }
    return HEADER_TO_OBJECT(h);
}

static void ObpInsert(OBJECT_DIRECTORY *Dir, PVOID Body)
{
    POBJECT_HEADER h = OBJECT_TO_HEADER(Body);
    h->Directory = Dir;
    InsertTailList(&Dir->Entries, &h->DirectoryLink);
    Dir->Count++;
}

static PVOID ObpFindInDirectory(OBJECT_DIRECTORY *Dir, PCSTR Name, SIZE_T Len)
{
    for (PLIST_ENTRY e = Dir->Entries.Flink; e != &Dir->Entries; e = e->Flink) {
        POBJECT_HEADER h = CONTAINING_RECORD(e, OBJECT_HEADER, DirectoryLink);
        if (strlen(h->Name) == Len) {
            SIZE_T i = 0;
            for (; i < Len; i++) {
                char a = h->Name[i], b = Name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
            }
            if (i == Len) return HEADER_TO_OBJECT(h);
        }
    }
    return NULL;
}

/*
 * Resolve a path. With LeafName == NULL the whole path must exist and the
 * object is returned. Otherwise the parent directory is returned and
 * *LeafName points at the final component (used when creating).
 * Symbolic links are followed (bounded). Caller holds ObpLock.
 */
static PVOID ObpLookup(PCSTR Path, PCSTR *LeafName)
{
    char buf[160];
    RtlStringCopy(buf, sizeof(buf), Path);
    for (int depth = 0; depth < 8; depth++) {
        const char *p = buf;
        if (*p != '\\') return NULL;
        PVOID cur = ObRootDirectory;
        BOOLEAN restarted = FALSE;
        for (;;) {
            while (*p == '\\') p++;
            if (!*p) return LeafName ? NULL : cur;
            const char *end = strchr(p, '\\');
            SIZE_T len = end ? (SIZE_T)(end - p) : strlen(p);
            if (!end && LeafName) {
                if (OBJECT_TO_HEADER(cur)->Type != ObDirectoryObjectType) return NULL;
                *LeafName = Path + (p - buf);
                return cur;
            }
            if (OBJECT_TO_HEADER(cur)->Type != ObDirectoryObjectType) return NULL;
            PVOID next = ObpFindInDirectory(cur, p, len);
            if (!next) return NULL;
            if (OBJECT_TO_HEADER(next)->Type == ObSymbolicLinkObjectType) {
                if (LeafName) return NULL;      /* no links in creation paths */
                char tmp[160];
                RtlStringCopy(tmp, sizeof(tmp), ((OBJECT_SYMBOLIC_LINK *)next)->Target);
                if (end) RtlStringCat(tmp, sizeof(tmp), end);
                RtlStringCopy(buf, sizeof(buf), tmp);
                restarted = TRUE;
                break;
            }
            cur = next;
            p += len;
        }
        if (!restarted) break;
    }
    return NULL;
}

/* ---- Public API ------------------------------------------------------------------ */
NTSTATUS ObCreateObject(POBJECT_TYPE Type, PCSTR Path, SIZE_T BodySize, PVOID *Body)
{
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    OBJECT_DIRECTORY *dir = NULL; PCSTR leaf = NULL;
    if (Path) {
        dir = ObpLookup(Path, &leaf);
        if (!dir || !*leaf || strlen(leaf) >= OB_NAME_MAX) {
            KeReleaseSpinLock(&ObpLock, irql);
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (ObpFindInDirectory(dir, leaf, strlen(leaf))) {
            KeReleaseSpinLock(&ObpLock, irql);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    PVOID obj = ObpAllocate(Type, leaf, BodySize, Type->PoolTag);
    if (!obj) { KeReleaseSpinLock(&ObpLock, irql); return STATUS_NO_MEMORY; }
    if (dir) ObpInsert(dir, obj);
    KeReleaseSpinLock(&ObpLock, irql);
    *Body = obj;
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateObjectType(PCSTR Name, ULONG Tag, OB_DELETE_METHOD Delete, POBJECT_TYPE *Type)
{
    char path[64];
    RtlStringPrintf(path, sizeof(path), "\\ObjectTypes\\%s", Name);
    PVOID body;
    NTSTATUS st = ObCreateObject(ObTypeObjectType, path, sizeof(OBJECT_TYPE), &body);
    if (!NT_SUCCESS(st)) return st;
    POBJECT_TYPE t = body;
    RtlStringCopy(t->Name, sizeof(t->Name), Name);
    t->PoolTag = Tag;
    t->DeleteProcedure = Delete;
    *Type = t;
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateDirectory(PCSTR Path, OBJECT_DIRECTORY **Directory)
{
    PVOID body;
    NTSTATUS st = ObCreateObject(ObDirectoryObjectType, Path, sizeof(OBJECT_DIRECTORY), &body);
    if (!NT_SUCCESS(st)) return st;
    InitializeListHead(&((OBJECT_DIRECTORY *)body)->Entries);
    if (Directory) *Directory = body;
    return STATUS_SUCCESS;
}

NTSTATUS ObCreateSymbolicLink(PCSTR Path, PCSTR Target)
{
    PVOID body;
    NTSTATUS st = ObCreateObject(ObSymbolicLinkObjectType, Path, sizeof(OBJECT_SYMBOLIC_LINK), &body);
    if (!NT_SUCCESS(st)) return st;
    RtlStringCopy(((OBJECT_SYMBOLIC_LINK *)body)->Target, 96, Target);
    return STATUS_SUCCESS;
}

void ObReferenceObject(PVOID Body)
{
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    OBJECT_TO_HEADER(Body)->PointerCount++;
    KeLowerIrql(irql);
}

void ObDereferenceObject(PVOID Body)
{
    POBJECT_HEADER h = OBJECT_TO_HEADER(Body);
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    if (h->Magic != OB_MAGIC || h->PointerCount <= 0)
        KeBugCheckEx(0x00000018 /* REFERENCE_BY_POINTER */, (ULONG64)Body, h->PointerCount, 0, 0);
    if (--h->PointerCount == 0) {
        if (h->Directory) { RemoveEntryList(&h->DirectoryLink); h->Directory->Count--; }
        if (h->Type->DeleteProcedure) h->Type->DeleteProcedure(Body);
        h->Type->TotalNumberOfObjects--;
        h->Magic = 0;
        ExFreePool(h);
    }
    KeLowerIrql(irql);
}

void ObMakeTemporaryObject(PVOID Body)
{
    POBJECT_HEADER h = OBJECT_TO_HEADER(Body);
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    if (h->Directory) {
        RemoveEntryList(&h->DirectoryLink);
        h->Directory->Count--;
        h->Directory = NULL;
    }
    KeReleaseSpinLock(&ObpLock, irql);
}

NTSTATUS ObReferenceObjectByName(PCSTR Path, POBJECT_TYPE Type, PVOID *Body)
{
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    PVOID obj = ObpLookup(Path, NULL);
    NTSTATUS st = STATUS_SUCCESS;
    if (!obj) st = STATUS_OBJECT_NAME_NOT_FOUND;
    else if (Type && OBJECT_TO_HEADER(obj)->Type != Type) st = STATUS_OBJECT_TYPE_MISMATCH;
    else { OBJECT_TO_HEADER(obj)->PointerCount++; *Body = obj; }
    KeReleaseSpinLock(&ObpLock, irql);
    return st;
}

ULONG ObQueryDirectory(PCSTR Path, OB_DIRECTORY_ENTRY *Out, ULONG Max)
{
    ULONG n = 0;
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    PVOID obj = ObpLookup(Path, NULL);
    if (obj && OBJECT_TO_HEADER(obj)->Type == ObDirectoryObjectType) {
        OBJECT_DIRECTORY *dir = obj;
        for (PLIST_ENTRY e = dir->Entries.Flink; e != &dir->Entries && n < Max; e = e->Flink) {
            POBJECT_HEADER h = CONTAINING_RECORD(e, OBJECT_HEADER, DirectoryLink);
            OB_DIRECTORY_ENTRY *o = &Out[n++];
            memset(o, 0, sizeof(*o));
            RtlStringCopy(o->Name, sizeof(o->Name), h->Name);
            RtlStringCopy(o->TypeName, sizeof(o->TypeName), h->Type->Name);
            o->PointerCount = h->PointerCount;
            o->HandleCount = h->HandleCount;
            o->IsDirectory = h->Type == ObDirectoryObjectType;
            if (h->Type == ObSymbolicLinkObjectType)
                RtlStringCopy(o->Target, sizeof(o->Target),
                              ((OBJECT_SYMBOLIC_LINK *)HEADER_TO_OBJECT(h))->Target);
        }
    }
    KeReleaseSpinLock(&ObpLock, irql);
    return n;
}

ULONG ObQueryTypes(OBJECT_TYPE *Out, ULONG Max)
{
    ULONG n = 0;
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    OBJECT_DIRECTORY *dir = ObpLookup("\\ObjectTypes", NULL);
    if (dir) {
        for (PLIST_ENTRY e = dir->Entries.Flink; e != &dir->Entries && n < Max; e = e->Flink)
            Out[n++] = *(POBJECT_TYPE)HEADER_TO_OBJECT(CONTAINING_RECORD(e, OBJECT_HEADER, DirectoryLink));
    }
    KeReleaseSpinLock(&ObpLock, irql);
    return n;
}

/* ---- Handles ----------------------------------------------------------------------- */
HANDLE_TABLE *ObCreateHandleTable(void)
{
    return ExAllocatePoolWithTag(sizeof(HANDLE_TABLE), TAG('O', 'b', 't', 'b'));
}

NTSTATUS ObCreateHandle(HANDLE_TABLE *Table, PVOID Body, HANDLE *Handle)
{
    NTSTATUS st = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    for (ULONG i = 0; i < OB_HANDLE_TABLE_SIZE; i++) {
        if (!Table->Objects[i]) {
            POBJECT_HEADER h = OBJECT_TO_HEADER(Body);
            Table->Objects[i] = Body;
            Table->HandleCount++;
            h->PointerCount++; h->HandleCount++;
            h->Type->TotalNumberOfHandles++;
            *Handle = (HANDLE)((i + 1) * 4);
            st = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&ObpLock, irql);
    return st;
}

static PVOID ObpHandleToObject(HANDLE_TABLE *Table, HANDLE Handle, ULONG *Index)
{
    if (!Handle || (Handle & 3) || Handle / 4 > OB_HANDLE_TABLE_SIZE) return NULL;
    *Index = (ULONG)(Handle / 4 - 1);
    return Table->Objects[*Index];
}

NTSTATUS ObCloseHandle(HANDLE_TABLE *Table, HANDLE Handle)
{
    ULONG idx;
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    PVOID obj = ObpHandleToObject(Table, Handle, &idx);
    if (obj) {
        Table->Objects[idx] = NULL;
        Table->HandleCount--;
        OBJECT_TO_HEADER(obj)->HandleCount--;
        OBJECT_TO_HEADER(obj)->Type->TotalNumberOfHandles--;
    }
    KeReleaseSpinLock(&ObpLock, irql);
    if (!obj) return STATUS_INVALID_HANDLE;
    ObDereferenceObject(obj);
    return STATUS_SUCCESS;
}

NTSTATUS ObReferenceObjectByHandle(HANDLE_TABLE *Table, HANDLE Handle, POBJECT_TYPE Type, PVOID *Body)
{
    ULONG idx;
    NTSTATUS st = STATUS_SUCCESS;
    KIRQL irql; KeAcquireSpinLock(&ObpLock, &irql);
    PVOID obj = ObpHandleToObject(Table, Handle, &idx);
    if (!obj) st = STATUS_INVALID_HANDLE;
    else if (Type && OBJECT_TO_HEADER(obj)->Type != Type) st = STATUS_OBJECT_TYPE_MISMATCH;
    else { OBJECT_TO_HEADER(obj)->PointerCount++; *Body = obj; }
    KeReleaseSpinLock(&ObpLock, irql);
    return st;
}

void ObKillHandleTable(HANDLE_TABLE *Table)
{
    if (!Table) return;
    for (ULONG i = 0; i < OB_HANDLE_TABLE_SIZE; i++)
        if (Table->Objects[i]) ObCloseHandle(Table, (HANDLE)((i + 1) * 4));
    ExFreePool(Table);
}

/* ---- Bootstrap ---------------------------------------------------------------------- */
void ObInitSystem(void)
{
    /* The "Type" type describes itself, so it is built by hand. */
    ObTypeObjectType = ObpAllocate(NULL, "Type", sizeof(OBJECT_TYPE), TAG('O', 'b', 'j', 'T'));
    RtlStringCopy(ObTypeObjectType->Name, sizeof(ObTypeObjectType->Name), "Type");
    ObTypeObjectType->PoolTag = TAG('O', 'b', 'j', 'T');
    OBJECT_TO_HEADER(ObTypeObjectType)->Type = ObTypeObjectType;
    ObTypeObjectType->TotalNumberOfObjects = ObTypeObjectType->HighWaterNumberOfObjects = 1;

    ObDirectoryObjectType = ObpAllocate(ObTypeObjectType, "Directory", sizeof(OBJECT_TYPE), TAG('O', 'b', 'j', 'T'));
    RtlStringCopy(ObDirectoryObjectType->Name, sizeof(ObDirectoryObjectType->Name), "Directory");
    ObDirectoryObjectType->PoolTag = TAG('D', 'i', 'r', 'e');

    ObRootDirectory = ObpAllocate(ObDirectoryObjectType, "", sizeof(OBJECT_DIRECTORY), TAG('D', 'i', 'r', 'e'));
    InitializeListHead(&ObRootDirectory->Entries);

    OBJECT_DIRECTORY *types = NULL;
    ObCreateDirectory("\\ObjectTypes", &types);
    ObpInsert(types, ObTypeObjectType);
    ObpInsert(types, ObDirectoryObjectType);
    ObCreateObjectType("SymbolicLink", TAG('S', 'y', 'm', 'b'), NULL, &ObSymbolicLinkObjectType);

    ObCreateDirectory("\\Device", NULL);
    ObCreateDirectory("\\Driver", NULL);
    ObCreateDirectory("\\KernelObjects", NULL);
    ObCreateDirectory("\\GLOBAL??", NULL);
    ObCreateDirectory("\\SystemRoot", NULL);
    ObCreateSymbolicLink("\\DosDevices", "\\GLOBAL??");
}
