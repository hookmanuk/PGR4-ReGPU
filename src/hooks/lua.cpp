// The title's Lua 5.1 runtime: log script errors as they are thrown.
// luaD_throw(L, errcode) has the message on the stack top for runtime
// errors. Layout (32-bit big-endian): L->top at +8, TValue 16 bytes with
// the type tag at +8, TString characters at +16.
#include <cstring>
#include <string>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_82510260);

// luaD_throw
PPC_FUNC(sub_82510260)
{
    const uint32_t L = ctx.r3.u32, errcode = ctx.r4.u32;
    const uint32_t top = __builtin_bswap32(*GuestPtr<uint32_t>(L + 8));
    const uint32_t tag = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16 + 8));
    if (tag == 4) // LUA_TSTRING
    {
        const uint32_t str = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16));
        const uint32_t len = __builtin_bswap32(*GuestPtr<uint32_t>(str + 12));
        Log("lua: error %u: %.*s", errcode, int(len < 1024 ? len : 1024), GuestPtr<const char>(str + 16));
    }
    else
        Log("lua: error %u (value type %u)", errcode, tag);
    __imp__sub_82510260(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82506700);

// lua_getfield(L, index, k): pushes t[k]. PGR4_TRACE_LUA_NIL=1 logs each lookup that finds nil (a
// script function or table the engine expected and the scripts didn't define).
PPC_FUNC(sub_82506700)
{
    const uint32_t L = ctx.r3.u32, index = ctx.r4.u32, key = ctx.r5.u32;
    std::string name = key ? GuestPtr<const char>(key) : "";
    __imp__sub_82506700(ctx, base);
    static const bool trace = getenv("PGR4_TRACE_LUA_NIL") != nullptr;
    if (!trace)
        return;
    const uint32_t top = __builtin_bswap32(*GuestPtr<uint32_t>(L + 8));
    const uint32_t tag = __builtin_bswap32(*GuestPtr<uint32_t>(top - 16 + 8));
    if (tag == 0) // LUA_TNIL
        Log("lua: getfield(%d, '%s') is nil", int32_t(index), name.c_str());
}

PPC_FUNC_IMPL(__imp__sub_82508568);

// Runs a named script from the loaded packages (UIEnums.lua, UISetup.lua, ...); the caller ignores failure.
PPC_FUNC(sub_82508568)
{
    std::string name = ctx.r4.u32 ? GuestPtr<const char>(ctx.r4.u32) : "";
    __imp__sub_82508568(ctx, base);
    static const bool trace = getenv("PGR4_TRACE_LUA_NIL") != nullptr;
    if (trace)
        Log("lua: run '%s' -> %d", name.c_str(), int32_t(ctx.r3.u32));
}

PPC_FUNC_IMPL(__imp__sub_82883CC0);

// Finds a script by name in the loaded script table (count at +27536, 16-byte entries at +60, names at
// +64 + entry[0]); sets the current index at +27540 only when it matches.
PPC_FUNC(sub_82883CC0)
{
    const uint32_t mgr = ctx.r3.u32;
    std::string name = ctx.r4.u32 ? GuestPtr<const char>(ctx.r4.u32) : "";
    static const bool trace = getenv("PGR4_TRACE_LUA_NIL") != nullptr;
    if (trace)
        *GuestPtr<uint32_t>(mgr + 27540) = 0xFFFFFFFF; // so a miss logs -1
    __imp__sub_82883CC0(ctx, base);
    if (trace)
        Log("lua: find script '%s' in %d -> %d", name.c_str(), int32_t(__builtin_bswap32(*GuestPtr<uint32_t>(mgr + 27536))),
            int32_t(__builtin_bswap32(*GuestPtr<uint32_t>(mgr + 27540))));
}
