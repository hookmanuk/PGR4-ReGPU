// The car's projected sun shadow (the 512x512 shadow map drawn onto the road, psCarShadow mode 0) never
// showed: only the drop-shadow blob under the car did. The game decides per car whether that shadow is in
// view in sub_82437230 (frustum test, from the update in sub_824DC5E8) and stores it at car+324; the
// renderer's own per-view reset (sub_82437158, from sub_824CBFB0 right before the shadow pass
// sub_824CC738) clears that byte again, so the pass (sub_82438418 -> sub_82419DB8) always read 0 and
// drew the blob. On the console and in Xenia the shadow is visible in the same scene, so the frustum
// result is meant to reach the render: the update's result is kept here and restored before the pass.
// PGR4_NO_CAR_SHADOW_FIX=1 (live switch) turns this off for A/B.
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

namespace
{
std::mutex g_mutex;
std::unordered_map<uint32_t, uint8_t> g_shadowInView; // car -> car+324 as the update left it
bool g_logged = false;
}

PPC_FUNC_IMPL(__imp__sub_82437230);
PPC_FUNC_IMPL(__imp__sub_82438418);

// sub_82437230(mgr, view, camera): the frustum test that sets car+324 for the cars at mgr+0.. (count mgr+40).
PPC_FUNC(sub_82437230)
{
    const uint32_t mgr = ctx.r3.u32;
    __imp__sub_82437230(ctx, base);
    static LiveSwitch off("PGR4_NO_CAR_SHADOW_FIX");
    if (off)
        return;
    const uint32_t n = LoadBE32(mgr + 40);
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < n && i < 16; i++)
        if (const uint32_t car = LoadBE32(mgr + i * 4))
            g_shadowInView[car] = *GuestPtr<uint8_t>(car + 324);
}

// sub_82438418(mgr, mode): the car shadow pass over the cars at mgr+44.. (count mgr+84).
PPC_FUNC(sub_82438418)
{
    static LiveSwitch off("PGR4_NO_CAR_SHADOW_FIX");
    if (!off)
    {
        const uint32_t mgr = ctx.r3.u32, n = LoadBE32(mgr + 84);
        std::lock_guard lock(g_mutex);
        for (uint32_t i = 0; i < n && i < 16; i++)
            if (const uint32_t car = LoadBE32(mgr + 44 + i * 4))
                if (auto it = g_shadowInView.find(car); it != g_shadowInView.end() && it->second)
                {
                    *GuestPtr<uint8_t>(car + 324) = 1;
                    if (!g_logged)
                    {
                        g_logged = true;
                        Log("car shadow: restoring the projected-shadow flag before the shadow pass (PGR4_NO_CAR_SHADOW_FIX=1 to disable)");
                    }
                }
    }
    __imp__sub_82438418(ctx, base);
}
