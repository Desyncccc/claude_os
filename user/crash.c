/* crash.exe -- proves that ring 3 cannot touch the kernel. */
#include "ntdll.h"

int main(void)
{
    volatile unsigned long long *kernel = (unsigned long long *)0xFFFFFFFF80000000ull;
    PrintColor(CON_ORANGE, "About to scribble on kernel memory at 0x%p from user mode...\n", (void *)kernel);
    NtDelayExecution(400);
    *kernel = 0xDEADBEEF;
    PrintColor(CON_RED, "This line must never appear.\n");
    return 0;
}
