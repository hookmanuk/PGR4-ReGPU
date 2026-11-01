// PGR4_TRACE_XMA_CALLS=1: how often the title's XMA decoder path runs (every 500th call logged),
// to see where it stops: sub_82806450 (XMADecoder::GetData) -> sub_82805820 (its context update).
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

namespace
{
const bool g_trace = getenv("PGR4_TRACE_XMA_CALLS") != nullptr;
void Count(const char* name, std::atomic<uint32_t>& n)
{
    const uint32_t k = ++n;
    if (g_trace && (k == 1 || k % 500 == 0))
        Log("xma calls: %s x%u", name, k);
}
std::atomic<uint32_t> g_getData{ 0 }, g_update{ 0 };
}

PPC_FUNC_IMPL(__imp__sub_82806450);
PPC_FUNC_IMPL(__imp__sub_82805820);

// PGR4_TRACE_XMA_STARVE=1: once a second, how many of the voices' updates (XMADecoder::GetData) ran out
// of decoded data and filled the rest with silence (it sets its byte at +115 then, loc_8280676C).
PPC_FUNC(sub_82806450)
{
    Count("GetData (82806450)", g_getData);
    static const bool traceStarve = getenv("PGR4_TRACE_XMA_STARVE") != nullptr;
    const uint32_t self = ctx.r3.u32;
    const uint8_t before = traceStarve ? *GuestPtr<uint8_t>(self + 115) : 0;
    __imp__sub_82806450(ctx, base);
    if (traceStarve)
    {
        static std::atomic<uint32_t> calls{ 0 }, starved{ 0 };
        static std::atomic<int64_t> last{ 0 };
        calls++;
        if (!before && *GuestPtr<uint8_t>(self + 115))
        {
            starved++;
            // each one: the voice, its XMA playback (+80) and that playback's contexts (+8)
            const uint32_t playback = LoadBE32(self + 80);
            Log("xma starved voice %08X playback %08X contexts %08X", self, playback, playback ? LoadBE32(playback + 8) : 0);
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (int64_t t = last.load(); now - t >= 1000 && last.compare_exchange_strong(t, now))
            Log("xma starve: %u of %u voice updates ran out of data", starved.exchange(0), calls.exchange(0));
    }
}

PPC_FUNC(sub_82805820)
{
    Count("update (82805820)", g_update);
    __imp__sub_82805820(ctx, base);
}
