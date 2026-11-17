/* RadiaNT -- gdi.h : software rasteriser used by win32k (surfaces, AA shapes, text). */
#pragma once
#include "ntdef.h"

typedef struct _GDI_RECT { int x, y, w, h; } GDI_RECT;

typedef struct _GDI_SURFACE {
    ULONG   *Pixels;        /* 0x00RRGGBB */
    int      Width, Height;
    GDI_RECT Clip;
} GDI_SURFACE;

typedef struct _GDI_GLYPH {
    ULONG Offset; UCHAR W, H; signed char X, Y; UCHAR Advance;
} GDI_GLYPH;
typedef struct _GDI_FONT {
    int Height, Ascent;
    const UCHAR *Bits;
    GDI_GLYPH Glyphs[95];
} GDI_FONT;
extern const GDI_FONT GdiFontUi, GdiFontUiBold, GdiFontSmall, GdiFontMono, GdiFontTitle, GdiFontHuge;

#define RGB(r, g, b)      (((ULONG)(r) << 16) | ((ULONG)(g) << 8) | (ULONG)(b))
#define ARGB(a, rgb)      (((ULONG)(a) << 24) | ((rgb) & 0xFFFFFF))
#define OPAQUE(rgb)       ARGB(255, rgb)

/* RadiaNT palette */
#define COL_BG        0x07060F
#define COL_PINK      0xFF3D8B
#define COL_ORANGE    0xFF9A3D
#define COL_YELLOW    0xFFD93D
#define COL_VIOLET    0x8B5CFF
#define COL_CYAN      0x3DE8FF
#define COL_GREEN     0x4DFFA6
#define COL_RED       0xFF4D5E
#define COL_TEXT      0xECE9F5
#define COL_DIM       0x9A94B8

#define GDI_CORNER_TL 1
#define GDI_CORNER_TR 2
#define GDI_CORNER_BL 4
#define GDI_CORNER_BR 8
#define GDI_CORNER_ALL 15

GDI_SURFACE *GdiCreateSurface(int Width, int Height);
void  GdiDestroySurface(GDI_SURFACE *S);
void  GdiSetClip(GDI_SURFACE *S, int x, int y, int w, int h);
void  GdiResetClip(GDI_SURFACE *S);
BOOLEAN GdiIntersect(GDI_RECT *r, const GDI_RECT *clip);
ULONG GdiLerpColor(ULONG a, ULONG b, int t, int max);
ULONG GdiBlendColor(ULONG dst, ULONG src, ULONG alpha256);

void GdiFill(GDI_SURFACE *S, int x, int y, int w, int h, ULONG rgb);
void GdiBlend(GDI_SURFACE *S, int x, int y, int w, int h, ULONG argb);
void GdiGradientV(GDI_SURFACE *S, int x, int y, int w, int h, ULONG top, ULONG bottom);
void GdiGradientH(GDI_SURFACE *S, int x, int y, int w, int h, ULONG left, ULONG right);
void GdiRoundRect(GDI_SURFACE *S, int x, int y, int w, int h, int r, ULONG argbTop, ULONG argbBottom);
void GdiRoundFrame(GDI_SURFACE *S, int x, int y, int w, int h, int r, int t, ULONG argbLeft, ULONG argbRight);
void GdiShadow(GDI_SURFACE *S, int x, int y, int w, int h, int r, int spread, ULONG argb);
void GdiDiamond(GDI_SURFACE *S, int cx, int cy, int r, int thickness, ULONG argbTop, ULONG argbBottom);
void GdiCircle(GDI_SURFACE *S, int cx, int cy, int r, ULONG argb);
void GdiRing(GDI_SURFACE *S, int cx, int cy, int r, int t, ULONG argb);
void GdiLine(GDI_SURFACE *S, int x0, int y0, int x1, int y1, ULONG argb);
void GdiPixel(GDI_SURFACE *S, int x, int y, ULONG argb);
void GdiBlit(GDI_SURFACE *D, int dx, int dy, const GDI_SURFACE *Src, int sx, int sy, int w, int h);
void GdiBlitRounded(GDI_SURFACE *D, int dx, int dy, const GDI_SURFACE *Src, int sx, int sy,
                    int w, int h, int r, ULONG corners, ULONG alpha255);
int  GdiDrawText(GDI_SURFACE *S, const GDI_FONT *F, int x, int y, const char *Text, ULONG argb);
int  GdiDrawTextN(GDI_SURFACE *S, const GDI_FONT *F, int x, int y, const char *Text, int n, ULONG argb);
int  GdiTextWidth(const GDI_FONT *F, const char *Text);
void GdiBoxBlur(GDI_SURFACE *S, int radius);

/* Screen */
extern GDI_SURFACE *GdiScreen;          /* back buffer */
void GdiInitScreen(void);
void GdiPresent(int x, int y, int w, int h);
void GdiPresentAll(void);
