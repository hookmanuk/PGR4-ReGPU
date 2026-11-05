// XAM: users, profiles, system UI, content packages, input, notifications.
// The port runs headless: one signed-in local profile, every system dialog
// answered with its default.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "input/wheel.h"
#include "kernel/imports.h"
#include "kernel/wheel_ff.h"
#include "kernel/runtime.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "live_switch.h"
#include "loader.h"
#include "settings.h"
#include "regpu/regpu.h"

#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

namespace
{
// The profile's offline XUID (0xE000...). The saves are signed for their owner, so it is kept in
// the profile folder (profiles\<name>\xuid): a copied, renamed or imported profile stays valid.
// A profile without the file gets the one derived from its name (what every profile had before).
uint64_t Xuid()
{
    static const uint64_t xuid = [] {
        const auto file = ContentDir().parent_path() / "xuid";
        if (FILE* f = _wfopen(file.c_str(), L"r"))
        {
            unsigned long long v = 0;
            const bool ok = fscanf(f, "%llx", &v) == 1 && (v >> 60) == 0xE;
            fclose(f);
            if (ok)
                return uint64_t(v);
        }
        uint64_t h = 0xCBF29CE484222325ull;
        for (char c : GetSettings().profile)
            h = (h ^ uint8_t(c)) * 0x100000001B3ull;
        const uint64_t derived = 0xE000000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
        if (FILE* f = _wfopen(file.c_str(), L"w"))
        {
            fprintf(f, "%016llX\n", (unsigned long long)derived);
            fclose(f);
        }
        return derived;
    }();
    return xuid;
}

constexpr uint32_t kContentDataSize = 0x134;


// XCONTENT_DATA: deviceId, contentType, displayName[128] (UTF-16 BE), fileName[42].
struct ContentItem
{
    uint32_t type = 0;
    std::u16string displayName;
    std::string fileName;
};

std::filesystem::path ContentTypeDir(uint32_t type)
{
    char t[16];
    snprintf(t, sizeof(t), "%08X", type);
    return ContentDir() / t;
}

// Display names live beside the packages (<fileName>.title, UTF-16 LE), outside what the game sees.
std::filesystem::path TitlePath(uint32_t type, const std::string& fileName) { return ContentTypeDir(type) / (fileName + ".title"); }

struct ListenerObject : KernelObject
{
    ListenerObject() : KernelObject(Kind::Other) {}
    uint64_t mask = 0;
    std::deque<std::pair<uint32_t, uint32_t>> queue; // id, param
};

struct EnumeratorObject : KernelObject
{
    EnumeratorObject() : KernelObject(Kind::Other) {}
    std::vector<ContentItem> items;
    size_t next = 0;
};

std::mutex g_listenerMutex;
}

// Users ------------------------------------------------------------------------

GUEST_EXPORT(XamUserGetSigninState)
{
    Return(ctx, ctx.r3.u32 == 0 ? 1 : 0); // signed in locally
}

GUEST_EXPORT(XamUserGetXUID)
{
    // (user, type, xuid*)
    if (ctx.r3.u32 != 0)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    StoreBE64(ctx.r5.u32, Xuid());
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserGetName)
{
    // (user, buffer, size)
    if (ctx.r3.u32 != 0)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    const char* name = GetSettings().profile.c_str();
    const uint32_t size = ctx.r5.u32;
    if (size)
    {
        strncpy(GuestPtr<char>(ctx.r4.u32), name, size);
        GuestPtr<char>(ctx.r4.u32)[size - 1] = 0;
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserGetSigninInfo)
{
    // (user, flags, X_USER_SIGNIN_INFO*): { u64 xuid; u32 ?; u32 signinState; u32 ?; u32 ?; char name[16] }
    const uint32_t info = ctx.r5.u32;
    if (ctx.r3.u32 != 0 || !info)
    {
        Return(ctx, X_ERROR_NO_SUCH_USER);
        return;
    }
    memset(GuestPtr(info), 0, 0x28);
    StoreBE64(info, Xuid());
    StoreBE32(info + 0x0C, 1); // signed in locally
    strncpy(GuestPtr<char>(info + 0x18), GetSettings().profile.c_str(), 15);
    Return(ctx, X_ERROR_SUCCESS);
}

// Where the system's notification pop-ups appear: nothing shows them here.
GUEST_EXPORT(XNotifyPositionUI) { Return(ctx, 0); }

GUEST_EXPORT(XamUserCheckPrivilege)
{
    // (user, mask, result*)
    StoreIfNonNull(ctx.r5.u32, 1);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserAreUsersFriends)
{
    Return(ctx, X_ERROR_NO_SUCH_USER);
}

GUEST_EXPORT(XamUserReadProfileSettings)
{
    // (titleId, user, xuidCount, xuids, settingCount, settingIds*, bufferSize*, buffer, overlapped)
    // Answer every setting as "not set": the title then uses its defaults.
    const uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, sizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    const uint32_t overlapped = StackArg(ctx, 8);
    // XUSER_READ_PROFILE_SETTING_RESULT { u32 count, u32 settings* } then
    // XUSER_PROFILE_SETTING { u32 source, u32 user, u64 xuid? ..., u32 id, XUSER_DATA (0x10) } of 0x28 bytes.
    const uint32_t needed = 8 + settingCount * 0x28;
    if (!buffer || LoadBE32(sizePtr) < needed)
    {
        StoreBE32(sizePtr, needed);
        Return(ctx, 122); // ERROR_INSUFFICIENT_BUFFER
        return;
    }
    memset(GuestPtr(buffer), 0, needed);
    StoreBE32(buffer, settingCount);
    StoreBE32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        const uint32_t s = buffer + 8 + i * 0x28;
        StoreBE32(s + 0x00, 0); // source: not set (default)
        StoreBE32(s + 0x04, ctx.r4.u32);
        StoreBE32(s + 0x10, LoadBE32(settingIds + i * 4));
    }
    if (overlapped)
    {
        CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
        Return(ctx, X_ERROR_IO_PENDING);
        return;
    }
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamUserCreateStatsEnumerator)
{
    Return(ctx, X_ERROR_FUNCTION_FAILED);
}

// System ------------------------------------------------------------------------

GUEST_EXPORT(XGetAVPack) { Return(ctx, 6); }        // HDMI
GUEST_EXPORT(XGetGameRegion) { Return(ctx, 0xFFFF); }
GUEST_EXPORT(XGetLanguage) { Return(ctx, GetSettings().language); } // settings.ini language
GUEST_EXPORT(XamGetSystemVersion) { Return(ctx, 0x20004B07); }
GUEST_EXPORT(XNetLogonGetTitleID) { Return(ctx, g_title.titleId); }

GUEST_EXPORT(XamGetExecutionId)
{
    StoreIfNonNull(ctx.r3.u32, g_title.executionId);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XamAlloc)
{
    // (flags, size, out*)
    const uint32_t address = GuestAlloc(Heap::Virtual, ctx.r4.u32);
    StoreIfNonNull(ctx.r5.u32, address);
    Return(ctx, address ? X_ERROR_SUCCESS : 8);
}

GUEST_EXPORT(XamFree)
{
    GuestFree(ctx.r3.u32);
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamLoaderGetLaunchDataSize)
{
    StoreIfNonNull(ctx.r3.u32, 0);
    Return(ctx, X_ERROR_NOT_FOUND);
}

GUEST_EXPORT(XamLoaderGetLaunchData)
{
    Return(ctx, X_ERROR_NOT_FOUND);
}

GUEST_EXPORT(XamLoaderSetLaunchData)
{
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamLoaderLaunchTitle)
{
    Log("XamLoaderLaunchTitle(\"%s\"): exiting", GuestString(ctx.r3.u32).c_str());
    std::_Exit(0);
}

GUEST_EXPORT(XamLoaderTerminateTitle)
{
    Log("XamLoaderTerminateTitle: exiting");
    std::_Exit(0);
}

// Notifications -----------------------------------------------------------------

namespace
{
std::vector<std::weak_ptr<ListenerObject>> g_listeners;
}

// Queues a notification for every listener whose areas mask takes it (area = id bits 25-30, as Xenia).
void BroadcastNotification(uint32_t id, uint32_t data)
{
    std::lock_guard lock(g_listenerMutex);
    for (auto& weak : g_listeners)
        if (auto l = weak.lock(); l && (l->mask & (1ull << ((id >> 25) & 0x3F))))
            l->queue.push_back({ id, data });
}

GUEST_EXPORT(XamNotifyCreateListener)
{
    // (areas mask (u64), unknown)
    auto listener = std::make_shared<ListenerObject>();
    listener->mask = ctx.r3.u64;
    // Startup state as a fresh console reports it.
    listener->queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED: user 0
    listener->queue.push_back({ 0x00000009, 0 }); // XN_SYS_UI: hidden
    {
        std::lock_guard lock(g_listenerMutex);
        g_listeners.push_back(listener);
    }
    Return(ctx, CreateHandle(listener));
}

GUEST_EXPORT(XNotifyGetNext)
{
    // (handle, matchId, id*, param*)
    auto object = GetHandle(ctx.r3.u32);
    if (!object)
    {
        Return(ctx, 0);
        return;
    }
    auto& listener = static_cast<ListenerObject&>(*object);
    std::lock_guard lock(g_listenerMutex);
    const uint32_t match = ctx.r4.u32;
    for (auto it = listener.queue.begin(); it != listener.queue.end(); ++it)
    {
        if (match && it->first != match)
            continue;
        StoreIfNonNull(ctx.r5.u32, it->first);
        StoreIfNonNull(ctx.r6.u32, it->second);
        listener.queue.erase(it);
        Return(ctx, 1);
        return;
    }
    Return(ctx, 0);
}

// System UI: answered at once ------------------------------------------------------

namespace
{
void FinishUi(PPCContext& ctx, uint32_t overlapped, uint32_t result = X_ERROR_SUCCESS)
{
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result);
        Return(ctx, X_ERROR_IO_PENDING);
    }
    else
        Return(ctx, result);
}
}

GUEST_EXPORT(XamShowSigninUI)
{
    // The sign-in guide opens and closes at once, with the profile signed in.
    std::lock_guard lock(g_listenerMutex);
    for (auto& weak : g_listeners)
        if (auto l = weak.lock())
        {
            l->queue.push_back({ 0x00000009, 1 }); // XN_SYS_UI: shown
            l->queue.push_back({ 0x0000000A, 1 }); // XN_SYS_SIGNINCHANGED: user 0
            l->queue.push_back({ 0x00000009, 0 }); // XN_SYS_UI: hidden
        }
    Return(ctx, X_ERROR_SUCCESS);
}
GUEST_EXPORT(XamShowFriendsUI) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowGamerCardUIForXUID) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamShowDirtyDiscErrorUI)
{
    Fatal("the title reported a disc read error (XamShowDirtyDiscErrorUI): check the game files");
}

GUEST_EXPORT(XamShowDeviceSelectorUI)
{
    // (user, contentType, flags, totalRequested (u64, one register), deviceId*, overlapped)
    StoreIfNonNull(ctx.r7.u32, 1); // the hard drive
    FinishUi(ctx, ctx.r8.u32);
}

GUEST_EXPORT(XamShowMessageBoxUI)
{
    // (user, title, text, buttonCount, buttons, focus, flags, result* (r10), overlapped (stack))
    auto wide = [](uint32_t p) {
        std::string out;
        for (uint32_t i = 0; p && i < 512; i++)
        {
            const uint16_t c = __builtin_bswap16(GuestPtr<uint16_t>(p)[i]);
            if (!c)
                break;
            out += c < 0x80 ? char(c) : '?';
        }
        return out;
    };
    std::string text = wide(ctx.r4.u32) + " | " + wide(ctx.r5.u32);
    for (uint32_t b = 0; b < ctx.r6.u32 && b < 4; b++)
        text += " [" + wide(LoadBE32(ctx.r7.u32 + b * 4)) + "]";
    Log("message box: %s", text.c_str());
    StoreIfNonNull(ctx.r10.u32, 0); // result: first button
    FinishUi(ctx, StackArg(ctx, 8));
}

GUEST_EXPORT(XamShowMessageBoxUIEx)
{
    StoreIfNonNull(StackArg(ctx, 8), 0);
    FinishUi(ctx, StackArg(ctx, 9));
}

GUEST_EXPORT(XamShowKeyboardUI)
{
    // (user, flags, default text, title, description, result buffer, max length, overlapped)
    const uint32_t def = ctx.r5.u32, out = ctx.r8.u32, max = ctx.r9.u32;
    if (out && max)
    {
        uint32_t i = 0;
        for (; def && i + 1 < max; i++)
        {
            const uint16_t c = GuestPtr<uint16_t>(def)[i];
            if (!c)
                break;
            GuestPtr<uint16_t>(out)[i] = c;
        }
        GuestPtr<uint16_t>(out)[i] = 0;
    }
    FinishUi(ctx, ctx.r10.u32);
}

// Input ----------------------------------------------------------------------------------

// Guest users 0-3 are host XInput pads 0-3. User 0 also takes the keyboard
// (merged with pad 0 if one is connected), so the game is playable without
// a controller:
//   W / S        throttle / brake (right / left trigger)
//   A / D        steer (left stick)
//   arrow keys   d-pad (menus)      I/J/K/L   right stick (camera)
//   Enter        A (select)         Backspace B (back)
//   Space        X (handbrake)      Y         Y
//   Q / E        left / right shoulder (gear down / up)
//   Tab          Back (view)        Escape    Start (pause)
// PGR4_AUTOSTART taps START + A every 3 s for unattended test runs.
namespace
{
struct PadState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

bool Key(int vk)
{
    return GetAsyncKeyState(vk) & 0x8000;
}

// Keys, mouse buttons and controller inputs bound in the launcher's Controls tab (wheel_bindings.ini; from FM2):
// "keyboard/k<virtual key>" (mouse buttons are virtual keys 1-6) and "pad/b<n>", "pad/a<n>[-]" in the standard
// gamepad layout (b0 A ... b6 LT, b7 RT ... b15 D-pad right; a0 left stick X ... a3 right stick Y). A pad input
// with keys bound uses those instead of its default key; one with controller inputs bound takes them from the
// controller instead of its own button. Wheels, pedals and button boxes are read by input/wheel.cpp. Read once at start.
struct PadSource
{
    char kind; // 'b' button, 'a' axis
    int index;
    bool reverse;
};
struct Bound
{
    std::vector<int> keys;
    std::vector<PadSource> pad;
};

const std::map<std::string, Bound>& Bindings()
{
    static const std::map<std::string, Bound> bound = [] {
        std::map<std::string, Bound> m;
        std::ifstream in(UserDir() / "wheel_bindings.ini");
        for (std::string line; std::getline(in, line);)
        {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos)
                continue;
            const std::string target = line.substr(0, eq);
            for (size_t at = line.find("keyboard/k", eq); at != std::string::npos; at = line.find("keyboard/k", at + 1))
                if (const int vk = atoi(line.c_str() + at + 10); vk > 0 && vk < 256)
                    m[target].keys.push_back(vk);
            for (size_t at = line.find("pad/", eq); at != std::string::npos; at = line.find("pad/", at + 1))
            {
                const char kind = line[at + 4];
                const int index = atoi(line.c_str() + at + 5);
                const size_t end = line.find(',', at);
                const bool reverse = line[(end == std::string::npos ? line.find_last_not_of(" \r") + 1 : end) - 1] == '-';
                if ((kind == 'b' && index >= 0 && index < 16) || (kind == 'a' && index >= 0 && index < 4))
                    m[target].pad.push_back({ kind, index, reverse });
            }
        }
        if (!m.empty())
            Log("controls: %zu pad inputs bound to keys, mouse or controller (wheel_bindings.ini)", m.size());
        return m;
    }();
    return bound;
}

// The pad input's bound keys if it has any, else its default key, unless another input has that key bound (a key
// does one thing: bound to the throttle, the up arrow no longer also presses D-pad up).
bool Pressed(const char* target, int defaultKey)
{
    const auto& bound = Bindings();
    if (const auto it = bound.find(target); it != bound.end() && !it->second.keys.empty())
        return std::any_of(it->second.keys.begin(), it->second.keys.end(), Key);
    static const std::set<int> taken = [&] {
        std::set<int> keys;
        for (const auto& [name, b] : bound)
            keys.insert(b.keys.begin(), b.keys.end());
        return keys;
    }();
    return defaultKey && !taken.count(defaultKey) && Key(defaultKey);
}

// A controller's inputs as bound (Bindings): each pad input with controller inputs bound is driven by those
// (any of them, the largest for triggers and steering) instead of its own button, stick or trigger.
void RemapPad(PadState& s)
{
    const auto& bound = Bindings();
    if (bound.empty())
        return;
    static const uint16_t kStandard[16] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT };
    const PadState raw = s;
    // A source's value, 0..1 (sticks: -1..1).
    auto value = [&](const PadSource& p) -> float {
        if (p.kind == 'a')
        {
            const int16_t v[4] = { raw.lx, raw.ly, raw.rx, raw.ry };
            return (p.reverse ? -1.0f : 1.0f) * float(v[p.index]) / 32767.0f;
        }
        if (p.index == 6)
            return raw.leftTrigger / 255.0f;
        if (p.index == 7)
            return raw.rightTrigger / 255.0f;
        return (raw.buttons & kStandard[p.index]) ? 1.0f : 0.0f;
    };
    static const std::pair<const char*, uint16_t> buttons[] = { { "a", XINPUT_GAMEPAD_A }, { "b", XINPUT_GAMEPAD_B },
        { "x", XINPUT_GAMEPAD_X }, { "y", XINPUT_GAMEPAD_Y }, { "lb", XINPUT_GAMEPAD_LEFT_SHOULDER },
        { "rb", XINPUT_GAMEPAD_RIGHT_SHOULDER }, { "back", XINPUT_GAMEPAD_BACK }, { "start", XINPUT_GAMEPAD_START },
        { "ls", XINPUT_GAMEPAD_LEFT_THUMB }, { "rs", XINPUT_GAMEPAD_RIGHT_THUMB }, { "up", XINPUT_GAMEPAD_DPAD_UP },
        { "down", XINPUT_GAMEPAD_DPAD_DOWN }, { "left", XINPUT_GAMEPAD_DPAD_LEFT }, { "right", XINPUT_GAMEPAD_DPAD_RIGHT } };
    for (const auto& [target, bit] : buttons)
        if (const auto it = bound.find(target); it != bound.end() && !it->second.pad.empty())
        {
            bool on = false;
            for (const PadSource& p : it->second.pad)
                on |= value(p) >= 0.5f;
            s.buttons = uint16_t((s.buttons & ~bit) | (on ? bit : 0));
        }
    auto trigger = [&](const char* target, uint8_t& out) {
        if (const auto it = bound.find(target); it != bound.end() && !it->second.pad.empty())
        {
            float v = 0;
            for (const PadSource& p : it->second.pad)
                v = std::max(v, std::fabs(value(p)));
            out = uint8_t(std::lround(std::min(v, 1.0f) * 255.0f));
        }
    };
    trigger("throttle", s.rightTrigger);
    trigger("brake", s.leftTrigger);
    // Steering: a bound stick, and buttons for each side.
    float steer = 0;
    bool steerBound = false;
    if (const auto it = bound.find("steer"); it != bound.end() && !it->second.pad.empty())
        for (const PadSource& p : it->second.pad)
            if (p.kind == 'a' && std::fabs(value(p)) > std::fabs(steer))
                steer = value(p), steerBound = true;
    for (const auto& [target, sign] : { std::pair{ "steerleft", -1.0f }, { "steerright", 1.0f } })
        if (const auto it = bound.find(target); it != bound.end())
            for (const PadSource& p : it->second.pad)
                if (value(p) >= 0.5f)
                    steer = sign, steerBound = true; // only while held: the stick steers otherwise
    if (steerBound)
        s.lx = int16_t(std::lround(std::clamp(steer, -1.0f, 1.0f) * 32767.0f));
    // The right stick's directions, by button (only while held, like steering).
    for (const auto& [target, axis, value16] : { std::tuple{ "lookleft", &s.rx, int16_t(-32767) }, { "lookright", &s.rx, int16_t(32767) },
             { "lookdown", &s.ry, int16_t(-32767) }, { "lookup", &s.ry, int16_t(32767) } })
        if (const auto it = bound.find(target); it != bound.end())
            for (const PadSource& p : it->second.pad)
                if (value(p) >= 0.5f)
                    *axis = value16;
}

void AddKeyboard(PadState& s)
{
    if (!regpu::WindowHasFocus())
        return;
    struct Map
    {
        const char* target;
        int vk;
        uint16_t button;
    };
    static const Map buttons[] = {
        { "up", VK_UP, XINPUT_GAMEPAD_DPAD_UP }, { "down", VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
        { "left", VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { "right", VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
        { "start", VK_ESCAPE, XINPUT_GAMEPAD_START }, { "back", VK_TAB, XINPUT_GAMEPAD_BACK },
        { "lb", 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER }, { "rb", 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { "a", VK_RETURN, XINPUT_GAMEPAD_A }, { "b", VK_BACK, XINPUT_GAMEPAD_B }, { "x", VK_SPACE, XINPUT_GAMEPAD_X },
        { "y", 'Y', XINPUT_GAMEPAD_Y }, { "ls", 0, XINPUT_GAMEPAD_LEFT_THUMB }, { "rs", 0, XINPUT_GAMEPAD_RIGHT_THUMB },
    };
    for (const Map& m : buttons)
        if (Pressed(m.target, m.vk))
            s.buttons |= m.button;
    if (Pressed("throttle", 'W'))
        s.rightTrigger = 255;
    if (Pressed("brake", 'S'))
        s.leftTrigger = 255;
    if (const bool l = Pressed("steerleft", 'A'), r = Pressed("steerright", 'D'); l != r)
        s.lx = l ? -32767 : 32767;
    if (const bool l = Pressed("lookleft", 'J'), r = Pressed("lookright", 'L'); l != r)
        s.rx = l ? -32767 : 32767;
    if (const bool u = Pressed("lookup", 'I'), d = Pressed("lookdown", 'K'); u != d)
        s.ry = d ? -32767 : 32767;
}

int g_xinputPads = 0; // connected XInput pads (PadSlot): SDL's pads (GamepadRead) are the players after them

// Players are the connected pads in slot order: player 1 is the first connected pad whatever its XInput slot (a pad
// left in slot 2, after a virtual pad from Steam Input or DS4Windows took slot 1, otherwise played as player 2). The
// slots are looked up again every second: XInputGetState on an empty slot enumerates the devices (DeviceIoControl),
// 1-2% of the title's main thread when it read all four every frame. (From FM2.)
DWORD PadSlot(uint32_t user)
{
    static std::mutex slotMutex;
    static int slotOf[XUSER_MAX_COUNT] = { 0, 1, 2, 3 };
    static Clock::time_point slotsChecked{};
    std::lock_guard lock(slotMutex);
    if (const auto now = Clock::now(); now - slotsChecked > std::chrono::seconds(1))
    {
        slotsChecked = now;
        int n = 0;
        XINPUT_STATE probe{};
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; slot++)
            if (XInputGetState(slot, &probe) == ERROR_SUCCESS)
                slotOf[n++] = int(slot);
        g_xinputPads = n;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT && n < int(XUSER_MAX_COUNT); slot++)
            if (std::find(slotOf, slotOf + n, int(slot)) == slotOf + n)
                slotOf[n++] = int(slot);
    }
    return DWORD(slotOf[user < XUSER_MAX_COUNT ? user : 0]);
}

// Only player 1 is signed in, so a pad in a later position (an idle virtual pad or a second controller ahead of the one
// in hand) started the game without a profile: new driver, nothing saved. The first pad pressed swaps places with
// player 1; that press is held back, to reach the game as player 1's. PGR4_NO_LEAD_PAD=1: off.
std::atomic<int> g_leadPad{ -1 }; // the device playing as player 1; -1 until a pad is pressed

uint32_t DeviceOf(uint32_t user)
{
    const int lead = g_leadPad.load(std::memory_order_relaxed);
    return lead <= 0 ? user : user == 0 ? uint32_t(lead) : user == uint32_t(lead) ? 0 : user;
}

// Player `user`'s controller: an XInput pad, else a pad XInput doesn't see (DualSense, DualShock 4, Switch Pro and
// other DirectInput pads through SDL: input/wheel.cpp), with the Controls tab's bindings applied.
bool ReadDevice(uint32_t player, PadState& s)
{
    if (player >= XUSER_MAX_COUNT)
        return false;
    const uint32_t user = DeviceOf(player);
    const DWORD slot = PadSlot(user);
    XINPUT_STATE x{};
    if (int(user) < g_xinputPads && XInputGetState(slot, &x) == ERROR_SUCCESS)
    {
        s.buttons = x.Gamepad.wButtons; // same bit layout as the 360
        s.leftTrigger = x.Gamepad.bLeftTrigger;
        s.rightTrigger = x.Gamepad.bRightTrigger;
        s.lx = x.Gamepad.sThumbLX;
        s.ly = x.Gamepad.sThumbLY;
        s.rx = x.Gamepad.sThumbRX;
        s.ry = x.Gamepad.sThumbRY;
        if (s.buttons || s.leftTrigger > 64 || s.rightTrigger > 64 || std::abs(s.lx) > 16000 || std::abs(s.ly) > 16000)
            NoteXInputPadUsed();
    }
    else if (int(user) >= g_xinputPads)
    {
        const GamepadInput g = GamepadRead(user - uint32_t(g_xinputPads));
        if (!g.present)
            return false;
        s.buttons = g.buttons;
        s.leftTrigger = g.leftTrigger;
        s.rightTrigger = g.rightTrigger;
        s.lx = g.lx, s.ly = g.ly, s.rx = g.rx, s.ry = g.ry;
    }
    else
        return false;
    RemapPad(s);
    static const bool noLead = getenv("PGR4_NO_LEAD_PAD") != nullptr;
    if (s.buttons && !noLead && g_leadPad.load(std::memory_order_relaxed) < 0)
    {
        g_leadPad = int(user);
        if (user != 0)
        {
            Log("input: the first pad pressed is device %u, now player 1", user);
            s.buttons = 0;
        }
    }
    return true;
}

// Returns false if nothing is connected for this user.
bool ReadPad(uint32_t user, PadState& s)
{
    // Each player's own controller (split screen); the keyboard and a racing wheel are player 1's.
    bool connected = ReadDevice(user, s);
    if (user == 0)
    {
        connected = true;
        AddKeyboard(s);
        // A racing wheel (input/wheel.cpp): its steering replaces the left stick's X, its pedals and buttons add to
        // the pad's.
        if (const WheelInput w = WheelRead(); w.present)
        {
            s.lx = w.steer;
            s.leftTrigger = std::max(s.leftTrigger, w.brake);
            s.rightTrigger = std::max(s.rightTrigger, w.gas);
            s.buttons |= w.buttons;
        }
        static LiveSwitch autoStart("PGR4_AUTOSTART");
        static const auto start = Clock::now();
        if (autoStart)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            if (ms % 3000 > 2800)
                s.buttons |= XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A;
        }
    }
    // PGR4_RECORD_INPUT=<file>: player 1's input (pad and keyboard) written as it changes, with the time
    // since the title first read the pad. PGR4_REPLAY_INPUT=<file> plays such a recording back instead
    // of the live input (then holds its last state; PGR4_REPLAY_EXIT=<seconds> quits that long after its
    // end), so a route to a rendering bug can be repeated without a person. (From the FM2 port.)
    if (user == 0)
    {
        static const auto epoch = Clock::now();
        const double now = std::chrono::duration<double>(Clock::now() - epoch).count();
        static FILE* record = getenv("PGR4_RECORD_INPUT") ? fopen(getenv("PGR4_RECORD_INPUT"), "w") : nullptr;
        struct Sample
        {
            double t;
            PadState p;
        };
        static const std::vector<Sample> replay = [] {
            std::vector<Sample> v;
            if (FILE* f = getenv("PGR4_REPLAY_INPUT") ? fopen(getenv("PGR4_REPLAY_INPUT"), "r") : nullptr)
            {
                Sample x;
                int b, lt, rt, lx, ly, rx, ry;
                while (fscanf(f, "%lf %x %d %d %d %d %d %d", &x.t, &b, &lt, &rt, &lx, &ly, &rx, &ry) == 8)
                {
                    x.p.buttons = uint16_t(b);
                    x.p.leftTrigger = uint8_t(lt);
                    x.p.rightTrigger = uint8_t(rt);
                    x.p.lx = int16_t(lx), x.p.ly = int16_t(ly), x.p.rx = int16_t(rx), x.p.ry = int16_t(ry);
                    v.push_back(x);
                }
                fclose(f);
                Log("input: replaying %zu changes over %.1f s", v.size(), v.empty() ? 0.0 : v.back().t);
            }
            return v;
        }();
        if (!replay.empty())
        {
            size_t i = 0;
            while (i + 1 < replay.size() && replay[i + 1].t <= now)
                i++;
            s = now >= replay[0].t ? replay[i].p : PadState{};
            static const double exitAfter = getenv("PGR4_REPLAY_EXIT") ? atof(getenv("PGR4_REPLAY_EXIT")) : -1;
            if (exitAfter >= 0 && now > replay.back().t + exitAfter)
            {
                Log("input: replay finished");
                fflush(nullptr);
                ExitProcess(0);
            }
        }
        else if (record)
        {
            static PadState last{};
            static bool first = true;
            if (first || memcmp(&last, &s, sizeof(s)) != 0)
            {
                fprintf(record, "%.3f %04X %u %u %d %d %d %d\n", now, s.buttons, s.leftTrigger, s.rightTrigger, s.lx, s.ly, s.rx, s.ry);
                fflush(record);
                last = s;
                first = false;
            }
        }
    }
    // The debug server's input (debug.cpp) on top of everything else.
    if (regpu::DebugPad d; user == 0 && regpu::DebugPadState(d))
    {
        connected = true;
        s.buttons |= d.buttons;
        if (d.triggers)
            s.leftTrigger = d.leftTrigger, s.rightTrigger = d.rightTrigger;
        if (d.sticks)
            s.lx = d.lx, s.ly = d.ly, s.rx = d.rx, s.ry = d.ry;
    }
    return connected;
}
}

GUEST_EXPORT(XamInputGetCapabilities)
{
    // (user, flags, XINPUT_CAPABILITIES*)
    PadState s;
    if (!ReadPad(ctx.r3.u32, s))
    {
        Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    const uint32_t caps = ctx.r5.u32;
    memset(GuestPtr(caps), 0, 0x14);
    *GuestPtr<uint8_t>(caps + 0) = 1; // XINPUT_DEVTYPE_GAMEPAD
    *GuestPtr<uint8_t>(caps + 1) = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
    if (ctx.r3.u32 == 0 && (WheelFakeEnabled() || WheelRead().present))
    {
        // A racing wheel (or PGR4_FAKE_WHEEL) as an Xbox 360 wheel, with force feedback when it
        // has some: the game then starts its XInputdFF* effects (kernel/wheel_ff.cpp).
        *GuestPtr<uint8_t>(caps + 1) = 2; // XINPUT_DEVSUBTYPE_WHEEL
        if (WheelFakeEnabled() || WheelHasForceFeedback())
            *GuestPtr<uint8_t>(caps + 3) = 1; // flags (big-endian): XINPUT_CAPS_FFB_SUPPORTED
    }
    Return(ctx, X_ERROR_SUCCESS);
}

bool CarsDriving(); // hooks/gear_latch.cpp

// Menus: a held direction (D-pad or left stick) moves once, then waits kMenuHoldDelay before the game's own
// auto-repeat may start: it reads as released until then. The game counts its repeat delay in frames, so at
// high frame rates one press moved several items. Off while the cars drive (steering, D-pad in a race).
// PGR4_MENU_HOLD_MS=<ms> changes the delay (0: off).
static void MenuHoldDelay(uint32_t user, PadState& s)
{
    static const int delayMs = getenv("PGR4_MENU_HOLD_MS") ? atoi(getenv("PGR4_MENU_HOLD_MS")) : 500;
    if (user >= XUSER_MAX_COUNT || delayMs <= 0)
        return;
    using Clock = std::chrono::steady_clock;
    constexpr int16_t kStick = 16000;
    enum { Up, Down, Left, Right, Count };
    static Clock::time_point since[XUSER_MAX_COUNT][Count];
    static bool held[XUSER_MAX_COUNT][Count];
    const bool on[Count] = {
        (s.buttons & XINPUT_GAMEPAD_DPAD_UP) || s.ly > kStick,
        (s.buttons & XINPUT_GAMEPAD_DPAD_DOWN) || s.ly < -kStick,
        (s.buttons & XINPUT_GAMEPAD_DPAD_LEFT) || s.lx < -kStick,
        (s.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || s.lx > kStick,
    };
    const auto now = Clock::now();
    const bool driving = CarsDriving();
    static bool wasDriving = false;
    if (driving != wasDriving)
    {
        wasDriving = driving;
        Log("input: menu hold delay %s", driving ? "off (driving)" : "on");
    }
    for (int d = 0; d < Count; d++)
    {
        if (!on[d])
        {
            held[user][d] = false;
            continue;
        }
        if (!held[user][d])
        {
            held[user][d] = true;
            since[user][d] = now;
        }
        // The press's first 40 ms go through (a frame or two, at any frame rate); the rest of the delay
        // reads as released.
        const auto held_for = now - since[user][d];
        if (driving || held_for < std::chrono::milliseconds(40) || held_for >= std::chrono::milliseconds(delayMs))
            continue;
        static const uint16_t bit[Count] = { XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT };
        s.buttons &= ~bit[d];
        if (d == Up || d == Down)
            s.ly = 0;
        else
            s.lx = 0;
    }
}

GUEST_EXPORT(XamInputGetState)
{
    // (user, flags, XINPUT_STATE*): packet, buttons, triggers, sticks (big-endian).
    PadState s;
    if (!ReadPad(ctx.r3.u32, s))
    {
        Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    // PGR4_LOOK_BACK_AT=<seconds>: from then on the right stick is held down (the chase camera
    // looks back at the car's front), over a replay too: for captures of the bonnet.
    if (static const double lookBack = getenv("PGR4_LOOK_BACK_AT") ? atof(getenv("PGR4_LOOK_BACK_AT")) : 0; lookBack > 0)
    {
        static const auto start = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= lookBack)
            s.ry = -32767;
    }
    MenuHoldDelay(ctx.r3.u32, s);
    static std::atomic<uint32_t> packet{ 0 };
    const uint32_t state = ctx.r5.u32;
    StoreBE32(state, ++packet);
    *GuestPtr<uint16_t>(state + 4) = __builtin_bswap16(s.buttons);
    *GuestPtr<uint8_t>(state + 6) = s.leftTrigger;
    *GuestPtr<uint8_t>(state + 7) = s.rightTrigger;
    *GuestPtr<uint16_t>(state + 8) = __builtin_bswap16(uint16_t(s.lx));
    *GuestPtr<uint16_t>(state + 10) = __builtin_bswap16(uint16_t(s.ly));
    *GuestPtr<uint16_t>(state + 12) = __builtin_bswap16(uint16_t(s.rx));
    *GuestPtr<uint16_t>(state + 14) = __builtin_bswap16(uint16_t(s.ry));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamInputSetState)
{
    // (user, flags, XINPUT_VIBRATION*): left and right motor speeds, big-endian.
    const uint32_t user = ctx.r3.u32;
    PadState s;
    if (!ReadPad(user, s))
    {
        Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (ctx.r5.u32 && user < XUSER_MAX_COUNT)
    {
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = __builtin_bswap16(*GuestPtr<uint16_t>(ctx.r5.u32));
        v.wRightMotorSpeed = __builtin_bswap16(*GuestPtr<uint16_t>(ctx.r5.u32 + 2));
        const uint32_t device = DeviceOf(user);
        if (int(device) < g_xinputPads)
            XInputSetState(PadSlot(device), &v);
        else
            GamepadRumble(device - uint32_t(g_xinputPads), v.wLeftMotorSpeed, v.wRightMotorSpeed);
    }
    Return(ctx, X_ERROR_SUCCESS);
}

// Content packages (saves) --------------------------------------------------------------

GUEST_EXPORT(XamContentCreateEx)
{
    // (user, rootName, XCONTENT_DATA*, flags, disposition*, license*, cacheSize, contentSize (u64), overlapped)
    const std::string root = GuestString(ctx.r4.u32);
    const uint32_t data = ctx.r5.u32;
    // XCONTENT_DATA: deviceId, contentType, displayName[128] (wide), fileName[42].
    const std::string fileName(GuestPtr<char>(data + 8 + 256), strnlen(GuestPtr<char>(data + 8 + 256), 42));
    const uint32_t contentType = LoadBE32(data + 4);
    std::filesystem::path dir = ContentTypeDir(contentType) / fileName;
    std::error_code ec;
    // A downloadable package installed in the shared folder: mounted read-only from there.
    const bool shared = contentType == 2 && std::filesystem::is_directory(DlcDir() / fileName, ec);
    if (shared)
        dir = DlcDir() / fileName;
    const bool existed = shared || std::filesystem::exists(dir, ec);
    const uint32_t flags = ctx.r6.u32;
    const uint32_t mode = flags & 0xF; // 1 create new, 2 create always, 3 open, 4 open always
    if (!existed && mode == 3)
    {
        FinishUi(ctx, StackArg(ctx, 8), X_ERROR_FILE_NOT_FOUND);
        return;
    }
    if (!shared)
        std::filesystem::create_directories(dir, ec);
    if (!existed)
    {
        std::u16string title;
        for (uint32_t i = 0; i < 128; i++)
        {
            const uint16_t c = __builtin_bswap16(*GuestPtr<uint16_t>(data + 8 + i * 2));
            if (!c)
                break;
            title += char16_t(c);
        }
        std::ofstream(TitlePath(contentType, fileName), std::ios::binary).write(reinterpret_cast<const char*>(title.data()), title.size() * 2);
    }
    const std::string device = shared ? "\\Device\\Dlc\\" + fileName
                                      : "\\Device\\Content\\" + dir.parent_path().filename().string() + "\\" + fileName;
    VfsAddLink(root + ":", device);
    Log("content: %s: -> %s", root.c_str(), dir.string().c_str());
    StoreIfNonNull(ctx.r7.u32, existed ? 2 : 1); // XCONTENT_OPENED_EXISTING : XCONTENT_CREATED_NEW
    // The license mask of a downloadable package (<package>.license, from its header; see
    // tools/stfs.py install): titles skip content that is not licensed.
    uint32_t licenseMask = 0;
    if (shared)
        if (std::ifstream in(DlcDir() / (fileName + ".license")); in)
            in >> std::hex >> licenseMask;
    // A zero mask (the Kellogg's Typhon promo package) unlocks nothing: treat it as fully licensed.
    if (shared && !licenseMask)
        licenseMask = 0xFFFFFFFF;
    StoreIfNonNull(ctx.r8.u32, licenseMask);
    FinishUi(ctx, StackArg(ctx, 8));
}

GUEST_EXPORT(XamContentClose)
{
    VfsRemoveLink(GuestString(ctx.r3.u32) + ":");
    FinishUi(ctx, ctx.r4.u32);
}

GUEST_EXPORT(XamContentFlush) { FinishUi(ctx, ctx.r4.u32); }
GUEST_EXPORT(XamContentDelete)
{
    // (user, XCONTENT_DATA*, overlapped)
    const uint32_t data = ctx.r4.u32;
    const std::string fileName(GuestPtr<char>(data + 8 + 256), strnlen(GuestPtr<char>(data + 8 + 256), 42));
    std::error_code ec;
    if (!fileName.empty())
    {
        const uint32_t type = LoadBE32(data + 4);
        std::filesystem::remove_all(ContentTypeDir(type) / fileName, ec);
        std::filesystem::remove(TitlePath(type, fileName), ec);
        Log("content: deleted %08X/%s", type, fileName.c_str());
    }
    FinishUi(ctx, ctx.r5.u32);
}

GUEST_EXPORT(XamContentGetCreator)
{
    // (user, data*, isCreator*, xuid*, overlapped)
    StoreIfNonNull(ctx.r5.u32, 1);
    if (ctx.r6.u32)
        StoreBE64(ctx.r6.u32, Xuid());
    FinishUi(ctx, ctx.r7.u32);
}

GUEST_EXPORT(XamContentGetDeviceState)
{
    FinishUi(ctx, ctx.r4.u32);
}

GUEST_EXPORT(XamContentGetDeviceData)
{
    // (deviceId, XDEVICE_DATA*)
    const uint32_t data = ctx.r4.u32;
    memset(GuestPtr(data), 0, 0x50);
    StoreBE32(data + 0, ctx.r3.u32);
    StoreBE32(data + 4, 1);                 // device type: HDD
    StoreBE64(data + 8, 20ull << 30);        // total bytes
    StoreBE64(data + 16, 16ull << 30);       // free bytes
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamContentCreateEnumerator)
{
    // (user, deviceId, contentType, flags, itemCount, bufferSize*, handle*): lists the
    // profile's packages of that type (all types for 0).
    auto e = std::make_shared<EnumeratorObject>();
    const uint32_t wanted = ctx.r5.u32;
    std::error_code ec;
    for (const auto& typeDir : std::filesystem::directory_iterator(ContentDir(), ec))
    {
        uint32_t type = 0;
        if (!typeDir.is_directory() || sscanf(typeDir.path().filename().string().c_str(), "%x", &type) != 1 || (wanted && type != wanted))
            continue;
        for (const auto& package : std::filesystem::directory_iterator(typeDir.path(), ec))
        {
            const std::string fileName = package.path().filename().string();
            if (!package.is_directory() || fileName.size() > 42)
                continue;
            ContentItem item{ type, {}, fileName };
            std::ifstream in(TitlePath(type, fileName), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), {});
            item.displayName.assign(reinterpret_cast<const char16_t*>(bytes.data()), bytes.size() / 2);
            if (item.displayName.empty())
                item.displayName.assign(fileName.begin(), fileName.end());
            e->items.push_back(std::move(item));
        }
    }
    // Downloadable content is shared by all profiles: <user dir>\dlc\<package>.
    if (!wanted || wanted == 2)
        for (const auto& package : std::filesystem::directory_iterator(DlcDir(), ec))
        {
            const std::string fileName = package.path().filename().string();
            if (!package.is_directory() || fileName.size() > 42)
                continue;
            ContentItem item{ 2, {}, fileName };
            std::ifstream in(DlcDir() / (fileName + ".title"), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), {});
            item.displayName.assign(reinterpret_cast<const char16_t*>(bytes.data()), bytes.size() / 2);
            if (item.displayName.empty())
                item.displayName.assign(fileName.begin(), fileName.end());
            e->items.push_back(std::move(item));
        }
    Log("content: enumerating %zu packages of type %08X", e->items.size(), wanted);
    StoreIfNonNull(ctx.r8.u32, std::max(ctx.r7.u32, 1u) * kContentDataSize);
    StoreIfNonNull(ctx.r9.u32, CreateHandle(e));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamCreateEnumeratorHandle)
{
    StoreIfNonNull(StackArg(ctx, 8), CreateHandle(std::make_shared<EnumeratorObject>()));
    Return(ctx, X_ERROR_SUCCESS);
}

GUEST_EXPORT(XamEnumerate)
{
    // (handle, flags, buffer, size, itemsReturned*, overlapped)
    auto e = std::dynamic_pointer_cast<EnumeratorObject>(GetHandle(ctx.r3.u32));
    uint32_t count = 0;
    if (e)
    {
        const uint32_t buffer = ctx.r5.u32, capacity = ctx.r6.u32 / kContentDataSize;
        for (; count < capacity && e->next < e->items.size(); count++, e->next++)
        {
            const ContentItem& item = e->items[e->next];
            const uint32_t d = buffer + count * kContentDataSize;
            memset(GuestPtr(d), 0, kContentDataSize);
            StoreBE32(d + 0, 1); // the hard drive
            StoreBE32(d + 4, item.type);
            for (size_t i = 0; i < item.displayName.size() && i < 127; i++)
                *GuestPtr<uint16_t>(d + 8 + uint32_t(i) * 2) = __builtin_bswap16(uint16_t(item.displayName[i]));
            memcpy(GuestPtr(d + 8 + 256), item.fileName.data(), item.fileName.size());
        }
    }
    StoreIfNonNull(ctx.r7.u32, count);
    FinishUi(ctx, ctx.r8.u32, count ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES);
}

// Tasks ----------------------------------------------------------------------------

GUEST_EXPORT(XamTaskSchedule)
{
    // (callback, message, unknown, handle*)
    auto t = CreateGuestThread(ctx.r3.u32, ctx.r4.u32, 0, 0x10000, false, "xam task");
    StoreIfNonNull(ctx.r6.u32, t->handle);
    Return(ctx, X_STATUS_SUCCESS);
}

GUEST_EXPORT(XamTaskShouldExit) { Return(ctx, 0); }
GUEST_EXPORT(XamTaskCloseHandle) { CloseHandle(ctx.r3.u32); Return(ctx, X_STATUS_SUCCESS); }

// Live, sessions, voice: offline ------------------------------------------------------

uint32_t XmpMessage(uint32_t message, uint32_t args, uint32_t length); // audio/xmp.cpp

// XMsg to system apps. 0xFA is XMP, the music player: report it idle
// (no custom soundtrack) at full volume. Arguments arrive as a struct of
// big-endian dwords at r5.
// PGR4_TRACE_XMP=1: the media player messages with their first argument dwords (the status and
// controller polls, about 600 a second, are left out).
static void XmpTrace(uint32_t message, uint32_t args, uint32_t length, const char* how)
{
    static const bool trace = getenv("PGR4_TRACE_XMP") != nullptr;
    if (!trace || message == 0x00070009 || message == 0x0007000B || message == 0x0007001B)
        return;
    char words[256];
    int n = 0;
    for (int i = 0; args && i < 10; i++)
        n += snprintf(words + n, sizeof(words) - size_t(n), "%08X ", LoadBE32(args + uint32_t(i) * 4));
    Log("xmp: %s message %08X (%u bytes): %s", how, message, length, words);
}

GUEST_EXPORT(XMsgInProcessCall)
{
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, args = ctx.r5.u32;
    if (app == 0xFC)
    {
        // XLiveBase: the title's profile manager starts with a request here (00058004) and
        // gives up if it fails: no sign-in tracking, so "No Gamer Profiles" and no saves.
        LOG_ONCE("XLiveBase message %08X answered with success", message);
        Return(ctx, 0);
        return;
    }
    if (app != 0xFA)
    {
        LOG_ONCE("XMsgInProcessCall to app %X (message %08X) not supported", app, message);
        Return(ctx, 0x80004005); // E_FAIL
        return;
    }
    XmpTrace(message, args, ctx.r6.u32, "call");
    Return(ctx, XmpMessage(message, args, ctx.r6.u32));
}

// The asynchronous variants carry the same messages; the title passes no overlapped (they finish
// at once). (app, message, overlapped, buffer, buffer length)
static void XmpAsync(PPCContext& ctx, const char* how)
{
    if (ctx.r3.u32 != 0xFA)
    {
        FinishUi(ctx, ctx.r5.u32, X_ERROR_FUNCTION_FAILED);
        return;
    }
    XmpTrace(ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, how);
    FinishUi(ctx, ctx.r5.u32, XmpMessage(ctx.r4.u32, ctx.r6.u32, ctx.r7.u32));
}
GUEST_EXPORT(XMsgStartIORequest) { XmpAsync(ctx, "async"); }
GUEST_EXPORT(XMsgStartIORequestEx) { XmpAsync(ctx, "async-ex"); }
GUEST_EXPORT(XMsgCancelIORequest) { Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamSessionCreateHandle) { StoreIfNonNull(ctx.r3.u32, CreateHandle(std::make_shared<EnumeratorObject>())); Return(ctx, X_ERROR_SUCCESS); }
GUEST_EXPORT(XamSessionRefObjByHandle) { StoreIfNonNull(ctx.r4.u32, 0); Return(ctx, X_ERROR_FUNCTION_FAILED); }
GUEST_EXPORT(XamVoiceCreate) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }
GUEST_EXPORT(XamVoiceClose) { Return(ctx, 0); }
GUEST_EXPORT(XamVoiceHeadsetPresent) { Return(ctx, 0); }
GUEST_EXPORT(XamVoiceSubmitPacket) { Return(ctx, X_ERROR_DEVICE_NOT_CONNECTED); }

// Keys and signing: report every signature as valid ---------------------------------------

GUEST_EXPORT(XeKeysConsolePrivateKeySign)
{
    memset(GuestPtr(ctx.r4.u32), 0, 0x228);
    Return(ctx, 1);
}

GUEST_EXPORT(XeKeysConsoleSignatureVerification)
{
    StoreIfNonNull(ctx.r5.u32, 0);
    Return(ctx, 1);
}

// Network: no link. Every call fails cleanly ------------------------------------------

GUEST_EXPORT(NetDll_XNetStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetCleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAStartup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSACleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAGetLastError) { Return(ctx, 10050); } // WSAENETDOWN
GUEST_EXPORT(NetDll_XNetGetEthernetLinkStatus) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetGetTitleXnAddr) { Return(ctx, 1); } // XNET_GET_XNADDR_NONE
GUEST_EXPORT(NetDll_XNetGetConnectStatus) { Return(ctx, 3); } // lost
GUEST_EXPORT(NetDll_socket) { Return(ctx, 0xFFFFFFFF); }
GUEST_EXPORT(NetDll_closesocket) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetRandom)
{
    // (caller, buffer, length)
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        GuestPtr<uint8_t>(ctx.r4.u32)[i] = uint8_t(rand());
    Return(ctx, 0);
}
