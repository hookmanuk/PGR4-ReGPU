// With fences that land after the host GPU (REGPU_TRUTHFUL_FENCES),
// the title's Present would wait for the GPU every frame through its block-on-fence. Only that one call is skipped:
// the frame may overlap the GPU (the command processor's frame slots still hold it back), and every other wait,
// the ones that guard memory about to be freed, sees the truth.
#include <cstdlib>

#include "kernel/imports.h"

PPC_FUNC_IMPL(__imp__sub_826CE0E8);

PPC_FUNC(sub_826CE0E8)
{
    static const bool truthful = getenv("REGPU_TRUTHFUL_FENCES") != nullptr;
    constexpr uint32_t kPresentReturn = 0x826CB664; // after the call in sub_826CB148 (Present)
    if (truthful && ctx.lr == kPresentReturn)
        return;
    __imp__sub_826CE0E8(ctx, base);
}
