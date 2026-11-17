/* spin.exe -- an infinite loop. The desktop stays responsive: preemption works. */
#include "ntdll.h"

int main(void)
{
    PrintColor(CON_ORANGE, "Spinning forever in user mode (pid %u).\n", NtGetCurrentProcessId());
    Print("Watch System Monitor, then press Ctrl+C here or 'kill' me from another terminal.\n");
    volatile unsigned long long n = 0;
    for (;;) n++;
}
