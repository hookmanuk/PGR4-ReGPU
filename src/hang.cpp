// Hang diagnostics: the host call stack of every thread in the process, logged when a
// renderer wait runs far too long, so a lockup in the field says what each worker is
// stuck in.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>
#include <tlhelp32.h>

#include "log.h"

std::string HostStack(void* threadHandle, int maxFrames);

void LogAllHostThreadStacks(const char* why)
{
    Log("hang: %s; host thread stacks:", why);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        const std::string stack = HostStack(thread, 14);
        Log("hang:   thread %lu (priority %d): %s", entry.th32ThreadID, GetThreadPriority(thread), stack.c_str());
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

// PGR4_STACKS_AT=<seconds>[,<seconds>...]: every thread's stack at those times after start (what a
// subsystem that never gets going is waiting on).
namespace
{
struct StacksAt
{
    StacksAt()
    {
        const char* v = getenv("PGR4_STACKS_AT");
        if (!v)
            return;
        std::string times = v;
        CreateThread(nullptr, 0, [](void* p) -> DWORD {
            std::string* list = static_cast<std::string*>(p);
            double last = 0;
            for (size_t at = 0; at < list->size();)
            {
                const double t = atof(list->c_str() + at);
                Sleep(DWORD((t - last) * 1000));
                last = t;
                char why[48];
                snprintf(why, sizeof(why), "PGR4_STACKS_AT %.0f s", t);
                LogAllHostThreadStacks(why);
                const size_t comma = list->find(',', at);
                at = comma == std::string::npos ? list->size() : comma + 1;
            }
            delete list;
            return 0;
        }, new std::string(times), 0, nullptr);
    }
} g_stacksAt;
}
