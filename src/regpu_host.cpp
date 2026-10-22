// ReGPU (thirdparty/ReGPU): the game's settings as its Config, the runtime's memory and guest threads
// as its Host, then the GPU starts.
#include <regpu/regpu.h>

#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "memory.h"
#include "settings.h"

void LogAllHostThreadStacks(const char* why); // hang.cpp
std::string CurrentHostStack(int maxFrames);  // crash.cpp
void SpikeDump(double ms);                    // kernel/thread.cpp

void GpuStart()
{
    const Settings& s = GetSettings();
    regpu::Config& c = regpu::GetConfig();
    c.width = s.width;
    c.height = s.height;
    c.aspectW = s.aspectW;
    c.aspectH = s.aspectH;
    c.fullscreen = s.fullscreen;
    c.vsync = s.vsync;
    c.maxFps = s.maxFps;
    c.enhance = s.enhance;
    c.msaa = s.msaa;
    c.upscaler = regpu::Config::Upscaler(uint32_t(s.upscaler));
    c.quality = s.quality;
    c.sharpness = s.sharpness;
    c.userDir = UserDir();
    c.windowTitle = L"Project Gotham Racing 4";
    // About 5,000 draws a frame in menus: 64 MB overflowed and dropped the scene's full-screen copy.
    c.uploadRingMB = 256;

    regpu::Host& h = regpu::GetHost();
    h.base = g_base;
    h.physicalSection = PhysicalSection();
    h.alloc = [](bool physical, uint32_t size, uint32_t align) { return GuestAlloc(physical ? Heap::Physical : Heap::Virtual, size, align); };
    h.createThread = [](const char* name, void (*body)(PPCContext&)) { CreateHostGuestThread(name, body); };
    h.currentPcr = [] { return CurrentGuestThread()->pcr; };
    h.guestCall = [](PPCContext& ctx, uint32_t address) { GuestCall(ctx, address); };
    h.interruptLockAcquire = [](int cpu) { InterruptLockAcquire(cpu); };
    h.interruptLockRelease = [] { InterruptLockRelease(); };
    h.cpuUsage = [](double wallSeconds) { return GuestThreadCpuUsage(wallSeconds); };
    h.logThreadStacks = [](const char* why) { LogAllHostThreadStacks(why); };
    h.currentStack = [](int maxFrames) { return CurrentHostStack(maxFrames); };
    h.slowFrame = [](double ms) { SpikeDump(ms); };
    regpu::Start();
}
