// The PGR Shop froze on the Lotus Esprit Essex (reports #7, #12). sub_822C55B0(grid, position, radius) returns
// the highest hit of the grid cells around a position (sub_822C7820 per cell's entry), starting from the
// constant at 0x820011B0 when nothing is found. For that car its caller (sub_8243D948) passes a position of
// ~1e37: the cell bounds saturate to INT_MAX and the loop counter wraps around past it, so it never ends.
// Bounds that far outside any grid find nothing: that answer is returned without the loop.
// PGR4_NO_GRID_GUARD=1: as the game does (A/B).
#include <cmath>
#include <cstring>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"

PPC_FUNC_IMPL(__imp__sub_822C55B0);

PPC_FUNC(sub_822C55B0)
{
    static LiveSwitch off("PGR4_NO_GRID_GUARD");
    const uint32_t obj = ctx.r3.u32;
    const uint32_t grid = PPC_LOAD_U32(obj);
    if (grid && !off)
    {
        auto f = [&](uint32_t a) { uint32_t u = PPC_LOAD_U32(a); float v; memcpy(&v, &u, 4); return v; };
        const uint32_t pa = ctx.r4.u32 & ~15u, oa = (obj + 16) & ~15u;
        const double s = f(grid + 24), r = ctx.f1.f64;
        const double x = double(f(pa)) - f(oa), z = double(f(pa + 8)) - f(oa + 8);
        constexpr double kFar = 1 << 24; // cells: far beyond any track's grid, well inside int range
        if (!(std::fabs((std::fabs(x) + std::fabs(r)) * s) < kFar && std::fabs((std::fabs(z) + std::fabs(r)) * s) < kFar))
        {
            static int logged = 0;
            if (logged++ < 4)
                Log("grid: position (%g, %g) off the grid: no hit (lr %08llX)", x, z, (unsigned long long)ctx.lr);
            const uint32_t none = PPC_LOAD_U32(0x820011B0);
            float v;
            memcpy(&v, &none, 4);
            ctx.f1.f64 = v;
            return;
        }
    }
    __imp__sub_822C55B0(ctx, base);
}
