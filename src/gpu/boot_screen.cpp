// The first-boot screen: while every game shader compiles, PGR4's own frontend art with a progress
// bar instead of a window that looks hung (as the FM2 and FH1 ports show theirs). The art is decoded
// from the player's UI\Data\ui_frontend.bpr, so nothing is bundled.
//
// A .bpr ("BZR2") is a texture pack: a 28-byte header, a table of 28-byte records (big endian: data
// offset, ?, size, 0, format << 16 | mips, width << 16 | height, ?), the textures' names (in record
// order), then one zlib stream with every texture's data, tiled as the console stores it. Formats:
// 1 DXT1, 5 DXT4_5, 6 8888.
#include "gpu/boot_screen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include <windows.h>

#include <regpu/regpu.h>

#include "gpu/texture.h"
#include "kernel/runtime.h"
#include "log.h"
#include "png.h"
#include "zip.h"

namespace
{
constexpr uint32_t kW = 1280, kH = 720;

struct Image
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// The named textures of a .bpr pack, decoded to RGBA8 (mip 0).
std::map<std::string, Image> LoadBpr(const std::filesystem::path& path, const std::vector<std::string>& wanted)
{
    std::map<std::string, Image> images;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return images;
    std::vector<uint8_t> file(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(file.data()), std::streamsize(file.size()));
    if (file.size() < 0x40 || memcmp(file.data(), "BZR2", 4) != 0)
        return images;
    const uint32_t count = ((file[8] << 8) | file[9]) / 28;
    const size_t table = 0x18, names = 0x1C + size_t(count) * 28;
    if (names >= file.size())
        return images;
    // The zlib stream (78 9C) follows the names.
    size_t z = names;
    while (z + 2 < file.size() && !(file[z] == 0x78 && file[z + 1] == 0x9C))
        z++;
    std::vector<std::string> nameList;
    for (size_t p = names; p < z;)
    {
        const size_t end = std::find(file.begin() + p, file.begin() + z, 0) - file.begin();
        const std::string n(reinterpret_cast<const char*>(&file[p]), end - p);
        if (!n.empty() && std::all_of(n.begin(), n.end(), [](char c) { return c >= 32 && c < 127; }))
            nameList.push_back(n);
        p = end + 1;
    }
    std::vector<uint8_t> blob;
    if (z + 2 >= file.size() || !Inflate(&file[z + 2], file.size() - z - 2, blob))
        return images;
    for (uint32_t i = 0; i < count && i < nameList.size(); i++)
    {
        if (std::find(wanted.begin(), wanted.end(), nameList[i]) == wanted.end())
            continue;
        const uint8_t* r = &file[table + size_t(i) * 28];
        const uint32_t offset = Be32(r), size = Be32(r + 8), format = Be32(r + 16) >> 16;
        const uint32_t w = Be32(r + 20) >> 16, h = Be32(r + 20) & 0xFFFF;
        uint32_t xenos, endian;
        switch (format)
        {
        case 1: xenos = 18, endian = 1; break; // DXT1, 8in16
        case 5: xenos = 20, endian = 1; break; // DXT4_5
        case 6: xenos = 6, endian = 2; break;  // 8888, 8in32
        default: continue;
        }
        if (!w || !h || w > 4096 || h > 4096 || size_t(offset) + size > blob.size())
            continue;
        TextureFetch fetch{};
        fetch.dwords[0] = 2u | (((w + 31) / 32) << 22) | (1u << 31); // a tiled 2D texture
        fetch.dwords[1] = xenos | (endian << 6);
        fetch.dwords[2] = (w - 1) | ((h - 1) << 13);
        fetch.dwords[3] = 0x688u << 1; // XYZW
        fetch.dwords[5] = 1u << 9;     // 2D
        Image img{ w, h };
        if (DecodeTexture(fetch, &blob[offset], img.rgba))
            images[nameList[i]] = std::move(img);
    }
    return images;
}

const char* const kCities[] = { "CB_London", "CB_NewYork", "CB_Tokyo", "CB_Shanghai", "CB_StPetes",
                                "CB_LasVegas", "CB_Macau", "CB_Quebec", "CB_Nurburgring", "CB_TestTrack" };

struct Assets
{
    std::once_flag once;
    bool ok = false;
    Image map, logo;
    std::vector<Image> badges;
} g_assets;

bool EnsureAssets()
{
    std::call_once(g_assets.once, [] {
        std::vector<std::string> wanted = { "World_Map", "PGR4_logo" };
        wanted.insert(wanted.end(), std::begin(kCities), std::end(kCities));
        const auto path = GameDir() / "UI" / "Data" / "ui_frontend.bpr";
        auto images = LoadBpr(path, wanted);
        g_assets.ok = images.count("World_Map") && images.count("PGR4_logo");
        if (!g_assets.ok)
        {
            Log("boot screen: the world map and logo weren't found in %s; ReGPU's plain overlay is shown", path.string().c_str());
            return;
        }
        g_assets.map = std::move(images["World_Map"]);
        g_assets.logo = std::move(images["PGR4_logo"]);
        for (const char* city : kCities)
            if (images.count(city))
                g_assets.badges.push_back(std::move(images[city]));
    });
    return g_assets.ok;
}

// Draws src scaled to (dw x dh) at (dx, dy), bilinear; its alpha times `opacity` blends it in. With
// tint, src's brightness is the coverage of that colour instead (the world map is a white mask).
void Blit(std::vector<uint8_t>& dst, const Image& src, int dx, int dy, int dw, int dh, float opacity = 1.0f, const uint8_t* tint = nullptr)
{
    for (int y = std::max(0, dy); y < std::min(int(kH), dy + dh); y++)
        for (int x = std::max(0, dx); x < std::min(int(kW), dx + dw); x++)
        {
            const float u = (x - dx + 0.5f) * src.w / dw - 0.5f, v = (y - dy + 0.5f) * src.h / dh - 0.5f;
            const int x0 = std::clamp(int(std::floor(u)), 0, int(src.w) - 1), y0 = std::clamp(int(std::floor(v)), 0, int(src.h) - 1);
            const int x1 = std::min(x0 + 1, int(src.w) - 1), y1 = std::min(y0 + 1, int(src.h) - 1);
            const float fx = std::clamp(u - x0, 0.0f, 1.0f), fy = std::clamp(v - y0, 0.0f, 1.0f);
            float c[4];
            for (int k = 0; k < 4; k++)
            {
                auto at = [&](int sx, int sy) { return float(src.rgba[(size_t(sy) * src.w + sx) * 4 + k]); };
                c[k] = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
            }
            float a = (tint ? c[0] : c[3]) / 255.0f * opacity;
            uint8_t* d = &dst[(size_t(y) * kW + x) * 4];
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t(d[k] * (1 - a) + (tint ? tint[k] : c[k]) * a + 0.5f);
        }
}

void FillRect(std::vector<uint8_t>& dst, int x0, int y0, int x1, int y1, const uint8_t rgb[3], float alpha)
{
    for (int y = std::max(0, y0); y < std::min(int(kH), y1); y++)
        for (int x = std::max(0, x0); x < std::min(int(kW), x1); x++)
        {
            uint8_t* d = &dst[(size_t(y) * kW + x) * 4];
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t(d[k] * (1 - alpha) + rgb[k] * alpha + 0.5f);
        }
}

// Text through GDI (grayscale antialiasing, used as coverage). align: 0 left, 1 centre, 2 right of x.
void DrawLabel(std::vector<uint8_t>& dst, const wchar_t* text, int x, int y, int height, int weight, int align, const uint8_t rgb[3])
{
    static std::mutex gdiMutex;
    std::lock_guard lock(gdiMutex);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = int(kW);
    bi.bmiHeader.biHeight = -height * 2;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SIZE size{};
    GetTextExtentPoint32W(dc, text, int(wcslen(text)), &size);
    const int left = align == 1 ? x - size.cx / 2 : align == 2 ? x - size.cx : x;
    TextOutW(dc, 0, 0, text, int(wcslen(text)));
    GdiFlush();
    const uint8_t* px = static_cast<const uint8_t*>(bits);
    for (int ty = 0; ty < height * 2; ty++)
        for (int tx = 0; tx < size.cx && tx < int(kW); tx++)
        {
            const int dxp = left + tx, dyp = y + ty;
            if (dxp < 0 || dxp >= int(kW) || dyp < 0 || dyp >= int(kH))
                continue;
            const float a = px[(size_t(ty) * kW + tx) * 4 + 1] / 255.0f;
            if (a <= 0)
                continue;
            uint8_t* d = &dst[(size_t(dyp) * kW + dxp) * 4];
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t(d[k] * (1 - a) + rgb[k] * a + 0.5f);
        }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}
} // namespace

bool BootScreenCompose(const wchar_t* title, uint32_t done, uint32_t total, std::vector<uint8_t>& rgba)
{
    if (!EnsureAssets())
        return false;
    static const uint8_t white[3] = { 255, 255, 255 }, grey[3] = { 175, 182, 194 }, black[3] = { 0, 0, 0 };
    static const uint8_t mapTint[3] = { 70, 98, 140 };
    // The logo's red along the bar, darker towards its end.
    static const uint8_t red[3] = { 228, 38, 30 }, deepRed[3] = { 150, 12, 14 };

    // A dark slate backdrop, lighter in the middle, with the frontend's world map faintly over it.
    rgba.assign(size_t(kW) * kH * 4, 255);
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++)
        {
            const float dx = (float(x) - kW * 0.5f) / kW, dy = (float(y) - kH * 0.45f) / kH;
            const float glow = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy) * 1.6f);
            // Lighter behind the logo, whose "GOTHAM" is black.
            const float lx = (float(x) - kW * 0.5f) / 520.0f, ly = (float(y) - 235.0f) / 150.0f;
            const float lift = 70.0f * std::max(0.0f, 1.0f - (lx * lx + ly * ly)) ;
            uint8_t* d = &rgba[(size_t(y) * kW + x) * 4];
            d[0] = uint8_t(10 + 18 * glow + lift);
            d[1] = uint8_t(13 + 22 * glow + lift);
            d[2] = uint8_t(19 + 32 * glow + lift * 1.1f);
        }
    Blit(rgba, g_assets.map, 40, 20, 1200, 600, 0.35f, mapTint);

    // Logo (its texture has room around the lettering), the title and the bar below it.
    Blit(rgba, g_assets.logo, (int(kW) - 820) / 2, 40, 820, 410);
    int y = 400;
    DrawLabel(rgba, title, int(kW) / 2, y, 40, FW_SEMIBOLD, 1, white);
    y += 68;
    const float progress = total ? std::min(1.0f, float(done) / float(total)) : 0.0f;
    const int bx0 = 240, bx1 = 1000, by0 = y, by1 = y + 26;
    FillRect(rgba, bx0 - 2, by0 - 2, bx1 + 2, by1 + 2, white, 0.85f);
    FillRect(rgba, bx0, by0, bx1, by1, black, 0.8f);
    const int fill = bx0 + int((bx1 - bx0) * progress + 0.5f);
    for (int x = bx0 + 3; x < fill - 3; x++)
    {
        const float t = float(x - bx0) / float(bx1 - bx0);
        const uint8_t c[3] = { uint8_t(red[0] + (deepRed[0] - red[0]) * t), uint8_t(red[1] + (deepRed[1] - red[1]) * t),
                               uint8_t(red[2] + (deepRed[2] - red[2]) * t) };
        FillRect(rgba, x, by0 + 3, x + 1, by1 - 3, c, 1.0f);
    }
    wchar_t text[96];
    if (total)
    {
        swprintf(text, 96, L"%d%%", int(progress * 100.0f));
        DrawLabel(rgba, text, bx1 + 22, by0 - 5, 30, FW_SEMIBOLD, 0, white);
        swprintf(text, 96, L"%u / %u %ls · first start only", done, total, wcsstr(title, L"PIPELINES") ? L"pipelines" : L"game shaders");
    }
    else
        swprintf(text, 96, L"Searching the game files · first start only");
    DrawLabel(rgba, text, int(kW) / 2, by1 + 16, 22, FW_NORMAL, 1, grey);

    // The cities' badges along the bottom.
    const int n = int(g_assets.badges.size()), size = 64, gap = 26;
    const int x0 = (int(kW) - (n * size + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; i++)
        Blit(rgba, g_assets.badges[i], x0 + i * (size + gap), 620, size, size, 0.9f);
    return true;
}

void BootScreenPreview()
{
    const auto dir = UserDir() / "bootscreen";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<uint8_t> frame;
    if (BootScreenCompose(L"COMPILING SHADERS", 712, 1548, frame))
        WritePng(dir / "composed.png", kW, kH, frame);
    if (BootScreenCompose(L"PREPARING SHADERS", 0, 0, frame))
        WritePng(dir / "searching.png", kW, kH, frame);
    if (BootScreenCompose(L"BUILDING PIPELINES", 3100, 4211, frame))
        WritePng(dir / "pipelines.png", kW, kH, frame);
    Log("boot screen preview: %s", dir.string().c_str());
}
