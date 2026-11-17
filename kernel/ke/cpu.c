/*
 * RadiaNT -- ke/cpu.c
 * Processor control structures: GDT, TSS, IDT and the SYSCALL MSRs.
 */
#include "ke.h"
#include "hal.h"
#include "rtl.h"

#define KGDT64_R0_CODE 0x08
#define KGDT64_R0_DATA 0x10
#define KGDT64_R3_DATA 0x18
#define KGDT64_R3_CODE 0x20
#define KGDT64_SYS_TSS 0x28

typedef struct _KTSS64 {
    ULONG   Reserved0;
    ULONG64 Rsp0, Rsp1, Rsp2;
    ULONG64 Reserved1;
    ULONG64 Ist[7];
    ULONG64 Reserved2;
    USHORT  Reserved3, IoMapBase;
} PACKED KTSS64;

typedef struct _KIDTENTRY64 {
    USHORT OffsetLow, Selector;
    UCHAR  Ist, TypeAttr;
    USHORT OffsetMiddle;
    ULONG  OffsetHigh, Reserved;
} PACKED KIDTENTRY64;

typedef struct { USHORT Limit; ULONG64 Base; } PACKED KDESCRIPTOR;

static ULONG64     KiGdt[7];
static KTSS64      KiTss;
static KIDTENTRY64 KiIdt[256];
static UCHAR       KiDoubleFaultStack[0x2000] __attribute__((aligned(16)));

ULONG64 KiSyscallStackTop;
ULONG64 KiUserRspScratch;

void KiSetKernelStack(ULONG64 Top)
{
    KiTss.Rsp0 = Top;
    KiSyscallStackTop = Top;
}

void KiInitializeProcessor(void)
{
    /* GDT -- the R3 data/code order is dictated by SYSRET. */
    KiGdt[0] = 0;
    KiGdt[1] = 0x00209A0000000000ull;       /* ring 0 code, long mode */
    KiGdt[2] = 0x0000920000000000ull;       /* ring 0 data */
    KiGdt[3] = 0x0000F20000000000ull;       /* ring 3 data */
    KiGdt[4] = 0x0020FA0000000000ull;       /* ring 3 code, long mode */

    memset(&KiTss, 0, sizeof(KiTss));
    KiTss.Ist[0] = (ULONG64)KiDoubleFaultStack + sizeof(KiDoubleFaultStack);
    KiTss.IoMapBase = sizeof(KiTss);
    ULONG64 base = (ULONG64)&KiTss, limit = sizeof(KiTss) - 1;
    KiGdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
               (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    KiGdt[6] = base >> 32;

    KDESCRIPTOR gdtr = { sizeof(KiGdt) - 1, (ULONG64)KiGdt };
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%eax\n"
        "mov %%eax, %%ds\n"
        "mov %%eax, %%es\n"
        "mov %%eax, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%eax, %%fs\n"
        "mov %%eax, %%gs\n"
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        : : "m"(gdtr) : "rax", "memory");

    for (int i = 0; i < 256; i++) {
        ULONG64 h = KiIsrStubTable[i];
        KiIdt[i].OffsetLow = (USHORT)h;
        KiIdt[i].Selector = KGDT64_R0_CODE;
        KiIdt[i].Ist = (i == 8 || i == 2) ? 1 : 0;     /* #DF and NMI on IST1 */
        KiIdt[i].TypeAttr = 0x8E;                       /* present, DPL0, interrupt gate */
        KiIdt[i].OffsetMiddle = (USHORT)(h >> 16);
        KiIdt[i].OffsetHigh = (ULONG)(h >> 32);
        KiIdt[i].Reserved = 0;
    }
    KDESCRIPTOR idtr = { sizeof(KiIdt) - 1, (ULONG64)KiIdt };
    __asm__ volatile("lidt %0" : : "m"(idtr));

    /* SYSCALL/SYSRET: kernel CS = 0x08, SYSRET CS = 0x10 + 16 | 3, SS = 0x10 + 8 | 3 */
    HalWriteMsr(0xC0000081, ((ULONG64)KGDT64_R0_DATA << 48) | ((ULONG64)KGDT64_R0_CODE << 32));
    HalWriteMsr(0xC0000082, (ULONG64)KiSystemCall64);
    HalWriteMsr(0xC0000084, 0x47700);       /* mask TF, IF, DF, NT, AC on entry */
}
