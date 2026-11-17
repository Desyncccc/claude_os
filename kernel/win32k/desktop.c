/*
 * RadiaNT -- win32k/desktop.c
 * Things that paint the whole screen: the procedural wallpaper, the boot
 * splash and the bugcheck screen.
 */
#include "win32k.h"
#include "ke.h"
#include "mm.h"
#include "hal.h"
#include "rtl.h"

static inline ULONG AddColor(ULONG d, ULONG c, ULONG k)     /* saturating d += c * k/256 */
{
    ULONG r = ((d >> 16) & 0xFF) + ((((c >> 16) & 0xFF) * k) >> 8);
    ULONG g = ((d >> 8) & 0xFF) + ((((c >> 8) & 0xFF) * k) >> 8);
    ULONG b = (d & 0xFF) + (((c & 0xFF) * k) >> 8);
    return RGB(MIN(r, 255), MIN(g, 255), MIN(b, 255));
}

/* ---- Wallpaper: "First Light" -- silk ribbons of light over deep space ------------- */
typedef struct { int Base, A1, F1, P1, A2, F2, P2, Width, Strength; ULONG Color; } STRAND;

void WmRenderWallpaper(GDI_SURFACE *Wall, GDI_SURFACE *Blur)
{
    int W = Wall->Width, H = Wall->Height;

    /* 1. Sky: vertical gradient plus large soft colour fields. */
    for (int y = 0; y < H; y++) {
        ULONG c = y < H / 2 ? GdiLerpColor(0x0A0720, 0x160B33, y, H / 2)
                            : GdiLerpColor(0x160B33, 0x06040D, y - H / 2, H / 2);
        ULONG *row = Wall->Pixels + (SIZE_T)y * W;
        for (int x = 0; x < W; x++) row[x] = c;
    }
    static const struct { int cx, cy, r; ULONG col; int k; } blobs[] = {
        { 18, 30, 55, 0xFF2E88, 120 }, { 82, 22, 50, 0x6A3DFF, 130 },
        { 55, 78, 60, 0xFF7A2E, 70 },  { 96, 80, 40, 0x1EC8FF, 70 },
        { 2, 92, 38, 0x7A2BFF, 90 },
    };
    for (ULONG b = 0; b < ARRAY_SIZE(blobs); b++) {
        LONG64 cx = blobs[b].cx * W / 100, cy = blobs[b].cy * H / 100;
        LONG64 R = (LONG64)blobs[b].r * W / 100, R2 = R * R;
        for (int y = MAX(0, (int)(cy - R)); y < MIN(H, (int)(cy + R)); y++) {
            ULONG *row = Wall->Pixels + (SIZE_T)y * W;
            for (int x = MAX(0, (int)(cx - R)); x < MIN(W, (int)(cx + R)); x++) {
                LONG64 d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (d2 >= R2) continue;
                ULONG k = (ULONG)((R2 - d2) * 256 / R2);
                k = (k * k) >> 8; k = (k * k) >> 8;
                row[x] = AddColor(row[x], blobs[b].col, k * blobs[b].k >> 8);
            }
        }
    }

    /* 2. Stars. */
    for (int i = 0; i < W * H / 3600; i++) {
        int x = RtlRandom() % W, y = RtlRandom() % H;
        ULONG k = 60 + RtlRandom() % 180;
        ULONG tint = (RtlRandom() & 3) == 0 ? 0xFFD9B0 : ((RtlRandom() & 3) == 1 ? 0xB8D8FF : 0xFFFFFF);
        ULONG *p = Wall->Pixels + (SIZE_T)y * W + x;
        *p = AddColor(*p, tint, k);
        if (k > 215 && x > 2 && y > 2 && x < W - 3 && y < H - 3) {      /* sparkle */
            for (int d = 1; d <= 2; d++) {
                ULONG kk = k / (d * 3);
                p[-d] = AddColor(p[-d], tint, kk); p[d] = AddColor(p[d], tint, kk);
                p[-d * W] = AddColor(p[-d * W], tint, kk); p[d * W] = AddColor(p[d * W], tint, kk);
            }
        }
    }

    /* 3. Ribbons: each strand is the sum of two sines; brightness falls off as a
     *    Lorentzian of the vertical distance to the curve. */
    static const STRAND strands[] = {
        /* base%  a1   f1    p1   a2   f2    p2  width strength colour */
        { 61, 120, 950,  100, 44, 2100, 300, 48, 62,  0xFF2E88 },
        { 61, 120, 950,  100, 44, 2100, 300, 3,  240, 0xFF5FA0 },
        { 62, 114, 950,  122, 48, 2100, 344, 2,  180, 0xFF8F5F },
        { 63, 108, 950,  144, 52, 2100, 388, 2,  140, 0xFFC25F },
        { 64, 102, 950,  166, 56, 2100, 432, 2,  100, 0xFFE28F },
        { 65, 96,  950,  188, 60, 2100, 476, 1,  70,  0xFFF0C8 },
        { 37, 104, 800,  600, 38, 1700, 40,  42, 58,  0x7A4DFF },
        { 37, 104, 800,  600, 38, 1700, 40,  3,  225, 0x9B7BFF },
        { 38, 98,  800,  626, 42, 1700, 92,  2,  160, 0x6FA8FF },
        { 39, 92,  800,  652, 46, 1700, 144, 2,  120, 0x5FE0FF },
        { 40, 86,  800,  678, 50, 1700, 196, 1,  80,  0xB8F2FF },
        { 80, 58,  1200, 900, 22, 2600, 500, 32, 44,  0x1EC8FF },
        { 80, 58,  1200, 900, 22, 2600, 500, 2,  160, 0x7FF0FF },
        { 81, 53,  1200, 934, 26, 2600, 566, 2,  100, 0xB7FFE8 },
    };
    enum { NS = ARRAY_SIZE(strands) };
    int *curve = ExAllocatePoolWithTag((SIZE_T)NS * W * sizeof(int), TAG('W', 'a', 'l', 'l'));   /* 1/16 px */
    if (curve) {
        for (int s = 0; s < NS; s++) {
            const STRAND *t = &strands[s];
            for (int x = 0; x < W; x++) {
                /* angles in 1/64ths of a sine-table step, interpolated for smooth curves */
                int a1 = t->P1 * 64 + (int)((LONG64)x * t->F1 * 65536 / ((LONG64)W * 1000));
                int a2 = t->P2 * 64 + (int)((LONG64)x * t->F2 * 65536 / ((LONG64)W * 1000));
                int s1 = RtlSin(a1 >> 6) + (RtlSin((a1 >> 6) + 1) - RtlSin(a1 >> 6)) * (a1 & 63) / 64;
                int s2 = RtlSin(a2 >> 6) + (RtlSin((a2 >> 6) + 1) - RtlSin(a2 >> 6)) * (a2 & 63) / 64;
                curve[s * W + x] = t->Base * H * 16 / 100 + (s1 * t->A1 * H / 720 >> 8) + (s2 * t->A2 * H / 720 >> 8);
            }
        }
        for (int s = 0; s < NS; s++) {
            const STRAND *t = &strands[s];
            LONG64 wd = MAX(16, t->Width * H * 16 / 720), w2 = wd * wd, cut = wd * 7;
            ULONG tail = (ULONG)(w2 * 256 / (cut * cut + w2));
            for (int x = 0; x < W; x++) {
                int yc = curve[s * W + x];
                /* strands are brightest in the middle of the screen */
                int fade = 256 - (x > W / 2 ? x - W / 2 : W / 2 - x) * 120 / (W / 2);
                for (int y = MAX(0, (int)((yc - cut) >> 4)); y < MIN(H, (int)((yc + cut) >> 4) + 2); y++) {
                    LONG64 d = (LONG64)y * 16 + 8 - yc;
                    ULONG k = (ULONG)(w2 * 256 / (d * d + w2));
                    if (k <= tail) continue;
                    k = (k - tail) * t->Strength * fade >> 16;
                    ULONG *p = Wall->Pixels + (SIZE_T)y * W + x;
                    *p = AddColor(*p, t->Color, k);
                }
            }
        }
        ExFreePool(curve);
    }

    /* 4. Vignette. */
    LONG64 hx = W / 2, hy = H / 2, m2 = hx * hx + hy * hy;
    for (int y = 0; y < H; y++) {
        ULONG *row = Wall->Pixels + (SIZE_T)y * W;
        for (int x = 0; x < W; x++) {
            LONG64 d2 = (x - hx) * (x - hx) + (y - hy) * (y - hy);
            ULONG k = 256 - (ULONG)(d2 * 150 / m2);
            ULONG c = row[x];
            row[x] = RGB(((c >> 16) & 0xFF) * k >> 8, ((c >> 8) & 0xFF) * k >> 8, (c & 0xFF) * k >> 8);
        }
    }

    /* 5. Frosted copy for glass surfaces: downsample 4x, blur, upsample bilinearly. */
    int sw = W / 4, sh = H / 4;
    GDI_SURFACE *small = GdiCreateSurface(sw, sh);
    if (!small) { memcpy(Blur->Pixels, Wall->Pixels, (SIZE_T)W * H * 4); return; }
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            ULONG r = 0, g = 0, b = 0;
            for (int j = 0; j < 4; j++) {
                const ULONG *p = Wall->Pixels + (SIZE_T)(y * 4 + j) * W + x * 4;
                for (int i = 0; i < 4; i++) { r += (p[i] >> 16) & 0xFF; g += (p[i] >> 8) & 0xFF; b += p[i] & 0xFF; }
            }
            small->Pixels[y * sw + x] = RGB(r >> 4, g >> 4, b >> 4);
        }
    }
    GdiBoxBlur(small, 5);
    GdiBoxBlur(small, 4);
    for (int y = 0; y < H; y++) {
        int fy = y * 256 / 4 - 96; if (fy < 0) fy = 0;
        int y0 = MIN(fy >> 8, sh - 1), y1 = MIN(y0 + 1, sh - 1), wy = fy & 255;
        ULONG *out = Blur->Pixels + (SIZE_T)y * W;
        const ULONG *r0 = small->Pixels + y0 * sw, *r1 = small->Pixels + y1 * sw;
        for (int x = 0; x < W; x++) {
            int fx = x * 256 / 4 - 96; if (fx < 0) fx = 0;
            int x0 = MIN(fx >> 8, sw - 1), x1 = MIN(x0 + 1, sw - 1), wx = fx & 255;
            ULONG top = GdiBlendColor(r0[x0], r0[x1], wx), bot = GdiBlendColor(r1[x0], r1[x1], wx);
            out[x] = GdiBlendColor(top, bot, wy);
        }
    }
    GdiDestroySurface(small);
}

/* ---- Logo ------------------------------------------------------------------------------ */
static void DrawLogo(GDI_SURFACE *S, int cx, int cy, int r, ULONG a)
{
    GdiDiamond(S, cx, cy, r, MAX(2, r / 9), ARGB(a, COL_PINK), ARGB(a, COL_ORANGE));
    GdiDiamond(S, cx, cy, r * 58 / 100, MAX(1, r / 14), ARGB(a * 3 / 4, COL_VIOLET), ARGB(a * 3 / 4, COL_PINK));
    GdiDiamond(S, cx, cy, r * 30 / 100, 0, ARGB(a, COL_YELLOW), ARGB(a, COL_ORANGE));
}

static void DrawBackdropGlow(GDI_SURFACE *S, ULONG top, ULONG mid, ULONG bottom)
{
    int W = S->Width, H = S->Height;
    GdiGradientV(S, 0, 0, W, H / 2, OPAQUE(top), OPAQUE(mid));
    GdiGradientV(S, 0, H / 2, W, H - H / 2, OPAQUE(mid), OPAQUE(bottom));
}

/* ---- Boot splash ------------------------------------------------------------------------- */
void Win32kBootSplash(int Percent, PCSTR Stage)
{
    GDI_SURFACE *S = GdiScreen;
    if (!S) return;
    int W = S->Width, H = S->Height, cx = W / 2, cy = H / 2 - 60;
    GdiResetClip(S);
    DrawBackdropGlow(S, 0x070512, 0x120A2A, 0x050409);
    for (int i = 5; i >= 1; i--)
        GdiCircle(S, cx, cy, 60 + i * 34, ARGB(7, i & 1 ? COL_PINK : COL_VIOLET));
    DrawLogo(S, cx, cy, 78, 255);

    const char *name = RADIANT_NAME;
    int tw = GdiTextWidth(&GdiFontHuge, name);
    GdiDrawText(S, &GdiFontHuge, cx - tw / 2, cy + 104, name, OPAQUE(COL_TEXT));
    const char *tag = "NT-architecture kernel  /  x64";
    GdiDrawText(S, &GdiFontUi, cx - GdiTextWidth(&GdiFontUi, tag) / 2, cy + 190, tag, OPAQUE(COL_DIM));

    int bw = 360, bx = cx - bw / 2, by = cy + 236;
    GdiRoundRect(S, bx, by, bw, 6, 3, ARGB(36, 0xFFFFFF), ARGB(36, 0xFFFFFF));
    int fw = MAX(6, bw * Percent / 100);
    GdiShadow(S, bx, by, fw, 6, 3, 12, ARGB(70, COL_PINK));
    GdiRoundRect(S, bx, by, fw, 6, 3, OPAQUE(COL_PINK), OPAQUE(COL_ORANGE));
    GdiDrawText(S, &GdiFontSmall, cx - GdiTextWidth(&GdiFontSmall, Stage) / 2, by + 20, Stage, OPAQUE(COL_DIM));

    const char *foot = RADIANT_NAME " Kernel Version " RADIANT_VERSION_STR "  \"" RADIANT_CODENAME "\"";
    GdiDrawText(S, &GdiFontSmall, cx - GdiTextWidth(&GdiFontSmall, foot) / 2, H - 40, foot, ARGB(150, COL_DIM));
    GdiPresentAll();
}

/* ---- Bugcheck screen ----------------------------------------------------------------------- */
void Win32kDrawBugCheck(ULONG Code, PCSTR Name, ULONG64 P[4], PCSTR Detail)
{
    GDI_SURFACE *S = GdiScreen;
    if (!S) return;
    int W = S->Width, H = S->Height;
    GdiResetClip(S);
    DrawBackdropGlow(S, 0x1F0416, 0x2C0718, 0x08030A);
    int x = MAX(60, W / 10), y = MAX(50, H / 9);

    GdiDiamond(S, x + 40, y + 44, 44, 5, OPAQUE(COL_RED), OPAQUE(COL_PINK));
    static const signed char crack[][2] = { { 44, 6 }, { 34, 30 }, { 47, 42 }, { 37, 58 }, { 43, 82 } };
    for (int i = 0; i + 1 < (int)ARRAY_SIZE(crack); i++)
        for (int o = -1; o <= 1; o++)
            GdiLine(S, x + crack[i][0] + o, y + crack[i][1], x + crack[i + 1][0] + o, y + crack[i + 1][1], OPAQUE(COL_YELLOW));
    GdiDrawText(S, &GdiFontTitle, x + 104, y + 6, RADIANT_NAME " hit a fatal error and was halted", OPAQUE(0xFFFFFF));
    GdiDrawText(S, &GdiFontUi, x + 104, y + 50,
                "A kernel bugcheck stopped the system to protect its state. Nothing more will run.",
                OPAQUE(0xE7B9C8));
    y += 124;

    char line[200];
    RtlStringPrintf(line, sizeof(line), "*** STOP: 0x%08X", Code);
    GdiDrawText(S, &GdiFontTitle, x, y, line, OPAQUE(COL_YELLOW));
    y += 46;
    GdiDrawText(S, &GdiFontTitle, x, y, Name, OPAQUE(0xFFFFFF));
    y += 58;
    for (int i = 0; i < 4; i++) {
        RtlStringPrintf(line, sizeof(line), "Parameter %d   0x%016llX", i + 1, P[i]);
        GdiDrawText(S, &GdiFontMono, x, y, line, OPAQUE(0xF3D6DF));
        y += 20;
    }
    y += 16;
    GdiBlend(S, x, y, W - 2 * x, 1, ARGB(70, 0xFFFFFF));
    y += 16;
    for (const char *p = Detail; *p; ) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        GdiDrawTextN(S, &GdiFontMono, x, y, p, n, OPAQUE(0xD9B4C2));
        y += 20;
        p += n + (nl ? 1 : 0);
    }
    GdiDrawText(S, &GdiFontUi, x, H - 60,
                "Reset or power off the machine to continue.  " RADIANT_NAME " " RADIANT_VERSION_STR " (x64)",
                OPAQUE(0xB98A9C));
    GdiPresentAll();
}
