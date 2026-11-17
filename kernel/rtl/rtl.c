/* RadiaNT -- rtl/rtl.c : freestanding C runtime + formatted output. */
#include "rtl.h"
#include "hal.h"
#include "ke.h"

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    size_t q = n >> 3;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    n &= 7;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n)
{
    UCHAR *dp = d; const UCHAR *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    void *r = d;
    ULONG64 v = (UCHAR)c * 0x0101010101010101ull;
    size_t q = n >> 3;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
    n &= 7;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
    return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const UCHAR *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (UCHAR)*a - (UCHAR)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (UCHAR)*a - (UCHAR)*b;
        if (!*a) break;
    }
    return 0;
}

static int RtlLower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int RtlCompareStringI(const char *a, const char *b)
{
    while (*a && RtlLower(*a) == RtlLower(*b)) { a++; b++; }
    return RtlLower((UCHAR)*a) - RtlLower((UCHAR)*b);
}

int RtlPrefixStringI(const char *prefix, const char *s)
{
    while (*prefix) {
        if (RtlLower(*prefix) != RtlLower(*s)) return 0;
        prefix++; s++;
    }
    return 1;
}

char *strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}

void RtlStringCopy(char *dst, SIZE_T cap, const char *src)
{
    SIZE_T i = 0;
    if (!cap) return;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

void RtlStringCat(char *dst, SIZE_T cap, const char *src)
{
    SIZE_T n = strlen(dst);
    if (n < cap) RtlStringCopy(dst + n, cap - n, src);
}

ULONG64 RtlStringToInteger(const char *s, int base)
{
    ULONG64 v = 0;
    while (*s == ' ') s++;
    if (base == 0) {
        base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    }
    for (;; s++) {
        int c = RtlLower(*s), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    return v;
}

/* ---- printf family: %d %i %u %x %X %p %s %c %%, flags 0 - , width, l/ll --- */
typedef struct { char *buf; SIZE_T cap, len; } RTL_OUT;
static void RtlPut(RTL_OUT *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

int RtlStringVPrintf(char *buf, SIZE_T cap, const char *fmt, va_list ap)
{
    RTL_OUT o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { RtlPut(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, lng = 0, comma = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == ',') comma = 1;
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0; fmt++;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') { lng++; fmt++; }
        char tmp[40]; int n = 0; const char *s = NULL; int neg = 0;
        switch (*fmt) {
        case 'c': tmp[n++] = (char)va_arg(ap, int); break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
            ULONG64 v; int base = 10; const char *digs = "0123456789abcdef";
            if (*fmt == 'p') { v = va_arg(ap, ULONG64); base = 16; zero = 1; if (!width) width = 16; }
            else if (*fmt == 'd' || *fmt == 'i') {
                LONG64 sv = lng ? va_arg(ap, LONG64) : va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (ULONG64)(-sv); } else v = (ULONG64)sv;
            } else {
                v = lng ? va_arg(ap, ULONG64) : va_arg(ap, unsigned int);
                if (*fmt != 'u') base = 16;
                if (*fmt == 'X') digs = "0123456789ABCDEF";
            }
            int digits = 0;
            do {
                if (comma && digits && digits % 3 == 0) tmp[n++] = ',';
                tmp[n++] = digs[v % base]; v /= base; digits++;
            } while (v && n < 38);
            if (neg) tmp[n++] = '-';
            for (int i = 0; i < n / 2; i++) { char t = tmp[i]; tmp[i] = tmp[n - 1 - i]; tmp[n - 1 - i] = t; }
            break;
        }
        case '%': tmp[n++] = '%'; break;
        default:  tmp[n++] = '%'; tmp[n++] = *fmt; break;
        }
        int len = s ? (int)strlen(s) : n;
        if (s && prec >= 0 && len > prec) len = prec;
        const char *p = s ? s : tmp;
        if (!left) {
            if (zero && neg) { RtlPut(&o, '-'); p++; len--; width--; }
            for (int i = len; i < width; i++) RtlPut(&o, zero && !s ? '0' : ' ');
        }
        for (int i = 0; i < len; i++) RtlPut(&o, p[i]);
        if (left) for (int i = len; i < width; i++) RtlPut(&o, ' ');
        if (!*fmt) break;
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int RtlStringPrintf(char *buf, SIZE_T cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = RtlStringVPrintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- Debug output ---------------------------------------------------------- */
char  RtlBootLog[RTL_BOOT_LOG_LINES][96];
ULONG RtlBootLogCount;

void DbgPrint(const char *fmt, ...)
{
    char line[256];
    va_list ap; va_start(ap, fmt);
    int n = RtlStringVPrintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    ULONG64 f = HalSaveFlagsAndCli();
    HalSerialWrite(line, n);
    if (RtlBootLogCount < RTL_BOOT_LOG_LINES) {
        char *d = RtlBootLog[RtlBootLogCount++];
        RtlStringCopy(d, 96, line);
        SIZE_T l = strlen(d);
        while (l && (d[l - 1] == '\n' || d[l - 1] == '\r')) d[--l] = 0;
    }
    HalRestoreFlags(f);
}

/* ---- Misc math -------------------------------------------------------------- */
static ULONG RtlSeed = 0x2611C0DE;
ULONG RtlRandom(void)
{
    RtlSeed ^= RtlSeed << 13; RtlSeed ^= RtlSeed >> 17; RtlSeed ^= RtlSeed << 5;
    return RtlSeed;
}

ULONG RtlIntegerSqrt(ULONG64 v)
{
    ULONG64 r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (ULONG)r;
}

/* Bhaskara I approximation, max error ~0.16% -- plenty for eye candy.
 * The kernel is built without FPU/SSE code, so everything is fixed point. */
int RtlSin(int a)
{
    a &= 1023;
    int sign = 1;
    if (a >= 512) { a -= 512; sign = -1; }
    LONG64 x = a, p = x * (512 - x);            /* half turn = 512 units */
    LONG64 v = (4 * p * 4096) / (327680 - p);   /* 5/4 * 512^2 = 327680 */
    return (int)(sign * v);
}
int RtlCos(int a) { return RtlSin(a + 256); }
