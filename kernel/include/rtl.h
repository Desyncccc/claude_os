/* RadiaNT -- rtl.h : run-time library (strings, memory, formatted output). */
#pragma once
#include "ntdef.h"

void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    RtlCompareStringI(const char *a, const char *b);      /* case-insensitive */
int    RtlPrefixStringI(const char *prefix, const char *s);  /* 1 if s starts with prefix */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
void   RtlStringCopy(char *dst, SIZE_T cap, const char *src);
void   RtlStringCat(char *dst, SIZE_T cap, const char *src);
ULONG64 RtlStringToInteger(const char *s, int base);
int    RtlStringVPrintf(char *buf, SIZE_T cap, const char *fmt, va_list ap);
int    RtlStringPrintf(char *buf, SIZE_T cap, const char *fmt, ...)
           __attribute__((format(printf, 3, 4)));
ULONG  RtlRandom(void);
int    RtlSin(int angle);   /* angle: 0..1023 = full turn, result -4096..4096 */
int    RtlCos(int angle);
ULONG  RtlIntegerSqrt(ULONG64 v);

/* Kernel debugger output: COM1 + the in-memory boot log. */
void DbgPrint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define RTL_BOOT_LOG_LINES 64
extern char  RtlBootLog[RTL_BOOT_LOG_LINES][96];
extern ULONG RtlBootLogCount;

#define ASSERT(x) do { if (!(x)) KeBugCheckEx(0x000000A0 /* ASSERTION */, \
    (ULONG64)__FILE__, __LINE__, 0, 0); } while (0)
