// PGR4_TRACE_MUSIC=1: the title's music scanners (sub_8269DFD0, sub_8269E668: they build the
// GAME:\game\audio\Music%d playlists and the custom soundtrack list), when they run and what
// they return.
#include <cstdlib>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

namespace
{
const bool g_trace = getenv("PGR4_TRACE_MUSIC") != nullptr;
}

PPC_FUNC_IMPL(__imp__sub_8269DFD0);
PPC_FUNC_IMPL(__imp__sub_8269E668);

PPC_FUNC(sub_8269DFD0)
{
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, lr = uint32_t(ctx.lr);
    if (g_trace)
    {
        // The function's gates: a global that must not be -1, the pack table's count, and the
        // per-track status bytes (table at 0x82C1E6F4, one byte per track, must be 1).
        const uint32_t gate1 = LoadBE32(0x82C88E50), count = LoadBE32(0x82BFDE4C);
        char status[40];
        for (int i = 0; i < 16; i++)
            snprintf(status + i * 2, 3, "%02X", *GuestPtr<uint8_t>(0x82C1E6F4 + i));
        Log("music: sub_8269DFD0 enter r3 %08X pack %u (lr %08X): gate1 %08X packs %u status %s", r3, r4, lr, gate1, count, status);
    }
    __imp__sub_8269DFD0(ctx, base);
    if (g_trace)
        Log("music: sub_8269DFD0 returns %08X", ctx.r3.u32);
}

PPC_FUNC(sub_8269E668)
{
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, lr = uint32_t(ctx.lr);
    if (g_trace)
        Log("music: sub_8269E668 enter r3 %08X r4 %08X (lr %08X)", r3, r4, lr);
    __imp__sub_8269E668(ctx, base);
    if (g_trace)
        Log("music: sub_8269E668 returns %08X", ctx.r3.u32);
}

// The callees of sub_8269DFD0 (formatting a path, then the file-system calls).
#include <cstring>
#include <string>

static std::string GuestStr(uint32_t a)
{
    if (!a)
        return "(null)";
    return std::string(GuestPtr<const char>(a), strnlen(GuestPtr<const char>(a), 120));
}

PPC_FUNC_IMPL(__imp__sub_828DA838);
PPC_FUNC_IMPL(__imp__sub_8231E2E8);
PPC_FUNC_IMPL(__imp__sub_82712EC8);
PPC_FUNC_IMPL(__imp__sub_8272F218);
PPC_FUNC_IMPL(__imp__sub_826A4898);

PPC_FUNC(sub_828DA838)
{
    if (g_trace)
        Log("music:   828DA838(r3 %08X '%s', r4 %08X '%s', r5 %08X '%s') lr %08X", ctx.r3.u32, GuestStr(ctx.r3.u32).c_str(), ctx.r4.u32,
            GuestStr(ctx.r4.u32).c_str(), ctx.r5.u32, GuestStr(ctx.r5.u32).c_str(), uint32_t(ctx.lr));
    __imp__sub_828DA838(ctx, base);
}

PPC_FUNC(sub_8231E2E8)
{
    if (g_trace)
        Log("music:   8231E2E8(r3 %08X '%s', r4 %08X) lr %08X", ctx.r3.u32, GuestStr(ctx.r3.u32).c_str(), ctx.r4.u32, uint32_t(ctx.lr));
    __imp__sub_8231E2E8(ctx, base);
}

PPC_FUNC(sub_82712EC8)
{
    if (g_trace)
        Log("music:   82712EC8(r3 %08X '%s', r4 %08X, r5 %08X) lr %08X", ctx.r3.u32, GuestStr(ctx.r3.u32).c_str(), ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
    __imp__sub_82712EC8(ctx, base);
}

PPC_FUNC(sub_8272F218)
{
    if (g_trace)
        Log("music:   8272F218(r3 %08X '%s', r4 %08X, r5 %08X) lr %08X", ctx.r3.u32, GuestStr(ctx.r3.u32).c_str(), ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
    __imp__sub_8272F218(ctx, base);
}

PPC_FUNC(sub_826A4898)
{
    if (g_trace)
        Log("music:   826A4898(r3 %08X, r4 %08X) lr %08X", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
    __imp__sub_826A4898(ctx, base);
}
