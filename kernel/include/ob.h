/* RadiaNT -- ob.h : object manager (typed, named, reference counted objects). */
#pragma once
#include "ntdef.h"

#define OB_NAME_MAX 48
typedef void (*OB_DELETE_METHOD)(PVOID Body);

typedef struct _OBJECT_TYPE {
    CHAR             Name[24];
    ULONG            PoolTag;
    OB_DELETE_METHOD DeleteProcedure;
    ULONG            TotalNumberOfObjects;
    ULONG            TotalNumberOfHandles;
    ULONG            HighWaterNumberOfObjects;
} OBJECT_TYPE, *POBJECT_TYPE;

struct _OBJECT_DIRECTORY;
typedef struct _OBJECT_HEADER {
    ULONG64                   Magic;
    LONG                      PointerCount;
    LONG                      HandleCount;
    POBJECT_TYPE              Type;
    struct _OBJECT_DIRECTORY *Directory;       /* parent, NULL if unnamed */
    LIST_ENTRY                DirectoryLink;
    CHAR                      Name[OB_NAME_MAX];
    ULONG64                   Pad;
} OBJECT_HEADER, *POBJECT_HEADER;              /* body follows, 16-byte aligned */

#define OBJECT_TO_HEADER(b) ((POBJECT_HEADER)(b) - 1)
#define HEADER_TO_OBJECT(h) ((PVOID)((POBJECT_HEADER)(h) + 1))

typedef struct _OBJECT_DIRECTORY   { LIST_ENTRY Entries; ULONG Count; } OBJECT_DIRECTORY;
typedef struct _OBJECT_SYMBOLIC_LINK { CHAR Target[96]; } OBJECT_SYMBOLIC_LINK;

extern POBJECT_TYPE ObTypeObjectType, ObDirectoryObjectType, ObSymbolicLinkObjectType;
extern OBJECT_DIRECTORY *ObRootDirectory;

void     ObInitSystem(void);
NTSTATUS ObCreateObjectType(PCSTR Name, ULONG Tag, OB_DELETE_METHOD Delete, POBJECT_TYPE *Type);
NTSTATUS ObCreateObject(POBJECT_TYPE Type, PCSTR Path, SIZE_T BodySize, PVOID *Body);
NTSTATUS ObCreateDirectory(PCSTR Path, OBJECT_DIRECTORY **Directory);
NTSTATUS ObCreateSymbolicLink(PCSTR Path, PCSTR Target);
void     ObReferenceObject(PVOID Body);
void     ObDereferenceObject(PVOID Body);
NTSTATUS ObReferenceObjectByName(PCSTR Path, POBJECT_TYPE Type, PVOID *Body);
void     ObMakeTemporaryObject(PVOID Body);    /* unlink from the namespace */

/* Directory enumeration (snapshot, safe from any thread) */
typedef struct _OB_DIRECTORY_ENTRY {
    CHAR Name[OB_NAME_MAX];
    CHAR TypeName[24];
    CHAR Target[64];        /* symbolic links only */
    LONG PointerCount, HandleCount;
    BOOLEAN IsDirectory;
} OB_DIRECTORY_ENTRY;
ULONG ObQueryDirectory(PCSTR Path, OB_DIRECTORY_ENTRY *Out, ULONG Max);
ULONG ObQueryTypes(OBJECT_TYPE *Out, ULONG Max);

/* Handle tables: handles are multiples of 4, exactly like NT. */
#define OB_HANDLE_TABLE_SIZE 128
typedef struct _HANDLE_TABLE {
    PVOID Objects[OB_HANDLE_TABLE_SIZE];
    ULONG HandleCount;
} HANDLE_TABLE;
HANDLE_TABLE *ObCreateHandleTable(void);
void     ObKillHandleTable(HANDLE_TABLE *Table);
NTSTATUS ObCreateHandle(HANDLE_TABLE *Table, PVOID Body, HANDLE *Handle);
NTSTATUS ObCloseHandle(HANDLE_TABLE *Table, HANDLE Handle);
NTSTATUS ObReferenceObjectByHandle(HANDLE_TABLE *Table, HANDLE Handle,
                                   POBJECT_TYPE Type, PVOID *Body);
