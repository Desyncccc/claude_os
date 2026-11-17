/*
 * RadiaNT -- win32k/gdi.c
 * Software rasteriser: clipped fills, anti-aliased rounded shapes, soft
 * shadows, alpha-blended glyphs and blits. Fixed point only -- the kernel is
 * built without floating point.
 */
#include "gdi.h"
#include "mm.h"
#include "hal.h"
#include "ke.h"
#include "rtl.h"

GDI_SURFACE *GdiScreen;

static inline ULONG Blend256(ULONG d, ULONG s, ULONG a)     /* a: 0..256 */
{
    ULONG rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8) & 0xFF00FF;
    ULONG g  = (((s & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8) & 0x00FF00;
    return rb | g;
}
#define A256(argb) ((((argb) >> 24) * 257 + 255) >> 8)      /* 0..255 -> 0..256 */

ULONG GdiBlendColor(ULONG dst, ULONG src, ULONG alpha256) { return Blend256(dst, src, alpha256); }

ULONG GdiLerpColor(ULONG a, ULONG b, int t, int max)
{
    if (max <= 0) return a;
    if (t < 0) t = 0;
    if (t > max) t = max;
    ULONG r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        r |= (ULONG)(ca + (cb - ca) * t / max) << sh;
    }
    return r;
}

/* ---- Surfaces ---------------------------------------------------------------------- */
GDI_SURFACE *GdiCreateSurface(int Width, int Height)
{
    GDI_SURFACE *s = ExAllocatePoolWithTag(sizeof(*s), TAG('G', 's', 'u', 'r'));
    if (!s) return NULL;
    s->Pixels = MmAllocateContiguousMemory((SIZE_T)Width * Height * 4);
    if (!s->Pixels) { ExFreePool(s); return NULL; }
    s->Width = Width; s->Height = Height;
    GdiResetClip(s);
    return s;
}

void GdiDestroySurface(GDI_SURFACE *S)
{
    if (!S) return;
    MmFreeContiguousMemory(S->Pixels, (SIZE_T)S->Width * S->Height * 4);
    ExFreePool(S);
}

void GdiResetClip(GDI_SURFACE *S) { S->Clip = (GDI_RECT){ 0, 0, S->Width, S->Height }; }

BOOLEAN GdiIntersect(GDI_RECT *r, const GDI_RECT *c)
{
    int x0 = MAX(r->x, c->x), y0 = MAX(r->y, c->y);
    int x1 = MIN(r->x + r->w, c->x + c->w), y1 = MIN(r->y + r->h, c->y + c->h);
    if (x1 <= x0 || y1 <= y0) { r->w = r->h = 0; return FALSE; }
    r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
    return TRUE;
}

void GdiSetClip(GDI_SURFACE *S, int x, int y, int w, int h)
{
    GDI_RECT r = { x, y, w, h }, full = { 0, 0, S->Width, S->Height };
    GdiIntersect(&r, &full);
    S->Clip = r;
}

#define ROW(S, y) ((S)->Pixels + (SIZE_T)(y) * (S)->Width)

static inline void FillSpan(ULONG *p, int n, ULONG c)
{
    __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
}

static inline void BlendSpan(ULONG *p, int n, ULONG rgb, ULONG a)
{
    if (a >= 256) { FillSpan(p, n, rgb & 0xFFFFFF); return; }
    if (!a) return;
    ULONG srb = (rgb & 0xFF00FF) * a, sg = (rgb & 0x00FF00) * a, ia = 256 - a;
    for (int i = 0; i < n; i++) {
        ULONG d = p[i];
        p[i] = (((srb + (d & 0xFF00FF) * ia) >> 8) & 0xFF00FF) |
               (((sg  + (d & 0x00FF00) * ia) >> 8) & 0x00FF00);
    }
}

/* ---- Rectangles ----------------------------------------------------------------------- */
void GdiFill(GDI_SURFACE *S, int x, int y, int w, int h, ULONG rgb)
{
    GDI_RECT r = { x, y, w, h };
    if (!GdiIntersect(&r, &S->Clip)) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) FillSpan(ROW(S, yy) + r.x, r.w, rgb & 0xFFFFFF);
}

void GdiBlend(GDI_SURFACE *S, int x, int y, int w, int h, ULONG argb)
{
    GDI_RECT r = { x, y, w, h };
    if (!GdiIntersect(&r, &S->Clip)) return;
    ULONG a = A256(argb);
    for (int yy = r.y; yy < r.y + r.h; yy++) BlendSpan(ROW(S, yy) + r.x, r.w, argb, a);
}

void GdiGradientV(GDI_SURFACE *S, int x, int y, int w, int h, ULONG top, ULONG bottom)
{
    GDI_RECT r = { x, y, w, h };
    if (!GdiIntersect(&r, &S->Clip)) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        ULONG c = GdiLerpColor(top, bottom, yy - y, h - 1);
        BlendSpan(ROW(S, yy) + r.x, r.w, c, A256(c));
    }
}

void GdiGradientH(GDI_SURFACE *S, int x, int y, int w, int h, ULONG left, ULONG right)
{
    GDI_RECT r = { x, y, w, h };
    if (!GdiIntersect(&r, &S->Clip)) return;
    for (int xx = r.x; xx < r.x + r.w; xx++) {
        ULONG c = GdiLerpColor(left, right, xx - x, w - 1), a = A256(c);
        for (int yy = r.y; yy < r.y + r.h; yy++) {
            ULONG *p = ROW(S, yy) + xx;
            *p = Blend256(*p, c, a);
        }
    }
}

void GdiPixel(GDI_SURFACE *S, int x, int y, ULONG argb)
{
    if (x < S->Clip.x || y < S->Clip.y || x >= S->Clip.x + S->Clip.w || y >= S->Clip.y + S->Clip.h) return;
    ULONG *p = ROW(S, y) + x;
    *p = Blend256(*p, argb, A256(argb));
}

void GdiLine(GDI_SURFACE *S, int x0, int y0, int x1, int y1, ULONG argb)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        GdiPixel(S, x0, y0, argb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- Rounded corners -------------------------------------------------------------------- */
#define MAX_RADIUS 40
static struct { int r; UCHAR m[MAX_RADIUS * MAX_RADIUS]; } CornerCache[6];
static ULONG CornerNext;

/* Coverage (0..255) of the top-left corner square of a circle of radius r. */
static const UCHAR *CornerMask(int r)
{
    for (ULONG i = 0; i < ARRAY_SIZE(CornerCache); i++)
        if (CornerCache[i].r == r) return CornerCache[i].m;
    ULONG64 f = HalSaveFlagsAndCli();
    ULONG slot = CornerNext++ % ARRAY_SIZE(CornerCache);
    CornerCache[slot].r = 0;
    UCHAR *m = CornerCache[slot].m;
    for (int cy = 0; cy < r; cy++) {
        for (int cx = 0; cx < r; cx++) {
            int DX = 2 * r - 2 * cx - 1, DY = 2 * r - 2 * cy - 1;
            int d16 = (int)RtlIntegerSqrt((ULONG64)(DX * DX + DY * DY) * 64);
            int cov = (16 * r + 8 - d16) * 16;
            m[cy * r + cx] = (UCHAR)(cov < 0 ? 0 : cov > 255 ? 255 : cov);
        }
    }
    CornerCache[slot].r = r;
    HalRestoreFlags(f);
    return m;
}

/* Coverage of pixel (cx, cy) inside a w*h rounded rectangle, 0..255. */
static inline ULONG RoundCoverage(const UCHAR *mask, int r, int cx, int cy, int w, int h, ULONG corners)
{
    if (cx < 0 || cy < 0 || cx >= w || cy >= h) return 0;
    if (cy < r) {
        if (cx < r)       return (corners & GDI_CORNER_TL) ? mask[cy * r + cx] : 255;
        if (cx >= w - r)  return (corners & GDI_CORNER_TR) ? mask[cy * r + (w - 1 - cx)] : 255;
    } else if (cy >= h - r) {
        if (cx < r)       return (corners & GDI_CORNER_BL) ? mask[(h - 1 - cy) * r + cx] : 255;
        if (cx >= w - r)  return (corners & GDI_CORNER_BR) ? mask[(h - 1 - cy) * r + (w - 1 - cx)] : 255;
    }
    return 255;
}

static int ClampRadius(int r, int w, int h)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > MAX_RADIUS) r = MAX_RADIUS;
    return r < 0 ? 0 : r;
}

void GdiRoundRect(GDI_SURFACE *S, int x, int y, int w, int h, int r, ULONG top, ULONG bottom)
{
    GDI_RECT c = { x, y, w, h };
    if (w <= 0 || h <= 0 || !GdiIntersect(&c, &S->Clip)) return;
    r = ClampRadius(r, w, h);
    const UCHAR *mask = r ? CornerMask(r) : NULL;
    for (int yy = c.y; yy < c.y + c.h; yy++) {
        ULONG col = top == bottom ? top : GdiLerpColor(top, bottom, yy - y, h - 1);
        ULONG a = A256(col);
        int cy = yy - y;
        ULONG *row = ROW(S, yy);
        if (!r || (cy >= r && cy < h - r)) { BlendSpan(row + c.x, c.w, col, a); continue; }
        int x0 = c.x, x1 = c.x + c.w;
        int l1 = MIN(x1, x + r), r0 = MAX(x0, x + w - r);
        for (int xx = x0; xx < l1; xx++)
            row[xx] = Blend256(row[xx], col, a * RoundCoverage(mask, r, xx - x, cy, w, h, GDI_CORNER_ALL) / 255);
        if (r0 > l1) BlendSpan(row + MAX(x0, l1), r0 - MAX(x0, l1), col, a);
        for (int xx = MAX(r0, l1); xx < x1; xx++)
            row[xx] = Blend256(row[xx], col, a * RoundCoverage(mask, r, xx - x, cy, w, h, GDI_CORNER_ALL) / 255);
    }
}

void GdiRoundFrame(GDI_SURFACE *S, int x, int y, int w, int h, int r, int t, ULONG left, ULONG right)
{
    GDI_RECT c = { x, y, w, h };
    if (w <= 2 * t || h <= 2 * t || !GdiIntersect(&c, &S->Clip)) return;
    r = ClampRadius(r, w, h);
    int ri = MAX(r - t, 0);
    const UCHAR *mo = r ? CornerMask(r) : NULL, *mi = ri ? CornerMask(ri) : NULL;
    int band = MAX(r, t);
    for (int yy = c.y; yy < c.y + c.h; yy++) {
        int cy = yy - y;
        ULONG *row = ROW(S, yy);
        BOOLEAN edgeRow = cy < band || cy >= h - band;
        for (int xx = c.x; xx < c.x + c.w; xx++) {
            int cx = xx - x;
            if (!edgeRow && cx >= band && cx < w - band) { xx = MIN(c.x + c.w, x + w - band) - 1; continue; }
            ULONG outer = r ? RoundCoverage(mo, r, cx, cy, w, h, GDI_CORNER_ALL) : 255;
            ULONG inner = ri ? RoundCoverage(mi, ri, cx - t, cy - t, w - 2 * t, h - 2 * t, GDI_CORNER_ALL)
                             : ((cx >= t && cy >= t && cx < w - t && cy < h - t) ? 255 : 0);
            if (outer <= inner) continue;
            ULONG col = left == right ? left : GdiLerpColor(left, right, cx, w - 1);
            row[xx] = Blend256(row[xx], col, A256(col) * (outer - inner) / 255);
        }
    }
}

/* Soft drop shadow / glow outside a rounded rectangle. */
void GdiShadow(GDI_SURFACE *S, int x, int y, int w, int h, int r, int spread, ULONG argb)
{
    GDI_RECT c = { x - spread, y - spread, w + 2 * spread, h + 2 * spread };
    if (spread <= 0 || !GdiIntersect(&c, &S->Clip)) return;
    r = ClampRadius(r, w, h);
    ULONG a0 = A256(argb);
    int ix0 = x + r, ix1 = x + w - 1 - r, iy0 = y + r, iy1 = y + h - 1 - r;
    for (int yy = c.y; yy < c.y + c.h; yy++) {
        ULONG *row = ROW(S, yy);
        int qy = yy < iy0 ? iy0 - yy : (yy > iy1 ? yy - iy1 : 0);
        BOOLEAN middle = qy == 0;
        for (int xx = c.x; xx < c.x + c.w; xx++) {
            if (middle && xx >= x && xx < x + w) { xx = x + w - 1; continue; }
            int qx = xx < ix0 ? ix0 - xx : (xx > ix1 ? xx - ix1 : 0);
            int d = (qx && qy) ? (int)RtlIntegerSqrt((ULONG64)(qx * qx + qy * qy)) - r : MAX(qx, qy) - r;
            if (d <= 0 || d >= spread) continue;
            int k = spread - d;
            row[xx] = Blend256(row[xx], argb, a0 * (ULONG)(k * k) / (ULONG)(spread * spread));
        }
    }
}

void GdiCircle(GDI_SURFACE *S, int cx, int cy, int r, ULONG argb)
{
    GdiRing(S, cx, cy, r, r + 1, argb);
}

/* Anti-aliased annulus: outer radius r, thickness t (t > r fills the disc). */
void GdiRing(GDI_SURFACE *S, int cx, int cy, int r, int t, ULONG argb)
{
    GDI_RECT c = { cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3 };
    if (r <= 0 || !GdiIntersect(&c, &S->Clip)) return;
    ULONG a0 = A256(argb);
    int ri = r - t;
    for (int yy = c.y; yy < c.y + c.h; yy++) {
        ULONG *row = ROW(S, yy);
        int dy = yy - cy;
        for (int xx = c.x; xx < c.x + c.w; xx++) {
            int dx = xx - cx;
            int d16 = (int)RtlIntegerSqrt((ULONG64)(dx * dx + dy * dy) * 256);
            int co = (16 * r + 8 - d16) * 16;
            if (co <= 0) continue;
            if (co > 255) co = 255;
            if (ri > 0) {
                int ci = (16 * ri + 8 - d16) * 16;
                if (ci > 255) ci = 255;
                if (ci > 0) co -= ci;
                if (co <= 0) continue;
            }
            row[xx] = Blend256(row[xx], argb, a0 * (ULONG)co / 255);
        }
    }
}

/* Anti-aliased diamond (rotated square). thickness == 0 fills it. */
void GdiDiamond(GDI_SURFACE *S, int cx, int cy, int r, int thickness, ULONG top, ULONG bottom)
{
    GDI_RECT c = { cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3 };
    if (r <= 0 || !GdiIntersect(&c, &S->Clip)) return;
    int inner = thickness ? r - ((thickness * 362) >> 8) : -1;
    for (int yy = c.y; yy < c.y + c.h; yy++) {
        ULONG *row = ROW(S, yy);
        ULONG col = top == bottom ? top : GdiLerpColor(top, bottom, yy - (cy - r), 2 * r);
        ULONG a0 = A256(col);
        int ady = yy > cy ? yy - cy : cy - yy;
        for (int xx = c.x; xx < c.x + c.w; xx++) {
            int m = (xx > cx ? xx - cx : cx - xx) + ady;
            int e = (r - m) * 181 + 128;
            if (e <= 0) continue;
            if (e > 256) e = 256;
            if (inner >= 0) {
                int e2 = (inner - m) * 181 + 128;
                if (e2 > 256) e2 = 256;
                if (e2 > 0) e -= e2;
                if (e <= 0) continue;
            }
            row[xx] = Blend256(row[xx], col, a0 * (ULONG)e >> 8);
        }
    }
}

/* ---- Blits ------------------------------------------------------------------------------- */
void GdiBlit(GDI_SURFACE *D, int dx, int dy, const GDI_SURFACE *Src, int sx, int sy, int w, int h)
{
    GdiBlitRounded(D, dx, dy, Src, sx, sy, w, h, 0, 0, 255);
}

void GdiBlitRounded(GDI_SURFACE *D, int dx, int dy, const GDI_SURFACE *Src, int sx, int sy,
                    int w, int h, int r, ULONG corners, ULONG alpha255)
{
    GDI_RECT c = { dx, dy, w, h };
    if (w <= 0 || h <= 0 || !GdiIntersect(&c, &D->Clip)) return;
    /* keep the source read in bounds */
    int ox = sx - dx, oy = sy - dy;
    GDI_RECT sb = { -ox, -oy, Src->Width, Src->Height };
    if (!GdiIntersect(&c, &sb)) return;
    r = ClampRadius(r, w, h);
    if (!corners) r = 0;
    const UCHAR *mask = r ? CornerMask(r) : NULL;
    ULONG a = (alpha255 * 257 + 255) >> 8;
    for (int yy = c.y; yy < c.y + c.h; yy++) {
        ULONG *drow = ROW(D, yy);
        const ULONG *srow = Src->Pixels + (SIZE_T)(yy + oy) * Src->Width + ox;
        int cy = yy - dy;
        BOOLEAN band = r && (cy < r || cy >= h - r);
        if (!band) {
            if (a >= 256) memcpy(drow + c.x, srow + c.x, (SIZE_T)c.w * 4);
            else for (int xx = c.x; xx < c.x + c.w; xx++) drow[xx] = Blend256(drow[xx], srow[xx], a);
            continue;
        }
        for (int xx = c.x; xx < c.x + c.w; xx++) {
            ULONG cov = RoundCoverage(mask, r, xx - dx, cy, w, h, corners);
            if (!cov) continue;
            ULONG k = a * cov / 255;
            drow[xx] = k >= 256 ? srow[xx] : Blend256(drow[xx], srow[xx], k);
        }
    }
}

/* ---- Text ---------------------------------------------------------------------------------- */
int GdiTextWidth(const GDI_FONT *F, const char *Text)
{
    int w = 0;
    for (; *Text; Text++) {
        UCHAR ch = (UCHAR)*Text;
        if (ch < 32 || ch > 126) ch = '?';
        w += F->Glyphs[ch - 32].Advance;
    }
    return w;
}

int GdiDrawTextN(GDI_SURFACE *S, const GDI_FONT *F, int x, int y, const char *Text, int n, ULONG argb)
{
    ULONG a0 = A256(argb);
    int cx0 = S->Clip.x, cy0 = S->Clip.y, cx1 = cx0 + S->Clip.w, cy1 = cy0 + S->Clip.h;
    for (int i = 0; i < n && Text[i]; i++) {
        UCHAR ch = (UCHAR)Text[i];
        if (ch < 32 || ch > 126) ch = '?';
        const GDI_GLYPH *g = &F->Glyphs[ch - 32];
        int gx = x + g->X, gy = y + g->Y;
        if (g->W && gx < cx1 && gy < cy1 && gx + g->W > cx0 && gy + g->H > cy0) {
            const UCHAR *bits = F->Bits + g->Offset;
            int y0 = MAX(gy, cy0), y1 = MIN(gy + g->H, cy1);
            int x0 = MAX(gx, cx0), x1 = MIN(gx + g->W, cx1);
            for (int yy = y0; yy < y1; yy++) {
                const UCHAR *brow = bits + (yy - gy) * g->W - gx;
                ULONG *row = ROW(S, yy);
                for (int xx = x0; xx < x1; xx++) {
                    ULONG cov = brow[xx];
                    if (cov) row[xx] = Blend256(row[xx], argb, a0 * cov / 255);
                }
            }
        }
        x += g->Advance;
    }
    return x;
}

int GdiDrawText(GDI_SURFACE *S, const GDI_FONT *F, int x, int y, const char *Text, ULONG argb)
{
    return GdiDrawTextN(S, F, x, y, Text, 1 << 30, argb);
}

/* ---- Box blur (separable, in place) ------------------------------------------------------- */
void GdiBoxBlur(GDI_SURFACE *S, int radius)
{
    int w = S->Width, h = S->Height, n = MAX(w, h);
    ULONG *tmp = ExAllocatePoolWithTag((SIZE_T)n * 4, TAG('G', 'b', 'l', 'r'));
    if (!tmp || radius <= 0) { if (tmp) ExFreePool(tmp); return; }
    int win = 2 * radius + 1;
    for (int pass = 0; pass < 2; pass++) {
        int lines = pass ? w : h, len = pass ? h : w, step = pass ? w : 1;
        for (int l = 0; l < lines; l++) {
            ULONG *p = pass ? S->Pixels + l : ROW(S, l);
            ULONG sr = 0, sg = 0, sb = 0;
            for (int i = -radius; i <= radius; i++) {
                ULONG c = p[(SIZE_T)MIN(MAX(i, 0), len - 1) * step];
                sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF; sb += c & 0xFF;
            }
            for (int i = 0; i < len; i++) {
                tmp[i] = RGB(sr / win, sg / win, sb / win);
                ULONG out = p[(SIZE_T)MAX(i - radius, 0) * step];
                ULONG in  = p[(SIZE_T)MIN(i + radius + 1, len - 1) * step];
                sr += ((in >> 16) & 0xFF) - ((out >> 16) & 0xFF);
                sg += ((in >> 8) & 0xFF) - ((out >> 8) & 0xFF);
                sb += (in & 0xFF) - (out & 0xFF);
            }
            for (int i = 0; i < len; i++) p[(SIZE_T)i * step] = tmp[i];
        }
    }
    ExFreePool(tmp);
}

/* ---- Screen -------------------------------------------------------------------------------- */
static GDI_SURFACE GdiScreenSurface;

void GdiInitScreen(void)
{
    GDI_SURFACE *s = &GdiScreenSurface;
    s->Width = HalFramebuffer.Width;
    s->Height = HalFramebuffer.Height;
    s->Pixels = MmAllocateContiguousMemory((SIZE_T)s->Width * s->Height * 4);
    if (!s->Pixels) return;
    GdiResetClip(s);
    GdiScreen = s;
}

void GdiPresent(int x, int y, int w, int h)
{
    GDI_SURFACE *s = GdiScreen;
    if (!s) return;
    GDI_RECT r = { x, y, w, h }, full = { 0, 0, s->Width, s->Height };
    if (!GdiIntersect(&r, &full)) return;
    const HAL_FRAMEBUFFER *fb = &HalFramebuffer;
    BOOLEAN native = fb->Bpp == 32 && fb->RedShift == 16 && fb->GreenShift == 8 && fb->BlueShift == 0;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        const ULONG *src = ROW(s, yy) + r.x;
        UCHAR *dst = fb->Base + (SIZE_T)yy * fb->Pitch + (SIZE_T)r.x * (fb->Bpp / 8);
        if (native) { memcpy(dst, src, (SIZE_T)r.w * 4); continue; }
        for (int i = 0; i < r.w; i++) {
            ULONG c = src[i];
            ULONG v = (((c >> 16) & 0xFF) << fb->RedShift) | (((c >> 8) & 0xFF) << fb->GreenShift) |
                      ((c & 0xFF) << fb->BlueShift);
            if (fb->Bpp == 32) { *(ULONG *)dst = v; dst += 4; }
            else { dst[0] = (UCHAR)v; dst[1] = (UCHAR)(v >> 8); dst[2] = (UCHAR)(v >> 16); dst += 3; }
        }
    }
}

void GdiPresentAll(void)
{
    if (GdiScreen) GdiPresent(0, 0, GdiScreen->Width, GdiScreen->Height);
}
