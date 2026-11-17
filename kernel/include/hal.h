/* RadiaNT -- hal.h : hardware abstraction layer (PC/AT compatible, x64). */
#pragma once
#include "ntdef.h"

static inline void HalOut8(USHORT p, UCHAR v)  { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void HalOut16(USHORT p, USHORT v){ __asm__ volatile("outw %0,%1" : : "a"(v), "Nd"(p)); }
static inline void HalOut32(USHORT p, ULONG v) { __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline UCHAR HalIn8(USHORT p)  { UCHAR v;  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void HalIoDelay(void)   { HalOut8(0x80, 0); }
static inline void HalDisableInterrupts(void) { __asm__ volatile("cli"); }
static inline void HalEnableInterrupts(void)  { __asm__ volatile("sti"); }
static inline void HalHalt(void)              { __asm__ volatile("hlt"); }
static inline ULONG64 HalSaveFlagsAndCli(void)
{
    ULONG64 f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f)); return f;
}
static inline void HalRestoreFlags(ULONG64 f)
{
    if (f & 0x200) __asm__ volatile("sti");
}
static inline ULONG64 HalReadCr2(void) { ULONG64 v; __asm__ volatile("mov %%cr2,%0" : "=r"(v)); return v; }
static inline ULONG64 HalReadCr3(void) { ULONG64 v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline void HalWriteCr3(ULONG64 v) { __asm__ volatile("mov %0,%%cr3" : : "r"(v) : "memory"); }
static inline void HalInvalidatePage(ULONG64 va) { __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory"); }
static inline void HalWriteMsr(ULONG msr, ULONG64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((ULONG)v), "d"((ULONG)(v >> 32)));
}
static inline ULONG64 HalReadMsr(ULONG msr)
{
    ULONG lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((ULONG64)hi << 32) | lo;
}
static inline ULONG64 HalReadTsc(void)
{
    ULONG lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((ULONG64)hi << 32) | lo;
}

/* Serial debug port (COM1) */
void HalInitializeSerial(void);
void HalSerialWrite(const char *s, SIZE_T n);

/* Interrupt controller + system clock */
typedef void (*PHAL_ISR)(PVOID Context);
void  HalInitializeInterrupts(void);
void  HalConnectInterrupt(ULONG Irq, PHAL_ISR Routine, PVOID Context, PCSTR Name);
void  HalEndOfInterrupt(ULONG Irq);
BOOLEAN HalDispatchIrq(ULONG Irq);     /* returns FALSE for spurious */
void  HalInitializeClock(ULONG Hz);
#define HAL_IRQ_COUNT 16
extern ULONG64 HalIrqCounts[HAL_IRQ_COUNT];
extern PCSTR   HalIrqNames[HAL_IRQ_COUNT];

/* Processor identification */
typedef struct _HAL_CPU_INFO {
    CHAR  Vendor[13];
    CHAR  Brand[49];
    ULONG Family, Model, Stepping;
    BOOLEAN HasNx, HasSse2, HasApic, Hypervisor;
} HAL_CPU_INFO;
extern HAL_CPU_INFO HalCpuInfo;
void HalIdentifyProcessor(void);

/* Real time clock */
typedef struct _TIME_FIELDS {
    USHORT Year, Month, Day, Hour, Minute, Second, Weekday;
} TIME_FIELDS;
void    HalQueryRealTimeClock(TIME_FIELDS *t);
void    HalInitializeTime(void);
void    KeQuerySystemTimeFields(TIME_FIELDS *t);    /* boot RTC + ticks */

/* Power */
NORETURN void HalReturnToFirmware(BOOLEAN Reboot);

/* Boot framebuffer handed over by the loader */
typedef struct _HAL_FRAMEBUFFER {
    ULONG64 PhysicalAddress;
    UCHAR  *Base;       /* mapped */
    ULONG   Width, Height, Pitch, Bpp;
    UCHAR   RedShift, GreenShift, BlueShift;
} HAL_FRAMEBUFFER;
extern HAL_FRAMEBUFFER HalFramebuffer;
