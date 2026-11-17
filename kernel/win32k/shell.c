/*
 * RadiaNT -- win32k/shell.c
 * Desktop shell glue: application launcher, procedurally drawn icons and the
 * small built-in windows (About, Power).
 */
#include "win32k.h"
#include "ke.h"
#include "mm.h"
#include "hal.h"
#include "ps.h"
#include "rtl.h"

const char *const ShellAppNames[APP_COUNT] = {
    "Terminal", "System Monitor", "Object Explorer", "Nebula", "About RadiaNT", "Power",
};

void ShellLaunch(int AppId)
{
    WINDOW *w = WmFindWindowByApp(AppId);
    if (w) { WmActivate(w); return; }
    switch (AppId) {
    case APP_TERMINAL: TerminalCreate(); break;
    case APP_SYSMON:   SysmonCreate(); break;
    case APP_OBJECTS:  ObjectExplorerCreate(); break;
    case APP_NEBULA:   NebulaCreate(); break;
    case APP_ABOUT:    AboutCreate(); break;
    case APP_POWER:    PowerCreate(); break;
    default: break;
    }
}

/* ---- Icons --------------------------------------------------------------------------- */
static void IconBase(GDI_SURFACE *S, int x, int y, int n, ULONG top, ULONG bottom)
{
    GdiRoundRect(S, x, y, n, n, 13, OPAQUE(top), OPAQUE(bottom));
    GdiRoundRect(S, x + 1, y + 1, n - 2, n / 2, 12, ARGB(46, 0xFFFFFF), ARGB(0, 0xFFFFFF));
    GdiRoundFrame(S, x, y, n, n, 13, 1, ARGB(110, 0xFFFFFF), ARGB(40, 0xFFFFFF));
}

static void ThickLine(GDI_SURFACE *S, int x0, int y0, int x1, int y1, ULONG c)
{
    GdiLine(S, x0, y0, x1, y1, c);
    GdiLine(S, x0, y0 + 1, x1, y1 + 1, c);
    GdiLine(S, x0 + 1, y0, x1 + 1, y1, c);
}

void ShellDrawIcon(GDI_SURFACE *S, int App, int x, int y, int n)
{
    int cx = x + n / 2, cy = y + n / 2;
    ULONG white = OPAQUE(0xFFFFFF);
    switch (App) {
    case APP_TERMINAL:
        IconBase(S, x, y, n, 0x3A3368, 0x13111F);
        ThickLine(S, x + 12, y + 15, x + 21, y + 23, OPAQUE(COL_PINK));
        ThickLine(S, x + 21, y + 23, x + 12, y + 31, OPAQUE(COL_PINK));
        GdiFill(S, x + 25, y + 31, 12, 3, 0xFFFFFF);
        break;
    case APP_SYSMON:
        IconBase(S, x, y, n, 0x1FD6A0, 0x1767D9);
        ThickLine(S, x + 8, y + 31, x + 16, y + 22, white);
        ThickLine(S, x + 16, y + 22, x + 23, y + 28, white);
        ThickLine(S, x + 23, y + 28, x + 31, y + 13, white);
        ThickLine(S, x + 31, y + 13, x + 40, y + 21, white);
        break;
    case APP_OBJECTS:
        IconBase(S, x, y, n, 0x9A6BFF, 0xC936B8);
        GdiRoundRect(S, x + 19, y + 9, 10, 10, 3, white, white);
        GdiRoundRect(S, x + 8, y + 29, 10, 10, 3, white, white);
        GdiRoundRect(S, x + 30, y + 29, 10, 10, 3, white, white);
        GdiFill(S, x + 23, y + 19, 2, 5, 0xFFFFFF);
        GdiFill(S, x + 12, y + 23, 24, 2, 0xFFFFFF);
        GdiFill(S, x + 12, y + 23, 2, 6, 0xFFFFFF);
        GdiFill(S, x + 34, y + 23, 2, 6, 0xFFFFFF);
        break;
    case APP_NEBULA:
        IconBase(S, x, y, n, 0x24178A, 0xE2357F);
        GdiRing(S, cx, cy, 15, 2, ARGB(150, 0xFFFFFF));
        GdiRing(S, cx, cy, 10, 2, ARGB(220, 0xFFFFFF));
        GdiCircle(S, cx, cy, 4, OPAQUE(COL_YELLOW));
        GdiCircle(S, cx + 13, cy - 8, 2, white);
        break;
    case APP_ABOUT:
        IconBase(S, x, y, n, 0xFFA63D, 0xFF3D7F);
        GdiDiamond(S, cx, cy, 14, 3, white, white);
        GdiDiamond(S, cx, cy, 5, 0, white, white);
        break;
    case APP_POWER:
        IconBase(S, x, y, n, 0x5A5378, 0x1C192C);
        GdiRing(S, cx, cy + 2, 12, 3, OPAQUE(COL_RED));
        GdiFill(S, cx - 4, y + 8, 8, 12, 0x4A4466);
        GdiRoundRect(S, cx - 1, y + 9, 3, 14, 1, OPAQUE(COL_RED), OPAQUE(COL_RED));
        break;
    default: break;
    }
}

/* ---- About ----------------------------------------------------------------------------- */
static LONG_PTR AboutProc(WINDOW *w, ULONG msg, ULONG_PTR wp, LONG_PTR lp)
{
    (void)wp; (void)lp;
    if (msg != WM_PAINT) return 0;
    GDI_SURFACE *S = w->Client;
    int W = S->Width, H = S->Height;
    char buf[160];
    GdiGradientV(S, 0, 0, W, H, OPAQUE(0x17122F), OPAQUE(0x0B0916));

    for (int i = 4; i >= 1; i--) GdiCircle(S, 84, 86, 30 + i * 14, ARGB(9, COL_PINK));
    GdiDiamond(S, 84, 86, 54, 6, OPAQUE(COL_PINK), OPAQUE(COL_ORANGE));
    GdiDiamond(S, 84, 86, 31, 4, OPAQUE(COL_VIOLET), OPAQUE(COL_PINK));
    GdiDiamond(S, 84, 86, 15, 0, OPAQUE(COL_YELLOW), OPAQUE(COL_ORANGE));

    GdiDrawText(S, &GdiFontTitle, 164, 30, RADIANT_NAME " OS", OPAQUE(0xFFFFFF));
    GdiDrawText(S, &GdiFontUi, 166, 72, "Version " RADIANT_VERSION_STR "  \"" RADIANT_CODENAME "\"  (x64)", OPAQUE(COL_ORANGE));
    GdiDrawText(S, &GdiFontUi, 166, 96, "An original kernel built on the NT architecture.", OPAQUE(COL_TEXT));
    GdiDrawText(S, &GdiFontUi, 166, 116, "Written from scratch. Contains no Windows code.", OPAQUE(COL_DIM));

    int y = 166;
    GdiBlend(S, 24, y - 12, W - 48, 1, ARGB(40, 0xFFFFFF));
    static const char *const labels[] = { "Processor", "Memory", "Display", "Firmware", "Kernel" };
    for (int i = 0; i < 5; i++) {
        switch (i) {
        case 0: RtlStringCopy(buf, sizeof(buf), HalCpuInfo.Brand); break;
        case 1: RtlStringPrintf(buf, sizeof(buf), "%llu MB installed, %llu MB free",
                                (MmTotalPages * PAGE_SIZE) >> 20, (MmFreePagesCount * PAGE_SIZE) >> 20); break;
        case 2: RtlStringPrintf(buf, sizeof(buf), "%u x %u, %u-bit linear framebuffer",
                                HalFramebuffer.Width, HalFramebuffer.Height, HalFramebuffer.Bpp); break;
        case 3: RtlStringPrintf(buf, sizeof(buf), "%s, loaded by %s",
                                KeLoaderBlock.FirmwareIsEfi ? "UEFI" : "BIOS", KeLoaderBlock.LoaderName); break;
        default: RtlStringPrintf(buf, sizeof(buf), "Preemptive, 32 priorities, %u processes / %u threads",
                                 PsProcessCount, PsThreadCount); break;
        }
        GdiDrawText(S, &GdiFontUiBold, 28, y, labels[i], OPAQUE(COL_DIM));
        GdiDrawText(S, &GdiFontUi, 118, y, buf, OPAQUE(COL_TEXT));
        y += 23;
    }

    y += 10;
    GdiDrawText(S, &GdiFontSmall, 28, y, "EXECUTIVE COMPONENTS", OPAQUE(COL_DIM));
    y += 22;
    static const struct { const char *n; ULONG c; } parts[] = {
        { "HAL", COL_CYAN }, { "Ke", COL_PINK }, { "Mm", COL_ORANGE }, { "Ob", COL_VIOLET },
        { "Ps", COL_GREEN }, { "Io", COL_YELLOW }, { "Cm", COL_CYAN }, { "Ex", COL_PINK },
        { "Rtl", COL_ORANGE }, { "win32k", COL_VIOLET },
    };
    int x = 28;
    for (ULONG i = 0; i < ARRAY_SIZE(parts); i++) {
        int tw = GdiTextWidth(&GdiFontUiBold, parts[i].n) + 22;
        GdiRoundRect(S, x, y, tw, 26, 13, ARGB(40, parts[i].c), ARGB(22, parts[i].c));
        GdiRoundFrame(S, x, y, tw, 26, 13, 1, ARGB(190, parts[i].c), ARGB(110, parts[i].c));
        GdiDrawText(S, &GdiFontUiBold, x + 11, y + 4, parts[i].n, OPAQUE(parts[i].c));
        x += tw + 8;
    }
    return 0;
}

void AboutCreate(void)
{
    WINDOW *w = WmCreateWindow("About RadiaNT", 560, 364, AboutProc, NULL, APP_ABOUT, COL_ORANGE);
    if (w) w->Opacity = 228;
}

/* ---- Power ------------------------------------------------------------------------------- */
static int PowerHover;

static int PowerHit(int x, int y)
{
    if (y < 78 || y >= 122) return 0;
    if (x >= 24 && x < 174) return 1;
    if (x >= 186 && x < 336) return 2;
    return 0;
}

static LONG_PTR PowerProc(WINDOW *w, ULONG msg, ULONG_PTR wp, LONG_PTR lp)
{
    (void)wp;
    GDI_SURFACE *S = w->Client;
    switch (msg) {
    case WM_PAINT:
        GdiGradientV(S, 0, 0, S->Width, S->Height, OPAQUE(0x17122F), OPAQUE(0x0B0916));
        GdiDrawText(S, &GdiFontUiBold, 24, 20, "Leave RadiaNT?", OPAQUE(0xFFFFFF));
        GdiDrawText(S, &GdiFontUi, 24, 44, "Running programs will be stopped.", OPAQUE(COL_DIM));
        for (int i = 1; i <= 2; i++) {
            int x = i == 1 ? 24 : 186;
            ULONG c = i == 1 ? COL_VIOLET : COL_PINK;
            BOOLEAN h = PowerHover == i;
            if (h) GdiShadow(S, x, 78, 150, 44, 12, 10, ARGB(80, c));
            GdiRoundRect(S, x, 78, 150, 44, 12, ARGB(h ? 255 : 60, c), ARGB(h ? 220 : 36, c));
            GdiRoundFrame(S, x, 78, 150, 44, 12, 1, ARGB(230, c), ARGB(150, c));
            const char *t = i == 1 ? "Restart" : "Shut down";
            GdiDrawText(S, &GdiFontUiBold, x + 75 - GdiTextWidth(&GdiFontUiBold, t) / 2, 91, t, OPAQUE(0xFFFFFF));
        }
        break;
    case WM_MOUSEMOVE: {
        int h = PowerHit(LPARAM_X(lp), LPARAM_Y(lp));
        if (h != PowerHover) { PowerHover = h; WmInvalidate(w); }
        break;
    }
    case WM_MOUSELEAVE:
        if (PowerHover) { PowerHover = 0; WmInvalidate(w); }
        break;
    case WM_LBUTTONUP: {
        int h = PowerHit(LPARAM_X(lp), LPARAM_Y(lp));
        if (h) {
            DbgPrint("WIN32K: user requested %s\n", h == 1 ? "restart" : "shutdown");
            HalReturnToFirmware(h == 1);
        }
        break;
    }
    default: break;
    }
    return 0;
}

void PowerCreate(void)
{
    PowerHover = 0;
    WINDOW *w = WmCreateWindow("Power", 360, 146, PowerProc, NULL, APP_POWER, COL_RED);
    if (w) w->Opacity = 235;
}
