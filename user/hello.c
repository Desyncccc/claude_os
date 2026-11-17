/* hello.exe -- first contact with the native API from ring 3. */
#include "ntdll.h"

static unsigned short ReadCs(void) { unsigned short cs; __asm__("mov %%cs, %0" : "=r"(cs)); return cs; }

int main(void)
{
    SYSTEM_BASIC_INFORMATION info;
    SYSTEM_TIME_INFORMATION now;
    HANDLE event;
    int local;

    PrintColor(CON_PINK, "Hello from user mode!\n");
    Print("I am a PE32+ image running in ring %u (CS=0x%02X), process id %u.\n",
          ReadCs() & 3, ReadCs(), NtGetCurrentProcessId());
    Print("main() is at 0x%p, my stack is near 0x%p.\n", (void *)main, (void *)&local);

    if (NtQuerySystemInformation(SystemBasicInformation, &info, sizeof(info)) == STATUS_SUCCESS) {
        PrintColor(CON_ORANGE, "NtQuerySystemInformation says:\n");
        Print("  RadiaNT %u.%u build %u on %s\n", info.MajorVersion, info.MinorVersion,
              info.BuildNumber, info.ProcessorName);
        Print("  %llu MB RAM (%llu MB free), %u processes, %u threads, up %llu ms\n",
              info.TotalPhysicalPages * info.PageSize >> 20, info.FreePhysicalPages * info.PageSize >> 20,
              info.ProcessCount, info.ThreadCount, info.TickCount);
    }
    if (NtQuerySystemTime(&now) == STATUS_SUCCESS)
        Print("  System time %04u-%02u-%02u %02u:%02u:%02u UTC\n", now.Year, now.Month, now.Day,
              now.Hour, now.Minute, now.Second);

    /* Kernel objects through handles: create an event, wait on it, close it. */
    if (NtCreateEvent(&event, 0, 0) == STATUS_SUCCESS) {
        NTSTATUS st = NtWaitForSingleObject(event, 250);
        Print("Event handle 0x%X: 250 ms wait returned 0x%X (%s)\n", (ULONG)event, (ULONG)st,
              st == STATUS_TIMEOUT ? "STATUS_TIMEOUT" : "?");
        NtSetEvent(event);
        st = NtWaitForSingleObject(event, INFINITE);
        Print("After NtSetEvent the wait returned 0x%X (%s)\n", (ULONG)st, st == 0 ? "STATUS_SUCCESS" : "?");
        NtClose(event);
    }

    void *heap = 0;
    if (NtAllocateVirtualMemory(&heap, 1 << 20) == STATUS_SUCCESS) {
        char *p = heap;
        for (int i = 0; i < (1 << 20); i += 4096) p[i] = (char)i;
        Print("Allocated and touched 1 MB of private memory at 0x%p.\n", heap);
    }
    PrintColor(CON_GREEN, "Goodbye.\n");
    return 0;
}
