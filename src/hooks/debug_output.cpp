// The title's debug output (its OutputDebugString): asserts print their message through it
// before DbgBreakPoint, so without this they show only as "DbgBreakPoint (lr ...)".
#include <cstring>
#include <string>
#include <unordered_map>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_826B0FE0);

// The first few of each message, then every 1000th (some fire every frame).
PPC_FUNC(sub_826B0FE0)
{
    const uint32_t text = ctx.r3.u32;
    if (text)
    {
        std::string s(GuestPtr<const char>(text), strnlen(GuestPtr<const char>(text), 512));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        static std::unordered_map<std::string, uint32_t> seen;
        const uint32_t n = seen[s]++;
        if (n < 3 || n % 1000 == 0)
            Log("game: %s (lr %08X, %u times)", s.c_str(), uint32_t(ctx.lr), n + 1);
        std::string CurrentHostStack(int maxFrames);
        if (n == 0)
            Log("game:   from %s", CurrentHostStack(12).c_str());
    }
    __imp__sub_826B0FE0(ctx, base);
}
