/*
 * RadiaNT -- win32k/wm.c
 * The window manager: one kernel thread owns the desktop. It reads raw input
 * from the i8042prt class devices (IRP_MJ_READ), routes it to window
 * procedures, and composites damaged regions of the screen:
 *
 *      wallpaper -> windows (shadow, frosted glass, client, frame) -> top bar
 *      -> dock -> cursor
 */
#include "win32k.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"
#include "io.h"
#include "hal.h"
#include "rtl.h"

#define TOPBAR_H    30
#define DOCK_ICON   48
#define DOCK_GAP    14
#define DOCK_PAD    12
#define SHADOW      30
#define MAX_DIRTY   24

GDI_SURFACE *WmWallpaper, *WmWallpaperBlur;
ULONG WmFrameCount, WmFps, WmCpuPercent;

static LIST_ENTRY WmWindows = { &WmWindows, &WmWindows };
static int SW, SH;
static int CurX, CurY;
static USHORT MouseButtons;
static WINDOW *DragWnd, *CaptureWnd, *HoverWnd;
static int DragDX, DragDY;
static int DockHover = -1, ButtonHover;       /* ButtonHover: 1 = close, 2 = minimise */
static WINDOW *ButtonHoverWnd;
static GDI_RECT Dirty[MAX_DIRTY];
static int DirtyCount;
static PDEVICE_OBJECT KeyboardDevice, PointerDevice;
static BOOLEAN KeyShift, KeyCtrl, KeyAlt, KeyCaps;
static int WindowCascade;

/* ---- Damage tracking ------------------------------------------------------------------- */
static void AddDirty(int x, int y, int w, int h)
{
    GDI_RECT r = { x, y, w, h }, full = { 0, 0, SW, SH };
    if (!GdiIntersect(&r, &full)) return;
    for (int i = 0; i < DirtyCount; i++) {
        GDI_RECT *d = &Dirty[i];
        if (r.x <= d->x + d->w && d->x <= r.x + r.w && r.y <= d->y + d->h && d->y <= r.y + r.h) {
            int x0 = MIN(r.x, d->x), y0 = MIN(r.y, d->y);
            int x1 = MAX(r.x + r.w, d->x + d->w), y1 = MAX(r.y + r.h, d->y + d->h);
            Dirty[i] = Dirty[--DirtyCount];
            AddDirty(x0, y0, x1 - x0, y1 - y0);
            return;
        }
    }
    if (DirtyCount == MAX_DIRTY) { DirtyCount = 0; r = full; }
    Dirty[DirtyCount++] = r;
}

static void DirtyWindow(const WINDOW *w)
{
    AddDirty(w->X - SHADOW, w->Y - SHADOW, w->W + 2 * SHADOW, w->H + 2 * SHADOW + 8);
}

static WINDOW *TopWindow(void)
{
    for (PLIST_ENTRY e = WmWindows.Blink; e != &WmWindows; e = e->Blink) {
        WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
        if (!w->Minimized && !w->Dead) return w;
    }
    return NULL;
}

/* ---- Window API --------------------------------------------------------------------------- */
WINDOW *WmCreateWindow(PCSTR Title, int ClientW, int ClientH, WNDPROC Proc, PVOID UserData,
                       int AppId, ULONG Accent)
{
    WINDOW *w = ExAllocatePoolWithTag(sizeof(WINDOW), TAG('W', 'n', 'd', ' '));
    if (!w) return NULL;
    w->Client = GdiCreateSurface(ClientW, ClientH);
    if (!w->Client) { ExFreePool(w); return NULL; }
    w->W = ClientW; w->H = ClientH + WM_TITLE_H;
    w->X = MAX(20, (SW - w->W) / 2 - 110 + (WindowCascade % 6) * 44);
    w->Y = TOPBAR_H + 18 + (WindowCascade % 6) * 26;
    if (w->X + w->W > SW - 10) w->X = MAX(10, SW - w->W - 10);
    if (w->Y + w->H > SH - 96) w->Y = MAX(TOPBAR_H + 6, SH - w->H - 96);     /* stay clear of the dock */
    WindowCascade++;
    RtlStringCopy(w->Title, sizeof(w->Title), Title);
    w->Proc = Proc; w->UserData = UserData;
    w->Opacity = 255; w->AppId = AppId; w->Accent = Accent;
    w->NeedsPaint = TRUE;
    WINDOW *prev = TopWindow();
    InsertTailList(&WmWindows, &w->ZLink);
    if (prev) DirtyWindow(prev);
    Proc(w, WM_CREATE, 0, 0);
    DirtyWindow(w);
    AddDirty(0, 0, SW, TOPBAR_H);
    AddDirty(0, SH - 110, SW, 110);
    return w;
}

void WmInvalidate(WINDOW *Wnd) { if (Wnd) Wnd->NeedsPaint = TRUE; }

void WmMoveWindow(WINDOW *Wnd, int x, int y)
{
    DirtyWindow(Wnd);
    Wnd->X = MIN(MAX(x, 60 - Wnd->W), SW - 60);
    Wnd->Y = MIN(MAX(y, TOPBAR_H), SH - WM_TITLE_H);
    DirtyWindow(Wnd);
}

void WmSetTimer(WINDOW *Wnd, ULONG Milliseconds)
{
    Wnd->TimerMs = Milliseconds;
    Wnd->NextTimer = KeTickCount + Milliseconds;
}

void WmSetTitle(WINDOW *Wnd, PCSTR Title)
{
    RtlStringCopy(Wnd->Title, sizeof(Wnd->Title), Title);
    AddDirty(Wnd->X, Wnd->Y, Wnd->W, WM_TITLE_H);
    AddDirty(0, 0, SW, TOPBAR_H);
}

void WmActivate(WINDOW *Wnd)
{
    WINDOW *prev = TopWindow();
    Wnd->Minimized = FALSE;
    if (prev == Wnd) return;
    RemoveEntryList(&Wnd->ZLink);
    InsertTailList(&WmWindows, &Wnd->ZLink);
    if (prev) DirtyWindow(prev);
    DirtyWindow(Wnd);
    AddDirty(0, 0, SW, TOPBAR_H);
    AddDirty(0, SH - 110, SW, 110);
}

void WmCloseWindow(WINDOW *Wnd)
{
    if (Wnd->Dead) return;
    Wnd->Proc(Wnd, WM_CLOSE, 0, 0);
    Wnd->Dead = TRUE;
}

WINDOW *WmFindWindowByApp(int AppId)
{
    for (PLIST_ENTRY e = WmWindows.Flink; e != &WmWindows; e = e->Flink) {
        WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
        if (w->AppId == AppId && !w->Dead) return w;
    }
    return NULL;
}

static void ReapWindows(void)
{
    for (PLIST_ENTRY e = WmWindows.Flink; e != &WmWindows; ) {
        WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
        e = e->Flink;
        if (!w->Dead) continue;
        w->Proc(w, WM_DESTROY, 0, 0);
        RemoveEntryList(&w->ZLink);
        DirtyWindow(w);
        AddDirty(0, 0, SW, TOPBAR_H);
        AddDirty(0, SH - 110, SW, 110);
        if (DragWnd == w) DragWnd = NULL;
        if (CaptureWnd == w) CaptureWnd = NULL;
        if (HoverWnd == w) HoverWnd = NULL;
        if (ButtonHoverWnd == w) ButtonHoverWnd = NULL;
        GdiDestroySurface(w->Client);
        ExFreePool(w);
        WINDOW *top = TopWindow();
        if (top) DirtyWindow(top);
    }
}

/* ---- Painting: windows --------------------------------------------------------------------- */
static void DrawWindow(GDI_SURFACE *S, WINDOW *w, BOOLEAN focused)
{
    int x = w->X, y = w->Y, W = w->W, H = w->H;
    ULONG shade = ARGB(focused ? 120 : 70, 0x000000);
    GdiShadow(S, x, y + 8, W, H, WM_RADIUS, focused ? SHADOW : 18, shade);
    {   /* the shadow is offset downwards: fill the sliver the window does not cover */
        GDI_RECT saved = S->Clip, strip = { x, y + H - WM_RADIUS, W, WM_RADIUS + 8 };
        if (GdiIntersect(&strip, &saved)) {
            S->Clip = strip;
            GdiRoundRect(S, x, y + 8, W, H, WM_RADIUS, shade, shade);
            S->Clip = saved;
        }
    }
    if (focused) GdiShadow(S, x, y, W, H, WM_RADIUS, 12, ARGB(34, w->Accent));

    /* frosted glass body */
    GdiBlitRounded(S, x, y, WmWallpaperBlur, x, y, W, H, WM_RADIUS, GDI_CORNER_ALL, 255);
    GdiRoundRect(S, x, y, W, H, WM_RADIUS, ARGB(focused ? 150 : 175, 0x1A1636), ARGB(focused ? 185 : 200, 0x0A0916));
    GdiBlitRounded(S, x, y + WM_TITLE_H, w->Client, 0, 0, W, H - WM_TITLE_H, WM_RADIUS,
                   GDI_CORNER_BL | GDI_CORNER_BR, w->Opacity);
    GdiBlend(S, x + 1, y + WM_TITLE_H - 1, W - 2, 1, ARGB(34, 0xFFFFFF));
    if (focused) GdiRoundFrame(S, x, y, W, H, WM_RADIUS, 1, ARGB(215, w->Accent), ARGB(215, COL_VIOLET));
    else         GdiRoundFrame(S, x, y, W, H, WM_RADIUS, 1, ARGB(50, 0xFFFFFF), ARGB(50, 0xFFFFFF));

    /* caption */
    GdiDiamond(S, x + 18, y + WM_TITLE_H / 2, 6, 0, ARGB(focused ? 255 : 120, w->Accent), ARGB(focused ? 255 : 120, COL_ORANGE));
    GdiDrawText(S, &GdiFontUiBold, x + 32, y + 8, w->Title, ARGB(focused ? 255 : 140, COL_TEXT));

    /* caption buttons: minimise, close */
    int by = y + WM_TITLE_H / 2, cx = x + W - 22, mx = x + W - 48;
    BOOLEAN hc = ButtonHoverWnd == w && ButtonHover == 1, hm = ButtonHoverWnd == w && ButtonHover == 2;
    GdiCircle(S, cx, by, 9, hc ? OPAQUE(COL_RED) : ARGB(26, 0xFFFFFF));
    GdiCircle(S, mx, by, 9, hm ? ARGB(90, 0xFFFFFF) : ARGB(26, 0xFFFFFF));
    ULONG gc = ARGB(focused || hc ? 235 : 130, 0xFFFFFF);
    GdiLine(S, cx - 3, by - 3, cx + 3, by + 3, gc);
    GdiLine(S, cx + 3, by - 3, cx - 3, by + 3, gc);
    GdiLine(S, mx - 4, by, mx + 4, by, ARGB(focused || hm ? 235 : 130, 0xFFFFFF));
}

/* ---- Painting: top bar ------------------------------------------------------------------------ */
static void DrawTopBar(GDI_SURFACE *S)
{
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    GdiBlit(S, 0, 0, WmWallpaperBlur, 0, 0, SW, TOPBAR_H);
    GdiBlend(S, 0, 0, SW, TOPBAR_H, ARGB(150, 0x0C0A1C));
    GdiBlend(S, 0, TOPBAR_H - 1, SW, 1, ARGB(30, 0xFFFFFF));

    GdiDiamond(S, 20, TOPBAR_H / 2, 8, 2, OPAQUE(COL_PINK), OPAQUE(COL_ORANGE));
    GdiDiamond(S, 20, TOPBAR_H / 2, 3, 0, OPAQUE(COL_YELLOW), OPAQUE(COL_ORANGE));
    int x = GdiDrawText(S, &GdiFontUiBold, 36, 6, RADIANT_NAME, OPAQUE(COL_TEXT));
    WINDOW *top = TopWindow();
    GdiDrawText(S, &GdiFontUi, x + 18, 6, top ? top->Title : "Desktop", OPAQUE(COL_DIM));

    char buf[64];
    TIME_FIELDS t; KeQuerySystemTimeFields(&t);
    RtlStringPrintf(buf, sizeof(buf), "%s %u %s   %02u:%02u:%02u", days[t.Weekday % 7], t.Day,
                    months[(t.Month + 11) % 12], t.Hour, t.Minute, t.Second);
    int cx = SW - 16 - GdiTextWidth(&GdiFontUi, "Wed 88 Wed   88:88:88");
    GdiDrawText(S, &GdiFontUi, cx, 6, buf, OPAQUE(COL_TEXT));

    /* live meters */
    ULONG64 usedMb = ((MmTotalPages - MmFreePagesCount) * PAGE_SIZE) >> 20;
    ULONG64 totalMb = (MmTotalPages * PAGE_SIZE) >> 20;
    int mx = cx - 250;
    GdiDrawText(S, &GdiFontSmall, mx, 8, "CPU", OPAQUE(COL_DIM));
    GdiRoundRect(S, mx + 28, 12, 44, 6, 3, ARGB(40, 0xFFFFFF), ARGB(40, 0xFFFFFF));
    GdiRoundRect(S, mx + 28, 12, MAX(6, 44 * (int)MIN(WmCpuPercent, 100) / 100), 6, 3, OPAQUE(COL_PINK), OPAQUE(COL_ORANGE));
    RtlStringPrintf(buf, sizeof(buf), "%u%%", WmCpuPercent);
    GdiDrawText(S, &GdiFontSmall, mx + 78, 8, buf, OPAQUE(COL_TEXT));
    mx += 118;
    GdiDrawText(S, &GdiFontSmall, mx, 8, "MEM", OPAQUE(COL_DIM));
    GdiRoundRect(S, mx + 30, 12, 44, 6, 3, ARGB(40, 0xFFFFFF), ARGB(40, 0xFFFFFF));
    GdiRoundRect(S, mx + 30, 12, MAX(6, (int)(44 * usedMb / MAX(totalMb, 1))), 6, 3, OPAQUE(COL_VIOLET), OPAQUE(COL_CYAN));
    RtlStringPrintf(buf, sizeof(buf), "%llu MB", usedMb);
    GdiDrawText(S, &GdiFontSmall, mx + 80, 8, buf, OPAQUE(COL_TEXT));
}

/* ---- Painting: dock ---------------------------------------------------------------------------- */
static void DockRect(GDI_RECT *r)
{
    r->w = APP_COUNT * DOCK_ICON + (APP_COUNT - 1) * DOCK_GAP + 2 * DOCK_PAD;
    r->h = DOCK_ICON + 2 * DOCK_PAD - 4;
    r->x = (SW - r->w) / 2;
    r->y = SH - r->h - 14;
}

static void DrawDock(GDI_SURFACE *S)
{
    GDI_RECT d; DockRect(&d);
    GdiShadow(S, d.x, d.y + 6, d.w, d.h, 20, 22, ARGB(100, 0x000000));
    GdiBlitRounded(S, d.x, d.y, WmWallpaperBlur, d.x, d.y, d.w, d.h, 20, GDI_CORNER_ALL, 255);
    GdiRoundRect(S, d.x, d.y, d.w, d.h, 20, ARGB(120, 0x2A2452), ARGB(150, 0x0E0C20));
    GdiRoundFrame(S, d.x, d.y, d.w, d.h, 20, 1, ARGB(70, 0xFFFFFF), ARGB(40, 0xFFFFFF));
    for (int i = 0; i < APP_COUNT; i++) {
        int ix = d.x + DOCK_PAD + i * (DOCK_ICON + DOCK_GAP), iy = d.y + DOCK_PAD - 4;
        BOOLEAN hover = DockHover == i;
        if (hover) {
            iy -= 8;
            GdiShadow(S, ix, iy, DOCK_ICON, DOCK_ICON, 13, 14, ARGB(90, COL_PINK));
        }
        ShellDrawIcon(S, i, ix, iy, DOCK_ICON);
        WINDOW *w = WmFindWindowByApp(i);
        if (w) GdiRoundRect(S, ix + DOCK_ICON / 2 - 6, d.y + d.h - 7, 12, 3, 1,
                            OPAQUE(w->Minimized ? COL_DIM : COL_PINK), OPAQUE(w->Minimized ? COL_DIM : COL_ORANGE));
        if (hover) {
            const char *name = ShellAppNames[i];
            int tw = GdiTextWidth(&GdiFontSmall, name) + 20, tx = ix + DOCK_ICON / 2 - tw / 2, ty = iy - 34;
            GdiRoundRect(S, tx, ty, tw, 24, 12, ARGB(225, 0x1D1840), ARGB(225, 0x110E26));
            GdiRoundFrame(S, tx, ty, tw, 24, 12, 1, ARGB(150, COL_PINK), ARGB(150, COL_VIOLET));
            GdiDrawText(S, &GdiFontSmall, tx + 10, ty + 5, name, OPAQUE(COL_TEXT));
        }
    }
}

static int DockHitTest(int x, int y)
{
    GDI_RECT d; DockRect(&d);
    if (y < d.y - 6 || y >= d.y + d.h || x < d.x + DOCK_PAD - 6) return -1;
    int i = (x - (d.x + DOCK_PAD - DOCK_GAP / 2)) / (DOCK_ICON + DOCK_GAP);
    return (i >= 0 && i < APP_COUNT && x < d.x + d.w - DOCK_PAD + 6) ? i : -1;
}

/* ---- Painting: cursor --------------------------------------------------------------------------- */
static const char *const CursorShape[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..X X..X   ", "X.X  X..X   ", "XX    X..X  ", "X     X..X  ",
    "       XX   ",
};
#define CURSOR_W 12
#define CURSOR_H 19

static void DrawCursor(GDI_SURFACE *S)
{
    for (int j = 0; j < CURSOR_H; j++) {
        for (int i = 0; i < CURSOR_W; i++) {
            char c = CursorShape[j][i];
            if (c == ' ') continue;
            GdiPixel(S, CurX + i + 2, CurY + j + 3, ARGB(70, 0x000000));
        }
    }
    for (int j = 0; j < CURSOR_H; j++) {
        for (int i = 0; i < CURSOR_W; i++) {
            char c = CursorShape[j][i];
            if (c == ' ') continue;
            GdiPixel(S, CurX + i, CurY + j, c == 'X' ? OPAQUE(0x1A0F2E) : OPAQUE(GdiLerpColor(0xFFFFFF, 0xFFD0E4, j, CURSOR_H)));
        }
    }
}

/* ---- Compositor ----------------------------------------------------------------------------------- */
static void Compose(const GDI_RECT *r)
{
    GDI_SURFACE *S = GdiScreen;
    GdiSetClip(S, r->x, r->y, r->w, r->h);
    GdiBlit(S, r->x, r->y, WmWallpaper, r->x, r->y, r->w, r->h);
    WINDOW *top = TopWindow();
    for (PLIST_ENTRY e = WmWindows.Flink; e != &WmWindows; e = e->Flink) {
        WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
        if (w->Minimized || w->Dead) continue;
        if (w->X - SHADOW >= r->x + r->w || w->X + w->W + SHADOW <= r->x ||
            w->Y - SHADOW >= r->y + r->h || w->Y + w->H + SHADOW + 8 <= r->y) continue;
        DrawWindow(S, w, w == top);
    }
    if (r->y < TOPBAR_H) DrawTopBar(S);
    if (r->y + r->h > SH - 130) DrawDock(S);
    DrawCursor(S);
    GdiResetClip(S);
    GdiPresent(r->x, r->y, r->w, r->h);
}

static void ComposeDirty(void)
{
    if (!DirtyCount) return;
    GDI_RECT list[MAX_DIRTY];
    int n = DirtyCount;
    memcpy(list, Dirty, sizeof(GDI_RECT) * n);
    DirtyCount = 0;
    for (int i = 0; i < n; i++) Compose(&list[i]);
    WmFrameCount++;
}

/* ---- Input: mouse ----------------------------------------------------------------------------------- */
static WINDOW *WindowAt(int x, int y)
{
    for (PLIST_ENTRY e = WmWindows.Blink; e != &WmWindows; e = e->Blink) {
        WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
        if (w->Minimized || w->Dead) continue;
        if (x >= w->X && x < w->X + w->W && y >= w->Y && y < w->Y + w->H) return w;
    }
    return NULL;
}

static int CaptionButtonAt(const WINDOW *w, int x, int y)
{
    int by = w->Y + WM_TITLE_H / 2;
    if (y < by - 11 || y > by + 11) return 0;
    if (x >= w->X + w->W - 33 && x <= w->X + w->W - 11) return 1;
    if (x >= w->X + w->W - 59 && x <= w->X + w->W - 37) return 2;
    return 0;
}

static void SetButtonHover(WINDOW *w, int which)
{
    if (ButtonHoverWnd == w && ButtonHover == which) return;
    if (ButtonHoverWnd) AddDirty(ButtonHoverWnd->X + ButtonHoverWnd->W - 64, ButtonHoverWnd->Y, 64, WM_TITLE_H);
    ButtonHoverWnd = which ? w : NULL;
    ButtonHover = which;
    if (ButtonHoverWnd) AddDirty(w->X + w->W - 64, w->Y, 64, WM_TITLE_H);
}

static void MouseMoved(int nx, int ny)
{
    nx = MIN(MAX(nx, 0), SW - 1); ny = MIN(MAX(ny, 0), SH - 1);
    if (nx == CurX && ny == CurY) return;
    AddDirty(CurX, CurY, CURSOR_W + 3, CURSOR_H + 4);
    CurX = nx; CurY = ny;
    AddDirty(CurX, CurY, CURSOR_W + 3, CURSOR_H + 4);

    if (DragWnd) {
        WmMoveWindow(DragWnd, CurX - DragDX, CurY - DragDY);
        return;
    }
    int dh = DockHitTest(CurX, CurY);
    WINDOW *w = CaptureWnd ? CaptureWnd : WindowAt(CurX, CurY);
    if (w && !CaptureWnd) dh = -1;
    if (dh != DockHover) { DockHover = dh; AddDirty(0, SH - 130, SW, 130); }
    SetButtonHover(w, w && !CaptureWnd ? CaptionButtonAt(w, CurX, CurY) : 0);
    if (HoverWnd && HoverWnd != w) HoverWnd->Proc(HoverWnd, WM_MOUSELEAVE, 0, 0);
    HoverWnd = w;
    if (w && (CaptureWnd || CurY >= w->Y + WM_TITLE_H))
        w->Proc(w, WM_MOUSEMOVE, MouseButtons, MAKELPARAM(CurX - w->X, CurY - w->Y - WM_TITLE_H));
}

static void MouseButton(BOOLEAN down)
{
    if (!down) {
        DragWnd = NULL;
        if (CaptureWnd) {
            WINDOW *w = CaptureWnd; CaptureWnd = NULL;
            w->Proc(w, WM_LBUTTONUP, 0, MAKELPARAM(CurX - w->X, CurY - w->Y - WM_TITLE_H));
        }
        return;
    }
    WINDOW *w = WindowAt(CurX, CurY);
    if (!w) {
        int app = DockHitTest(CurX, CurY);
        if (app >= 0) ShellLaunch(app);
        return;
    }
    WmActivate(w);
    int btn = CaptionButtonAt(w, CurX, CurY);
    if (btn == 1) { WmCloseWindow(w); return; }
    if (btn == 2) {
        w->Minimized = TRUE;
        DirtyWindow(w);
        AddDirty(0, 0, SW, TOPBAR_H); AddDirty(0, SH - 110, SW, 110);
        WINDOW *top = TopWindow();
        if (top) DirtyWindow(top);
        return;
    }
    if (CurY < w->Y + WM_TITLE_H) { DragWnd = w; DragDX = CurX - w->X; DragDY = CurY - w->Y; return; }
    CaptureWnd = w;
    w->Proc(w, WM_LBUTTONDOWN, 0, MAKELPARAM(CurX - w->X, CurY - w->Y - WM_TITLE_H));
}

/* ---- Input: keyboard (scan code set 1, US layout) -------------------------------------------------- */
static const char KeymapLower[0x3A] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char KeymapUpper[0x3A] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static void KeyEvent(const KEYBOARD_INPUT_DATA *k)
{
    BOOLEAN down = !(k->Flags & KEY_BREAK);
    USHORT sc = k->MakeCode;
    switch (sc) {
    case 0x2A: case 0x36: if (!(k->Flags & KEY_E0)) KeyShift = down; return;
    case 0x1D: KeyCtrl = down; return;
    case 0x38: KeyAlt = down; return;
    case 0x3A: if (down) KeyCaps = !KeyCaps; return;
    default: break;
    }
    if (!down) return;
    WINDOW *w = TopWindow();

    if (KeyAlt && sc == 0x0F) {                     /* Alt+Tab: cycle windows */
        for (PLIST_ENTRY e = WmWindows.Flink; e != &WmWindows; e = e->Flink) {
            WINDOW *c = CONTAINING_RECORD(e, WINDOW, ZLink);
            if (!c->Minimized && !c->Dead && c != w) { WmActivate(c); break; }
        }
        return;
    }
    if (KeyAlt && sc == 0x3E) { if (w) WmCloseWindow(w); return; }      /* Alt+F4 */
    if (!w) return;

    ULONG mods = (KeyCtrl ? KEYMOD_CTRL : 0) | (KeyShift ? KEYMOD_SHIFT : 0) | (KeyAlt ? KEYMOD_ALT : 0);
    ULONG vk = 0;
    switch (sc) {
    case 0x48: vk = VK_UP; break;     case 0x50: vk = VK_DOWN; break;
    case 0x4B: vk = VK_LEFT; break;   case 0x4D: vk = VK_RIGHT; break;
    case 0x49: vk = VK_PRIOR; break;  case 0x51: vk = VK_NEXT; break;
    case 0x47: vk = VK_HOME; break;   case 0x4F: vk = VK_END; break;
    case 0x53: vk = VK_DELETE; break;
    default: if (sc >= 0x3B && sc <= 0x44) vk = VK_F1 + (sc - 0x3B); break;
    }
    if (vk) { w->Proc(w, WM_KEYDOWN, vk | mods, 0); return; }
    if (sc >= ARRAY_SIZE(KeymapLower)) return;
    char c = KeymapLower[sc];
    if (!c) return;
    BOOLEAN letter = c >= 'a' && c <= 'z';
    if (KeyCtrl && letter) { w->Proc(w, WM_CHAR, (ULONG_PTR)(c - 'a' + 1), mods); return; }
    if (KeyShift ^ (letter && KeyCaps)) c = KeymapUpper[sc];
    w->Proc(w, WM_CHAR, (ULONG_PTR)(UCHAR)c, mods);
}

static void PumpInput(void)
{
    KEYBOARD_INPUT_DATA keys[16];
    MOUSE_INPUT_DATA mice[32];
    ULONG_PTR got;
    for (;;) {
        IoSynchronousRequest(PointerDevice, IRP_MJ_READ, 0, mice, sizeof(mice), &got);
        ULONG n = (ULONG)(got / sizeof(mice[0]));
        if (!n) break;
        for (ULONG i = 0; i < n; i++) {
            if (mice[i].LastX || mice[i].LastY) MouseMoved(CurX + mice[i].LastX, CurY + mice[i].LastY);
            USHORT changed = mice[i].Buttons ^ MouseButtons;
            MouseButtons = mice[i].Buttons;
            if (changed & MOUSE_LEFT_BUTTON) MouseButton((MouseButtons & MOUSE_LEFT_BUTTON) != 0);
        }
    }
    for (;;) {
        IoSynchronousRequest(KeyboardDevice, IRP_MJ_READ, 0, keys, sizeof(keys), &got);
        ULONG n = (ULONG)(got / sizeof(keys[0]));
        if (!n) break;
        for (ULONG i = 0; i < n; i++) KeyEvent(&keys[i]);
    }
}

/* ---- The desktop thread -------------------------------------------------------------------------------- */
static void DesktopThread(PVOID Context)
{
    (void)Context;
    SW = GdiScreen->Width; SH = GdiScreen->Height;
    WmWallpaper = GdiCreateSurface(SW, SH);
    WmWallpaperBlur = GdiCreateSurface(SW, SH);
    if (!WmWallpaper || !WmWallpaperBlur) KeBugCheckEx(NO_MORE_SYSTEM_MEMORY, SW, SH, 0, 0);
    ULONG64 t0 = KeTickCount;
    WmRenderWallpaper(WmWallpaper, WmWallpaperBlur);
    DbgPrint("WIN32K: wallpaper rendered in %llu ms (%dx%d)\n", KeTickCount - t0, SW, SH);

    ObReferenceObjectByName("\\Device\\KeyboardClass0", IoDeviceObjectType, (PVOID *)&KeyboardDevice);
    ObReferenceObjectByName("\\Device\\PointerClass0", IoDeviceObjectType, (PVOID *)&PointerDevice);
    CurX = SW / 2; CurY = SH / 2;
    AddDirty(0, 0, SW, SH);
    ShellLaunch(APP_ABOUT);
    ShellLaunch(APP_TERMINAL);
    WINDOW *about = WmFindWindowByApp(APP_ABOUT), *term = WmFindWindowByApp(APP_TERMINAL);
    if (about) WmMoveWindow(about, SW - about->W - 44, SH - about->H - 104);
    if (term) WmMoveWindow(term, 44, TOPBAR_H + 26);
    DbgPrint("WIN32K: desktop ready\n");

    ULONG64 lastSecond = KeTickCount, lastIdle = KeIdleTicks;
    ULONG lastFrames = 0;
    for (;;) {
        KeWaitForSingleObject(&IoInputEvent, 8);
        PumpInput();

        ULONG64 now = KeTickCount;
        for (PLIST_ENTRY e = WmWindows.Flink; e != &WmWindows; e = e->Flink) {
            WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
            if (w->TimerMs && !w->Dead && now >= w->NextTimer) {
                w->NextTimer = now + w->TimerMs;
                w->Proc(w, WM_TIMER, 0, 0);
            }
        }
        if (now - lastSecond >= 1000) {
            ULONG64 idle = KeIdleTicks;
            ULONG64 span = now - lastSecond;
            WmCpuPercent = (ULONG)(100 - MIN((idle - lastIdle) * 100 / span, 100));
            WmFps = WmFrameCount - lastFrames;
            lastFrames = WmFrameCount; lastIdle = idle; lastSecond = now;
            AddDirty(SW - 460, 0, 460, TOPBAR_H);
        }
        ReapWindows();
        for (PLIST_ENTRY e = WmWindows.Flink; e != &WmWindows; e = e->Flink) {
            WINDOW *w = CONTAINING_RECORD(e, WINDOW, ZLink);
            if (w->NeedsPaint && !w->Dead) {
                w->NeedsPaint = FALSE;
                w->Proc(w, WM_PAINT, 0, 0);
                if (!w->Minimized) AddDirty(w->X, w->Y + WM_TITLE_H, w->W, w->H - WM_TITLE_H);
            }
        }
        ComposeDirty();
    }
}

void Win32kInitialize(void)
{
    if (!GdiScreen) { DbgPrint("WIN32K: no display, desktop not started\n"); return; }
    PsCreateSystemThread(DesktopThread, NULL, 13, "win32k!Desktop", NULL);
}
