/*
 * RadiaNT -- ke/init.c
 * System startup. Phase 0 runs on the boot stack with interrupts off and
 * brings up the kernel, HAL, Mm, Ob and Ps. Phase 1 runs as the first system
 * thread and starts the I/O system, the registry and win32k.
 */
#include "ke.h"
#include "hal.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"
#include "io.h"
#include "cm.h"
#include "rtl.h"
#include "win32k.h"

KE_LOADER_BLOCK KeLoaderBlock;
HAL_FRAMEBUFFER HalFramebuffer;
BOOLEAN KiNxEnabled;
void ExInitSystem(void);

#define MULTIBOOT2_MAGIC 0x36D76289

/* Copy what we need out of the Multiboot2 information structure before Mm
 * starts handing out the memory it lives in. */
static void KiParseLoaderBlock(ULONG64 MbiPhys)
{
    const UCHAR *mbi = MmPhysToVirt(MbiPhys);
    ULONG total = *(const ULONG *)mbi;
    KE_LOADER_BLOCK *lb = &KeLoaderBlock;
    RtlStringCopy(lb->LoaderName, sizeof(lb->LoaderName), "unknown");

    for (const UCHAR *tag = mbi + 8; tag < mbi + total; ) {
        ULONG type = *(const ULONG *)tag, size = *(const ULONG *)(tag + 4);
        if (type == 0 || size < 8) break;
        switch (type) {
        case 1: RtlStringCopy(lb->CommandLine, sizeof(lb->CommandLine), (const char *)tag + 8); break;
        case 2: RtlStringCopy(lb->LoaderName, sizeof(lb->LoaderName), (const char *)tag + 8); break;
        case 3:
            if (lb->ModuleCount < KE_MAX_BOOT_MODULES) {
                KE_BOOT_MODULE *m = &lb->Modules[lb->ModuleCount++];
                m->Start = *(const ULONG *)(tag + 8);
                m->End = *(const ULONG *)(tag + 12);
                RtlStringCopy(m->Name, sizeof(m->Name), (const char *)tag + 16);
            }
            break;
        case 6: {
            ULONG entrySize = *(const ULONG *)(tag + 8);
            for (const UCHAR *e = tag + 16; e + entrySize <= tag + size; e += entrySize) {
                if (lb->MemoryRunCount >= KE_MAX_MEMORY_RUNS) break;
                KE_MEMORY_RUN *r = &lb->MemoryRuns[lb->MemoryRunCount++];
                r->Base = *(const ULONG64 *)e;
                r->Length = *(const ULONG64 *)(e + 8);
                r->Type = *(const ULONG *)(e + 16);
            }
            break;
        }
        case 8: {
            HAL_FRAMEBUFFER *fb = &HalFramebuffer;
            fb->PhysicalAddress = *(const ULONG64 *)(tag + 8);
            fb->Pitch = *(const ULONG *)(tag + 16);
            fb->Width = *(const ULONG *)(tag + 20);
            fb->Height = *(const ULONG *)(tag + 24);
            fb->Bpp = tag[28];
            if (tag[29] == 1) {                 /* direct RGB */
                fb->RedShift = tag[32]; fb->GreenShift = tag[34]; fb->BlueShift = tag[36];
            } else {
                fb->RedShift = 16; fb->GreenShift = 8; fb->BlueShift = 0;
            }
            break;
        }
        case 11: case 12: case 19: case 20: lb->FirmwareIsEfi = TRUE; break;
        default: break;
        }
        tag += ALIGN_UP(size, 8);
    }
}

static void KiPhase1Initialization(PVOID Context)
{
    (void)Context;
    Win32kBootSplash(35, "Starting process manager");
    PsInitPhase1();
    KeDelayExecutionThread(120);

    Win32kBootSplash(50, "Loading boot-start drivers");
    IoInitSystem();
    KeDelayExecutionThread(120);

    Win32kBootSplash(68, "Building the registry");
    CmInitSystem();
    KeDelayExecutionThread(120);

    Win32kBootSplash(85, "Starting win32k");
    KeDelayExecutionThread(160);
    Win32kBootSplash(100, "Welcome");
    KeDelayExecutionThread(350);

    DbgPrint("KE: phase 1 complete -- %u processes, %u threads, %llu MB free\n",
             PsProcessCount, PsThreadCount, (MmFreePagesCount * PAGE_SIZE) >> 20);
    Win32kInitialize();
}

NORETURN void KiSystemStartup(ULONG Magic, ULONG MbiPhys, ULONG NxEnabled)
{
    HalInitializeSerial();
    DbgPrint(RADIANT_NAME " Kernel Version " RADIANT_VERSION_STR " (x64) \"" RADIANT_CODENAME "\"\n");
    if (Magic != MULTIBOOT2_MAGIC) {
        DbgPrint("FATAL: not loaded by a Multiboot2 loader (magic 0x%08X)\n", Magic);
        for (;;) HalHalt();
    }
    KiNxEnabled = NxEnabled != 0;

    KiInitializeProcessor();
    KiInitializeScheduler();
    KiParseLoaderBlock(MbiPhys);
    HalIdentifyProcessor();
    HalInitializeInterrupts();
    DbgPrint("HAL: %s, loader \"%s\" (%s)\n", HalCpuInfo.Brand, KeLoaderBlock.LoaderName,
             KeLoaderBlock.FirmwareIsEfi ? "UEFI" : "BIOS");

    MmInitSystem();
    DbgPrint("MM: %llu MB physical memory, %llu pages free, kernel image %llu KB\n",
             (MmTotalPages * PAGE_SIZE) >> 20, MmFreePagesCount, MmKernelImagePages * 4);

    if (HalFramebuffer.PhysicalAddress && HalFramebuffer.Bpp >= 24) {
        HalFramebuffer.Base = MmMapIoSpace(HalFramebuffer.PhysicalAddress,
                                           (SIZE_T)HalFramebuffer.Pitch * HalFramebuffer.Height);
        GdiInitScreen();
        DbgPrint("HAL: framebuffer %ux%ux%u at 0x%llX\n", HalFramebuffer.Width, HalFramebuffer.Height,
                 HalFramebuffer.Bpp, HalFramebuffer.PhysicalAddress);
    } else {
        DbgPrint("HAL: no linear framebuffer -- running headless on COM1\n");
    }
    Win32kBootSplash(8, "Initializing executive");

    ObInitSystem();
    ExInitSystem();
    HalInitializeTime();
    Win32kBootSplash(20, "Creating system process");
    PsInitSystem();
    DbgPrint("PS: System process created (pid %u)\n", PsInitialSystemProcess->UniqueProcessId);

    PsCreateSystemThread(KiPhase1Initialization, NULL, 10, "Phase1Initialization", NULL);
    KiIdleLoop();
}
