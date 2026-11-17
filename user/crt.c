/* RadiaNT user mode -- crt.c : process entry point and minimal C runtime. */
#include "ntdll.h"
#include <stdarg.h>

void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static void Format(const char *fmt, va_list ap)
{
    char out[512]; int n = 0;
#define PUT(ch) do { if (n < (int)sizeof(out)) out[n++] = (ch); } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        int width = 0, zero = 0, lng = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng++; fmt++; }
        char tmp[32]; int t = 0;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            int len = 0; while (s[len]) len++;
            for (int i = 0; i < len; i++) PUT(s[i]);
            for (int i = len; i < width; i++) PUT(' ');
            continue;
        } else if (*fmt == 'c') {
            PUT((char)va_arg(ap, int));
            continue;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X' || *fmt == 'p') {
            ULONG64 v; int base = 10, neg = 0;
            if (*fmt == 'p') { v = va_arg(ap, ULONG64); base = 16; width = 16; zero = 1; }
            else if (*fmt == 'd') {
                long long sv = lng ? va_arg(ap, long long) : va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (ULONG64)-sv; } else v = (ULONG64)sv;
            } else {
                v = lng ? va_arg(ap, ULONG64) : va_arg(ap, unsigned int);
                if (*fmt != 'u') base = 16;
            }
            do { tmp[t++] = "0123456789ABCDEF"[v % base]; v /= base; } while (v);
            if (neg) tmp[t++] = '-';
            for (int i = t; i < width; i++) PUT(zero ? '0' : ' ');
            while (t) PUT(tmp[--t]);
        } else {
            PUT(*fmt);
        }
    }
#undef PUT
    NtDisplayString(out, n);
}

void Print(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    Format(fmt, ap);
    va_end(ap);
}

void PrintColor(ULONG color, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    NtSetConsoleColor(color);
    Format(fmt, ap);
    NtSetConsoleColor(CON_DEFAULT);
    va_end(ap);
}

/* The kernel jumps here in ring 3 with a fresh stack. */
void NtProcessStartup(void)
{
    NtTerminateProcess(main());
}
