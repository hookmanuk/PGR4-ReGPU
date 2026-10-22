// The XMA hardware's current context register (0x7FEA1818, XMA register 0606h): the context the
// decoder is processing, rotating on the console. The title's XMA playback library takes a
// context's modify lock by polling it (sub_82915170) until no context of its own is the one in
// progress. As plain memory it never changed, the poll timed out ~8 times a second, the library
// reset the XMA hardware and asserted (XMAPlaybackWaitUntilModifyLockObtained() failed in
// XMADecoder::GetData()), and voices were lost: the game was barely audible. Like Xenia, the
// register now rotates through the hardware context IDs on each poll (never with bit 200h).
#include <cstdint>
#include <cstdlib>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_82915170);
PPC_FUNC_IMPL(__imp__sub_82915028);

// PGR4_TRACE_XMA_LOCK=1: the lock requests (count, flags, contexts and their hardware IDs).
PPC_FUNC(sub_82915028)
{
    static const bool trace = getenv("PGR4_TRACE_XMA_LOCK") != nullptr;
    static int logged = 0;
    if (trace && logged < 40)
    {
        logged++;
        const uint32_t s = ctx.r3.u32, count = LoadBE32(s), flags = LoadBE32(s + 4), list = LoadBE32(s + 8);
        Log("xma lock: request %08X count %u flags %08X ids %03X %03X (lr %08llX)", s, count, flags, count ? (*GuestPtr<uint16_t>(list + 80) >> 8 | (*GuestPtr<uint16_t>(list + 80) & 0xFF) << 8) : 0,
            count > 1 ? (*GuestPtr<uint16_t>(list + 96 + 80) >> 8 | (*GuestPtr<uint16_t>(list + 96 + 80) & 0xFF) << 8) : 0, ctx.lr);
    }
    __imp__sub_82915028(ctx, base);
}
