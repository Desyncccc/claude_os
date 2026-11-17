/*
 * RadiaNT -- win32k/terminal.c
 * The Terminal: a scrolling colour console plus the built-in command shell.
 * User-mode processes started from here write to the same console through
 * the NtDisplayString system service.
 */
#include "win32k.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"
#include "io.h"
#include "cm.h"
#include "hal.h"
#include "rtl.h"
#include "ntapi.h"

#define T_COLS    96
#define T_ROWS    27
#define T_PAD     12
#define T_CH      18
#define T_SCROLL  500
#define T_HISTORY 16

typedef struct { CHAR Ch; UCHAR Color; } CELL;

typedef struct _CONSOLE {
    WINDOW    *Wnd;
    KSPIN_LOCK Lock;
    CELL     (*Lines)[T_COLS];
    int        First, Count, CurX, Scroll;
    UCHAR      Color;
    char       Input[160];
    int        InputLen;
    char       History[T_HISTORY][160];
    int        HistCount, HistPos;
    PEPROCESS  Foreground;
    BOOLEAN    Blink;
    ULONG      Ticks;
} CONSOLE;

static const ULONG ConPalette[] = {
    COL_TEXT, COL_DIM, COL_PINK, COL_ORANGE, COL_CYAN, COL_GREEN, COL_RED, COL_VIOLET, 0xFFFFFF, COL_YELLOW,
};

/* ---- Console buffer ------------------------------------------------------------------- */
static CELL *ConLine(CONSOLE *c, int i) { return c->Lines[(c->First + i) % T_SCROLL]; }

static void ConNewLine(CONSOLE *c)
{
    if (c->Count == T_SCROLL) c->First = (c->First + 1) % T_SCROLL;
    else c->Count++;
    memset(ConLine(c, c->Count - 1), 0, sizeof(CELL) * T_COLS);
    c->CurX = 0;
}

static void ConPutChar(CONSOLE *c, char ch)
{
    switch (ch) {
    case '\n': ConNewLine(c); return;
    case '\r': c->CurX = 0; return;
    case '\t': do { ConPutChar(c, ' '); } while (c->CurX & 7); return;
    case '\b':
        if (c->CurX > 0) { c->CurX--; ConLine(c, c->Count - 1)[c->CurX].Ch = 0; }
        return;
    default: break;
    }
    if ((UCHAR)ch < 32 || (UCHAR)ch > 126) ch = '?';
    if (c->CurX >= T_COLS) ConNewLine(c);
    CELL *cell = &ConLine(c, c->Count - 1)[c->CurX++];
    cell->Ch = ch; cell->Color = c->Color;
}

void ConWrite(PVOID Console, const char *Text, SIZE_T Length)
{
    CONSOLE *c = Console;
    if (!c) return;
    KIRQL irql; KeAcquireSpinLock(&c->Lock, &irql);
    for (SIZE_T i = 0; i < Length; i++) ConPutChar(c, Text[i]);
    c->Scroll = 0;
    KeReleaseSpinLock(&c->Lock, irql);
    c->Wnd->NeedsPaint = TRUE;
}

void ConSetColor(PVOID Console, ULONG ColorIndex)
{
    CONSOLE *c = Console;
    if (c) c->Color = (UCHAR)(ColorIndex < ARRAY_SIZE(ConPalette) ? ColorIndex : 0);
}

/* A process may outlive its console window: look the console up and use it
 * without letting the window manager free it in between. */
void ConWriteForProcess(PVOID Process, const char *Text, SIZE_T Length)
{
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    PVOID con = ((PEPROCESS)Process)->Console;
    if (con) ConWrite(con, Text, Length);
    KeLowerIrql(irql);
    if (con) KeSetEvent(&IoInputEvent);         /* wake the desktop thread to repaint */
}

void ConSetColorForProcess(PVOID Process, ULONG ColorIndex)
{
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    ConSetColor(((PEPROCESS)Process)->Console, ColorIndex);
    KeLowerIrql(irql);
}

static void ConPrintf(CONSOLE *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ConPrintf(CONSOLE *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = RtlStringVPrintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ConWrite(c, buf, MIN((SIZE_T)n, sizeof(buf) - 1));
}

static void ConColorPrintf(CONSOLE *c, ULONG color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ConColorPrintf(CONSOLE *c, ULONG color, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = RtlStringVPrintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ConSetColor(c, color);
    ConWrite(c, buf, MIN((SIZE_T)n, sizeof(buf) - 1));
    ConSetColor(c, CON_DEFAULT);
}

static void ConPrompt(CONSOLE *c)
{
    if (c->CurX) ConWrite(c, "\n", 1);
    ConColorPrintf(c, CON_PINK, "RadiaNT");
    ConColorPrintf(c, CON_VIOLET, " \\System");
    ConColorPrintf(c, CON_DIM, " > ");
}

/* ---- Shell commands --------------------------------------------------------------------- */
static const char *const StateNames[] = { "Init", "Ready", "Running", "Waiting", "Dead" };
static PS_SNAPSHOT_ENTRY ShSnapshot[64];
static OB_DIRECTORY_ENTRY ShDir[64];
static POOL_TAG_INFO ShTags[96];
static OBJECT_TYPE ShTypes[32];

static void CmdHelp(CONSOLE *c)
{
    static const struct { const char *n, *d; } cmds[] = {
        { "sysinfo",      "System summary" },
        { "ps",           "Processes and threads (scheduler view)" },
        { "mem",          "Physical memory and pool usage" },
        { "pool",         "Pool allocations by tag" },
        { "obj [path]",   "List an object manager directory (default \\)" },
        { "types",        "Object types and live object counts" },
        { "drivers",      "Loaded drivers and their devices" },
        { "irq",          "Hardware interrupt counters" },
        { "syscalls",     "System service table and call counts" },
        { "reg [key]",    "Browse the registry (default \\Registry)" },
        { "ls",           "Boot images in \\SystemRoot" },
        { "run <image>",  "Start a user-mode PE program (or just type its name)" },
        { "kill <pid>",   "Terminate a process" },
        { "dmesg",        "Kernel boot log" },
        { "date, uptime", "Clock" },
        { "echo, clear",  "Console helpers" },
        { "bugcheck",     "Crash the kernel on purpose" },
        { "reboot, shutdown", "Power" },
    };
    ConColorPrintf(c, CON_ORANGE, "Built-in commands\n");
    for (ULONG i = 0; i < ARRAY_SIZE(cmds); i++) {
        ConColorPrintf(c, CON_CYAN, "  %-18s", cmds[i].n);
        ConPrintf(c, "%s\n", cmds[i].d);
    }
    ConColorPrintf(c, CON_DIM, "Programs: hello, primes, clock, crash, spin   (Ctrl+C stops one, PgUp/PgDn scrolls)\n");
}

static void CmdSysinfo(CONSOLE *c)
{
    static const char *const art[] = {
        "       /\\       ", "      /  \\      ", "     / /\\ \\     ", "    / /  \\ \\    ",
        "    \\ \\  / /    ", "     \\ \\/ /     ", "      \\  /      ", "       \\/       ",
    };
    char info[8][96];
    ULONG64 up = KeTickCount / 1000;
    RtlStringPrintf(info[0], 96, RADIANT_NAME " OS " RADIANT_VERSION_STR " \"" RADIANT_CODENAME "\"");
    RtlStringPrintf(info[1], 96, "Kernel    NT-architecture hybrid kernel, x64, uniprocessor");
    RtlStringPrintf(info[2], 96, "CPU       %s", HalCpuInfo.Brand);
    RtlStringPrintf(info[3], 96, "Memory    %llu MB used of %llu MB",
                    ((MmTotalPages - MmFreePagesCount) * PAGE_SIZE) >> 20, (MmTotalPages * PAGE_SIZE) >> 20);
    RtlStringPrintf(info[4], 96, "Display   %ux%u, %u bpp", HalFramebuffer.Width, HalFramebuffer.Height, HalFramebuffer.Bpp);
    RtlStringPrintf(info[5], 96, "Firmware  %s via %s", KeLoaderBlock.FirmwareIsEfi ? "UEFI" : "BIOS", KeLoaderBlock.LoaderName);
    RtlStringPrintf(info[6], 96, "Uptime    %lluh %llum %llus", up / 3600, up / 60 % 60, up % 60);
    RtlStringPrintf(info[7], 96, "Objects   %u processes, %u threads, %u registry keys", PsProcessCount, PsThreadCount, CmKeyCount);
    for (int i = 0; i < 8; i++) {
        ConColorPrintf(c, i < 4 ? CON_PINK : CON_ORANGE, "%s", art[i]);
        if (i == 0) ConColorPrintf(c, CON_WHITE, "%s\n", info[i]);
        else {
            ConColorPrintf(c, CON_CYAN, "%.10s", info[i]);
            ConPrintf(c, "%s\n", info[i] + 10);
        }
    }
}

static void CmdPs(CONSOLE *c)
{
    ULONG n = PsSnapshot(ShSnapshot, ARRAY_SIZE(ShSnapshot));
    ConColorPrintf(c, CON_ORANGE, "  PID   TID  PRI  STATE     MODE    KTIME   UTIME      CSW  HND  IMAGE!THREAD\n");
    for (ULONG i = 0; i < n; i++) {
        PS_SNAPSHOT_ENTRY *e = &ShSnapshot[i];
        ConSetColor(c, e->State == Running ? CON_GREEN : (e->UserMode ? CON_CYAN : CON_DEFAULT));
        ConPrintf(c, "%5u %5u  %3u  %-8s  %-6s %6llu  %6llu  %7llu  %3u  %s!%s\n",
                  e->Pid, e->Tid, e->Priority, StateNames[MIN(e->State, 4)], e->UserMode ? "user" : "kernel",
                  e->KernelTime, e->UserTime, e->ContextSwitches, e->Handles, e->ProcessName, e->ThreadName);
    }
    ConSetColor(c, CON_DEFAULT);
    ConColorPrintf(c, CON_DIM, "%u threads. Times are clock ticks (1 ms). %llu context switches since boot.\n",
                   n, KeContextSwitches);
}

static void CmdMem(CONSOLE *c)
{
    ULONG64 used = MmTotalPages - MmFreePagesCount;
    ConColorPrintf(c, CON_ORANGE, "Physical memory\n");
    ConPrintf(c, "  Total        %8llu pages  %6llu MB\n", MmTotalPages, (MmTotalPages * PAGE_SIZE) >> 20);
    ConPrintf(c, "  In use       %8llu pages  %6llu MB\n", used, (used * PAGE_SIZE) >> 20);
    ConPrintf(c, "  Free         %8llu pages  %6llu MB\n", MmFreePagesCount, (MmFreePagesCount * PAGE_SIZE) >> 20);
    ConPrintf(c, "  Kernel image %8llu pages  %6llu KB\n", MmKernelImagePages, MmKernelImagePages * 4);
    ConColorPrintf(c, CON_ORANGE, "Executive pool\n");
    ConPrintf(c, "  Committed    %8llu KB\n  Allocated    %8llu KB\n", ExPoolBytesTotal >> 10, ExPoolBytesInUse >> 10);
    ConColorPrintf(c, CON_ORANGE, "Virtual layout\n");
    ConPrintf(c, "  User space     0x%016llX - 0x%016llX\n", 0x10000ull, MM_USER_LIMIT - 1);
    ConPrintf(c, "  Physical map   0x%016llX  (%llu GB mapped, 2 MB pages)\n", MM_PHYSMAP_BASE,
              MAX(4ull, (MmHighestPhysicalPage * PAGE_SIZE + (1ull << 30) - 1) >> 30));
    ConPrintf(c, "  Kernel image   0x%016llX\n", MM_KERNEL_BASE);
    ConPrintf(c, "  System PML4    0x%016llX (physical)\n", MmSystemPml4);
}

static void CmdPool(CONSOLE *c)
{
    ULONG n = ExQueryPoolTags(ShTags, ARRAY_SIZE(ShTags));
    ConColorPrintf(c, CON_ORANGE, "  TAG     ALLOCS    FREES     LIVE      BYTES\n");
    for (ULONG i = 0; i < n; i++) {
        POOL_TAG_INFO *t = &ShTags[i];
        char tag[5] = { (char)t->Tag, (char)(t->Tag >> 8), (char)(t->Tag >> 16), (char)(t->Tag >> 24), 0 };
        ConColorPrintf(c, CON_CYAN, "  %-4s", tag);
        ConPrintf(c, "  %8u %8u %8u %10llu\n", t->Allocs, t->Frees, t->Allocs - t->Frees, t->Bytes);
    }
}

static void CmdObj(CONSOLE *c, const char *path)
{
    if (!*path) path = "\\";
    ULONG n = ObQueryDirectory(path, ShDir, ARRAY_SIZE(ShDir));
    ConColorPrintf(c, CON_ORANGE, "Directory of %s\n", path);
    for (ULONG i = 0; i < n; i++) {
        OB_DIRECTORY_ENTRY *e = &ShDir[i];
        ConColorPrintf(c, e->IsDirectory ? CON_PINK : CON_CYAN, "  %-28s", e->Name);
        ConColorPrintf(c, CON_DIM, "%-14s", e->TypeName);
        if (e->Target[0]) ConPrintf(c, "-> %s", e->Target);
        else ConColorPrintf(c, CON_DIM, "refs %d  handles %d", e->PointerCount, e->HandleCount);
        ConWrite(c, "\n", 1);
    }
    if (!n) ConColorPrintf(c, CON_DIM, "  (empty, or not a directory)\n");
}

static void CmdTypes(CONSOLE *c)
{
    ULONG n = ObQueryTypes(ShTypes, ARRAY_SIZE(ShTypes));
    ConColorPrintf(c, CON_ORANGE, "  TYPE            OBJECTS   PEAK  HANDLES  TAG\n");
    for (ULONG i = 0; i < n; i++) {
        OBJECT_TYPE *t = &ShTypes[i];
        char tag[5] = { (char)t->PoolTag, (char)(t->PoolTag >> 8), (char)(t->PoolTag >> 16), (char)(t->PoolTag >> 24), 0 };
        ConColorPrintf(c, CON_CYAN, "  %-14s", t->Name);
        ConPrintf(c, "  %7u  %5u  %7u  %s\n", t->TotalNumberOfObjects, t->HighWaterNumberOfObjects,
                  t->TotalNumberOfHandles, tag);
    }
}

static void CmdDrivers(CONSOLE *c)
{
    ULONG n = ObQueryDirectory("\\Driver", ShDir, ARRAY_SIZE(ShDir));
    for (ULONG i = 0; i < n; i++) {
        char path[80];
        PDRIVER_OBJECT drv;
        RtlStringPrintf(path, sizeof(path), "\\Driver\\%s", ShDir[i].Name);
        if (!NT_SUCCESS(ObReferenceObjectByName(path, IoDriverObjectType, (PVOID *)&drv))) continue;
        ConColorPrintf(c, CON_PINK, "  \\Driver\\%-12s", drv->Name);
        ConPrintf(c, "%s\n", drv->Description);
        for (PDEVICE_OBJECT d = drv->DeviceObject; d; d = d->NextDevice) {
            ConColorPrintf(c, CON_CYAN, "      \\Device\\%-18s", d->Name);
            ConColorPrintf(c, CON_DIM, "type 0x%02X  %llu IRPs\n", d->DeviceType, d->IrpCount);
        }
        ObDereferenceObject(drv);
    }
}

static void CmdIrq(CONSOLE *c)
{
    ConColorPrintf(c, CON_ORANGE, "  IRQ  VECTOR       COUNT  SOURCE\n");
    for (ULONG i = 0; i < HAL_IRQ_COUNT; i++) {
        if (!HalIrqNames[i]) continue;
        ConPrintf(c, "  %3u    0x%02X  %10llu  %s\n", i, 0x20 + i, HalIrqCounts[i], HalIrqNames[i]);
    }
    ConColorPrintf(c, CON_DIM, "  %llu interrupts, %llu DPCs, %llu exceptions, current IRQL %u\n",
                   KeInterruptCount, KeDpcCount, KeExceptionCount, KeGetCurrentIrql());
}

static void CmdSyscalls(CONSOLE *c)
{
    ConColorPrintf(c, CON_ORANGE, "  NUM  SERVICE                         CALLS\n");
    for (ULONG i = 0; i < KiServiceLimit; i++) {
        ConColorPrintf(c, CON_DIM, "  %3u  ", i);
        ConColorPrintf(c, CON_CYAN, "%-28s", KiServiceNames[i]);
        ConPrintf(c, "%9llu\n", KiServiceCounts[i]);
    }
    ConColorPrintf(c, CON_DIM, "  Entry: SYSCALL, RAX = number, R10/RDX/R8/R9 = arguments. %llu calls total.\n", KeSystemCalls);
}

static void CmdReg(CONSOLE *c, const char *path)
{
    CM_KEY_NODE *key = CmOpenKey(*path ? path : "\\Registry");
    if (!key) { ConColorPrintf(c, CON_RED, "Key not found: %s\n", path); return; }
    char full[200] = "";
    const CM_KEY_NODE *chain[12]; int depth = 0;
    for (const CM_KEY_NODE *k = key; k && depth < 12; k = k->Parent) chain[depth++] = k;
    for (int i = depth - 1; i >= 0; i--) { RtlStringCat(full, sizeof(full), "\\"); RtlStringCat(full, sizeof(full), chain[i]->Name); }
    ConColorPrintf(c, CON_ORANGE, "%s\n", full);
    for (CM_KEY_NODE *k = key->FirstChild; k; k = k->NextSibling) ConColorPrintf(c, CON_PINK, "  [%s]\n", k->Name);
    for (CM_KEY_VALUE *v = key->Values; v; v = v->Next) {
        ConColorPrintf(c, CON_CYAN, "  %-22s", v->Name);
        if (v->Type == REG_DWORD) { ConColorPrintf(c, CON_DIM, "REG_DWORD  "); ConPrintf(c, "0x%08X (%u)\n", v->Dword, v->Dword); }
        else { ConColorPrintf(c, CON_DIM, "REG_SZ     "); ConPrintf(c, "%s\n", v->String); }
    }
}

static void CmdLs(CONSOLE *c)
{
    ConColorPrintf(c, CON_ORANGE, "Directory of \\SystemRoot\n");
    for (ULONG i = 0; i < KeLoaderBlock.ModuleCount; i++) {
        KE_BOOT_MODULE *m = &KeLoaderBlock.Modules[i];
        ConColorPrintf(c, CON_GREEN, "  %-16s", m->Name);
        ConPrintf(c, "%8llu bytes   PE32+ image\n", m->End - m->Start);
    }
    if (!KeLoaderBlock.ModuleCount) ConColorPrintf(c, CON_DIM, "  (no boot images were loaded)\n");
}

static void CmdRun(CONSOLE *c, const char *image)
{
    PEPROCESS p;
    NTSTATUS st = PsCreateUserProcess(image, c, &p);
    if (st == STATUS_OBJECT_NAME_NOT_FOUND) {
        ConColorPrintf(c, CON_RED, "'%s' is not a command or a program in \\SystemRoot. Try 'help'.\n", image);
        return;
    }
    if (!NT_SUCCESS(st)) { ConColorPrintf(c, CON_RED, "Cannot start %s: status 0x%08X\n", image, (ULONG)st); return; }
    c->Foreground = p;
}

static void Execute(CONSOLE *c, char *line)
{
    while (*line == ' ') line++;
    SIZE_T len = strlen(line);
    while (len && line[len - 1] == ' ') line[--len] = 0;
    if (!*line) return;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = line + len;
#define IS(s) (!RtlCompareStringI(line, s))
    if (IS("help") || IS("?")) CmdHelp(c);
    else if (IS("sysinfo") || IS("ver")) CmdSysinfo(c);
    else if (IS("ps") || IS("tlist")) CmdPs(c);
    else if (IS("mem")) CmdMem(c);
    else if (IS("pool")) CmdPool(c);
    else if (IS("obj") || IS("dir")) CmdObj(c, arg);
    else if (IS("types")) CmdTypes(c);
    else if (IS("drivers")) CmdDrivers(c);
    else if (IS("irq")) CmdIrq(c);
    else if (IS("syscalls")) CmdSyscalls(c);
    else if (IS("reg")) CmdReg(c, arg);
    else if (IS("ls")) CmdLs(c);
    else if (IS("run")) { if (*arg) CmdRun(c, arg); else ConPrintf(c, "usage: run <image>\n"); }
    else if (IS("kill")) {
        ULONG pid = (ULONG)RtlStringToInteger(arg, 0);
        NTSTATUS st = PsTerminateProcessById(pid, STATUS_CONTROL_C_EXIT);
        if (NT_SUCCESS(st)) ConPrintf(c, "Process %u terminated.\n", pid);
        else ConColorPrintf(c, CON_RED, "Cannot terminate pid %u (status 0x%08X).\n", pid, (ULONG)st);
    }
    else if (IS("dmesg")) {
        for (ULONG i = 0; i < RtlBootLogCount; i++) ConColorPrintf(c, CON_DIM, "%s\n", RtlBootLog[i]);
    }
    else if (IS("date")) {
        TIME_FIELDS t; KeQuerySystemTimeFields(&t);
        ConPrintf(c, "%04u-%02u-%02u %02u:%02u:%02u UTC\n", t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second);
    }
    else if (IS("uptime")) {
        ULONG64 ms = KeTickCount;
        ConPrintf(c, "Up %llu.%03llu s, %llu%% idle, %llu context switches\n", ms / 1000, ms % 1000,
                  KeIdleTicks * 100 / MAX(ms, 1), KeContextSwitches);
    }
    else if (IS("echo")) ConPrintf(c, "%s\n", arg);
    else if (IS("clear") || IS("cls")) {
        KIRQL irql; KeAcquireSpinLock(&c->Lock, &irql);
        c->First = 0; c->Count = 0; ConNewLine(c);
        KeReleaseSpinLock(&c->Lock, irql);
    }
    else if (IS("bugcheck")) KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0x52414449414E54ull, KeTickCount, 0, 0);
    else if (IS("reboot")) HalReturnToFirmware(TRUE);
    else if (IS("shutdown")) HalReturnToFirmware(FALSE);
    else if (IS("sysmon")) ShellLaunch(APP_SYSMON);
    else if (IS("objects")) ShellLaunch(APP_OBJECTS);
    else if (IS("nebula")) ShellLaunch(APP_NEBULA);
    else if (IS("about")) ShellLaunch(APP_ABOUT);
    else CmdRun(c, line);
#undef IS
}

/* ---- Window procedure ------------------------------------------------------------------- */
static void TerminalPaint(WINDOW *w, CONSOLE *c)
{
    static CELL view[T_ROWS][T_COLS];
    GDI_SURFACE *S = w->Client;
    int cw = GdiFontMono.Glyphs['M' - 32].Advance;

    KIRQL irql; KeAcquireSpinLock(&c->Lock, &irql);
    int start = MAX(0, c->Count - T_ROWS - c->Scroll);
    int rows = MIN(T_ROWS, c->Count - start);
    for (int r = 0; r < rows; r++) memcpy(view[r], ConLine(c, start + r), sizeof(CELL) * T_COLS);
    int curRow = c->Count - 1 - start, curX = c->CurX;
    KeReleaseSpinLock(&c->Lock, irql);

    GdiFill(S, 0, 0, S->Width, S->Height, 0x0C0A18);
    for (int r = 0; r < rows; r++) {
        int y = T_PAD + r * T_CH;
        for (int col = 0; col < T_COLS; col++) {
            CELL cell = view[r][col];
            if (cell.Ch && cell.Ch != ' ')
                GdiDrawTextN(S, &GdiFontMono, T_PAD + col * cw, y, &cell.Ch, 1,
                             OPAQUE(ConPalette[cell.Color % ARRAY_SIZE(ConPalette)]));
        }
    }
    if (c->Scroll) {
        const char *t = "SCROLLBACK";
        int tw = GdiTextWidth(&GdiFontSmall, t) + 16;
        GdiRoundRect(S, S->Width - tw - 10, 8, tw, 20, 10, ARGB(220, COL_VIOLET), ARGB(220, COL_VIOLET));
        GdiDrawText(S, &GdiFontSmall, S->Width - tw - 2, 11, t, OPAQUE(0xFFFFFF));
    } else if (curRow >= 0 && curRow < T_ROWS && curX < T_COLS) {
        ULONG a = c->Foreground ? 70 : (c->Blink ? 230 : 60);
        GdiBlend(S, T_PAD + curX * cw, T_PAD + curRow * T_CH + 1, cw, T_CH - 2, ARGB(a, COL_PINK));
    }
}

static LONG_PTR TerminalProc(WINDOW *w, ULONG msg, ULONG_PTR wp, LONG_PTR lp)
{
    CONSOLE *c = w->UserData;
    (void)lp;
    switch (msg) {
    case WM_PAINT:
        TerminalPaint(w, c);
        break;
    case WM_TIMER:
        if (c->Foreground && c->Foreground->Exited) {
            NTSTATUS st = c->Foreground->ExitStatus;
            if (st != STATUS_SUCCESS)
                ConColorPrintf(c, st == STATUS_CONTROL_C_EXIT ? CON_DIM : CON_RED,
                               "[%s exited with status 0x%08X]\n", c->Foreground->ImageFileName, (ULONG)st);
            ObDereferenceObject(c->Foreground);
            c->Foreground = NULL;
            ConPrompt(c);
        }
        if (++c->Ticks % 5 == 0) { c->Blink = !c->Blink; WmInvalidate(w); }
        break;
    case WM_KEYDOWN: {
        ULONG vk = (ULONG)wp & 0xFFF;
        if (vk == VK_PRIOR) c->Scroll = MIN(c->Scroll + T_ROWS / 2, MAX(0, c->Count - T_ROWS));
        else if (vk == VK_NEXT) c->Scroll = MAX(0, c->Scroll - T_ROWS / 2);
        else if ((vk == VK_UP || vk == VK_DOWN) && !c->Foreground && c->HistCount) {
            c->HistPos = MIN(MAX(c->HistPos + (vk == VK_UP ? -1 : 1), 0), c->HistCount);
            while (c->InputLen) { ConWrite(c, "\b", 1); c->InputLen--; }
            if (c->HistPos < c->HistCount) {
                RtlStringCopy(c->Input, sizeof(c->Input), c->History[c->HistPos]);
                c->InputLen = (int)strlen(c->Input);
                ConWrite(c, c->Input, c->InputLen);
            }
        }
        WmInvalidate(w);
        break;
    }
    case WM_CHAR: {
        char ch = (char)wp;
        c->Blink = TRUE;
        if (c->Foreground) {
            if (ch == 3) {
                ConColorPrintf(c, CON_DIM, "^C\n");
                PsTerminateProcess(c->Foreground, STATUS_CONTROL_C_EXIT);
            }
            break;
        }
        if (ch == '\n') {
            ConWrite(c, "\n", 1);
            c->Input[c->InputLen] = 0;
            if (c->InputLen) {
                if (c->HistCount == T_HISTORY) {
                    memmove(c->History[0], c->History[1], sizeof(c->History[0]) * (T_HISTORY - 1));
                    c->HistCount--;
                }
                RtlStringCopy(c->History[c->HistCount++], sizeof(c->History[0]), c->Input);
            }
            c->HistPos = c->HistCount;
            c->InputLen = 0;
            Execute(c, c->Input);
            if (!c->Foreground) ConPrompt(c);
        } else if (ch == '\b') {
            if (c->InputLen) { c->InputLen--; ConWrite(c, "\b", 1); }
        } else if (ch == 3) {
            ConColorPrintf(c, CON_DIM, "^C");
            c->InputLen = 0;
            ConPrompt(c);
        } else if (ch == 12) {
            char clear[] = "clear";
            Execute(c, clear);
            ConPrompt(c);
        } else if (ch >= 32 && ch < 127 && c->InputLen < (int)sizeof(c->Input) - 1 && c->InputLen < T_COLS * 2 - 20) {
            c->Input[c->InputLen++] = ch;
            ConWrite(c, &ch, 1);
        }
        WmInvalidate(w);
        break;
    }
    case WM_CLOSE:
        PsDetachConsole(c, TRUE);
        if (c->Foreground) { ObDereferenceObject(c->Foreground); c->Foreground = NULL; }
        break;
    case WM_DESTROY:
        MmFreeContiguousMemory(c->Lines, sizeof(CELL) * T_COLS * T_SCROLL);
        ExFreePool(c);
        break;
    default: break;
    }
    return 0;
}

void TerminalCreate(void)
{
    CONSOLE *c = ExAllocatePoolWithTag(sizeof(CONSOLE), TAG('C', 'o', 'n', 's'));
    if (!c) return;
    c->Lines = MmAllocateContiguousMemory(sizeof(CELL) * T_COLS * T_SCROLL);
    if (!c->Lines) { ExFreePool(c); return; }
    int cw = GdiFontMono.Glyphs['M' - 32].Advance;
    WINDOW *w = WmCreateWindow("Terminal", T_COLS * cw + 2 * T_PAD, T_ROWS * T_CH + 2 * T_PAD,
                               TerminalProc, c, APP_TERMINAL, COL_PINK);
    if (!w) { MmFreeContiguousMemory(c->Lines, sizeof(CELL) * T_COLS * T_SCROLL); ExFreePool(c); return; }
    c->Wnd = w;
    w->Opacity = 222;
    ConNewLine(c);
    ConColorPrintf(c, CON_PINK, RADIANT_NAME " OS ");
    ConColorPrintf(c, CON_WHITE, "[Version " RADIANT_VERSION_STR "]");
    ConColorPrintf(c, CON_DIM, "  x64, NT-architecture kernel\n");
    ConColorPrintf(c, CON_DIM, "Type 'help' for commands, 'sysinfo' for a summary, or 'hello' to run a user-mode program.\n\n");
    ConPrompt(c);
    WmSetTimer(w, 100);
}
