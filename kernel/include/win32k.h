/* RadiaNT -- win32k.h : in-kernel window manager, compositor and shell. */
#pragma once
#include "ntdef.h"
#include "gdi.h"

#define WM_CREATE       1
#define WM_DESTROY      2
#define WM_PAINT        3
#define WM_KEYDOWN      4
#define WM_CHAR         5
#define WM_MOUSEMOVE    6
#define WM_LBUTTONDOWN  7
#define WM_LBUTTONUP    8
#define WM_TIMER        9
#define WM_CLOSE        10
#define WM_MOUSELEAVE   11

#define VK_UP     0x100
#define VK_DOWN   0x101
#define VK_LEFT   0x102
#define VK_RIGHT  0x103
#define VK_PRIOR  0x104
#define VK_NEXT   0x105
#define VK_HOME   0x106
#define VK_END    0x107
#define VK_DELETE 0x108
#define VK_F1     0x110
#define KEYMOD_CTRL  0x1000
#define KEYMOD_SHIFT 0x2000
#define KEYMOD_ALT   0x4000

#define MAKELPARAM(x, y) ((LONG_PTR)(((ULONG)(x) & 0xFFFF) | (((ULONG)(y) & 0xFFFF) << 16)))
#define LPARAM_X(l) ((int)(short)((l) & 0xFFFF))
#define LPARAM_Y(l) ((int)(short)(((l) >> 16) & 0xFFFF))

#define WM_TITLE_H 34
#define WM_RADIUS  12

struct _WINDOW;
typedef LONG_PTR (*WNDPROC)(struct _WINDOW *Wnd, ULONG Msg, ULONG_PTR W, LONG_PTR L);

typedef struct _WINDOW {
    LIST_ENTRY   ZLink;             /* tail == topmost */
    int          X, Y, W, H;        /* outer frame */
    GDI_SURFACE *Client;
    CHAR         Title[48];
    WNDPROC      Proc;
    PVOID        UserData;
    UCHAR        Opacity;           /* client area over the frosted backdrop */
    BOOLEAN      Minimized, NeedsPaint, Dead;
    ULONG        TimerMs;
    ULONG64      NextTimer;
    int          AppId;             /* dock slot, -1 if none */
    ULONG        Accent;
} WINDOW;

void    Win32kInitialize(void);         /* creates the desktop thread */
void    Win32kBootSplash(int Percent, PCSTR Stage);
void    Win32kDrawBugCheck(ULONG Code, PCSTR Name, ULONG64 P[4], PCSTR Detail);

WINDOW *WmCreateWindow(PCSTR Title, int ClientW, int ClientH, WNDPROC Proc, PVOID UserData,
                       int AppId, ULONG Accent);
void    WmCloseWindow(WINDOW *Wnd);
void    WmInvalidate(WINDOW *Wnd);
void    WmMoveWindow(WINDOW *Wnd, int x, int y);
void    WmSetTimer(WINDOW *Wnd, ULONG Milliseconds);
void    WmSetTitle(WINDOW *Wnd, PCSTR Title);
WINDOW *WmFindWindowByApp(int AppId);
void    WmActivate(WINDOW *Wnd);
extern  ULONG WmFrameCount, WmFps, WmCpuPercent;
extern  GDI_SURFACE *WmWallpaper, *WmWallpaperBlur;
void    WmRenderWallpaper(GDI_SURFACE *Wall, GDI_SURFACE *Blur);

/* Shell applications */
enum { APP_TERMINAL, APP_SYSMON, APP_OBJECTS, APP_NEBULA, APP_ABOUT, APP_POWER, APP_COUNT };
void ShellLaunch(int AppId);
void TerminalCreate(void);
void SysmonCreate(void);
void ObjectExplorerCreate(void);
void NebulaCreate(void);
void AboutCreate(void);
void PowerCreate(void);
void ShellDrawIcon(GDI_SURFACE *S, int App, int x, int y, int size);
extern const char *const ShellAppNames[APP_COUNT];

/* Console service used by NtDisplayString */
void ConWrite(PVOID Console, const char *Text, SIZE_T Length);
void ConSetColor(PVOID Console, ULONG ColorIndex);

/* Safe against the console window closing underneath a running process. */
void ConWriteForProcess(PVOID Process, const char *Text, SIZE_T Length);
void ConSetColorForProcess(PVOID Process, ULONG ColorIndex);
