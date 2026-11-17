/* primes.exe -- CPU-bound work in ring 3; the scheduler preempts it freely. */
#include "ntdll.h"

int main(void)
{
    enum { LIMIT = 2000000 };
    void *mem = 0;
    if (NtAllocateVirtualMemory(&mem, LIMIT + 1) != STATUS_SUCCESS) {
        PrintColor(CON_RED, "out of memory\n");
        return 1;
    }
    unsigned char *composite = mem;
    ULONG64 start = NtGetTickCount();
    unsigned count = 0, last = 0;
    PrintColor(CON_ORANGE, "Sieve of Eratosthenes up to %u...\n", (unsigned)LIMIT);
    for (unsigned i = 2; i <= LIMIT; i++) {
        if (composite[i]) continue;
        count++; last = i;
        for (unsigned long long j = (unsigned long long)i * i; j <= LIMIT; j += i) composite[j] = 1;
    }
    ULONG64 ms = NtGetTickCount() - start;
    Print("%u primes found, the largest is %u.\n", count, last);
    Print("First few:");
    for (unsigned i = 2, shown = 0; shown < 16; i++) if (!composite[i]) { Print(" %u", i); shown++; }
    Print("\nTook %llu ms of wall time in user mode.\n", ms);
    return 0;
}
