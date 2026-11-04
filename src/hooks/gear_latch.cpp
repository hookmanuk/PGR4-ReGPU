// A gear press lasts one frame in the car's controls (bytes 16 and 17 of the control block, rewritten every frame by
// sub_822DC420), but the gearbox (sub_822DE628) only looks in the 60 Hz physics step. On the console's 30 fps every
// frame had a step; above 60 fps the press is often gone before the next one (40 taps: 40 shifts at 60 fps, 31 at 120,
// 19 at 130). The press is held until a step takes it, or for 50 ms (3 steps): one made while the physics is stopped
// is dropped. Both functions run on the physics thread. PGR4_NO_GEAR_LATCH=1: off.
#include <chrono>
#include <cstdlib>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_822DC420);
PPC_FUNC_IMPL(__imp__sub_822DE628);

namespace
{
struct HeldPress
{
    uint32_t controls = 0;
    uint8_t up = 0, down = 0;
    std::chrono::steady_clock::time_point since;
};
HeldPress g_held[4];

LiveSwitch& NoGearLatch()
{
    static LiveSwitch s("PGR4_NO_GEAR_LATCH");
    return s;
}
}

// sub_822DC420(control block r3, pad r4)
PPC_FUNC(sub_822DC420)
{
    const uint32_t controls = ctx.r3.u32, pad = ctx.r4.u32 & 0xFF;
    __imp__sub_822DC420(ctx, base);
    if (pad >= 4 || NoGearLatch())
        return;
    HeldPress& held = g_held[pad];
    uint8_t* const up = GuestPtr<uint8_t>(controls + 16);
    uint8_t* const down = GuestPtr<uint8_t>(controls + 17);
    const auto now = std::chrono::steady_clock::now();
    if (*up || *down)
    {
        held = { controls, *up, *down, now };
        return;
    }
    if (held.controls != controls || (!held.up && !held.down))
        return;
    if (now - held.since > std::chrono::milliseconds(50))
    {
        held.up = held.down = 0;
        return;
    }
    *up = held.up;
    *down = held.down;
}

// sub_822DE628(gearbox r3): +36 the car, whose +808 is its control block; +29 set when a press was already taken since
// the last frame (two steps in one frame), and that step ignores the press.
PPC_FUNC(sub_822DE628)
{
    static const bool trace = getenv("PGR4_TRACE_SHIFT") != nullptr;
    const uint32_t gearbox = ctx.r3.u32;
    const uint32_t car = __builtin_bswap32(*GuestPtr<uint32_t>(gearbox + 36));
    const uint32_t controls = __builtin_bswap32(*GuestPtr<uint32_t>(car + 808));
    const bool pressed = *GuestPtr<uint8_t>(controls + 16) || *GuestPtr<uint8_t>(controls + 17);
    const bool taken = pressed && !*GuestPtr<uint8_t>(gearbox + 29);
    const int gear = *GuestPtr<int8_t>(gearbox + 30);
    __imp__sub_822DE628(ctx, base);
    if (!taken)
        return;
    if (trace)
        Log("gear: press taken, gear %d -> %d", gear, *GuestPtr<int8_t>(gearbox + 30));
    for (HeldPress& held : g_held)
        if (held.controls == controls)
            held.up = held.down = 0;
}
