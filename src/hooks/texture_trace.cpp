// PGR4_TRACE_LOCK=1: what the title's D3D texture calls return to the car damage table setup (sub_824629B0) (GetLevelDesc's size,
// LockRect's pitch and pointer), to see where CPU-filled textures end up.
#include <cstdlib>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

namespace
{
const bool g_trace = getenv("PGR4_TRACE_LOCK") != nullptr;
int g_logged = 0;
}

PPC_FUNC_IMPL(__imp__sub_82714DE0);
PPC_FUNC_IMPL(__imp__sub_826C8858);

// GetLevelDesc(texture, level, desc)
PPC_FUNC(sub_82714DE0)
{
    const uint32_t texture = ctx.r3.u32, level = ctx.r4.u32, desc = ctx.r5.u32, lr = uint32_t(ctx.lr);
    __imp__sub_82714DE0(ctx, base);
    if (g_trace && lr >= 0x824629B0 && lr < 0x82462C00 && g_logged < 200)
    {
        g_logged++;
        Log("d3d: GetLevelDesc tex %08X level %u (lr %08X): %08X %08X %08X %08X", texture, level, lr, LoadBE32(desc), LoadBE32(desc + 4),
            LoadBE32(desc + 8), LoadBE32(desc + 12));
    }
}

// LockRect(texture, level, locked, rect, flags)
PPC_FUNC(sub_826C8858)
{
    const uint32_t texture = ctx.r3.u32, level = ctx.r4.u32, locked = ctx.r5.u32, lr = uint32_t(ctx.lr);
    __imp__sub_826C8858(ctx, base);
    if (g_trace && lr >= 0x824629B0 && lr < 0x82462C00 && g_logged < 200)
    {
        g_logged++;
        Log("d3d: LockRect tex %08X level %u (lr %08X): pitch %u bits %08X; header %08X %08X %08X %08X %08X %08X %08X %08X", texture, level, lr,
            LoadBE32(locked), LoadBE32(locked + 4), LoadBE32(texture), LoadBE32(texture + 4), LoadBE32(texture + 8), LoadBE32(texture + 12),
            LoadBE32(texture + 16), LoadBE32(texture + 20), LoadBE32(texture + 24), LoadBE32(texture + 28));
    }
}
