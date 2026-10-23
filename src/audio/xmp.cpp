// XMP, the console's media player (message app 0xFA), as a title uses it for its own music.
//
// PGR4's in-race music is a title playlist: the game gives XMP the song files (.wma, ASF
// container, WMA v2), registers a capture callback (XMPCaptureOutput, title_render=1) and the
// player hands it the decoded audio, which the game submits to a voice of its own and mixes with
// its volume settings. The callback takes (pcm, context) and submits 2048 bytes to that voice:
// 256 frames (5.33 ms, one frame of the game's mixer) of stereo 32-bit float, big-endian, 48 kHz,
// interleaved (L R L R ...; PGR4_XMP_PLANAR=1 for the A/B that proved planar wrong).
// (16-bit PCM was tried first: the mixer read it as floats, values around 1e38.)
//
// Message ids and argument layouts are Xenia's xmp_app.cc, checked against what the game sends
// (PGR4_TRACE_XMP): client, then the message's own fields, as big-endian dwords at `args`.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "kernel/imports.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

namespace
{
constexpr uint32_t kOk = 0, kFail = 0x80004005;
constexpr uint32_t kRate = 48000, kCaptureFrames = 256, kCaptureBytes = kCaptureFrames * 8;
constexpr size_t kRingLimit = size_t(kRate) * 2 * 2; // two seconds of stereo samples

enum State : uint32_t { kIdle = 0, kPlaying = 1, kPaused = 2 };

struct Song
{
    std::string path; // guest
    std::string title, artist, album;
    uint32_t handle = 0;
};

struct Playlist
{
    uint32_t handle = 0;
    std::string name;
    std::vector<Song> songs;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::map<uint32_t, Playlist> g_playlists;
uint32_t g_nextPlaylist = 0xB1000001u, g_nextSong = 0xB2000001u;

// Playback.
State g_state = kIdle;
uint32_t g_playlist = 0;          // playlist being played
std::vector<size_t> g_order;      // song indices, in playing order
size_t g_position = 0;            // index into g_order
uint32_t g_mode = 0, g_repeat = 0, g_flags = 0; // 0 in order / 1 shuffle; 0 repeat the playlist / 1 no repeat
float g_volume = 1.0f;
uint32_t g_controller = 0, g_controllerLocked = 0;
std::atomic<uint32_t> g_generation{ 0 }; // bumped when the song to decode changes
std::deque<int16_t> g_ring;               // decoded, stereo, 48 kHz (host order)

// Capture callback (XMPCaptureOutput).
uint32_t g_callback = 0, g_callbackContext = 0, g_titleRender = 0;
std::shared_ptr<ThreadObject> g_captureThread;
std::once_flag g_decoderStarted;

bool Trace()
{
    static const bool on = getenv("PGR4_TRACE_XMP") != nullptr;
    return on;
}

std::string Wide(uint32_t address)
{
    std::string out;
    for (uint32_t i = 0; address && i < 520; i++)
    {
        const uint16_t c = __builtin_bswap16(*GuestPtr<uint16_t>(address + i * 2));
        if (!c)
            break;
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
        {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
        else
        {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// The song order for the playlist: as given, or shuffled (the current song first).
void BuildOrder(const Playlist& p, size_t first)
{
    g_order.clear();
    for (size_t i = 0; i < p.songs.size(); i++)
        g_order.push_back(i);
    if (g_mode == 1 && g_order.size() > 1)
    {
        static std::mt19937 rng{ std::random_device{}() };
        std::shuffle(g_order.begin(), g_order.end(), rng);
    }
    for (size_t i = 0; i < g_order.size(); i++)
        if (g_order[i] == first)
        {
            std::swap(g_order[0], g_order[i]);
            break;
        }
    g_position = 0;
}

const Song* CurrentSong()
{
    const auto it = g_playlists.find(g_playlist);
    if (it == g_playlists.end() || g_position >= g_order.size() || g_order[g_position] >= it->second.songs.size())
        return nullptr;
    return &it->second.songs[g_order[g_position]];
}

// Starts decoding the current song from its beginning (caller holds the lock).
void StartCurrent()
{
    g_ring.clear();
    g_generation++;
    g_wake.notify_all();
}

// The song after this one, or false when the playlist ends (no repeat). Caller holds the lock.
bool Advance(int step)
{
    if (g_order.empty())
        return false;
    const int64_t next = int64_t(g_position) + step;
    if (next < 0)
        g_position = g_order.size() - 1;
    else if (size_t(next) >= g_order.size())
    {
        if (g_repeat == 1)
            return false;
        g_position = 0;
    }
    else
        g_position = size_t(next);
    return true;
}

// The decoder thread: reads the current song (ASF, WMA v2) and keeps the ring full.
void DecoderMain()
{
    SetThreadDescription(GetCurrentThread(), L"xmp decoder");
    AVFormatContext* fmt = nullptr;
    AVCodecContext* av = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint32_t openGeneration = ~0u;
    int stream = -1;
    std::vector<int16_t> out;

    auto close = [&] {
        swr_free(&swr);
        avcodec_free_context(&av);
        avformat_close_input(&fmt);
        stream = -1;
    };

    for (;;)
    {
        std::string path;
        uint32_t generation;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait_for(lock, std::chrono::milliseconds(20), [&] { return g_state == kPlaying && g_ring.size() < kRingLimit; });
            if (g_state != kPlaying)
            {
                if (g_state == kIdle && fmt)
                    close();
                continue;
            }
            if (g_ring.size() >= kRingLimit)
                continue;
            generation = g_generation.load();
            if (generation != openGeneration || !fmt)
            {
                close();
                openGeneration = generation;
                const Song* song = CurrentSong();
                if (!song)
                {
                    g_state = kIdle;
                    continue;
                }
                path = song->path;
            }
        }

        if (!path.empty())
        {
            std::filesystem::path host;
            bool readOnly = false;
            bool ok = VfsResolve(path, host, &readOnly);
            ok = ok && avformat_open_input(&fmt, reinterpret_cast<const char*>(host.u8string().c_str()), nullptr, nullptr) >= 0;
            ok = ok && avformat_find_stream_info(fmt, nullptr) >= 0;
            if (ok)
            {
                stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
                const AVCodec* codec = stream >= 0 ? avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id) : nullptr;
                ok = codec && (av = avcodec_alloc_context3(codec)) && avcodec_parameters_to_context(av, fmt->streams[stream]->codecpar) >= 0 &&
                    avcodec_open2(av, codec, nullptr) >= 0;
            }
            if (ok)
            {
                AVChannelLayout stereo{};
                av_channel_layout_default(&stereo, 2);
                ok = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, int(kRate), &av->ch_layout, av->sample_fmt, av->sample_rate, 0, nullptr) >= 0 &&
                    swr_init(swr) >= 0;
            }
            if (Trace())
                Log("xmp: playing %s -> %s (%s)", path.c_str(), host.string().c_str(), ok ? "opened" : "FAILED to open");
            if (!ok)
            {
                close();
                // Skip an unplayable song rather than stalling the playlist.
                std::lock_guard lock(g_mutex);
                if (g_generation.load() == openGeneration && !Advance(1))
                    g_state = kIdle;
                else
                    g_generation++;
                continue;
            }
        }

        // One packet.
        const int read = av_read_frame(fmt, packet);
        if (read < 0)
        {
            // The end of the song: the next one.
            close();
            std::lock_guard lock(g_mutex);
            if (g_generation.load() == openGeneration)
            {
                if (Advance(1))
                {
                    g_generation++;
                    if (Trace())
                        Log("xmp: song finished, next");
                }
                else
                {
                    g_state = kIdle;
                    if (Trace())
                        Log("xmp: playlist finished");
                }
            }
            continue;
        }
        if (packet->stream_index == stream && avcodec_send_packet(av, packet) >= 0)
            while (avcodec_receive_frame(av, frame) == 0)
            {
                const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
                out.resize(size_t(maxOut) * 2);
                uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());
                const int n = swr_convert(swr, &dst, maxOut, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                if (n > 0)
                {
                    std::lock_guard lock(g_mutex);
                    if (g_generation.load() == openGeneration)
                        g_ring.insert(g_ring.end(), out.begin(), out.begin() + ptrdiff_t(n) * 2);
                }
                av_frame_unref(frame);
            }
        av_packet_unref(packet);
    }
}

// The capture thread: one buffer per 5.33 ms of playback, to the game's callback.
void CaptureMain(PPCContext& ctx)
{
    using Clock = std::chrono::steady_clock;
    // The callback submits the buffer to the game's voice, which keeps the pointer and plays it
    // later (no copy): writing the next 5.33 ms into the same buffer overwrote audio still queued,
    // and the music sounded crunchy. A ring of buffers, far deeper than the voice's queue.
    static const uint32_t kBuffers = getenv("PGR4_XMP_ONE_BUFFER") ? 1 : 64;
    const uint32_t buffers = GuestAlloc(Heap::Virtual, kCaptureBytes * kBuffers, 16);
    uint32_t index = 0;
    auto next = Clock::now();
    for (;;)
    {
        const uint32_t buffer = buffers + kCaptureBytes * (index++ % kBuffers);
        next += std::chrono::microseconds(1000000ull * kCaptureFrames / kRate);
        std::this_thread::sleep_until(next);
        uint32_t callback, context;
        bool deliver = false;
        float volume;
        {
            std::lock_guard lock(g_mutex);
            callback = g_callback;
            context = g_callbackContext;
            volume = g_volume;
            if (g_state == kPlaying && callback)
            {
                // Whatever is decoded; silence for the rest (the start of a song, a slow disc).
                // Interleaved L R L R ...
                static const bool interleaved = getenv("PGR4_XMP_PLANAR") == nullptr; // measured: planar gives L/R correlation 0.95 (wrong), interleaved 0.48
                uint32_t* out = GuestPtr<uint32_t>(buffer);
                for (uint32_t i = 0; i < kCaptureFrames; i++)
                    for (uint32_t c = 0; c < 2; c++)
                    {
                        float v = 0.0f;
                        if (!g_ring.empty())
                        {
                            v = float(g_ring.front()) * (volume / 32768.0f);
                            g_ring.pop_front();
                        }
                        uint32_t bits;
                        memcpy(&bits, &v, 4);
                        out[interleaved ? i * 2 + c : c * kCaptureFrames + i] = __builtin_bswap32(bits);
                    }
                deliver = true;
                g_wake.notify_all();
            }
        }
        if (deliver)
        {
            if (Trace())
            {
                // The level and the buffers delivered, once a second.
                static uint32_t calls = 0, silent = 0;
                static float peak = 0;
                static auto last = Clock::now();
                const uint32_t* w = GuestPtr<uint32_t>(buffer);
                float p = 0;
                for (uint32_t i = 0; i < kCaptureFrames * 2; i++)
                {
                    const uint32_t bits = __builtin_bswap32(w[i]);
                    float v;
                    memcpy(&v, &bits, 4);
                    p = std::max(p, std::abs(v));
                }
                calls++;
                silent += p == 0;
                peak = std::max(peak, p);
                if (Clock::now() - last >= std::chrono::seconds(1))
                {
                    size_t ringSize;
                    {
                        std::lock_guard lock(g_mutex);
                        ringSize = g_ring.size();
                    }
                    Log("xmp: delivered %u buffers in the last second (%u silent), peak %.3f, ring %zu samples", calls, silent, peak, ringSize);
                    calls = silent = 0;
                    peak = 0;
                    last = Clock::now();
                }
            }
            ctx.r3.u64 = buffer;
            ctx.r4.u64 = context;
            GuestCall(ctx, callback);
        }
        // Drifting behind (the machine stalled): do not fire a burst to catch up.
        if (Clock::now() - next > std::chrono::milliseconds(200))
            next = Clock::now();
    }
}

void EnsureThreads()
{
    std::call_once(g_decoderStarted, [] { std::thread(DecoderMain).detach(); });
    if (!g_captureThread)
        g_captureThread = CreateHostGuestThread("xmp capture", CaptureMain);
}

uint32_t Arg(uint32_t args, int index) { return LoadBE32(args + uint32_t(index) * 4); }

// Looks a playlist up by what Play / Delete carry: its handle, or a pointer to it.
uint32_t ResolvePlaylist(uint32_t value)
{
    if (g_playlists.count(value))
        return value;
    if (value > 0x10000)
        if (const uint32_t pointed = LoadBE32(value); g_playlists.count(pointed))
            return pointed;
    return 0;
}
}

// A message to the media player. `args` is the message's dwords (big-endian, the first is the
// client: 0 dash, 2 the game); results go to the pointers inside it.
uint32_t XmpMessage(uint32_t message, uint32_t args, uint32_t length)
{
    std::unique_lock lock(g_mutex);
    switch (message)
    {
    case 0x00070009: // XMPGetStatus (client, state*)
        StoreIfNonNull(Arg(args, 1), g_state);
        return kOk;
    case 0x0007000B: // XMPGetVolume (client, float*)
    {
        uint32_t bits;
        memcpy(&bits, &g_volume, 4);
        StoreIfNonNull(Arg(args, 1), bits);
        return kOk;
    }
    case 0x0007000C: // XMPSetVolume (client, float)
    {
        const uint32_t bits = Arg(args, 1);
        float v;
        memcpy(&v, &bits, 4);
        if (!(v >= 0.0f && v <= 1.0f))
            return 0x80070057; // E_INVALIDARG
        g_volume = v;
        return kOk;
    }
    case 0x0007001B: // XMPGetPlaybackController (client, controller*, locked*)
        StoreIfNonNull(Arg(args, 1), g_controller);
        StoreIfNonNull(Arg(args, 2), g_controllerLocked);
        return kOk;
    case 0x0007001A: // XMPSetPlaybackController (client, request, locked)
        if (Arg(args, 1) != 4) // not "restore"
            g_controller = Arg(args, 1);
        else
            g_controller = 0;
        g_controllerLocked = Arg(args, 2);
        return kOk;
    case 0x00070008: // XMPSetPlaybackBehavior (client, mode, repeat, flags)
        g_mode = Arg(args, 1);
        g_repeat = Arg(args, 2);
        g_flags = Arg(args, 3);
        if (Trace())
            Log("xmp: behavior mode %u repeat %u flags %u", g_mode, g_repeat, g_flags);
        return kOk;
    case 0x00070029: // XMPGetPlaybackBehavior (client, mode*, repeat*, flags*)
        StoreIfNonNull(Arg(args, 1), g_mode);
        StoreIfNonNull(Arg(args, 2), g_repeat);
        StoreIfNonNull(Arg(args, 3), g_flags);
        return kOk;
    case 0x0007003D: // XMPCaptureOutput (client, callback, context, title_render)
        g_callback = Arg(args, 1);
        g_callbackContext = Arg(args, 2);
        g_titleRender = Arg(args, 3);
        Log("xmp: capture callback %08X context %08X (title render %u)", g_callback, g_callbackContext, g_titleRender);
        lock.unlock();
        EnsureThreads();
        return kOk;
    case 0x0007002E: // XMPGetTitlePlaylistBufferSize (client, song count, size*)
        StoreIfNonNull(Arg(args, 2), Arg(args, 1) * 0x3E8 + 0x88);
        return kOk;
    case 0x0007000D: // XMPCreateTitlePlaylist (client, storage, ?, songs*, count, name*, flags, handles*, playlist handle*)
    {
        Playlist p;
        p.handle = g_nextPlaylist++;
        p.name = Wide(Arg(args, 5));
        const uint32_t songs = Arg(args, 3), count = Arg(args, 4), handles = Arg(args, 7);
        for (uint32_t i = 0; i < count && i < 1000; i++)
        {
            const uint32_t d = songs + i * 36; // path, title, artist, album, album artist, genre, track, duration, format
            Song s;
            s.path = Wide(LoadBE32(d));
            s.title = Wide(LoadBE32(d + 4));
            s.artist = Wide(LoadBE32(d + 8));
            s.album = Wide(LoadBE32(d + 12));
            s.handle = g_nextSong++;
            if (handles)
                StoreBE32(handles + i * 4, s.handle);
            p.songs.push_back(std::move(s));
        }
        StoreIfNonNull(Arg(args, 8), p.handle);
        if (Trace())
            Log("xmp: playlist %08X '%s': %zu songs, first %s", p.handle, p.name.c_str(), p.songs.size(), p.songs.empty() ? "-" : p.songs[0].path.c_str());
        g_playlists[p.handle] = std::move(p);
        return kOk;
    }
    case 0x00070013: // XMPDeleteTitlePlaylist (client, playlist)
    {
        const uint32_t h = ResolvePlaylist(Arg(args, 1));
        if (h == g_playlist)
        {
            g_state = kIdle;
            g_playlist = 0;
            g_order.clear();
            g_ring.clear();
        }
        g_playlists.erase(h);
        return kOk;
    }
    case 0x00070031: // XMPGetNumSongsInTitlePlaylist (client, playlist, count*)
    {
        const auto it = g_playlists.find(ResolvePlaylist(Arg(args, 1)));
        StoreIfNonNull(Arg(args, 2), it == g_playlists.end() ? 0 : uint32_t(it->second.songs.size()));
        return kOk;
    }
    case 0x00070002: // XMPPlayTitlePlaylist (client, playlist, song handle)
    {
        static const bool noMusic = getenv("PGR4_NO_XMP_MUSIC") != nullptr; // A/B: the player never plays
        if (noMusic)
            return kFail;
        const uint32_t h = ResolvePlaylist(Arg(args, 1));
        const auto it = g_playlists.find(h);
        if (it == g_playlists.end() || it->second.songs.empty())
        {
            if (Trace())
                Log("xmp: play: no playlist for %08X", Arg(args, 1));
            return kFail;
        }
        size_t first = 0;
        for (size_t i = 0; i < it->second.songs.size(); i++)
            if (it->second.songs[i].handle == Arg(args, 2))
                first = i;
        g_playlist = h;
        BuildOrder(it->second, first);
        g_state = kPlaying;
        StartCurrent();
        EnsureThreads();
        if (Trace())
            Log("xmp: play playlist %08X from song %zu", h, first);
        return kOk;
    }
    case 0x00070003: // XMPContinue
        if (g_state == kPaused)
            g_state = kPlaying;
        g_wake.notify_all();
        return kOk;
    case 0x00070005: // XMPPause
        if (g_state == kPlaying)
            g_state = kPaused;
        return kOk;
    case 0x00070004: // XMPStop (client, no wait)
        g_state = kIdle;
        g_ring.clear();
        g_generation++;
        return kOk;
    case 0x00070006: // XMPNext
    case 0x00070007: // XMPPrevious
        if (g_state != kIdle && Advance(message == 0x00070006 ? 1 : -1))
            StartCurrent();
        return kOk;
    case 0x0007000E: // XMPGetCurrentSong (client, info*, ?)
        return g_state == kIdle ? kFail : kOk;
    default:
        static std::atomic<int> logged{ 0 };
        if (logged++ < 20)
            Log("xmp: message %08X (%u bytes) answered with success", message, length);
        return kOk;
    }
}
