// Finds every shader the title ships before the game asks for one (from the FM2 port).
//
// PGR4 keeps its shader microcode in the executable (compressed on disc, so the loaded image is
// searched), dword aligned and with no packet header; its data files are compressed and hold none
// of it. A candidate is a shader when its control flow decodes cleanly: valid opcodes up to an end
// flag, the first exec address equal to the control-flow length, and exec ranges that tile the
// instruction slots without gaps. Only the used slots are kept. The result is saved, keyed by the
// image, so later boots skip the search.
#include "gpu/static_shaders.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "kernel/runtime.h"
#include "log.h"
#include "memory.h"

namespace
{
bool IsExecOp(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }
bool IsEndOp(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }

uint32_t Be(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint32_t Le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

// Used length in dwords of the shader at words[start] (0 if it isn't one); sets pixel.
uint32_t ShaderAt(const uint8_t* bytes, size_t words, size_t start, bool& pixel)
{
    struct Cf { uint32_t op, lo, hi; };
    Cf cfs[130];
    uint32_t ncf = 0, triples = 0;
    for (bool ended = false; !ended;)
    {
        if (triples > 64 || start + triples * 3 + 2 >= words)
            return 0;
        const uint8_t* p = bytes + (start + triples * 3) * 4;
        const uint32_t w0 = Be(p), w1 = Be(p + 4), w2 = Be(p + 8);
        const uint32_t pair[2][2] = { { w0, w1 & 0xFFFF }, { (w1 >> 16) | (w2 << 16), w2 >> 16 } };
        triples++;
        for (const auto& c : pair)
        {
            const uint32_t op = (c[1] >> 12) & 0xF;
            cfs[ncf++] = { op, c[0], c[1] };
            if (IsEndOp(op))
            {
                ended = true;
                break;
            }
        }
    }
    struct Exec { uint32_t addr, count, seq; };
    Exec execs[130];
    uint32_t nexec = 0;
    for (uint32_t i = 0; i < ncf; i++)
    {
        const Cf& c = cfs[i];
        if (c.op == 12 && ((c.lo & 0xFFFFFFF8) || (c.hi & 0x09FF)))
            return 0; // alloc with stray bits
        if (IsExecOp(c.op) && ((c.lo >> 12) & 7))
            execs[nexec++] = { c.lo & 0xFFF, (c.lo >> 12) & 7, (c.lo >> 16) & 0xFFF };
    }
    if (!nexec)
        return 0;
    std::sort(execs, execs + nexec, [](const Exec& a, const Exec& b) { return a.addr < b.addr; });
    uint32_t slot = triples;
    for (uint32_t i = 0; i < nexec; i++)
    {
        if (execs[i].addr != slot)
            return 0;
        slot += execs[i].count;
    }
    if (slot > 512 || start + slot * 3 > words)
        return 0;
    bool vertex = false;
    for (uint32_t i = 0; i < nexec && !vertex; i++)
        for (uint32_t k = 0; k < execs[i].count; k++)
            if ((execs[i].seq >> (2 * k)) & 1)
                if ((Be(bytes + (start + (execs[i].addr + k) * 3) * 4) & 0x1F) == 0)
                    vertex = true;
    bool position = false;
    for (uint32_t i = 0; i < ncf; i++)
        if (cfs[i].op == 12 && ((cfs[i].hi >> 9) & 3) == 1)
            position = true;
    pixel = !vertex && !position;
    return slot * 3;
}

uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 0xCBF29CE484222325ull)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Every dword is a candidate, also shaders that start with a loop, call or jump.
std::vector<regpu::ScannedShader> Scan(const uint8_t* bytes, size_t size)
{
    std::vector<regpu::ScannedShader> shaders;
    std::unordered_set<uint64_t> seen;
    const size_t words = size / 4;
    for (size_t i = 0; i + 4 <= words; i++)
    {
        const uint32_t w0 = Be(bytes + i * 4), w1 = Be(bytes + i * 4 + 4);
        const uint32_t op = (w1 >> 12) & 0xF, addr = w0 & 0xFFF;
        if (!((IsExecOp(op) && addr >= 1 && addr <= 64) || (op == 12 && !(w0 & 0xFFFFFFF8)) || op == 7 || op == 9 || op == 11))
            continue;
        bool pixel;
        const uint32_t used = ShaderAt(bytes, words, i, pixel);
        if (!used)
            continue;
        const uint8_t* code = bytes + i * 4;
        if (!seen.insert(Fnv(code, used * 4) ^ (pixel ? 1 : 0)).second)
            continue;
        regpu::ScannedShader s{ pixel, std::vector<uint32_t>(used) };
        memcpy(s.code.data(), code, used * 4);
        shaders.push_back(std::move(s));
    }
    return shaders;
}

constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kFileMagic = 0x53535250; // 'PRSS'
constexpr uint32_t kFileVersion = 1;

// The loaded image (its PE header's SizeOfImage), capped at 64 MB.
size_t ImageSize()
{
    const uint8_t* mz = GuestPtr<uint8_t>(kImageBase);
    if (mz[0] != 'M' || mz[1] != 'Z')
        return 0;
    const uint8_t* pe = mz + Le(mz + 0x3C);
    return pe[0] == 'P' && pe[1] == 'E' ? std::min<size_t>(Le(pe + 0x50), 64u << 20) : 0;
}

// Which image the saved list came from (the title update patches it): a 64-byte sample of every 4 KB.
uint32_t ImageSignature()
{
    const size_t size = ImageSize();
    const uint8_t* image = GuestPtr<uint8_t>(kImageBase);
    uint64_t h = Fnv(reinterpret_cast<const uint8_t*>(&size), sizeof(size));
    for (size_t at = 0; at + 64 <= size; at += 4096)
        h = Fnv(image + at, 64, h);
    return uint32_t(h ^ (h >> 32));
}

std::filesystem::path SavedPath() { return UserDir() / "static_shaders.bin"; }

bool LoadSaved(std::vector<regpu::ScannedShader>& out, uint32_t signature)
{
    std::ifstream in(SavedPath(), std::ios::binary);
    uint32_t head[4];
    if (!in.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] != kFileMagic || head[1] != kFileVersion || head[3] != signature)
        return false;
    out.resize(head[2]);
    for (auto& s : out)
    {
        uint32_t meta[2];
        if (!in.read(reinterpret_cast<char*>(meta), sizeof(meta)) || meta[1] > 0x2000)
            return false;
        s.pixel = meta[0] != 0;
        s.code.resize(meta[1]);
        if (!in.read(reinterpret_cast<char*>(s.code.data()), std::streamsize(meta[1] * 4)))
            return false;
    }
    return true;
}

void Save(const std::vector<regpu::ScannedShader>& shaders, uint32_t signature)
{
    std::ofstream out(SavedPath(), std::ios::binary);
    const uint32_t head[4] = { kFileMagic, kFileVersion, uint32_t(shaders.size()), signature };
    out.write(reinterpret_cast<const char*>(head), sizeof(head));
    for (const auto& s : shaders)
    {
        const uint32_t meta[2] = { s.pixel ? 1u : 0u, uint32_t(s.code.size()) };
        out.write(reinterpret_cast<const char*>(meta), sizeof(meta));
        out.write(reinterpret_cast<const char*>(s.code.data()), std::streamsize(s.code.size() * 4));
    }
}
} // namespace

bool StaticShadersSaved()
{
    std::ifstream in(SavedPath(), std::ios::binary);
    uint32_t head[4];
    return in.read(reinterpret_cast<char*>(head), sizeof(head)) && head[0] == kFileMagic && head[1] == kFileVersion &&
           head[3] == ImageSignature();
}

std::vector<regpu::ScannedShader> StaticShaders()
{
    std::vector<regpu::ScannedShader> result;
    const uint32_t signature = ImageSignature();
    if (LoadSaved(result, signature))
    {
        Log("static shaders: %zu loaded from %s", result.size(), SavedPath().string().c_str());
        return result;
    }
    const auto start = std::chrono::steady_clock::now();
    const size_t size = ImageSize();
    result = Scan(GuestPtr<uint8_t>(kImageBase), size);
    Save(result, signature);
    Log("static shaders: %zu found in the executable (%zu MB) in %.1f s", result.size(), size >> 20,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return result;
}
