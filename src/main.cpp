#include <filesystem>
#include <thread>

#include <windows.h>

#include <regpu/regpu.h>

#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "loader.h"
#include "log.h"
#include "memory.h"

void CrashHandlerInit();

int main(int argc, char** argv)
{
    CrashHandlerInit();
    std::filesystem::path gameDir = argc > 1 ? argv[1] : "game";
    Log("pgr4: game folder %s", std::filesystem::absolute(gameDir).string().c_str());

    MemoryInit();
    LoadTitle(gameDir / "default.xex");
    if (const char* dump = getenv("PGR4_DUMP_IMAGE")) // writes the loaded executable image (0x82000000..0x84000000)
    {
        if (FILE* f = fopen(dump, "wb"))
        {
            fwrite(GuestPtr(0x82000000), 1, 0x2000000, f);
            fclose(f);
            Log("wrote the loaded image to %s", dump);
        }
    }
    RuntimeInit(gameDir); // also starts ReGPU (regpu_host.cpp) and its debug server (REGPU_DEBUG=1)
    ThreadSystemInit(g_title.tlsTemplate, g_title.tlsDataSize, g_title.tlsSlotCount);

    void StartThreadProfiler();
    StartThreadProfiler();
    void StartSpikeRecorder();
    StartSpikeRecorder();
    auto main = CreateGuestThread(g_title.entryPoint, 0, 0, g_title.stackSize, false, "main");
    char watchdogMs[16] = {};
    const DWORD watchdog = GetEnvironmentVariableA("PGR4_WATCHDOG", watchdogMs, 16) ? (atoi(watchdogMs) > 1 ? std::max(50, atoi(watchdogMs)) : 3000) : INFINITE;
    if (getenv("PGR4_WATCHW"))
    {
        std::thread([] {
            void ArmWatchpointAll();
            for (;;)
            {
                Sleep(10);
                ArmWatchpointAll();
            }
        }).detach();
    }
    while (WaitForSingleObject(main->thread->hostHandle, watchdog) == WAIT_TIMEOUT)
    {
        Log("watchdog:");
        LogGuestThreads();
        regpu::LogState();
        void LogFileStats();
        LogFileStats();
    }
    Log("pgr4: main thread exited with %08X", main->exitCode);
    return 0;
}
