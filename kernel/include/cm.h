/* RadiaNT -- cm.h : configuration manager (the in-memory registry). */
#pragma once
#include "ntdef.h"

#define REG_SZ    1
#define REG_DWORD 4

typedef struct _CM_KEY_VALUE {
    struct _CM_KEY_VALUE *Next;
    CHAR  Name[40];
    ULONG Type;
    ULONG Dword;
    CHAR  String[96];
} CM_KEY_VALUE;

typedef struct _CM_KEY_NODE {
    CHAR  Name[40];
    struct _CM_KEY_NODE *Parent, *FirstChild, *NextSibling;
    CM_KEY_VALUE *Values;
} CM_KEY_NODE;

void         CmInitSystem(void);
CM_KEY_NODE *CmCreateKey(PCSTR Path);       /* creates intermediate keys */
CM_KEY_NODE *CmOpenKey(PCSTR Path);
NTSTATUS     CmSetValueString(PCSTR Path, PCSTR Name, PCSTR Value);
NTSTATUS     CmSetValueDword(PCSTR Path, PCSTR Name, ULONG Value);
extern ULONG CmKeyCount, CmValueCount;
