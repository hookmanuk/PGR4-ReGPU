// The title's Lua 5.1 runtime: log script errors as they are thrown.
// luaD_throw(L, errcode) has the message on the stack top for runtime
// errors. Layout (32-bit big-endian): L->top at +8, TValue 16 bytes with
// the type tag at +8, TString characters at +16.
#include <windows.h>
#include <cstring>
#include <string>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

// hooks/graphics_menu.cpp
std::string GraphicsMenuPatch();
bool GraphicsMenuCommand(const char* text, size_t len);

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
        if (!GraphicsMenuCommand(GuestPtr<const char>(str + 16), len))
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

static std::string g_runningScript;

static bool EndsWith(const std::string& s, const char* tail)
{
    const size_t n = strlen(tail);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, tail) == 0;
}

PPC_EXTERN_FUNC(sub_825081B8); // luaL_loadstring(L, s)
PPC_EXTERN_FUNC(sub_82506E10); // lua_pcall(L, nargs, nresults, errfunc)
PPC_EXTERN_FUNC(sub_82508400); // the loader's error report (L, status): logs and pops

// Runs Lua source in the game's state (after a screen script, to change it). PGR4_LUA_PATCH=path runs
// that file instead (trying changes without a rebuild).
static void RunLua(PPCContext& ctx, uint8_t* base, uint32_t L, const char* source, const char* what)
{
    std::string text = source;
    if (const char* path = getenv("PGR4_LUA_PATCH"))
        if (FILE* f = fopen(path, "rb"))
        {
            text.clear();
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
                text.append(buf, n);
            fclose(f);
        }
    const uint32_t guest = GuestAlloc(Heap::Virtual, uint32_t(text.size() + 1), 16);
    if (!guest)
        return;
    memcpy(GuestPtr<char>(guest), text.c_str(), text.size() + 1);
    const PPCContext saved = ctx;
    ctx.r3.u64 = L;
    ctx.r4.u64 = guest;
    sub_825081B8(ctx, base);
    uint32_t status = ctx.r3.u32;
    if (status == 0)
    {
        ctx.r3.u64 = L; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        sub_82506E10(ctx, base);
        status = ctx.r3.u32;
    }
    if (status != 0)
    {
        Log("lua: %s failed (%u)", what, status);
        ctx.r3.u64 = L; ctx.r4.u64 = status;
        sub_82508400(ctx, base);
    }
    ctx = saved;
    GuestFree(guest);
}

// Rewrites a UTF-16BE string in a loaded strings file in place (the new text no longer than the old).
static void ReplaceText(uint8_t* data, uint32_t size, const char* from, const char* to)
{
    const size_t n = strlen(from);
    for (uint32_t i = 0; i + n * 2 + 2 <= size; i += 2)
    {
        bool match = data[i + n * 2] == 0 && data[i + n * 2 + 1] == 0;
        for (size_t k = 0; match && k < n; k++)
            match = data[i + k * 2] == 0 && data[i + k * 2 + 1] == uint8_t(from[k]);
        if (!match)
            continue;
        const size_t m = strlen(to);
        for (size_t k = 0; k < n; k++)
        {
            data[i + k * 2] = 0;
            data[i + k * 2 + 1] = k < m ? uint8_t(to[k]) : 0;
        }
        return;
    }
}

// The English UI strings: the demo's unused "exit" texts become the Quit item's (kMainMenuPatch). Other
// languages keep their own translation of them.
void PatchUiStrings(uint8_t* data, uint32_t size)
{
    ReplaceText(data, size, "EXIT DEMO", "QUIT GAME");
    ReplaceText(data, size, "Quit the PGR 4 Demo", "Close PGR 4");
    // Options > Graphics is the PC's resolution and anti-aliasing (graphics_menu.cpp).
    ReplaceText(data, size, "Adjust your TV settings to optimize PGR 4's graphical look", "Resolution, anti-aliasing and display mode");
}

// The main menu's Quit item: SetNextScreen("Quit") is caught by the script lookup below and closes the game.
static const char kMainMenuPatch[] = R"lua(
local quitId = 9
local init = Init
function Init()
    init()
    UIButtons.AddItem(GUI.menu_ID, quitId, UIText.IDS_EXIT_DEMO, false)
    UIButtons.AddItemHelp(GUI.menu_ID, UIText.IDS_EXIT_DEMO_HELP)
    GUI.sticker_types[quitId] = UIEnums.Stickers.OPTIONS
    RestoreMenuSelection(GUI.menu_ID)
end
local update = MessageUpdate
function MessageUpdate(msg, a, b, c, d)
    if GUI.finished ~= true and msg == "BUTTON_NEXT" and UIButtons.GetSelection(GUI.menu_ID) == quitId then
        PlayMenuNext()
        UIScreen.SetNextScreen("PGR4Quit")
        return false
    end
    return update(msg, a, b, c, d)
end
)lua";

PPC_FUNC_IMPL(__imp__sub_82508308);

// The script loader's lua_Reader (L, data, size*): PGR4_DUMP_LUA=1 saves each chunk it hands Lua to
// out/lua/<script name>.luac (reading the game's menu scripts).
PPC_FUNC(sub_82508308)
{
    const uint32_t sizePtr = ctx.r5.u32;
    __imp__sub_82508308(ctx, base);
    static const bool dump = getenv("PGR4_DUMP_LUA") != nullptr;
    const uint32_t size = sizePtr ? __builtin_bswap32(*GuestPtr<uint32_t>(sizePtr)) : 0;
    if (!dump || !ctx.r3.u32 || !size)
        return;
    std::string name = g_runningScript.empty() ? "unnamed" : g_runningScript;
    for (char& c : name)
        if (c == '\\' || c == '/' || c == ':')
            c = '_';
    CreateDirectoryA("out", nullptr);
    CreateDirectoryA("out\\lua", nullptr);
    if (FILE* f = fopen(("out\\lua\\" + name + "c").c_str(), "ab"))
    {
        fwrite(GuestPtr<const char>(ctx.r3.u32), 1, size, f);
        fclose(f);
    }
}

PPC_FUNC_IMPL(__imp__sub_82508568);

// Runs a named script from the loaded packages (UIEnums.lua, UISetup.lua, ...); the caller ignores failure.
PPC_FUNC(sub_82508568)
{
    std::string name = ctx.r4.u32 ? GuestPtr<const char>(ctx.r4.u32) : "";
    const uint32_t L = ctx.r3.u32;
    g_runningScript = name;
    __imp__sub_82508568(ctx, base);
    g_runningScript.clear();
    if (ctx.r3.u32 == 0 && EndsWith(name, "Intro\\MainMenu.lua"))
        RunLua(ctx, base, L, kMainMenuPatch, "MainMenu patch");
    else if (ctx.r3.u32 == 0 && EndsWith(name, "Options\\Graphics.lua"))
        RunLua(ctx, base, L, GraphicsMenuPatch().c_str(), "Graphics patch");
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
    if (EndsWith(name, "PGR4Quit")) // the main menu's Quit item (kMainMenuPatch)
    {
        Log("ui: Quit chosen on the main menu");
        fflush(nullptr);
        ExitProcess(0);
    }
    if (trace)
        *GuestPtr<uint32_t>(mgr + 27540) = 0xFFFFFFFF; // so a miss logs -1
    __imp__sub_82883CC0(ctx, base);
    if (trace)
        Log("lua: find script '%s' in %d -> %d", name.c_str(), int32_t(__builtin_bswap32(*GuestPtr<uint32_t>(mgr + 27536))),
            int32_t(__builtin_bswap32(*GuestPtr<uint32_t>(mgr + 27540))));
}
