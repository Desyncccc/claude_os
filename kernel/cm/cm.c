/*
 * RadiaNT -- cm/cm.c
 * Configuration manager: a small in-memory registry tree populated at boot
 * with hardware and version information.
 */
#include "cm.h"
#include "mm.h"
#include "ke.h"
#include "hal.h"
#include "rtl.h"

static CM_KEY_NODE *CmRoot;
ULONG CmKeyCount, CmValueCount;

static CM_KEY_NODE *CmpFindChild(CM_KEY_NODE *Parent, PCSTR Name, SIZE_T Len)
{
    for (CM_KEY_NODE *c = Parent->FirstChild; c; c = c->NextSibling) {
        if (strlen(c->Name) != Len) continue;
        char tmp[40];
        memcpy(tmp, Name, Len); tmp[Len] = 0;
        if (!RtlCompareStringI(c->Name, tmp)) return c;
    }
    return NULL;
}

static CM_KEY_NODE *CmpWalk(PCSTR Path, BOOLEAN Create)
{
    CM_KEY_NODE *cur = CmRoot;
    PCSTR p = Path;
    if (RtlPrefixStringI("\\Registry", p)) p += 9;
    for (;;) {
        while (*p == '\\') p++;
        if (!*p) return cur;
        PCSTR end = strchr(p, '\\');
        SIZE_T len = end ? (SIZE_T)(end - p) : strlen(p);
        if (len >= sizeof(cur->Name)) return NULL;
        CM_KEY_NODE *child = CmpFindChild(cur, p, len);
        if (!child) {
            if (!Create) return NULL;
            child = ExAllocatePoolWithTag(sizeof(CM_KEY_NODE), TAG('C', 'M', 'k', 'n'));
            if (!child) return NULL;
            memcpy(child->Name, p, len);
            child->Parent = cur;
            CM_KEY_NODE **link = &cur->FirstChild;      /* keep insertion order */
            while (*link) link = &(*link)->NextSibling;
            *link = child;
            CmKeyCount++;
        }
        cur = child;
        p += len;
    }
}

CM_KEY_NODE *CmCreateKey(PCSTR Path) { return CmpWalk(Path, TRUE); }
CM_KEY_NODE *CmOpenKey(PCSTR Path)   { return CmpWalk(Path, FALSE); }

static CM_KEY_VALUE *CmpValue(PCSTR Path, PCSTR Name)
{
    CM_KEY_NODE *key = CmCreateKey(Path);
    if (!key) return NULL;
    for (CM_KEY_VALUE *v = key->Values; v; v = v->Next)
        if (!RtlCompareStringI(v->Name, Name)) return v;
    CM_KEY_VALUE *v = ExAllocatePoolWithTag(sizeof(CM_KEY_VALUE), TAG('C', 'M', 'v', 'l'));
    if (!v) return NULL;
    RtlStringCopy(v->Name, sizeof(v->Name), Name);
    CM_KEY_VALUE **link = &key->Values;
    while (*link) link = &(*link)->Next;
    *link = v;
    CmValueCount++;
    return v;
}

NTSTATUS CmSetValueString(PCSTR Path, PCSTR Name, PCSTR Value)
{
    CM_KEY_VALUE *v = CmpValue(Path, Name);
    if (!v) return STATUS_NO_MEMORY;
    v->Type = REG_SZ;
    RtlStringCopy(v->String, sizeof(v->String), Value);
    return STATUS_SUCCESS;
}

NTSTATUS CmSetValueDword(PCSTR Path, PCSTR Name, ULONG Value)
{
    CM_KEY_VALUE *v = CmpValue(Path, Name);
    if (!v) return STATUS_NO_MEMORY;
    v->Type = REG_DWORD;
    v->Dword = Value;
    return STATUS_SUCCESS;
}

void CmInitSystem(void)
{
    CmRoot = ExAllocatePoolWithTag(sizeof(CM_KEY_NODE), TAG('C', 'M', 'k', 'n'));
    RtlStringCopy(CmRoot->Name, sizeof(CmRoot->Name), "Registry");
    CmKeyCount = 1;

    PCSTR ver = "\\Registry\\Machine\\Software\\RadiaNT\\CurrentVersion";
    CmSetValueString(ver, "ProductName", RADIANT_NAME " OS");
    CmSetValueString(ver, "CurrentVersion", RADIANT_VERSION_STR);
    CmSetValueString(ver, "ReleaseName", RADIANT_CODENAME);
    CmSetValueDword(ver, "CurrentBuildNumber", RADIANT_BUILD);
    CmSetValueString(ver, "BuildLab", "radiant_main(" __DATE__ ")");
    CmSetValueString(ver, "KernelType", "Uniprocessor Free (x64)");
    CmSetValueString(ver, "SystemRoot", "\\SystemRoot");

    PCSTR cpu = "\\Registry\\Machine\\Hardware\\Description\\System\\CentralProcessor\\0";
    char ident[80];
    CmSetValueString(cpu, "ProcessorNameString", HalCpuInfo.Brand);
    CmSetValueString(cpu, "VendorIdentifier", HalCpuInfo.Vendor);
    RtlStringPrintf(ident, sizeof(ident), "AMD64 Family %u Model %u Stepping %u",
                    HalCpuInfo.Family, HalCpuInfo.Model, HalCpuInfo.Stepping);
    CmSetValueString(cpu, "Identifier", ident);
    CmSetValueDword(cpu, "NxEnabled", HalCpuInfo.HasNx);
    CmSetValueDword(cpu, "Hypervisor", HalCpuInfo.Hypervisor);

    PCSTR sys = "\\Registry\\Machine\\Hardware\\Description\\System";
    CmSetValueString(sys, "Identifier", "AT/AT COMPATIBLE");
    CmSetValueString(sys, "BootLoader", KeLoaderBlock.LoaderName);
    CmSetValueString(sys, "Firmware", KeLoaderBlock.FirmwareIsEfi ? "UEFI" : "BIOS");

    PCSTR video = "\\Registry\\Machine\\Hardware\\DeviceMap\\Video";
    char mode[64];
    RtlStringPrintf(mode, sizeof(mode), "%ux%ux%u", HalFramebuffer.Width, HalFramebuffer.Height, HalFramebuffer.Bpp);
    CmSetValueString(video, "\\Device\\Video0", mode);
    CmSetValueDword(video, "FramebufferBase", (ULONG)HalFramebuffer.PhysicalAddress);

    PCSTR ctl = "\\Registry\\Machine\\System\\CurrentControlSet\\Control";
    CmSetValueString(ctl, "SystemStartOptions", KeLoaderBlock.CommandLine);
    CmSetValueDword("\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                    "PhysicalPages", (ULONG)MmTotalPages);
    CmSetValueDword("\\Registry\\Machine\\System\\CurrentControlSet\\Control\\PriorityControl",
                    "QuantumMs", THREAD_QUANTUM);

    static const struct { PCSTR Name, Image; ULONG Start; } services[] = {
        { "Null", "Null device", 0 }, { "Serial", "16550 UART", 0 },
        { "Video", "Framebuffer miniport", 0 }, { "i8042prt", "PS/2 port driver", 0 },
        { "win32k", "Window manager + GDI", 1 },
    };
    for (ULONG i = 0; i < ARRAY_SIZE(services); i++) {
        char path[96];
        RtlStringPrintf(path, sizeof(path), "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%s",
                        services[i].Name);
        CmSetValueString(path, "DisplayName", services[i].Image);
        CmSetValueDword(path, "Start", services[i].Start);
        CmSetValueDword(path, "Type", 1);
    }
    CmCreateKey("\\Registry\\User\\.Default");
}
