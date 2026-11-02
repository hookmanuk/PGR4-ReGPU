// ReGPU (thirdparty/ReGPU): the game's settings as its Config, the runtime's memory and guest threads
// as its Host, then the GPU starts.
#include <regpu/regpu.h>

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "gpu/boot_screen.h"
#include "gpu/pipeline_seed.h"
#include "gpu/static_shaders.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "memory.h"
#include "settings.h"

void LogAllHostThreadStacks(const char* why); // hang.cpp
std::string CurrentHostStack(int maxFrames);  // crash.cpp
void SpikeDump(double ms);                    // kernel/thread.cpp

namespace
{
// The window title: "Project Gotham Racing 4 - ReGPU - <fps> FPS" (as Forza Horizon ReImagined shows it), updated once a
// second. The renderer only counts frames (onSwap); a thread of its own sets the title, so a window message never
// holds up a frame.
constexpr const wchar_t* kTitle = L"Project Gotham Racing 4 - ReGPU";
std::atomic<uint64_t> g_swaps{ 0 };

BOOL CALLBACK FindOurWindow(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[16] = {};
    if (pid == GetCurrentProcessId() && GetClassNameW(w, cls, 16) && wcscmp(cls, L"regpu") == 0 && !GetParent(w))
    {
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

void TitleLoop()
{
    SetThreadDescription(GetCurrentThread(), L"window title");
    HWND window = nullptr;
    uint64_t last = g_swaps.load();
    for (;;)
    {
        Sleep(1000);
        const uint64_t now = g_swaps.load();
        const uint64_t fps = now - last;
        last = now;
        if (!window || !IsWindow(window))
        {
            window = nullptr;
            EnumWindows(FindOurWindow, LPARAM(&window));
            if (!window)
                continue;
        }
        const std::wstring title = std::wstring(kTitle) + L" - " + std::to_wstring(fps) + L" FPS";
        SetWindowTextW(window, title.c_str());
    }
}
}

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
    // The command processor is PGR4's limit (one thread, ~2600 draws a frame; the game's Present waits for
    // it). Early fences let the title's frame overlap the host GPU, and draw workers translate the long
    // segments between resolves (the race scene: ~1500 draws) on three more threads, in four pieces.
    // 5800X St Petersburg race at 720p and 1440p alike: start line 116 -> 158 fps, opening straight
    // 111 -> 137; paused frames pixel for pixel the same. Workers without early fences are slower (the
    // CPU and GPU still take turns), so they go together. Not on CPUs under 6 threads; the workers stay
    // off with an upscaler on (RendererParallelReady). `regpuctl switch REGPU_NO_DRAW_WORKERS 1`: serial.
    if (!getenv("REGPU_EARLY_FENCES"))
        _putenv_s("REGPU_EARLY_FENCES", "1");
    if (!getenv("REGPU_DRAW_WORKERS") && std::thread::hardware_concurrency() >= 6)
        _putenv_s("REGPU_DRAW_WORKERS", "3");
    // The launcher's "Hide the shader compiler box" (the first-boot screen stays).
    if (s.hideShaderOverlay && !getenv("REGPU_NO_OVERLAY"))
        _putenv_s("REGPU_NO_OVERLAY", "1");
    c.upscaler = regpu::Config::Upscaler(uint32_t(s.upscaler));
    c.quality = s.quality;
    c.sharpness = s.sharpness;
    c.userDir = UserDir();
    c.windowTitle = kTitle;
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
    h.onSwap = [](uint64_t) {
        g_swaps.fetch_add(1, std::memory_order_relaxed);
    };
    std::thread(TitleLoop).detach();
    // The first boot: every shader in the executable, then the shipped pipeline list, behind PGR4's own screen.
    h.staticShaders = StaticShaders;
    h.staticShadersSaved = StaticShadersSaved;
    h.bootScreen = BootScreenCompose;
    h.pipelineSeed = [] { return kPipelineSeed; };
    if (getenv("PGR4_BOOTSCREEN_PREVIEW"))
        BootScreenPreview();
    regpu::Start();
    if (s.anisotropy >= 0)
        regpu::SetAnisotropy(s.anisotropy);
}
