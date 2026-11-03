// The garage's walking camera moves through this collision sweep, which cuts a move into steps of 1/20 m and rounds
// the count down: a move under 5 cm is no move. Walking is 2.24 m/s, 7 cm a frame at the console's 30 fps; above
// 45 fps every frame's move is dropped and the player stands still. Small moves are saved up until they make a step.
#include <cmath>
#include <cstring>

#include "kernel/imports.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_824BB618);

static float LoadFloat(uint32_t address)
{
    const uint32_t v = __builtin_bswap32(*GuestPtr<uint32_t>(address));
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void StoreFloat(uint32_t address, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    *GuestPtr<uint32_t>(address) = __builtin_bswap32(v);
}

// sub_824BB618(position in/out r3, move r4, ...)
PPC_FUNC(sub_824BB618)
{
    static float saved[3];
    const uint32_t move = ctx.r4.u32;
    const float total[3] = { LoadFloat(move) + saved[0], LoadFloat(move + 4) + saved[1], LoadFloat(move + 8) + saved[2] };
    if (std::sqrt(total[0] * total[0] + total[1] * total[1] + total[2] * total[2]) * 20.0f < 1.0f)
    {
        memcpy(saved, total, sizeof(saved));
        return;
    }
    memset(saved, 0, sizeof(saved));
    for (int i = 0; i < 3; i++)
        StoreFloat(move + i * 4, total[i]);
    __imp__sub_824BB618(ctx, base);
}
