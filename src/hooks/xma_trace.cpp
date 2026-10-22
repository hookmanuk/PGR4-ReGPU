// PGR4_TRACE_XMA_CALLS=1: how often the title's XMA decoder path runs (every 500th call logged),
// to see where it stops: sub_82806450 (XMADecoder::GetData) -> sub_82805820 (its context update).
#include <atomic>
#include <cstdlib>

#include "kernel/imports.h"
#include "log.h"

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

PPC_FUNC(sub_82806450)
{
    Count("GetData (82806450)", g_getData);
    __imp__sub_82806450(ctx, base);
}

PPC_FUNC(sub_82805820)
{
    Count("update (82805820)", g_update);
    __imp__sub_82805820(ctx, base);
}
