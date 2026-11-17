/*
 * RadiaNT -- hal/hal.c
 * Hardware abstraction layer for PC/AT compatible x64 machines:
 * 16550 serial, 8259A PIC, 8254 PIT, CMOS RTC, CPUID and power control.
 */
#include "hal.h"
#include "ke.h"
#include "rtl.h"

/* ---- COM1 ------------------------------------------------------------------ */
#define COM1 0x3F8
static BOOLEAN HalSerialPresent;

void HalInitializeSerial(void)
{
    HalOut8(COM1 + 1, 0x00);
    HalOut8(COM1 + 3, 0x80);
    HalOut8(COM1 + 0, 0x01);        /* 115200 baud */
    HalOut8(COM1 + 1, 0x00);
    HalOut8(COM1 + 3, 0x03);        /* 8N1 */
    HalOut8(COM1 + 2, 0xC7);
    HalOut8(COM1 + 4, 0x1E);        /* loopback self test */
    HalOut8(COM1 + 0, 0xAE);
    HalSerialPresent = (HalIn8(COM1) == 0xAE);
    HalOut8(COM1 + 4, 0x0F);
}

void HalSerialWrite(const char *s, SIZE_T n)
{
    if (!HalSerialPresent) return;
    for (SIZE_T i = 0; i < n; i++) {
        if (s[i] == '\n') {
            for (int spin = 0; spin < 100000 && !(HalIn8(COM1 + 5) & 0x20); spin++) { }
            HalOut8(COM1, '\r');
        }
        for (int spin = 0; spin < 100000 && !(HalIn8(COM1 + 5) & 0x20); spin++) { }
        HalOut8(COM1, (UCHAR)s[i]);
    }
}

/* ---- 8259A programmable interrupt controller -------------------------------- */
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

static struct { PHAL_ISR Routine; PVOID Context; } HalIsrTable[HAL_IRQ_COUNT];
ULONG64 HalIrqCounts[HAL_IRQ_COUNT];
PCSTR   HalIrqNames[HAL_IRQ_COUNT];
static USHORT HalIrqMask = 0xFFFB;      /* everything masked except the cascade */

static void HalWriteMask(void)
{
    HalOut8(PIC1_DAT, (UCHAR)HalIrqMask);
    HalOut8(PIC2_DAT, (UCHAR)(HalIrqMask >> 8));
}

void HalInitializeInterrupts(void)
{
    HalOut8(PIC1_CMD, 0x11); HalIoDelay();
    HalOut8(PIC2_CMD, 0x11); HalIoDelay();
    HalOut8(PIC1_DAT, 0x20); HalIoDelay();   /* IRQ0-7  -> vectors 0x20-0x27 */
    HalOut8(PIC2_DAT, 0x28); HalIoDelay();   /* IRQ8-15 -> vectors 0x28-0x2F */
    HalOut8(PIC1_DAT, 0x04); HalIoDelay();
    HalOut8(PIC2_DAT, 0x02); HalIoDelay();
    HalOut8(PIC1_DAT, 0x01); HalIoDelay();
    HalOut8(PIC2_DAT, 0x01); HalIoDelay();
    HalWriteMask();
}

void HalConnectInterrupt(ULONG Irq, PHAL_ISR Routine, PVOID Context, PCSTR Name)
{
    ULONG64 f = HalSaveFlagsAndCli();
    HalIsrTable[Irq].Routine = Routine;
    HalIsrTable[Irq].Context = Context;
    HalIrqNames[Irq] = Name;
    HalIrqMask &= ~(1u << Irq);
    HalWriteMask();
    HalRestoreFlags(f);
}

void HalEndOfInterrupt(ULONG Irq)
{
    if (Irq >= 8) HalOut8(PIC2_CMD, 0x20);
    HalOut8(PIC1_CMD, 0x20);
}

BOOLEAN HalDispatchIrq(ULONG Irq)
{
    /* Spurious IRQ7/IRQ15: the in-service bit is clear. */
    if (Irq == 7) {
        HalOut8(PIC1_CMD, 0x0B);
        if (!(HalIn8(PIC1_CMD) & 0x80)) return FALSE;
    } else if (Irq == 15) {
        HalOut8(PIC2_CMD, 0x0B);
        if (!(HalIn8(PIC2_CMD) & 0x80)) { HalOut8(PIC1_CMD, 0x20); return FALSE; }
    }
    HalIrqCounts[Irq]++;
    if (HalIsrTable[Irq].Routine) HalIsrTable[Irq].Routine(HalIsrTable[Irq].Context);
    HalEndOfInterrupt(Irq);
    return TRUE;
}

/* ---- 8254 interval timer ------------------------------------------------------ */
void HalInitializeClock(ULONG Hz)
{
    ULONG divisor = 1193182 / Hz;
    HalOut8(0x43, 0x36);
    HalOut8(0x40, (UCHAR)divisor);
    HalOut8(0x40, (UCHAR)(divisor >> 8));
}

/* ---- CPUID --------------------------------------------------------------------- */
HAL_CPU_INFO HalCpuInfo;

static void HalCpuid(ULONG leaf, ULONG sub, ULONG r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                             : "a"(leaf), "c"(sub));
}

void HalIdentifyProcessor(void)
{
    ULONG r[4];
    HalCpuid(0, 0, r);
    ULONG maxLeaf = r[0];
    memcpy(HalCpuInfo.Vendor + 0, &r[1], 4);
    memcpy(HalCpuInfo.Vendor + 4, &r[3], 4);
    memcpy(HalCpuInfo.Vendor + 8, &r[2], 4);
    if (maxLeaf >= 1) {
        HalCpuid(1, 0, r);
        HalCpuInfo.Stepping = r[0] & 0xF;
        HalCpuInfo.Model = (r[0] >> 4) & 0xF;
        HalCpuInfo.Family = (r[0] >> 8) & 0xF;
        if (HalCpuInfo.Family == 6 || HalCpuInfo.Family == 15)
            HalCpuInfo.Model |= ((r[0] >> 16) & 0xF) << 4;
        if (HalCpuInfo.Family == 15) HalCpuInfo.Family += (r[0] >> 20) & 0xFF;
        HalCpuInfo.HasSse2 = (r[3] >> 26) & 1;
        HalCpuInfo.HasApic = (r[3] >> 9) & 1;
        HalCpuInfo.Hypervisor = (r[2] >> 31) & 1;
    }
    HalCpuid(0x80000000, 0, r);
    ULONG maxExt = r[0];
    if (maxExt >= 0x80000001) {
        HalCpuid(0x80000001, 0, r);
        HalCpuInfo.HasNx = (r[3] >> 20) & 1;
    }
    if (maxExt >= 0x80000004) {
        for (ULONG i = 0; i < 3; i++) {
            HalCpuid(0x80000002 + i, 0, r);
            memcpy(HalCpuInfo.Brand + i * 16, r, 16);
        }
        /* trim leading spaces */
        char *b = HalCpuInfo.Brand; int skip = 0;
        while (b[skip] == ' ') skip++;
        if (skip) memmove(b, b + skip, 48 - skip);
    } else {
        RtlStringCopy(HalCpuInfo.Brand, sizeof(HalCpuInfo.Brand), "x86-64 processor");
    }
}

/* ---- CMOS real time clock ------------------------------------------------------- */
static UCHAR HalCmosRead(UCHAR reg)
{
    HalOut8(0x70, reg | 0x80);
    return HalIn8(0x71);
}

static int HalBcd(UCHAR v) { return (v & 0x0F) + (v >> 4) * 10; }

void HalQueryRealTimeClock(TIME_FIELDS *t)
{
    UCHAR s, m, h, d, mo, y, s2;
    do {
        for (int spin = 0; spin < 100000 && (HalCmosRead(0x0A) & 0x80); spin++) { }
        s = HalCmosRead(0x00); m = HalCmosRead(0x02); h = HalCmosRead(0x04);
        d = HalCmosRead(0x07); mo = HalCmosRead(0x08); y = HalCmosRead(0x09);
        s2 = HalCmosRead(0x00);
    } while (s != s2);
    UCHAR regB = HalCmosRead(0x0B);
    int pm = h & 0x80; h &= 0x7F;
    if (!(regB & 0x04)) {
        t->Second = HalBcd(s); t->Minute = HalBcd(m); t->Hour = HalBcd(h);
        t->Day = HalBcd(d); t->Month = HalBcd(mo); t->Year = 2000 + HalBcd(y);
    } else {
        t->Second = s; t->Minute = m; t->Hour = h; t->Day = d; t->Month = mo; t->Year = 2000 + y;
    }
    if (!(regB & 0x02)) { t->Hour %= 12; if (pm) t->Hour += 12; }
    if (t->Month < 1 || t->Month > 12) t->Month = 1;
    if (t->Day < 1 || t->Day > 31) t->Day = 1;
}

/* Days since 1970-01-01 for a civil date. */
static LONG64 HalDaysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    LONG64 era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void HalCivilFromDays(LONG64 z, TIME_FIELDS *t)
{
    t->Weekday = (USHORT)((z + 4) % 7);         /* 1970-01-01 was a Thursday */
    z += 719468;
    LONG64 era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + (int)era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    t->Day = (USHORT)(doy - (153 * mp + 2) / 5 + 1);
    t->Month = (USHORT)(mp < 10 ? mp + 3 : mp - 9);
    t->Year = (USHORT)(y + (t->Month <= 2));
}

static ULONG64 HalBootEpochSeconds;

void HalInitializeTime(void)
{
    TIME_FIELDS t;
    HalQueryRealTimeClock(&t);
    HalBootEpochSeconds = (ULONG64)HalDaysFromCivil(t.Year, t.Month, t.Day) * 86400
                        + t.Hour * 3600 + t.Minute * 60 + t.Second;
}

void KeQuerySystemTimeFields(TIME_FIELDS *t)
{
    ULONG64 now = HalBootEpochSeconds + KeTickCount / 1000;
    HalCivilFromDays((LONG64)(now / 86400), t);
    ULONG rem = (ULONG)(now % 86400);
    t->Hour = (USHORT)(rem / 3600);
    t->Minute = (USHORT)(rem / 60 % 60);
    t->Second = (USHORT)(rem % 60);
}

/* ---- Power ----------------------------------------------------------------------- */
NORETURN void HalReturnToFirmware(BOOLEAN Reboot)
{
    HalDisableInterrupts();
    if (Reboot) {
        for (int i = 0; i < 100000 && (HalIn8(0x64) & 2); i++) { }
        HalOut8(0x64, 0xFE);                /* pulse the CPU reset line */
        HalOut8(0xCF9, 0x06);               /* PCI reset */
        struct { USHORT l; ULONG64 b; } PACKED nullIdt = { 0, 0 };
        __asm__ volatile("lidt %0; int3" : : "m"(nullIdt));
    } else {
        HalOut16(0x604, 0x2000);            /* QEMU (q35/i440fx ACPI PM1a) */
        HalOut16(0xB004, 0x2000);           /* Bochs / old QEMU */
        HalOut16(0x4004, 0x3400);           /* VirtualBox */
    }
    for (;;) HalHalt();
}
