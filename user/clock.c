/* clock.exe -- sleeps in the kernel between lines (NtDelayExecution). */
#include "ntdll.h"

int main(void)
{
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    PrintColor(CON_ORANGE, "Ticking once a second, ten times. Ctrl+C stops me.\n");
    for (int i = 0; i < 10; i++) {
        SYSTEM_TIME_INFORMATION t;
        NtQuerySystemTime(&t);
        PrintColor(CON_CYAN, "  %s %02u:%02u:%02u", days[t.Weekday % 7], t.Hour, t.Minute, t.Second);
        Print("   tick %llu\n", NtGetTickCount());
        NtDelayExecution(1000);
    }
    return 0;
}
