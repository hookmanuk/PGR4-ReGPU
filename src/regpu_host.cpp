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
    c.lowPerformance = s.lowPerformance;
    // The game's Present waits for the GPU to finish an earlier frame, and the GPU waited for each batch of
    // the frame to be recorded: recording and GPU work took turns (FX-8350 + GTX 970 race: GPU busy 13.5 ms
    // of a 27.8 ms frame). Submitting every 512 draws lets the GPU start early: 35.7 -> 43.9 fps there
    // (256: 44.2, 128: 43.1). Now always, not just in low performance mode; REGPU_SUBMIT_DRAWS=0 turns it off.
    if (!getenv("REGPU_SUBMIT_DRAWS"))
        _putenv("REGPU_SUBMIT_DRAWS=512");
    // ReGPU's copies of rewritten buffers made for FH1 (its shadow cascades' and skid marks' buffers,
    // rewritten in place) cost PGR4 16 fps on the FX race grid (45 -> 29.5) and fix nothing here: small
    // index buffers copied caught the strip-cut strips (drawn as expanded triangles again), full upload
    // hashes replaced the sampled ones, and a vertex buffer seen changing once stayed copied for good
    // (back in place after 120 quiet frames, as before). All three are live switches:
    // `regpuctl switch NAME 0` gives ReGPU's default.
    // REGPU_LEAN_FETCH (one sample per 2D pixel fetch, through a sampler table) is +1 fps there, the
    // race grid unchanged; REGPU_DRAW_THREAD is -7 fps on the FX, so it stays off.
    for (const char* name : { "REGPU_DIRECT_DYNAMIC_INDICES", "REGPU_UPLOAD_SAMPLED", "REGPU_DYNAMIC_EXPIRE", "REGPU_LEAN_FETCH" })
        if (!getenv(name))
            _putenv_s(name, "1");
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
