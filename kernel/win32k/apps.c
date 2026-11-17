/*
 * RadiaNT -- win32k/apps.c
 * Built-in desktop applications: System Monitor, Object Explorer and Nebula.
 */
#include "win32k.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"
#include "hal.h"
#include "rtl.h"

static void Card(GDI_SURFACE *S, int x, int y, int w, int h)
{
    GdiRoundRect(S, x, y, w, h, 12, ARGB(70, 0x2C2658), ARGB(50, 0x15122B));
    GdiRoundFrame(S, x, y, w, h, 12, 1, ARGB(46, 0xFFFFFF), ARGB(24, 0xFFFFFF));
}

/* ======================================================================================
 *  System Monitor
 * ==================================================================================== */
#define SM_HISTORY 60
typedef struct _SYSMON {
    UCHAR   Cpu[SM_HISTORY], Mem[SM_HISTORY];
    ULONG64 LastTick, LastIdle, LastCsw, LastIrq, LastSys, LastDpc;
    ULONG   CswRate, IrqRate, SysRate, DpcRate, CpuNow;
    struct { ULONG Tid; ULONG64 Time; } Prev[64];
    ULONG   PrevCount;
    struct { CHAR Name[64]; ULONG Pid, Permille; UCHAR State; BOOLEAN User; } Rows[10];
    ULONG   RowCount, ThreadCount;
} SYSMON;

static PS_SNAPSHOT_ENTRY SmSnapshot[64];

static void SysmonSample(SYSMON *m)
{
    ULONG64 now = KeTickCount, span = MAX(now - m->LastTick, 1);
    ULONG64 idle = KeIdleTicks;
    m->CpuNow = (ULONG)(100 - MIN((idle - m->LastIdle) * 100 / span, 100));
    memmove(m->Cpu, m->Cpu + 1, SM_HISTORY - 1);
    memmove(m->Mem, m->Mem + 1, SM_HISTORY - 1);
    m->Cpu[SM_HISTORY - 1] = (UCHAR)m->CpuNow;
    m->Mem[SM_HISTORY - 1] = (UCHAR)((MmTotalPages - MmFreePagesCount) * 100 / MAX(MmTotalPages, 1));
    m->CswRate = (ULONG)((KeContextSwitches - m->LastCsw) * 1000 / span);
    m->IrqRate = (ULONG)((KeInterruptCount - m->LastIrq) * 1000 / span);
    m->SysRate = (ULONG)((KeSystemCalls - m->LastSys) * 1000 / span);
    m->DpcRate = (ULONG)((KeDpcCount - m->LastDpc) * 1000 / span);
    m->LastTick = now; m->LastIdle = idle; m->LastCsw = KeContextSwitches;
    m->LastIrq = KeInterruptCount; m->LastSys = KeSystemCalls; m->LastDpc = KeDpcCount;

    /* per-thread CPU share since the previous sample */
    ULONG n = PsSnapshot(SmSnapshot, ARRAY_SIZE(SmSnapshot));
    m->ThreadCount = n;
    m->RowCount = 0;
    ULONG share[64];
    for (ULONG i = 0; i < n; i++) {
        ULONG64 total = SmSnapshot[i].KernelTime + SmSnapshot[i].UserTime, prev = total;
        for (ULONG j = 0; j < m->PrevCount; j++)
            if (m->Prev[j].Tid == SmSnapshot[i].Tid) { prev = m->Prev[j].Time; break; }
        share[i] = (ULONG)((total - prev) * 1000 / span);
    }
    for (ULONG i = 0; i < n; i++) {
        m->Prev[i].Tid = SmSnapshot[i].Tid;
        m->Prev[i].Time = SmSnapshot[i].KernelTime + SmSnapshot[i].UserTime;
    }
    m->PrevCount = n;
    BOOLEAN used[64] = { 0 };
    for (ULONG r = 0; r < ARRAY_SIZE(m->Rows) && r < n; r++) {
        int best = -1;
        for (ULONG i = 0; i < n; i++) {
            if (used[i] || SmSnapshot[i].Pid == 0) continue;
            if (best < 0 || share[i] > share[best]) best = (int)i;
        }
        if (best < 0) break;
        used[best] = TRUE;
        RtlStringPrintf(m->Rows[r].Name, sizeof(m->Rows[r].Name), "%s!%s",
                        SmSnapshot[best].ProcessName, SmSnapshot[best].ThreadName);
        m->Rows[r].Pid = SmSnapshot[best].Pid;
        m->Rows[r].Permille = share[best];
        m->Rows[r].State = SmSnapshot[best].State;
        m->Rows[r].User = SmSnapshot[best].UserMode;
        m->RowCount = r + 1;
    }
}

static void Graph(GDI_SURFACE *S, int x, int y, int w, int h, const UCHAR *data, ULONG c1, ULONG c2)
{
    for (int i = 1; i < 4; i++) GdiBlend(S, x, y + h * i / 4, w, 1, ARGB(18, 0xFFFFFF));
    int prevY = 0;
    for (int px = 0; px < w; px++) {
        int f = px * (SM_HISTORY - 1) * 256 / MAX(w - 1, 1);
        int i0 = f >> 8, i1 = MIN(i0 + 1, SM_HISTORY - 1), fr = f & 255;
        int v = (data[i0] * (256 - fr) + data[i1] * fr) >> 8;
        int top = y + h - 1 - v * (h - 2) / 100;
        ULONG col = GdiLerpColor(c1, c2, px, w - 1);
        for (int yy = top; yy < y + h; yy++)
            GdiPixel(S, x + px, yy, ARGB(12 + 80 * (y + h - yy) / h, col));
        if (px) GdiLine(S, x + px - 1, prevY, x + px, top, OPAQUE(col));
        GdiPixel(S, x + px, top + 1, ARGB(120, col));
        prevY = top;
    }
}

static LONG_PTR SysmonProc(WINDOW *w, ULONG msg, ULONG_PTR wp, LONG_PTR lp)
{
    SYSMON *m = w->UserData;
    (void)wp; (void)lp;
    if (msg == WM_TIMER) { SysmonSample(m); WmInvalidate(w); return 0; }
    if (msg == WM_DESTROY) { ExFreePool(m); return 0; }
    if (msg != WM_PAINT) return 0;

    static const char *const states[] = { "Init", "Ready", "Running", "Waiting", "Dead" };
    GDI_SURFACE *S = w->Client;
    int W = S->Width, H = S->Height;
    char buf[96];
    GdiGradientV(S, 0, 0, W, H, OPAQUE(0x151130), OPAQUE(0x0A0815));

    int cw = (W - 48) / 2;
    for (int k = 0; k < 2; k++) {
        int x = 16 + k * (cw + 16), y = 14;
        Card(S, x, y, cw, 158);
        GdiDrawText(S, &GdiFontSmall, x + 16, y + 12, k ? "MEMORY" : "PROCESSOR", OPAQUE(COL_DIM));
        if (k == 0) RtlStringPrintf(buf, sizeof(buf), "%u%%", m->CpuNow);
        else RtlStringPrintf(buf, sizeof(buf), "%llu MB", ((MmTotalPages - MmFreePagesCount) * PAGE_SIZE) >> 20);
        int tx = GdiDrawText(S, &GdiFontTitle, x + 16, y + 26, buf, OPAQUE(0xFFFFFF));
        if (k == 1) {
            RtlStringPrintf(buf, sizeof(buf), "of %llu MB", (MmTotalPages * PAGE_SIZE) >> 20);
            GdiDrawText(S, &GdiFontUi, tx + 8, y + 42, buf, OPAQUE(COL_DIM));
        }
        GdiSetClip(S, x + 12, y + 70, cw - 24, 78);
        Graph(S, x + 12, y + 70, cw - 24, 78, k ? m->Mem : m->Cpu, k ? COL_VIOLET : COL_PINK, k ? COL_CYAN : COL_ORANGE);
        GdiResetClip(S);
    }

    int y = 186;
    Card(S, 16, y, W - 32, 52);
    static const char *const labels[] = { "Threads", "Ctx switch/s", "Interrupts/s", "DPCs/s", "Syscalls/s", "Frames/s" };
    ULONG vals[6] = { m->ThreadCount, m->CswRate, m->IrqRate, m->DpcRate, m->SysRate, WmFps };
    for (int i = 0; i < 6; i++) {
        int x = 30 + i * (W - 60) / 6;
        GdiDrawText(S, &GdiFontSmall, x, y + 9, labels[i], OPAQUE(COL_DIM));
        RtlStringPrintf(buf, sizeof(buf), "%u", vals[i]);
        GdiDrawText(S, &GdiFontUiBold, x, y + 26, buf, OPAQUE(COL_TEXT));
    }

    y = 252;
    Card(S, 16, y, W - 32, H - y - 14);
    GdiDrawText(S, &GdiFontSmall, 30, y + 10, "THREAD", OPAQUE(COL_DIM));
    GdiDrawText(S, &GdiFontSmall, W - 250, y + 10, "PID", OPAQUE(COL_DIM));
    GdiDrawText(S, &GdiFontSmall, W - 200, y + 10, "STATE", OPAQUE(COL_DIM));
    GdiDrawText(S, &GdiFontSmall, W - 130, y + 10, "CPU", OPAQUE(COL_DIM));
    for (ULONG r = 0; r < m->RowCount; r++) {
        int ry = y + 32 + (int)r * 21;
        if (ry + 20 > H - 16) break;
        ULONG c = m->Rows[r].User ? COL_CYAN : COL_TEXT;
        GdiCircle(S, 34, ry + 9, 3, OPAQUE(m->Rows[r].State == Running ? COL_GREEN : (m->Rows[r].User ? COL_CYAN : COL_VIOLET)));
        GdiDrawText(S, &GdiFontUi, 46, ry, m->Rows[r].Name, OPAQUE(c));
        RtlStringPrintf(buf, sizeof(buf), "%u", m->Rows[r].Pid);
        GdiDrawText(S, &GdiFontUi, W - 250, ry, buf, OPAQUE(COL_DIM));
        GdiDrawText(S, &GdiFontUi, W - 200, ry, states[MIN(m->Rows[r].State, 4)], OPAQUE(COL_DIM));
        int bw = 60, fill = (int)MIN(m->Rows[r].Permille, 1000) * bw / 1000;
        GdiRoundRect(S, W - 130, ry + 6, bw, 6, 3, ARGB(34, 0xFFFFFF), ARGB(34, 0xFFFFFF));
        if (fill > 1) GdiRoundRect(S, W - 130, ry + 6, MAX(fill, 6), 6, 3, OPAQUE(COL_PINK), OPAQUE(COL_ORANGE));
        RtlStringPrintf(buf, sizeof(buf), "%u.%u%%", m->Rows[r].Permille / 10, m->Rows[r].Permille % 10);
        GdiDrawText(S, &GdiFontSmall, W - 62, ry + 2, buf, OPAQUE(COL_TEXT));
    }
    return 0;
}

void SysmonCreate(void)
{
    SYSMON *m = ExAllocatePoolWithTag(sizeof(SYSMON), TAG('S', 'm', 'o', 'n'));
    if (!m) return;
    m->LastTick = KeTickCount; m->LastIdle = KeIdleTicks;
    m->LastCsw = KeContextSwitches; m->LastIrq = KeInterruptCount;
    m->LastSys = KeSystemCalls; m->LastDpc = KeDpcCount;
    WINDOW *w = WmCreateWindow("System Monitor", 600, 490, SysmonProc, m, APP_SYSMON, COL_GREEN);
    if (!w) { ExFreePool(m); return; }
    w->Opacity = 232;
    SysmonSample(m);
    WmSetTimer(w, 1000);
}

/* ======================================================================================
 *  Object Explorer -- a WinObj-style view of the object manager namespace
 * ==================================================================================== */
#define OX_ROW_H 24
#define OX_TOP   52
typedef struct _OBJEXP {
    CHAR  Path[128];
    OB_DIRECTORY_ENTRY Entries[64];
    ULONG Count;
    int   Hover;
} OBJEXP;

static void ObjExpLoad(OBJEXP *o)
{
    o->Count = ObQueryDirectory(o->Path, o->Entries, ARRAY_SIZE(o->Entries));
    o->Hover = -1;
}

static BOOLEAN ObjExpIsRoot(const OBJEXP *o) { return o->Path[0] == '\\' && !o->Path[1]; }

static int ObjExpRowAt(const OBJEXP *o, int y)
{
    if (y < OX_TOP) return -1;
    int row = (y - OX_TOP) / OX_ROW_H;
    int rows = (int)o->Count + (ObjExpIsRoot(o) ? 0 : 1);
    return row < rows ? row : -1;
}

static void ObjExpUp(OBJEXP *o)
{
    char *slash = strrchr(o->Path, '\\');
    if (slash == o->Path) o->Path[1] = 0; else if (slash) *slash = 0;
    ObjExpLoad(o);
}

static LONG_PTR ObjExpProc(WINDOW *w, ULONG msg, ULONG_PTR wp, LONG_PTR lp)
{
    OBJEXP *o = w->UserData;
    GDI_SURFACE *S = w->Client;
    switch (msg) {
    case WM_PAINT: {
        int W = S->Width, H = S->Height;
        char buf[96];
        GdiGradientV(S, 0, 0, W, H, OPAQUE(0x151130), OPAQUE(0x0A0815));
        GdiRoundRect(S, 14, 12, W - 28, 30, 10, ARGB(90, 0x000000), ARGB(90, 0x000000));
        GdiRoundFrame(S, 14, 12, W - 28, 30, 10, 1, ARGB(120, COL_VIOLET), ARGB(120, COL_PINK));
        GdiDrawText(S, &GdiFontMono, 26, 18, o->Path, OPAQUE(COL_YELLOW));
        RtlStringPrintf(buf, sizeof(buf), "%u object%s", o->Count, o->Count == 1 ? "" : "s");
        GdiDrawText(S, &GdiFontSmall, W - 30 - GdiTextWidth(&GdiFontSmall, buf), 20, buf, OPAQUE(COL_DIM));

        int row = 0;
        BOOLEAN up = !ObjExpIsRoot(o);
        for (int i = up ? -1 : 0; i < (int)o->Count; i++, row++) {
            int y = OX_TOP + row * OX_ROW_H;
            if (y + OX_ROW_H > H - 4) break;
            if (row == o->Hover) GdiRoundRect(S, 12, y, W - 24, OX_ROW_H - 2, 8, ARGB(70, COL_VIOLET), ARGB(40, COL_PINK));
            if (i < 0) {
                GdiDrawText(S, &GdiFontUiBold, 44, y + 3, "..", OPAQUE(COL_DIM));
                GdiDrawText(S, &GdiFontSmall, 280, y + 5, "Parent directory", OPAQUE(COL_DIM));
                continue;
            }
            OB_DIRECTORY_ENTRY *e = &o->Entries[i];
            ULONG c = e->IsDirectory ? COL_ORANGE : (e->Target[0] ? COL_GREEN : COL_CYAN);
            if (e->IsDirectory) {
                GdiRoundRect(S, 22, y + 5, 14, 12, 3, OPAQUE(COL_ORANGE), OPAQUE(COL_PINK));
                GdiRoundRect(S, 22, y + 3, 7, 4, 2, OPAQUE(COL_ORANGE), OPAQUE(COL_ORANGE));
            } else {
                GdiDiamond(S, 29, y + 11, 6, e->Target[0] ? 2 : 0, OPAQUE(c), OPAQUE(c));
            }
            GdiDrawText(S, &GdiFontUi, 44, y + 3, e->Name[0] ? e->Name : "(unnamed)", OPAQUE(COL_TEXT));
            GdiDrawText(S, &GdiFontSmall, 280, y + 5, e->TypeName, OPAQUE(c));
            if (e->Target[0]) RtlStringPrintf(buf, sizeof(buf), "-> %s", e->Target);
            else RtlStringPrintf(buf, sizeof(buf), "%d refs, %d handles", e->PointerCount, e->HandleCount);
            GdiDrawText(S, &GdiFontSmall, 390, y + 5, buf, OPAQUE(COL_DIM));
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int r = ObjExpRowAt(o, LPARAM_Y(lp));
        if (r != o->Hover) { o->Hover = r; WmInvalidate(w); }
        break;
    }
    case WM_MOUSELEAVE:
        if (o->Hover != -1) { o->Hover = -1; WmInvalidate(w); }
        break;
    case WM_LBUTTONUP: {
        int r = ObjExpRowAt(o, LPARAM_Y(lp));
        if (r < 0) break;
        if (!ObjExpIsRoot(o)) { if (r == 0) { ObjExpUp(o); WmInvalidate(w); break; } r--; }
        OB_DIRECTORY_ENTRY *e = &o->Entries[r];
        if (e->IsDirectory) {
            if (!ObjExpIsRoot(o)) RtlStringCat(o->Path, sizeof(o->Path), "\\");
            RtlStringCat(o->Path, sizeof(o->Path), e->Name);
            ObjExpLoad(o);
        } else if (e->Target[0]) {
            OB_DIRECTORY_ENTRY probe;
            char target[128];
            RtlStringCopy(target, sizeof(target), e->Target);
            if (ObQueryDirectory(target, &probe, 1)) { RtlStringCopy(o->Path, sizeof(o->Path), target); ObjExpLoad(o); }
        }
        WmInvalidate(w);
        break;
    }
    case WM_CHAR:
        if ((char)wp == '\b' && !ObjExpIsRoot(o)) { ObjExpUp(o); WmInvalidate(w); }
        break;
    case WM_TIMER:
        { int h = o->Hover; ObjExpLoad(o); o->Hover = h; WmInvalidate(w); }
        break;
    case WM_DESTROY:
        ExFreePool(o);
        break;
    default: break;
    }
    return 0;
}

void ObjectExplorerCreate(void)
{
    OBJEXP *o = ExAllocatePoolWithTag(sizeof(OBJEXP), TAG('O', 'x', 'p', 'l'));
    if (!o) return;
    RtlStringCopy(o->Path, sizeof(o->Path), "\\");
    ObjExpLoad(o);
    WINDOW *w = WmCreateWindow("Object Explorer", 640, 420, ObjExpProc, o, APP_OBJECTS, COL_VIOLET);
    if (!w) { ExFreePool(o); return; }
    w->Opacity = 232;
    WmSetTimer(w, 2000);
}

/* ======================================================================================
 *  Nebula -- real-time plasma with a spinning wireframe, all in fixed point
 * ==================================================================================== */
#define NB_W 220
#define NB_H 150
typedef struct _NEBULA {
    short  Sin[1024];
    ULONG  Palette[256];
    UCHAR *Dist;
    ULONG  Frame, Fps, FpsFrames;
    ULONG64 FpsTick;
} NEBULA;

static LONG_PTR NebulaProc(WINDOW *w, ULONG msg, ULONG_PTR wp, LONG_PTR lp)
{
    NEBULA *n = w->UserData;
    (void)wp; (void)lp;
    if (msg == WM_TIMER) { n->Frame++; WmInvalidate(w); return 0; }
    if (msg == WM_DESTROY) { ExFreePool(n->Dist); ExFreePool(n); return 0; }
    if (msg != WM_PAINT) return 0;

    GDI_SURFACE *S = w->Client;
    int t = (int)n->Frame;
    for (int y = 0; y < NB_H; y++) {
        ULONG *r0 = S->Pixels + (SIZE_T)(y * 2) * S->Width, *r1 = r0 + S->Width;
        int sy = n->Sin[(y * 9 - t * 5) & 1023];
        for (int x = 0; x < NB_W; x++) {
            int v = n->Sin[(x * 7 + t * 6) & 1023] + sy + n->Sin[((x + y) * 5 + t * 3) & 1023]
                  + n->Sin[(n->Dist[y * NB_W + x] * 6 - t * 9) & 1023];
            ULONG c = n->Palette[((v + 16384) * 255 / 32768 + t) & 255];
            r0[x * 2] = r0[x * 2 + 1] = r1[x * 2] = r1[x * 2 + 1] = c;
        }
    }

    /* rotating octahedron inside a cube */
    static const signed char V[14][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 },
        { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 },
    };
    static const UCHAR E[24][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        { 8, 10 }, { 8, 11 }, { 8, 12 }, { 8, 13 }, { 9, 10 }, { 9, 11 }, { 9, 12 }, { 9, 13 },
        { 10, 12 }, { 12, 11 }, { 11, 13 }, { 13, 10 },
    };
    int px[14], py[14];
    int a = t * 7, b = t * 4, cx = S->Width / 2, cy = S->Height / 2;
    for (int i = 0; i < 14; i++) {
        int scale = i < 8 ? 70 : 108;
        int x = V[i][0] * scale, y = V[i][1] * scale, z = V[i][2] * scale;
        int x1 = (x * RtlCos(a) + z * RtlSin(a)) >> 12, z1 = (z * RtlCos(a) - x * RtlSin(a)) >> 12;
        int y1 = (y * RtlCos(b) - z1 * RtlSin(b)) >> 12, z2 = (z1 * RtlCos(b) + y * RtlSin(b)) >> 12;
        int d = 420 + z2;
        px[i] = cx + x1 * 420 / d; py[i] = cy + y1 * 420 / d;
    }
    for (int i = 0; i < 24; i++) {
        ULONG col = i < 12 ? ARGB(170, 0xFFFFFF) : ARGB(240, COL_YELLOW);
        GdiLine(S, px[E[i][0]], py[E[i][0]], px[E[i][1]], py[E[i][1]], col);
        GdiLine(S, px[E[i][0]] + 1, py[E[i][0]], px[E[i][1]] + 1, py[E[i][1]], ARGB(90, 0xFFFFFF));
    }

    n->FpsFrames++;
    if (KeTickCount - n->FpsTick >= 1000) { n->Fps = n->FpsFrames; n->FpsFrames = 0; n->FpsTick = KeTickCount; }
    char buf[64];
    RtlStringPrintf(buf, sizeof(buf), "software plasma %dx%d  %u fps", NB_W, NB_H, n->Fps);
    int tw = GdiTextWidth(&GdiFontSmall, buf) + 18;
    GdiRoundRect(S, 10, 10, tw, 22, 11, ARGB(150, 0x000000), ARGB(150, 0x000000));
    GdiDrawText(S, &GdiFontSmall, 19, 13, buf, OPAQUE(0xFFFFFF));
    return 0;
}

void NebulaCreate(void)
{
    NEBULA *n = ExAllocatePoolWithTag(sizeof(NEBULA), TAG('N', 'e', 'b', 'u'));
    if (!n) return;
    n->Dist = ExAllocatePoolWithTag(NB_W * NB_H, TAG('N', 'e', 'b', 'u'));
    if (!n->Dist) { ExFreePool(n); return; }
    for (int i = 0; i < 1024; i++) n->Sin[i] = (short)RtlSin(i);
    static const ULONG stops[] = { 0x0A0720, 0x4A1FB8, 0xFF2E88, 0xFF9A3D, 0xFFE28F, 0x3DE8FF, 0x2B1B8F, 0x0A0720 };
    for (int i = 0; i < 256; i++) {
        int seg = i * 7 / 256, f = i * 7 - seg * 256;
        n->Palette[i] = GdiLerpColor(stops[seg], stops[seg + 1], f, 256) & 0xFFFFFF;
    }
    for (int y = 0; y < NB_H; y++)
        for (int x = 0; x < NB_W; x++) {
            int dx = x - NB_W / 2, dy = y - NB_H / 2;
            n->Dist[y * NB_W + x] = (UCHAR)MIN(RtlIntegerSqrt((ULONG64)(dx * dx + dy * dy)), 255u);
        }
    n->FpsTick = KeTickCount;
    WINDOW *w = WmCreateWindow("Nebula", NB_W * 2, NB_H * 2, NebulaProc, n, APP_NEBULA, COL_CYAN);
    if (!w) { ExFreePool(n->Dist); ExFreePool(n); return; }
    WmSetTimer(w, 33);
}
