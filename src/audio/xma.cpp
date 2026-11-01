// XMA decoder contexts. The title (FMOD) drives the console's hardware decoder
// through the kernel's XMA* exports, which edit a 64-byte context in guest
// memory: two input buffers of 2 KB XMA packets, an output ring of 256-byte
// blocks of 16-bit big-endian PCM, loop points. Each context gets an FFmpeg
// xma2 decoder fed one packet at a time. Like the hardware it runs ahead
// whenever there is output space; the consumer sets the pace.
//
// XMA_CONTEXT_DATA, big-endian dwords, bit fields counted from bit 0:
//   0: in0 packet count 0-11, loop count 12-19, in0 valid 20, in1 valid 21,
//      output block count 22-26, output write offset 27-31
//   1: in1 packet count 0-11, loop subframe end 14-16, skip 17-19,
//      subframe decode count 20-23, sample rate 27-28, stereo 29,
//      output valid 31
//   2: input read offset (bits) 0-25
//   3: loop start 0-25      4: loop end 0-25, current input buffer 31
//   5: in0 address  6: in1 address  7: output address  8: work buffer
//   9: output read offset 0-4
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <vector>

#include <windows.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"
#include "audio/xma_trims.h"


namespace
{
constexpr uint32_t kContextSize = 0x40;
constexpr uint32_t kPacketBits = 2048 * 8;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kPacketBytes = 2048;

const bool g_traceXma = getenv("PGR4_TRACE_XMA") != nullptr;

// The buffer addresses in a context. XMAInitializeContext stores guest virtual addresses (FM2's
// library), but the title's own XMA library (PGR4, through the decoder's registers) stores
// PHYSICAL addresses, as the console's kernel does: below 0x20000000, the 0xA0000000 view.
uint32_t BufferAddress(uint32_t address)
{
    return address && address < 0x20000000 ? address + 0xA0000000 : address;
}

uint32_t Field(uint32_t ctx, int dword, int lo, int bits)
{
    return (LoadBE32(ctx + dword * 4) >> lo) & ((1u << bits) - 1);
}

void SetField(uint32_t ctx, int dword, int lo, int bits, uint32_t value)
{
    const uint32_t mask = ((1u << bits) - 1) << lo;
    const uint32_t a = ctx + dword * 4;
    StoreBE32(a, (LoadBE32(a) & ~mask) | ((value << lo) & mask));
}

std::mutex g_mutex;
// PGR4_TRACE_XMA_EVENTS=1: every loop wrap, new stream (clear) and starvation, timestamped (to line up with PGR4_DUMP_AUDIO).
const bool g_traceEvents = getenv("PGR4_TRACE_XMA_EVENTS") != nullptr;
std::atomic<double> g_maxTickGapMs{ 0 }, g_maxTitleWaitMs{ 0 }, g_slowestTakeMs{ 0 }; // PGR4_TRACE_XMA_TIMING
std::atomic<uint32_t> g_codecsTaken{ 0 }, g_codecsMissed{ 0 }, g_kicks{ 0 }, g_kicksEmpty{ 0 }, g_kicksLow{ 0 }, g_ringBlocksMin{ 99 }, g_ringBlocksMax{ 0 };

// Music packets, by a hash of their first 64 bytes: the title reads its music files (Media/Audio/Music/*.xma)
// into a file buffer and copies packets on to the decoder's input, so a packet is recognised by
// its contents (AudioNoteMusicRead). The "whysthisdealer" easter egg silences music while it plays.
std::mutex g_musicMutex;
std::set<uint64_t> g_musicPackets;
uint64_t PacketHash(const uint8_t* p)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 64; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
bool MusicPacket(const uint8_t* p)
{
    std::lock_guard lock(g_musicMutex);
    return g_musicPackets.count(PacketHash(p)) != 0;
}
std::set<uint32_t> g_enabled;
std::once_flag g_started;

// The hardware side (as Xenia's and ReXGlue's XmaDecoder): titles that use the XDK's XMA playback
// library (PGR4) only create contexts through the kernel, then edit them in memory and start,
// stop and reset them with the decoder's registers, one bit per hardware ID, 32 IDs a register.
// The contexts are slots of one array, allocated at startup, whose physical address is in
// ContextArrayAddress: the library reads it once when it starts (sub_82914398) and takes a
// context's ID as its index in the array.
constexpr uint32_t kHwContexts = 320; // Xenia's kContextCount
constexpr uint32_t kRegArray = 0x7FEA1800, kRegKick = 0x7FEA1940, kRegLock = 0x7FEA1A40, kRegClear = 0x7FEA1A80; // 0600h, 0650h, 0690h, 06A0h (Xenia's xma_register_table.inc)
uint32_t g_array = 0;
bool g_slotUsed[kHwContexts] = {};
// The hardware path (registers, as the console): how PGR4's XMA playback library works. On by
// default. PGR4_NO_XMA_HW=1 brings back the kernel-call path of FM2's library (silent in PGR4).
const bool g_hwPath = getenv("PGR4_NO_XMA_HW") == nullptr;

// Bits of an XMA stream, MSB first.
uint32_t ReadBits(const uint8_t* data, uint32_t bit, uint32_t count)
{
    const uint32_t byte = bit >> 3;
    const uint64_t window = (uint64_t(data[byte]) << 32) | (uint64_t(data[byte + 1]) << 24) | (uint64_t(data[byte + 2]) << 16) | (uint64_t(data[byte + 3]) << 8) | data[byte + 4];
    return uint32_t((window >> (40 - (bit & 7) - count)) & ((1ull << count) - 1));
}

// The frames of one 2 KB packet (positions in bits within the packet, headers included): the
// header gives how many frames start in it (6 bits) and where the first starts (15 bits, after
// the 32-bit header); each frame begins with its own length in bits (15 bits, headers of later
// packets excluded when it runs over). Returns how many frames start here; `ends` gets, for each,
// its end position relative to the packet start, over 16384 when it continues in the next packet.
// FFmpeg's XMA2 output is the sequence of the frames fed, 512 samples each, from the first frame that
// starts in the first packet fed (a frame carried over into it is skipped), held back about 4.7k samples
// until more input comes; nothing is trimmed at its start (measured: scratchpad frames.cpp, correlation
// 1.0000 at every packet tried). The console's decoded sample for frame F, subframe s is F * 512 + s * 128
// of a stream; it equals that sequence's sample F * 512 + s * 128 - kHwLead (the FM2 port's XBDM
// measurement: hardware output = FFmpeg's from sample 128 at a start with trim_start 512). Taken as
// equal, a loop (frame 0 subframe 3 to frame 377 subframe 3 on PGR4's engine layers) played the last
// frame's padding, 384 samples of silence, and skipped as much real audio, every pass of every layer:
// the engine note stopping for ~10 ms every loop at high revs.
constexpr uint64_t kHwLead = 384;

// The frames that start in a packet (after the bits carried over from the packet before, header bits 11-25),
// as FFmpeg decodes them: up to the frame whose last bit ends the list. The header's top 6 bits are no frame count in PGR4's
// streams (the menu music's step 2, 6, 10 ... 62 packet after packet: a limit of 2 lost 5 of a packet's 7
// frames), and a frame whose 15-bit length straddles the packet's end starts in it too (pgr4-native's patch
// 41). runsOver: the last one continues in the next packet (its length straddles: 16384).
uint32_t ParsePacket(const uint8_t* packet, uint32_t* starts, uint32_t* lengths, uint32_t maxFrames, bool* runsOver = nullptr)
{
    const uint32_t header = (uint32_t(packet[0]) << 24) | (uint32_t(packet[1]) << 16) | (uint32_t(packet[2]) << 8) | packet[3];
    uint32_t pos = 32 + ((header >> 11) & 0x7FFF);
    uint32_t n = 0;
    bool over = false;
    while (pos < 16384 && n < maxFrames)
    {
        starts[n] = pos;
        if (pos + 15 > 16384)
        {
            lengths[n++] = 16384;
            over = true;
            break;
        }
        const uint32_t length = ReadBits(packet, pos, 15);
        if (!length || length == 0x7FFF)
            break;
        lengths[n++] = length;
        pos += length;
        over = pos > 16384;
        // A frame's last bit says whether another follows in the packet (WMA Pro's more_frames, as FFmpeg reads
        // it): after the last, a stream's end packet holds padding, read as frames 12-21k bits long before.
        if (!over && !ReadBits(packet, pos - 1, 1))
            break;
    }
    if (runsOver)
        *runsOver = over;
    return n;
}

AVCodecContext* OpenCodec(uint32_t ch, uint32_t hz)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext* av = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!av)
        return nullptr;
    av->sample_rate = int(hz);
    av_channel_layout_default(&av->ch_layout, int(ch));
    av->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX (34 bytes): one stream; FFmpeg reads the stream count from it.
    av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    av->extradata_size = 34;
    av->extradata[0] = 1;
    if (avcodec_open2(av, codec, nullptr) < 0)
        avcodec_free_context(&av);
    return av;
}

// Codecs are opened ahead and freed by a thread of their own (as the FM2 port does). Every new
// stream needs a fresh codec (a cleared context re-opens it: FFmpeg's flush keeps the last frame's
// overlap), and opening one takes 2-3 ms on the FX. That ran on the decoder thread holding g_mutex,
// which the title's audio thread waits on at its next decoder register write: driving, with 15-57
// sound starts a second (gears, exhaust pops), every other stream's decoding and the title's mixer
// stalled for it. A stream that needs a codec when none is ready waits for the next pass (2 ms; the
// pool is empty, one is opened in place as before. PGR4_XMA_NO_CODEC_POOL=1: always opened in place.
constexpr size_t kPooledCodecs = 8;
std::mutex g_poolMutex;
std::condition_variable g_poolWake;
std::map<std::pair<uint32_t, uint32_t>, std::vector<AVCodecContext*>> g_codecPool;
std::vector<AVCodecContext*> g_retiredCodecs;

bool NoCodecPool()
{
    static const bool off = getenv("PGR4_XMA_NO_CODEC_POOL") != nullptr;
    return off;
}

void CodecPoolLoop()
{
    SetThreadDescription(GetCurrentThread(), L"xma codecs");
    std::unique_lock lock(g_poolMutex);
    for (;;)
    {
        g_poolWake.wait_for(lock, std::chrono::milliseconds(50));
        std::vector<AVCodecContext*> retired;
        retired.swap(g_retiredCodecs);
        std::vector<std::pair<uint32_t, uint32_t>> wanted;
        for (const auto& [key, pool] : g_codecPool)
            for (size_t i = pool.size(); i < kPooledCodecs; i++)
                wanted.push_back(key);
        lock.unlock();
        for (AVCodecContext* av : retired)
            avcodec_free_context(&av);
        std::vector<std::pair<std::pair<uint32_t, uint32_t>, AVCodecContext*>> opened;
        for (const auto& key : wanted)
            if (AVCodecContext* av = OpenCodec(key.first, key.second))
                opened.push_back({ key, av });
        lock.lock();
        for (const auto& [key, av] : opened)
            g_codecPool[key].push_back(av);
    }
}

// A codec for ch/hz: a pooled one, or one opened now.
AVCodecContext* TakeCodec(uint32_t ch, uint32_t hz)
{
    if (NoCodecPool())
        return OpenCodec(ch, hz);
    static std::once_flag started;
    std::call_once(started, [] { std::thread(CodecPoolLoop).detach(); });
    std::lock_guard lock(g_poolMutex);
    auto& pool = g_codecPool[{ ch, hz }]; // (a new entry: the pool thread keeps it filled)
    if (pool.empty())
    {
        // None ready: opened here, as before the pool (the stream starts now, as on the hardware; the
        // pool thread refills for the next).
        g_poolWake.notify_one();
        return OpenCodec(ch, hz);
    }
    AVCodecContext* av = pool.back();
    pool.pop_back();
    g_poolWake.notify_one();
    return av;
}

void RetireCodec(AVCodecContext*& av)
{
    if (!av)
        return;
    if (NoCodecPool())
        return avcodec_free_context(&av);
    std::lock_guard lock(g_poolMutex);
    g_retiredCodecs.push_back(av);
    av = nullptr;
}

struct Decoder
{
    AVCodecContext* av = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> pcm; // decoded, interleaved, not yet in the output ring
    size_t pcmRead = 0;
    bool flush = true; // start of a stream or a jump: reset before the next packet
    bool reopen = false; // a new logical stream (the context was cleared): drop the codec's history too
    bool starved = false;
    size_t packetsDecoded = 0, framesWritten = 0, framesDecoded = 0;
    // An input buffer whose packets are all decoded stays valid for the title until their audio
    // has reached the output ring (the hardware has no backlog, so the title takes "consumed"
    // to mean "delivered"); it would loop or stop a stream while the tail was still queued here.
    bool consumed[2] = {};
    bool holdsTail = false; // packets were decoded since the codec was last drained or reset
    bool seeked = false; // the title set the read offset since the last packet was decoded
    bool music = false;  // its last packet came from a music file (MusicPacket)
    uint32_t skipPending = 0; // samples per channel to drop from the next decoded output (a loop's subframe skip)
    bool looped = false; // it has wrapped (PGR4_DUMP_XMA)
    Clock::time_point starvedAt{}; // (PGR4_TRACE_XMA_EVENTS)
    size_t dumpPos = 0; // samples per channel written to its PGR4_DUMP_XMA file (never reset: event positions)
    // The stream from its start (a fresh codec) to its end, output as the hardware does (the FM2 port,
    // measured over XBDM): every frame played, trim_start dropped at the start, trim_end ignored:
    // frames * 512 - trim_start samples. FFmpeg keeps 128 of the start trim (a half-amplitude fade-in
    // blip at every engine voice restart, which FMOD does as the revs cross layers) and holds back
    // about a packet at the end (pushed out by feeding the first packet again).
    bool streamStart = false; // the next packet is a fresh codec's first
    uint32_t streamFrames = 0, trimStart = 0, trimEnd = 0, skipStart = 0;
    size_t streamOut = 0;
    bool wrapped = false; // it looped (its frame count no longer gives its length)
    bool midStart = false; // it started at a decode position inside the stream
    uint32_t startDrop = 0; // samples per channel dropped at its start (trim_start, or the position's)
    // Exact timing (every stream but music; PGR4_XMA_OLD_TIMING=1: the handling before it, A/B). The codec's
    // raw output, counted in rawOut, is the sequence of frames fed (fedFrames of them, see kHwLead); a
    // start and a stream end are cuts in it: only [dropUntil, cutAt) is kept, less each loop's seam (loopGaps:
    // from the loop end to the loop start of the packets fed after it). Every start takes a fresh codec, so the
    // sequence begins at a known frame; a loop keeps the codec, as the hardware does: the loop start frame
    // overlaps the loop end frame (a fresh codec per pass broke the menu music's seams, whose end frames are
    // not silent; the committed music fix's rule: never reset the decoder at a loop).
    bool exact = false;
    uint64_t rawOut = 0, fedFrames = 0, dropUntil = 0, cutAt = ~0ull, fedBefore = 0; // (fedBefore: frames fed before the last packet decoded)
    std::vector<std::pair<uint64_t, uint64_t>> loopGaps;
    // The sequence's frame count follows FFmpeg: a frame running on past its packet is decoded only when the next
    // packet fed carries its rest (header bits 11-25 above 0), else dropped (a loop end packet, then the loop start
    // packet: measured, the menu music's 85-packet loop is 710 frames, not 711).
    bool lastRunsOver = false;
    // A loop seam whose start is the loop start packet's frame `seamFrame`, subframe `seamSkip`: known once that
    // packet is fed (and the run-over frame before it is or isn't counted).
    bool seamPending = false;
    uint64_t seamEnd = 0;
    uint32_t seamFrame = 0, seamSkip = 0;
    // PGR4_DUMP_XMA: every packet fed to this context's codecs, out/xmain_<context>.bin (a packet of 0xFF marks
    // a fresh codec; a packet of 0xFE a pre-roll one, see Preroll), for decoding offline.
    uint32_t id = 0;
    FILE* inDump = nullptr;
    void DumpIn(const uint8_t* data, uint8_t marker = 0)
    {
        static const bool dumpXma = getenv("PGR4_DUMP_XMA") != nullptr;
        if (!dumpXma)
            return;
        if (!inDump)
        {
            char name[64];
            snprintf(name, sizeof(name), "out/xmain_%08X.bin", id);
            inDump = fopen(name, "ab");
        }
        if (!inDump)
            return;
        if (marker)
        {
            uint8_t m[kPacketBytes];
            memset(m, marker, sizeof(m));
            fwrite(m, 1, sizeof(m), inDump);
        }
        if (data)
            fwrite(data, 1, kPacketBytes, inDump);
    }
    uint32_t nextOffset = ~0u; // the read offset as this decoder last set it (another value: the title moved it)
    uint8_t firstPacket[kPacketBytes] = {};

    void Close()
    {
        RetireCodec(av);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    // A new logical stream (the context was cleared): avcodec_flush_buffers does not clear FFmpeg's
    // XMA overlap-add tail (the decoder has no flush callback), so the previous track's last frame
    // bled into the next one (a jarring cut at every song change). A fresh codec discards it (as
    // ReXGlue does). False when none is ready yet: decode next pass.
    bool PrepareStream()
    {
        if (!reopen)
            return true;
        static LiveSwitch noReopen("PGR4_NO_XMA_REOPEN");
        if (!noReopen && !Open(channels, rate))
            return false;
        reopen = false;
        flush = false;
        return true;
    }

    // A fresh codec for ch/hz (the old one retired); false when none is ready yet (try again next pass).
    bool Open(uint32_t ch, uint32_t hz)
    {
        const auto t0 = Clock::now();
        AVCodecContext* fresh = TakeCodec(ch, hz);
        if (!fresh)
        {
            g_codecsMissed++;
            return false;
        }
        g_codecsTaken++;
        g_slowestTakeMs = std::max(g_slowestTakeMs.load(), std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        RetireCodec(av);
        av = fresh;
        rawOut = fedFrames = dropUntil = 0;
        cutAt = ~0ull;
        loopGaps.clear();
        lastRunsOver = seamPending = false;
        streamStart = true;
        midStart = false;
        startDrop = 0;
        skipStart = 0;
        DumpIn(nullptr, 0xFF);
        if (!packet)
            packet = av_packet_alloc();
        if (!frame)
            frame = av_frame_alloc();
        channels = ch;
        rate = hz;
        return true;
    }

    // Feeds a packet for the codec's history only (a decode position's frame needs the frame before it
    // for its overlap, and a fresh codec drops its first frame): its output is discarded.
    size_t Preroll(const uint8_t* data)
    {
        DumpIn(data, 0xFE);
        if (av_new_packet(packet, kPacketBytes) < 0)
            return 0;
        memcpy(packet->data, data, kPacketBytes);
        avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        size_t out = 0;
        while (avcodec_receive_frame(av, frame) == 0)
        {
            out += size_t(frame->nb_samples);
            av_frame_unref(frame);
        }
        return out;
    }

    // Feeds a packet only to push out what the codec holds back (exact timing: its own frames lie past cutAt).
    void Push(const uint8_t* data)
    {
        if (av_new_packet(packet, kPacketBytes) < 0)
            return;
        memcpy(packet->data, data, kPacketBytes);
        avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        Receive();
    }

    // Appends what the codec has decoded, less the first skipStart samples of the stream.
    void Receive()
    {
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            // The samples of this frame kept: [from, to) ranges of it.
            std::pair<int, int> kept[8];
            int ranges = 0;
            if (exact)
            {
                const uint64_t a = rawOut, b = rawOut + uint64_t(n);
                uint64_t at = std::max(a, dropUntil);
                const uint64_t end = std::min(b, cutAt);
                for (const auto& [gapFrom, gapTo] : loopGaps)
                {
                    if (at >= end || ranges == 7)
                        break;
                    if (gapTo <= at || gapFrom >= end)
                        continue;
                    if (gapFrom > at)
                        kept[ranges++] = { int(at - a), int(gapFrom - a) };
                    at = std::max(at, gapTo);
                }
                if (at < end)
                    kept[ranges++] = { int(at - a), int(end - a) };
                rawOut = b;
                while (!loopGaps.empty() && loopGaps.front().second <= rawOut)
                    loopGaps.erase(loopGaps.begin());
            }
            else
            {
                const int skip = std::min<int>(n, int(skipStart));
                skipStart -= uint32_t(skip);
                kept[ranges++] = { skip, n };
            }
            for (int r = 0; r < ranges; r++)
            {
                const auto [from, to] = kept[r];
                const size_t base = pcm.size();
                pcm.resize(base + size_t(to - from) * channels);
                for (int c = 0; c < int(channels); c++)
                {
                    const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                    for (int i = from; i < to; i++)
                        pcm[base + size_t(i - from) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
                }
            }
            av_frame_unref(frame);
        }
    }

    // The end of a stream. The hardware plays frames * 512 - trim_start samples (see streamFrames);
    // FFmpeg's end of stream takes trim_start + trim_end - 192 off the end instead and holds back
    // about a packet until more input comes, so the first packet is fed again until the stream
    // reaches its length, and it is cut there (FM2's DrainExact). Music and looped streams (whose
    // frame count isn't their length) drain FFmpeg's way, as before. Then silence up to a whole
    // output block; the codec is opened again at the next stream. PGR4_XMA_PLAIN_DRAIN=1: the old
    // drain for every stream (A/B).
    void Drain(size_t block)
    {
        if (!av)
            return;
        const size_t pcmBefore = pcm.size();
        static LiveSwitch plain("PGR4_XMA_PLAIN_DRAIN");
        if (exact && !plain)
        {
            // The hardware plays every frame (trim_end ignored, FM2's XBDM measurement): the sequence up to its
            // last frame's end, less kHwLead. The first packet is fed again until that is out.
            cutAt = std::min(cutAt, fedFrames * 512 > kHwLead ? fedFrames * 512 - kHwLead : 0);
            for (int k = 0; k < 8 && rawOut < cutAt; k++)
                Push(firstPacket);
            reopen = true;
        }
        else if (plain || music || wrapped || !streamFrames)
        {
            avcodec_send_packet(av, nullptr);
            Receive();
        }
        else
        {
            static LiveSwitch noPreroll("PGR4_XMA_NO_PREROLL");
            const size_t drop = noPreroll ? trimStart : startDrop;
            const size_t want = size_t(streamFrames) * 512 - std::min<size_t>(size_t(streamFrames) * 512, drop);
            for (int k = 0; k < 8; k++)
            {
                // Twice at least: once was not always enough to push out what FFmpeg holds back.
                if (streamOut + (pcm.size() - pcmBefore) / channels >= want && k >= 2)
                    break;
                if (av_new_packet(packet, kPacketBytes) >= 0)
                {
                    memcpy(packet->data, firstPacket, kPacketBytes);
                    avcodec_send_packet(av, packet);
                    av_packet_unref(packet);
                }
                Receive();
            }
            streamOut += (pcm.size() - pcmBefore) / channels;
            if (streamOut > want)
            {
                const size_t cut = std::min(streamOut - want, (pcm.size() - pcmRead) / channels);
                pcm.resize(pcm.size() - cut * channels);
                streamOut -= cut;
            }
            reopen = true; // (the first packet fed again: a fresh codec for the next stream)
        }
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        if (const size_t rest = (pcm.size() - pcmRead) % block)
            pcm.resize(pcm.size() + (block - rest), 0);
        if (g_traceXma)
            Log("xma: drained %zu frames at the end of a stream", (pcm.size() - pcmBefore) / channels);
        flush = true;
        holdsTail = false;
    }

    // Decodes one 2 KB packet and appends its samples.
    void Decode(const uint8_t* data)
    {
        const bool wasFlush = flush;
        // (A new stream's codec, reopen, is taken in Step before this: PrepareStream.)
        if (flush)
        {
            avcodec_flush_buffers(av);
            flush = false;
        }
        if (streamStart)
        {
            streamStart = false;
            memcpy(firstPacket, data, kPacketBytes);
            streamFrames = trimStart = trimEnd = 0;
            streamOut = 0;
            wrapped = false;
            PacketTrims(data, channels, trimStart, trimEnd);
            // Without the pre-roll (PGR4_XMA_NO_PREROLL): FM2's rule, FFmpeg's output less 128 of the trim.
            static LiveSwitch noPreroll("PGR4_XMA_NO_PREROLL");
            skipStart = noPreroll && !music && !midStart ? std::min<uint32_t>(128, trimStart) : 0;
            if (!noPreroll)
                streamFrames = 0; // (counted from this packet on: the exact drain's length)
        }
        PacketTrims(data, channels, trimStart, trimEnd, &streamFrames); // later packets can carry the end trim
        const size_t pcmBefore = pcm.size();
        packetsDecoded++;
        if (av_new_packet(packet, kPacketBytes) < 0)
            return;
        memcpy(packet->data, data, kPacketBytes);
        DumpIn(data);
        const int sent = avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        if (sent < 0)
        {
            flush = true;
            return;
        }
        {
            uint32_t st[64], len[64];
            bool over = false;
            const uint32_t frames = ParsePacket(data, st, len, 64, &over);
            const uint32_t carried = ((uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | data[3]) >> 11) & 0x7FFF;
            if (lastRunsOver && !carried && fedFrames)
                fedFrames--; // (FFmpeg dropped the frame the last packet left unfinished)
            if (seamPending)
            {
                const uint64_t start = (fedFrames + seamFrame) * 512 + uint64_t(seamSkip) * 128 - kHwLead;
                if (start > seamEnd)
                    loopGaps.emplace_back(seamEnd, start);
                seamPending = false;
            }
            fedFrames += frames;
            lastRunsOver = over;
        }
        Receive();
        streamOut += (pcm.size() - pcmBefore) / channels;
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        holdsTail = true;
        if (g_traceXma)
        {
            Log("xma: decoded a packet: %zu frames (flush %d)", (pcm.size() - pcmBefore) / channels, int(wasFlush));
            // PGR4_TRACE_XMA_FRAMES=1: the packet's own frame structure against what the codec returned.
            static const bool traceFrames = getenv("PGR4_TRACE_XMA_FRAMES") != nullptr;
            static int shown = 0;
            if (traceFrames && shown < 120)
            {
                shown++;
                uint32_t st[64], len[64];
                const uint32_t n = ParsePacket(data, st, len, 64);
                uint32_t within = 0, over = 0;
                for (uint32_t i = 0; i < n; i++)
                    (st[i] + len[i] <= 16384 ? within : over)++;
                const uint32_t header = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
                Log("xma: frames: header count %u first at %u | parsed %u starting here (%u complete, %u run over) | codec returned %zu samples = %zu frames of 512", header >> 26,
                    32 + ((header >> 11) & 0x7FFF), n, within, over, (pcm.size() - pcmBefore) / channels, (pcm.size() - pcmBefore) / channels / 512);
            }
        }
    }
};

std::map<uint32_t, Decoder> g_decoders;
std::map<uint32_t, size_t> g_dumpPos; // PGR4_DUMP_XMA: samples per channel written per context (a release erases its decoder)

// The title refilled an input buffer: whatever "consumed" was still pending for it is over.
void ClearConsumed(uint32_t ctx, int dword, int bit)
{
    if (dword != 0 || (bit != 20 && bit != 21))
        return;
    if (auto it = g_decoders.find(ctx); it != g_decoders.end())
        it->second.consumed[bit - 20] = false;
}

// The output ring has a free block: fill it from decoded samples. Returns false when it
// has nothing decoded to give.
bool WriteBlock(uint32_t ctx, Decoder& d)
{
    const size_t perBlock = kBlockBytes / 2;
    if (d.pcm.size() - d.pcmRead < perBlock)
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5), write = Field(ctx, 0, 27, 5);
    uint16_t* out = GuestPtr<uint16_t>(BufferAddress(LoadBE32(ctx + 7 * 4)) + write * kBlockBytes);
    for (size_t i = 0; i < perBlock; i++)
        out[i] = __builtin_bswap16(uint16_t(d.pcm[d.pcmRead + i]));
    if (g_traceXma)
    {
        // PGR4_TRACE_XMA: the PCM handed to the title, per second (blocks written, loudest sample).
        static uint32_t blocksSeen = 0, peak = 0;
        static auto last = Clock::now();
        blocksSeen++;
        for (size_t i = 0; i < perBlock; i++)
            peak = std::max<uint32_t>(peak, uint32_t(std::abs(int(int16_t(d.pcm[d.pcmRead + i])))));
        if (Clock::now() - last >= std::chrono::seconds(1))
        {
            Log("xma: pcm out: %u blocks in the last second, peak %u", blocksSeen, peak);
            blocksSeen = peak = 0;
            last = Clock::now();
        }
    }
    // PGR4_DUMP_XMA=1: what each non-music context hands the title, as raw 16-bit PCM in
    // out/xma_<context>_<channels>ch.raw (its wraps logged as "xma wrap", by output sample; with
    // PGR4_TRACE_XMA_EVENTS each stream's start too).
    static const bool dumpXma = getenv("PGR4_DUMP_XMA") != nullptr, dumpMusic = getenv("PGR4_DUMP_XMA_MUSIC") != nullptr;
    if (dumpXma && (!d.music || dumpMusic))
    {
        static std::map<uint32_t, FILE*> files;
        FILE*& f = files[ctx];
        if (!f)
        {
            char name[64];
            snprintf(name, sizeof(name), "out/xma_%08X_%uch.raw", ctx, d.channels);
            f = fopen(name, "wb");
        }
        if (f)
        {
            fwrite(&d.pcm[d.pcmRead], 2, perBlock, f);
            g_dumpPos[ctx] += perBlock / std::max<uint32_t>(d.channels, 1);
        }
    }
    d.pcmRead += perBlock;
    d.framesWritten += perBlock / std::max<uint32_t>(d.channels, 1);
    if (d.pcmRead > (1u << 16))
    {
        d.pcm.erase(d.pcm.begin(), d.pcm.begin() + ptrdiff_t(d.pcmRead));
        d.pcmRead = 0;
    }
    SetField(ctx, 0, 27, 5, (write + 1) % blocks);
    // The ring is full: write == read, and the hardware clears the output buffer's valid bit to say so.
    // The title's XMA library reads write == read as the whole ring when that bit is clear, and nothing
    // when it is set (sub_82914978); each time it consumes it sets the bit again. One block kept free
    // instead left every engine layer 15/16 of a ring per mix update: FMOD's voice (XMADecoder::GetData,
    // sub_82806450) wants 1024 frames an update, more as the revs pitch it up, and fills what its buffers
    // lack with silence (loc_8280676C).
    // Not the default: on the FX it measured no less starvation (the title's voices only ran dry at the ends
    // of sounds either way) and the mix had more short dips with it.
    static LiveSwitch fullRing("PGR4_XMA_FULL_RING");
    if (fullRing && (write + 1) % blocks == Field(ctx, 9, 0, 5))
        SetField(ctx, 1, 31, 1, 0);
    return true;
}

// One decoder step for a context: fill one output block, decoding the next input packet
// when needed. Returns false when it has nothing to do.
bool Step(uint32_t ctx)
{
    if (!Field(ctx, 1, 31, 1)) // output buffer not valid
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5);
    if (!blocks)
        return false;
    const uint32_t write = Field(ctx, 0, 27, 5), read = Field(ctx, 9, 0, 5);
    // Keep one block free so a full ring never reads as empty. PGR4_XMA_FULL_RING=1 (A/B): fill it the
    // hardware's way instead, see WriteBlock.
    static LiveSwitch fullRing("PGR4_XMA_FULL_RING");
    if (!fullRing && (read + blocks - write - 1) % blocks == 0)
        return false;

    static const uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
    const uint32_t channels = Field(ctx, 1, 29, 1) ? 2 : 1, rate = kRates[Field(ctx, 1, 27, 2)];
    Decoder& d = g_decoders[ctx];
    d.id = ctx;
    // PGR4_TRACE_XMA_TIMING=1: once a second, output rings found empty while their stream was still
    // playing (the title's mixer may have run out), codec opens and the slowest.
    static const bool traceTiming = getenv("PGR4_TRACE_XMA_TIMING") != nullptr;
    static uint32_t dry = 0;
    if (traceTiming)
    {
        if (read == write && d.framesWritten && !d.starved)
            dry++;
        static auto last = Clock::now();
        if (Clock::now() - last >= std::chrono::seconds(1))
        {
            Log("xma timing: %u rings found empty while playing, %u codecs taken (slowest %.2f ms, %u not ready), longest decoder tick gap %.1f ms, title waited up to %.2f ms; "
                "%u kicks (%u found the ring empty, %u with one block), rings of %u-%u blocks",
                dry, g_codecsTaken.exchange(0), g_slowestTakeMs.exchange(0), g_codecsMissed.exchange(0), g_maxTickGapMs.exchange(0), g_maxTitleWaitMs.exchange(0),
                g_kicks.exchange(0), g_kicksEmpty.exchange(0), g_kicksLow.exchange(0), g_ringBlocksMin.exchange(99), g_ringBlocksMax.exchange(0));
            dry = 0;
            last = Clock::now();
        }
    }
    if (!d.av || d.channels != channels || d.rate != rate)
    {
        if (!d.Open(channels, rate))
            return false;
    }
    // The title moved the read offset (a decode position set while the context was locked: FMOD restarts
    // an engine layer where it keeps in phase with the others): a jump, decoded as a start below.
    // (Music as committed in c531c31, its 19 menu stems verified aligned: none of this session's start, loop and seek
    // handling applies to it.)
    if (const uint32_t readOffset = Field(ctx, 2, 0, 26); d.av && !d.music && !d.reopen && !d.streamStart && d.nextOffset != ~0u && readOffset != d.nextOffset)
    {
        if (g_traceEvents)
            Log("xma ev seek %08X to %u (expected %u): current %u, in0 %s %u packets, in1 %s %u packets, consumed %d%d, backlog %zu samples/ch, ring %u/%u",
                ctx, readOffset, d.nextOffset, Field(ctx, 4, 31, 1), Field(ctx, 0, 20, 1) ? "valid" : "done", Field(ctx, 0, 0, 12),
                Field(ctx, 0, 21, 1) ? "valid" : "done", Field(ctx, 1, 0, 12), int(d.consumed[0]), int(d.consumed[1]),
                (d.pcm.size() - d.pcmRead) / std::max<uint32_t>(d.channels, 1), (Field(ctx, 0, 27, 5) + Field(ctx, 0, 22, 5) - Field(ctx, 9, 0, 5)) % std::max<uint32_t>(Field(ctx, 0, 22, 5), 1), Field(ctx, 0, 22, 5));
        d.reopen = true;
        d.pcm.clear(); // (what was decoded past the old position: the hardware had no such backlog)
        d.pcmRead = 0;
    }
    if (WriteBlock(ctx, d))
        return true;

    // Everything decoded so far has been handed over. If no input is waiting, this is the end of
    // the stream: deliver what the codec still holds first.
    auto valid = [&](uint32_t index) { return Field(ctx, 0, 20 + index, 1) && !d.consumed[index]; };
    if (d.holdsTail && !valid(0) && !valid(1))
    {
        d.Drain(kBlockBytes / 2);
        if (WriteBlock(ctx, d))
            return true;
    }
    for (uint32_t i = 0; i < 2; i++)
        if (d.consumed[i])
        {
            d.consumed[i] = false;
            SetField(ctx, 0, 20 + i, 1, 0); // now the title may refill (or loop) it
        }

    uint32_t current = Field(ctx, 4, 31, 1);
    if (!valid(current))
    {
        current ^= 1;
        if (!valid(current))
        {
            if (g_traceXma && !d.starved)
                Log("xma: %08X starved (both input buffers consumed)", ctx);
            if (g_traceEvents && !d.starved)
                Log("xma ev starved %08X", ctx);
            if (!d.starved)
                d.starvedAt = Clock::now();
            d.starved = true;
            return false; // starved: both input buffers consumed
        }
        if (g_traceEvents && d.starved)
            Log("xma ev resumed %08X after %.1f ms, %zu frames still queued for the title", ctx,
                std::chrono::duration<double, std::milli>(Clock::now() - d.starvedAt).count(), (d.pcm.size() - d.pcmRead) / std::max<uint32_t>(d.channels, 1));
        d.starved = false;
        SetField(ctx, 4, 31, 1, current);
        SetField(ctx, 2, 0, 26, 32);
        d.nextOffset = 32;
    }
    const uint32_t packets = current ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12);
    const uint32_t buffer = BufferAddress(LoadBE32(ctx + (5 + current) * 4));
    const uint32_t packet = Field(ctx, 2, 0, 26) / kPacketBits;
    if (packet < packets)
    {
        if (!d.PrepareStream())
            return false; // its codec is being opened: next pass
        if (g_traceXma && d.seeked)
        {
            const uint32_t header = LoadBE32(buffer + packet * kPacketBytes), start = Field(ctx, 2, 0, 26) % kPacketBits;
            if (start != 32 + ((header >> 11) & 0x7FFF))
                Log("xma: %08X decode starts mid-packet: packet %u of %u, offset %u in it, the packet's first frame at %u", ctx, packet, packets, start,
                    32 + ((header >> 11) & 0x7FFF));
        }
        if (g_traceXma)
        {
            // PGR4_TRACE_XMA: the first packets each context decodes (header dword: frame count
            // bits 0-5? , first-frame offset bits 11-25, metadata 26-28, packet skip 29-31).
            static std::map<uint32_t, int> dumped;
            if (dumped[ctx]++ < 3)
            {
                const uint8_t* p = GuestPtr<uint8_t>(buffer + packet * kPacketBytes);
                Log("xma: %08X packet %u of %u at %08X, read offset %u (%u bits into it), ch %u rate %u: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                    ctx, packet, packets, buffer, Field(ctx, 2, 0, 26), Field(ctx, 2, 0, 26) % kPacketBits, Field(ctx, 1, 29, 1) ? 2 : 1,
                    Field(ctx, 1, 27, 2), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            }
        }
        d.seeked = false;
        d.music = MusicPacket(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
        if (d.music)
            LOG_ONCE("xma: %08X is music (the first stream recognised)", ctx);
        if (d.music)
            d.exact = false; // (raw FFmpeg output from a fresh codec at each clear, as committed)
        if (d.streamStart && !d.music)
        {
            // A stream's start is a decode position: the read offset's frame in its packet, played from
            // subframe `skip` (dword 1 bits 24-26, XMAPlaybackSetDecodePosition's dwSubframesToSkip) on.
            // FMOD restarts PGR4's engine layers this way, several times a second as the revs cross
            // layers, at the frame that keeps each in phase with the others. Before, the whole packet
            // played from its first frame, so a restarted layer came in up to 15 frames early and
            // out of phase with the layers it was crossfaded against (the hitch in the exhaust note).
            // FFmpeg decodes whole packets and drops a fresh codec's first frame: the packet before
            // goes in first, for the overlap, then the frames before the position are dropped.
            // PGR4_XMA_NO_DECODE_POSITION=1: whole packets from their start, as before (A/B).
            const uint8_t* p = GuestPtr<uint8_t>(buffer + packet * kPacketBytes);
            uint32_t st[64], len[64];
            const uint32_t n = ParsePacket(p, st, len, 64), bit = Field(ctx, 2, 0, 26) % kPacketBits, subframes = Field(ctx, 1, 24, 3);
            uint32_t k = 0;
            for (uint32_t i = 0; i < n; i++)
                k += st[i] < bit;
            if (g_traceEvents)
                Log("xma ev start %08X at %zu: packet %u of %u bit %u (frame %u of %u, first frame at %u) subframe skip %u", ctx,
                    g_dumpPos[ctx] + (d.pcm.size() - d.pcmRead) / std::max<uint32_t>(d.channels, 1), packet, packets, bit, k, n, n ? st[0] : 0,
                    subframes);
            // PGR4_XMA_OLD_TIMING=1 only: the codec pre-rolled with the packets before the start, from the
            // mistaken reading that a fresh codec discards ~4.7k samples (it holds them back: kHwLead), then
            // the start's drop. PGR4_XMA_NO_PREROLL=1 with it: no pre-roll, FM2's 128 rule.
            static LiveSwitch noPreroll("PGR4_XMA_NO_PREROLL");
            static LiveSwitch oldTiming("PGR4_XMA_OLD_TIMING");
            uint32_t trimStart = 0, trimEnd = 0;
            PacketTrims(GuestPtr<uint8_t>(buffer), channels, trimStart, trimEnd);
            if (g_traceEvents && packet == 0 && k == 0)
                Log("xma ev trim %08X start %u end %u", ctx, trimStart, trimEnd);
            d.exact = !d.music && !oldTiming;
            if (d.exact)
            {
                // Exact timing (kHwLead): the hardware starts at frame k of this packet, subframe `skip` (a stream's
                // own start: trim_start), which is the fresh codec's sequence sample k * 512 + skip * 128 - kHwLead.
                // Before the packet's first frame (a position at its frame 0 with a skip under 3), the packet before
                // goes in first.
                const uint32_t skip = subframes;
                const bool streamOwnStart = packet == 0 && (!n || bit <= st[0]);
                const int64_t hw = streamOwnStart ? std::max<int64_t>(trimStart, int64_t(skip) * 128) : int64_t(k) * 512 + int64_t(skip) * 128;
                int64_t at = hw - int64_t(kHwLead);
                const uint8_t* before = nullptr;
                if (at < 0)
                {
                    const uint32_t loopEndPacket = Field(ctx, 4, 0, 26) / kPacketBits;
                    const uint32_t other = current ^ 1, otherPackets = Field(ctx, 0, 20 + other, 1) ? (other ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12)) : 0;
                    const uint32_t otherBuffer = BufferAddress(LoadBE32(ctx + (5 + other) * 4));
                    if (packet > 0)
                        before = GuestPtr<uint8_t>(buffer + (packet - 1) * kPacketBytes);
                    else if (otherPackets && otherBuffer)
                        before = GuestPtr<uint8_t>(otherBuffer + (otherPackets - 1) * kPacketBytes);
                    else if (Field(ctx, 0, 12, 8) && loopEndPacket < packets)
                        before = GuestPtr<uint8_t>(buffer + loopEndPacket * kPacketBytes);
                }
                if (before)
                {
                    uint32_t bst[64], blen[64];
                    bool over = false;
                    const uint32_t frames = ParsePacket(before, bst, blen, 64, &over);
                    const uint32_t carried = (LoadBE32(buffer + packet * kPacketBytes) >> 11) & 0x7FFF;
                    at += int64_t(frames - (over && !carried ? 1 : 0)) * 512;
                }
                d.dropUntil = uint64_t(std::max<int64_t>(at, 0));
                d.flush = false;
                if (g_traceEvents)
                    Log("xma ev exact %08X: %s, hardware sample %lld of the packet, sequence drop %llu%s", ctx,
                        streamOwnStart ? "stream start" : "decode position", (long long)hw,
                        (unsigned long long)d.dropUntil, before ? " (after the packet before)" : "");
                if (before)
                    d.Decode(before);
            }
            else if (!noPreroll && packets)
            {
                // The packets before the start in loop order: a looped stream cycles through packets 0 to its
                // loop end's (the buffer may hold more after it), so a start at packet 0 is preceded by the
                // loop's end, as at the real wrap. A stream without a loop: the buffer's end (any history
                // does; its own first packet twice lost that packet, measured).
                // Before packet 0 of this buffer comes the other buffer's end when the title has filled it (a
                // stream or a loop through both), else the loop's end, else this buffer's end.
                const uint32_t loopEndPacket = Field(ctx, 4, 0, 26) / kPacketBits;
                const uint32_t ring = Field(ctx, 0, 12, 8) && loopEndPacket < packets && loopEndPacket >= packet ? loopEndPacket + 1 : packets;
                const uint32_t other = current ^ 1, otherPackets = Field(ctx, 0, 20 + other, 1) ? (other ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12)) : 0;
                const uint32_t otherBuffer = BufferAddress(LoadBE32(ctx + (5 + other) * 4));
                size_t rolled = 0;
                for (uint32_t i = 8; i >= 1 && rolled < 5184 + 1024; i--)
                {
                    const uint8_t* p;
                    if (i <= packet)
                        p = GuestPtr<uint8_t>(buffer + (packet - i) * kPacketBytes);
                    else if (otherPackets && otherBuffer && i - packet <= otherPackets)
                        p = GuestPtr<uint8_t>(otherBuffer + (otherPackets - (i - packet)) * kPacketBytes);
                    else
                        p = GuestPtr<uint8_t>(buffer + ((packet + ring * 8 - i) % ring) * kPacketBytes);
                    rolled += d.Preroll(p);
                }
                d.flush = false; // (a flush now would arm FFmpeg's discard again: measured in tools/xmaprobe)
                const bool spanIn = n && st[0] > 32; // the frame carried over from the packet before: the pre-roll's
                d.midStart = packet > 0 || k > 0;
                d.skipPending += (spanIn ? 512 : 0) + (d.midStart ? k * 512 + subframes * 128 : trimStart);
                d.startDrop = d.midStart ? k * 512 + subframes * 128 : trimStart;
            }
        }
        const size_t pcmBefore = d.pcm.size();
        d.fedBefore = d.fedFrames;
        d.Decode(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
        // A loop's subframe skip: after the jump back the first frame plays from subframe `skip` on
        // (its first 128-sample subframes are the encoder's lead-in), as the hardware does. Played
        // whole, every pass of PGR4's engine loops (skip 3: three quarters of the frame) put about
        // 6 ms of lead-in into the note, a hitch at each wrap.
        size_t skipped = 0;
        if (d.skipPending)
        {
            skipped = std::min(size_t(d.skipPending) * d.channels, d.pcm.size() - pcmBefore);
            d.pcm.erase(d.pcm.begin() + ptrdiff_t(pcmBefore), d.pcm.begin() + ptrdiff_t(pcmBefore + skipped));
            d.skipPending -= uint32_t(skipped / d.channels); // (the rest from the next packet's output)
        }
        // The loop ends at a frame, not at the end of its packet. The packet holding the loop end
        // also holds what follows it (padding, or the next part of the file): keep only the frames
        // that end at or before it. Left in, every pass of every looping layer ran some tens of
        // milliseconds long, by a different amount each, so layers that should stay aligned (the
        // menu music is 19 of them) drifted apart.
        static LiveSwitch packetLoops("PGR4_XMA_PACKET_LOOPS"); // A/B: the old, packet-accurate loop
        // The loop end's own frame (the one starting at it) plays up to its subframe end: `end`
        // subframes of 128 samples, dword 1 bits 12-13 (XMASTREAMFORMAT's SubframeEnd, 0-3: "the
        // subframe of the loop end frame where the loop ends"). Measured on an engine layer: trim_end
        // 128 (3 valid subframes in the last frame), end 3; the 4th subframe is the encoder's padding.
        // This was read from bits 14-16 (Xenia Canary's layout, always 0 here) as end + 1, so every
        // loop cut 256 samples early, a step in every engine layer at each pass (the exhaust hitch).
        // PGR4_XMA_NO_SUBFRAMES=1: frame-accurate loops without the subframe end or skip (A/B).
        static LiveSwitch noSubframes("PGR4_XMA_NO_SUBFRAMES");
        const uint32_t loopsNow = Field(ctx, 0, 12, 8), endNow = Field(ctx, 4, 0, 26);
        if (!d.exact && !packetLoops && loopsNow && endNow / kPacketBits == packet)
        {
            const uint8_t* data = GuestPtr<uint8_t>(buffer + packet * kPacketBytes);
            uint32_t st[64], len[64];
            const uint32_t n = ParsePacket(data, st, len, 64);
            const uint32_t start = packet * kPacketBits;
            const uint32_t header = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
            const uint32_t firstStart = 32 + ((header >> 11) & 0x7FFF);
            uint32_t keep = 0;
            if (packet > 0 && firstStart > 32 && start + firstStart <= endNow) // the frame carried over from the packet before
                keep++;
            for (uint32_t i = 0; i < n; i++)
                if (st[i] + len[i] <= kPacketBits && start + st[i] + len[i] <= endNow)
                    keep++;
            const size_t target = pcmBefore + size_t(keep) * 512 * d.channels - std::min(skipped, size_t(keep) * 512 * d.channels);
            if (g_traceXma)
                Log("xma: %08X loop end %u in packet %u: keeping %u of %zu frames", ctx, endNow, packet, keep, (d.pcm.size() - pcmBefore) / (512 * d.channels));
            const size_t endSamples = noSubframes || d.music ? 0 : size_t(Field(ctx, 1, 12, 2)) * 128 * d.channels;
            int endFrame = -1; // the loop end's frame among this packet's
            for (uint32_t i = 0; i < n; i++)
                if (start + st[i] == endNow)
                    endFrame = int(i);
            if (endSamples && endFrame >= 0 && st[endFrame] + len[endFrame] <= kPacketBits)
            {
                // It ends in this packet: it is the next frame of its output.
                if (d.pcm.size() > target)
                    d.pcm.resize(std::min(d.pcm.size(), target + endSamples));
            }
            else
            {
                if (d.pcm.size() > target)
                    d.pcm.resize(target);
                // It runs on into the next packet, whose output starts with it: decode that too and
                // keep the subframes wanted.
                const uint32_t packetCount = current ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12);
                if (endSamples && endFrame >= 0 && packet + 1 < packetCount)
                {
                    const size_t before = d.pcm.size();
                    d.Decode(GuestPtr<uint8_t>(buffer + (packet + 1) * kPacketBytes));
                    d.pcm.resize(std::min(d.pcm.size(), before + endSamples));
                }
            }
        }
    }

    // Loop: when the packet holding the loop end is done and loops remain, continue from
    // the loop start (packet-accurate).
    const uint32_t loops = Field(ctx, 0, 12, 8), loopEnd = Field(ctx, 4, 0, 26);
    if (loops && loopEnd / kPacketBits == packet)
    {
        // PGR4_TRACE_XMA_LOOPS=1: each context's loop points the first time it wraps (where the loop
        // start sits in its packet, how many frames start before it there, the subframe fields).
        static const bool traceLoops = getenv("PGR4_TRACE_XMA_LOOPS") != nullptr;
        if (traceLoops)
        {
            static std::set<uint64_t> seen;
            const uint32_t loopStart = Field(ctx, 3, 0, 26);
            if (seen.insert(uint64_t(ctx) << 32 | loopStart).second && seen.size() < 400)
            {
                const uint8_t* startPacket = GuestPtr<uint8_t>(buffer + (loopStart / kPacketBits) * kPacketBytes);
                uint32_t st[64], len[64];
                const uint32_t n = ParsePacket(startPacket, st, len, 64);
                uint32_t before = 0;
                for (uint32_t i = 0; i < n; i++)
                    before += st[i] < loopStart % kPacketBits;
                const uint32_t header = LoadBE32(buffer + (loopStart / kPacketBits) * kPacketBytes);
                Log("xma loop: %08X ch %u loops %u start %u (packet %u bit %u, first frame at %u, %u of %u frames before it) end %u (packet %u bit %u) "
                    "subframe end %u (bits 12-13) bits14-16 %u skip %u",
                    ctx, channels, loops, loopStart, loopStart / kPacketBits, loopStart % kPacketBits, 32 + ((header >> 11) & 0x7FFF), before, n,
                    loopEnd, loopEnd / kPacketBits, loopEnd % kPacketBits, Field(ctx, 1, 12, 2), Field(ctx, 1, 14, 3), Field(ctx, 1, 17, 3));
            }
        }
        d.wrapped = true;
        if (g_traceEvents)
            Log("xma ev wrap %08X ch %u", ctx, channels);
        if (d.exact)
        {
            // Exact timing: the pass ends at the loop end frame's subframe end (dword 1 bits 12-13) and the next
            // starts at the loop start frame's subframe skip (bits 17-19) in the packet fed next; between them, in
            // the codec's sequence (kHwLead), the rest of the loop end packet and the loop start packet's frames
            // before it: a seam dropped as the codec carries on (its start set when that packet is fed, Decode).
            const uint32_t loopStart = Field(ctx, 3, 0, 26);
            uint32_t st[64], len[64];
            const uint32_t n = ParsePacket(GuestPtr<uint8_t>(buffer + packet * kPacketBytes), st, len, 64);
            int endFrame = -1;
            for (uint32_t i = 0; i < n; i++)
                if (packet * kPacketBits + st[i] == loopEnd)
                    endFrame = int(i);
            const uint32_t sn = ParsePacket(GuestPtr<uint8_t>(buffer + (loopStart / kPacketBits) * kPacketBytes), st, len, 64);
            uint32_t startFrame = 0;
            for (uint32_t i = 0; i < sn; i++)
                startFrame += st[i] < loopStart % kPacketBits;
            if (endFrame >= 0)
            {
                d.seamPending = true;
                d.seamEnd = (d.fedBefore + uint64_t(endFrame)) * 512 + uint64_t(Field(ctx, 1, 12, 2)) * 128 - kHwLead;
                d.seamFrame = startFrame;
                d.seamSkip = Field(ctx, 1, 17, 3);
            }
            else
                LOG_ONCE("xma: %08X loop end %u is not a frame start in its packet: its seam is uncut", ctx, loopEnd);
        }
        if (loops != 255)
            SetField(ctx, 0, 12, 8, loops - 1);
        SetField(ctx, 2, 0, 26, Field(ctx, 3, 0, 26));
        d.nextOffset = Field(ctx, 3, 0, 26);
        static LiveSwitch noSubframes("PGR4_XMA_NO_SUBFRAMES");
        static LiveSwitch packetLoops("PGR4_XMA_PACKET_LOOPS");
        if (!d.exact && !d.music && !noSubframes && !packetLoops)
            d.skipPending = Field(ctx, 1, 17, 3) * 128;
        static const bool dumpXma = getenv("PGR4_DUMP_XMA") != nullptr, dumpMusic = getenv("PGR4_DUMP_XMA_MUSIC") != nullptr;
        if (dumpXma && (!d.music || dumpMusic))
        {
            d.looped = true;
            Log("xma wrap %08X at %zu", ctx, g_dumpPos[ctx] + (d.pcm.size() - d.pcmRead) / std::max<uint32_t>(d.channels, 1));
        }
        // The decoder carries on across a loop (its MDCT overlap, as the hardware's): resetting it
        // here swallowed the first frames of every pass, a ~25 ms hole at each wrap. A new stream
        // (a cleared context) does reset it. PGR4_XMA_LOOP_FLUSH=1 restores the old reset (A/B).
        static LiveSwitch loopFlush("PGR4_XMA_LOOP_FLUSH");
        if (loopFlush)
            d.flush = true;
        return true;
    }
    const uint32_t offset = (packet + 1) * kPacketBits + 32;
    if (packet + 1 >= packets)
    {
        d.consumed[current] = true; // (valid for the title until its audio is delivered)
        SetField(ctx, 4, 31, 1, current ^ 1);
        SetField(ctx, 2, 0, 26, 32);
        d.nextOffset = 32;
    }
    else
    {
        SetField(ctx, 2, 0, 26, offset);
        d.nextOffset = offset;
    }
    return true;
}

// Clear (register 06A0h+): the context's buffers become invalid and its output ring empty.
void ClearHwContext(uint32_t c)
{
    if (g_traceEvents)
    {
        // (with PGR4_DUMP_XMA: the output the title had not read yet, so a restart's join can be checked
        // against the dump: the title's position should continue from the last block it read)
        const uint32_t blocks = Field(c, 0, 22, 5), write = Field(c, 0, 27, 5), read = Field(c, 9, 0, 5);
        const uint32_t ch = Field(c, 1, 29, 1) ? 2 : 1;
        Log("xma ev clear %08X unread %u blocks (%u samples/ch), dump at %zu", c, blocks ? (write + blocks - read) % blocks : 0,
            blocks ? (write + blocks - read) % blocks * (kBlockBytes / 2) / ch : 0, g_dumpPos[c]);
    }
    SetField(c, 0, 20, 2, 0);  // in0, in1 valid
    SetField(c, 0, 27, 5, 0);  // output write offset
    SetField(c, 1, 31, 1, 0);  // output valid
    SetField(c, 9, 0, 5, 0);   // output read offset
    if (auto it = g_decoders.find(c); it != g_decoders.end())
    {
        it->second.consumed[0] = it->second.consumed[1] = false;
        it->second.holdsTail = false;
        it->second.flush = true;
        it->second.reopen = true;
        it->second.skipPending = 0;
        it->second.nextOffset = ~0u;
        it->second.pcm.clear();
        it->second.pcmRead = 0;
    }
}

// The kick, lock and clear registers, taken and zeroed atomically (the title sets them with
// byte-reversed stores, so they read in host order).
void PollHwRegisters()
{
    if (!g_array)
        return;
    for (uint32_t r = 0; r < kHwContexts / 32; r++)
    {
        auto take = [&](uint32_t reg) { return uint32_t(InterlockedExchange(reinterpret_cast<volatile LONG*>(GuestPtr<uint32_t>(reg + r * 4)), 0)); };
        const uint32_t clear = take(kRegClear), lock = take(kRegLock), kick = take(kRegKick);
        auto each = [&](uint32_t bits, auto&& fn) {
            for (; bits; bits &= bits - 1)
            {
                fn(g_array + (r * 32 + uint32_t(__builtin_ctz(bits))) * kContextSize);
            }
        };
        each(clear, [](uint32_t c) { ClearHwContext(c); });
        each(lock, [](uint32_t c) { g_enabled.erase(c); });
        each(kick, [](uint32_t c) {
            g_enabled.insert(c);
            {
                // (PGR4_TRACE_XMA_TIMING: how full the output ring is when the title kicks)
                const uint32_t blocks = Field(c, 0, 22, 5), write = Field(c, 0, 27, 5), read = Field(c, 9, 0, 5);
                if (blocks && Field(c, 1, 31, 1))
                {
                    const uint32_t filled = (write + blocks - read) % blocks;
                    g_kicks++;
                    g_kicksEmpty += filled == 0;
                    g_kicksLow += filled == 1;
                    // PGR4_TRACE_XMA_DRY=1: each kick of a playing context that found its output ring empty
                    // (the title read everything decoded: it may have wanted more, a gap in its mix)
                    static const bool traceDry = getenv("PGR4_TRACE_XMA_DRY") != nullptr;
                    if (traceDry && filled == 0)
                        if (auto it = g_decoders.find(c); it != g_decoders.end() && it->second.framesWritten && !it->second.starved)
                            Log("xma dry %08X ch %u ring %u blocks, pcm queued %zu", c, Field(c, 1, 29, 1) ? 2 : 1, blocks,
                                (it->second.pcm.size() - it->second.pcmRead) / std::max<uint32_t>(it->second.channels, 1));
                    g_ringBlocksMin = std::min(g_ringBlocksMin.load(), blocks);
                    g_ringBlocksMax = std::max(g_ringBlocksMax.load(), blocks);
                }
            }
            // Worked at once (as Xenia's inline path): the title's next edit assumes the
            // hardware has taken this one.
            // Bounded: a looping stream can make progress forever without filling the ring.
            for (uint32_t n = 0; n < 64 && Step(c); n++)
                ;
            if (g_traceXma)
                Log("xma: kick %08X: %08X %08X %08X %08X %08X in0 %08X in1 %08X out %08X work %08X rd %08X", c, LoadBE32(c), LoadBE32(c + 4),
                    LoadBE32(c + 8), LoadBE32(c + 12), LoadBE32(c + 16), LoadBE32(c + 20), LoadBE32(c + 24), LoadBE32(c + 28), LoadBE32(c + 32),
                    LoadBE32(c + 36));
        });
        if (g_traceXma && (clear | lock | kick))
            Log("xma: registers %u: kick %08X lock %08X clear %08X", r, kick, lock, clear);
    }
}

void DecoderLoop()
{
    SetThreadDescription(GetCurrentThread(), L"xma decoder");
    auto next = Clock::now();
    for (;;)
    {
        next += std::chrono::milliseconds(2);
        std::this_thread::sleep_until(next);
        {
            // (PGR4_TRACE_XMA_TIMING: the longest gap between ticks)
            static auto lastTick = Clock::now();
            const auto now = Clock::now();
            g_maxTickGapMs = std::max(g_maxTickGapMs.load(), std::chrono::duration<double, std::milli>(now - lastTick).count());
            lastTick = now;
        }
        std::vector<uint32_t> work;
        {
            std::lock_guard lock(g_mutex);
            PollHwRegisters();
            work.assign(g_enabled.begin(), g_enabled.end());
        }
        // The lock is held for one step at a time: the title's register stores (a lock waits out
        // the context's work) must not wait for every context's decoding. A context locked
        // meanwhile is skipped. Bounded per tick: a looping stream can make progress forever.
        for (uint32_t ctx : work)
            for (uint32_t n = 0; n < 64; n++)
            {
                std::lock_guard lock(g_mutex);
                if (!g_enabled.count(ctx) || !Step(ctx))
                    break;
            }
    }
}
}

// The title has just written the decoder's kick, lock or clear registers (its functions for them
// are hooked, src/hooks/xma_lock.cpp): act on them now, as the hardware does. Holding the decoder
// lock also waits out a context being worked, as a lock must (Xenia's XmaContext::Block).
void XmaRegistersWritten();

// At startup, before the title's audio starts: the context array and its address register.
void XmaInit()
{
    if (!g_hwPath)
        return;
    // PGR4_XMA_HW_NOREG=1: the array exists but ContextArrayAddress stays 0 (bisecting the mute).
    static const bool noReg = getenv("PGR4_XMA_HW_NOREG") != nullptr;
    g_array = GuestAlloc(Heap::Physical, kHwContexts * kContextSize, 0x1000);
    if (g_array && !noReg)
        *GuestPtr<uint32_t>(kRegArray) = g_array & 0x1FFFFFFF; // read with lwbrx: host order
    Log("xma: hardware contexts at %08X", g_array);
}

// A store from the recompiled code to the decoder's registers (PPC_MM_STORE_U32, ppc_hooks.h).
void XmaMmioStore(uint32_t address)
{
    if (address >= kRegKick && address < kRegClear + 4 * (kHwContexts / 32))
        XmaRegistersWritten();
}

void XmaRegistersWritten()
{
    if (!g_hwPath)
        return;
    const auto t0 = Clock::now();
    std::lock_guard lock(g_mutex);
    // (PGR4_TRACE_XMA_TIMING: the title's longest wait here, for the decoder)
    g_maxTitleWaitMs = std::max(g_maxTitleWaitMs.load(), std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    PollHwRegisters();
}

GUEST_EXPORT(XMACreateContext)
{
    uint32_t context = 0;
    if (!g_array)
        context = GuestAlloc(Heap::Physical, kContextSize, 0x100);
    else
    {
        std::lock_guard lock(g_mutex);
        for (uint32_t i = 0; i < kHwContexts && !context; i++)
            if (!g_slotUsed[i])
            {
                g_slotUsed[i] = true;
                context = g_array + i * kContextSize;
                memset(GuestPtr(context), 0, kContextSize);
            }
    }
    StoreIfNonNull(ctx.r3.u32, context);
    std::call_once(g_started, [] {
        av_log_set_level(AV_LOG_ERROR);
        std::thread(DecoderLoop).detach();
    });
    Return(ctx, context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY);
}

GUEST_EXPORT(XMAReleaseContext)
{
    {
        std::lock_guard lock(g_mutex);
        g_enabled.erase(ctx.r3.u32);
        if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
        {
            it->second.Close();
            g_decoders.erase(it);
        }
    }
    if (g_array && ctx.r3.u32 >= g_array && ctx.r3.u32 < g_array + kHwContexts * kContextSize)
    {
        std::lock_guard lock(g_mutex);
        g_slotUsed[(ctx.r3.u32 - g_array) / kContextSize] = false;
    }
    else
        GuestFree(ctx.r3.u32);
    Return(ctx, 0);
}

GUEST_EXPORT(XMAInitializeContext)
{
    // XMA_CONTEXT_INIT: in0, in0 packets, in1, in1 packets, read offset,
    // output, output blocks, work buffer, subframes, channels, sample rate,
    // loop start, loop end, loop count/subframe end/skip (bytes).
    const uint32_t c = ctx.r3.u32, init = ctx.r4.u32;
    std::lock_guard lock(g_mutex);
    memset(GuestPtr(c), 0, kContextSize);
    auto in = [&](uint32_t off) { return LoadBE32(init + off); };
    StoreBE32(c + 5 * 4, in(0x00));
    SetField(c, 0, 0, 12, in(0x04));
    StoreBE32(c + 6 * 4, in(0x08));
    SetField(c, 1, 0, 12, in(0x0C));
    SetField(c, 2, 0, 26, in(0x10));
    StoreBE32(c + 7 * 4, in(0x14));
    SetField(c, 0, 22, 5, in(0x18));
    StoreBE32(c + 8 * 4, in(0x1C));
    SetField(c, 1, 20, 4, in(0x20));
    SetField(c, 1, 29, 1, in(0x24) >= 1);
    SetField(c, 1, 27, 2, in(0x28));
    SetField(c, 3, 0, 26, in(0x2C));
    SetField(c, 4, 0, 26, in(0x30));
    SetField(c, 0, 12, 8, *GuestPtr<uint8_t>(init + 0x34));
    if (g_traceXma)
        Log("xma: init %08X in0 %08X x%u in1 %08X x%u offset %u out %08X blocks %u ch %u rate %u loop %u-%u x%u", c, in(0x00), in(0x04), in(0x08), in(0x0C),
            in(0x10), in(0x14), in(0x18), in(0x24), in(0x28), in(0x2C), in(0x30), *GuestPtr<uint8_t>(init + 0x34));
    if (auto it = g_decoders.find(c); it != g_decoders.end())
    {
        if (g_traceXma)
            Log("xma: re-init %08X: %zu packets decoded, %zu frames decoded, %zu delivered, %zu discarded unread", c, it->second.packetsDecoded,
                it->second.framesDecoded, it->second.framesWritten, (it->second.pcm.size() - it->second.pcmRead) / std::max<uint32_t>(it->second.channels, 1));
        it->second.packetsDecoded = it->second.framesDecoded = it->second.framesWritten = 0;
        it->second.consumed[0] = it->second.consumed[1] = false;
        it->second.holdsTail = false;
        it->second.flush = true;
        it->second.pcm.clear();
        it->second.pcmRead = 0;
    }
    Return(ctx, 0);
}

GUEST_EXPORT(XMAEnableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: enable %08X (read offset %u, in0 %u in1 %u valid, current %u) loops %u start %u end %u, subframes: end %u skip %u decode %u, packets %u/%u",
            ctx.r3.u32, Field(ctx.r3.u32, 2, 0, 26), Field(ctx.r3.u32, 0, 20, 1), Field(ctx.r3.u32, 0, 21, 1), Field(ctx.r3.u32, 4, 31, 1),
            Field(ctx.r3.u32, 0, 12, 8), Field(ctx.r3.u32, 3, 0, 26), Field(ctx.r3.u32, 4, 0, 26), Field(ctx.r3.u32, 1, 14, 3), Field(ctx.r3.u32, 1, 17, 3),
            Field(ctx.r3.u32, 1, 20, 4), Field(ctx.r3.u32, 0, 0, 12), Field(ctx.r3.u32, 1, 0, 12));
    g_enabled.insert(ctx.r3.u32);
    while (Step(ctx.r3.u32))
        ;
    Return(ctx, 0);
}

GUEST_EXPORT(XMADisableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: disable %08X", ctx.r3.u32);
    g_enabled.erase(ctx.r3.u32);
    Return(ctx, 0);
}

#define XMA_FIELD_EXPORTS(Name, dword, lo, bits)                     \
    GUEST_EXPORT(XMAGet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, lo, bits));             \
    }                                                                \
    GUEST_EXPORT(XMASet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        SetField(ctx.r3.u32, dword, lo, bits, ctx.r4.u32);           \
        Return(ctx, 0);                                              \
    }

GUEST_EXPORT(XMAGetInputBufferReadOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 2, 0, 26));
}

GUEST_EXPORT(XMASetInputBufferReadOffset)
{
    // A seek: the decoder starts over at the new packet.
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: seek %08X to offset %u (was %u), in0 %08X x%u in1 %08X x%u, out %08X blocks %u write %u read %u, skip %u", ctx.r3.u32, ctx.r4.u32,
            Field(ctx.r3.u32, 2, 0, 26), LoadBE32(ctx.r3.u32 + 20), Field(ctx.r3.u32, 0, 0, 12), LoadBE32(ctx.r3.u32 + 24), Field(ctx.r3.u32, 1, 0, 12),
            LoadBE32(ctx.r3.u32 + 28), Field(ctx.r3.u32, 0, 22, 5), Field(ctx.r3.u32, 0, 27, 5), Field(ctx.r3.u32, 9, 0, 5), Field(ctx.r3.u32, 1, 17, 3));
    SetField(ctx.r3.u32, 2, 0, 26, ctx.r4.u32);
    // The title restarts the decoder at the first frame of each next pair of packets (a seek
    // within a stream it reads in order). The codec keeps its overlap and its unfinished frame
    // across that, as the hardware does; resetting it dropped most of every pair's audio.
    static LiveSwitch flushOnSeek("PGR4_XMA_SEEK_FLUSH");
    if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
    {
        it->second.seeked = true;
        if (flushOnSeek)
            it->second.flush = true;
    }
    Return(ctx, 0);
}
XMA_FIELD_EXPORTS(OutputBufferReadOffset, 9, 0, 5)

GUEST_EXPORT(XMAGetOutputBufferWriteOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 0, 27, 5));
}

#define XMA_FLAG_EXPORTS(Name, dword, bit)                           \
    GUEST_EXPORT(XMAIs##Name##Valid)                                 \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, bit, 1));               \
    }                                                                \
    GUEST_EXPORT(XMASet##Name##Valid)                                \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        if (g_traceXma)                                                          Log("xma: %08X " #Name " valid", ctx.r3.u32);                    SetField(ctx.r3.u32, dword, bit, 1, 1);                              ClearConsumed(ctx.r3.u32, dword, bit);                       \
        Return(ctx, 0);                                              \
    }

XMA_FLAG_EXPORTS(InputBuffer0, 0, 20)
XMA_FLAG_EXPORTS(InputBuffer1, 0, 21)
XMA_FLAG_EXPORTS(OutputBuffer, 1, 31)

// A read from a music file (imports_io.cpp): its packets' hashes, located from the file's RIFF data chunk.
void AudioNoteMusicRead(const std::filesystem::path& file, uint64_t offset, const uint8_t* data, uint32_t bytes)
{
    static std::map<std::filesystem::path, uint64_t> dataStart;
    std::lock_guard lock(g_musicMutex);
    auto it = dataStart.find(file);
    if (it == dataStart.end())
    {
        uint64_t start = 0;
        if (FILE* f = _wfopen(file.c_str(), L"rb"))
        {
            uint8_t head[4096] = {};
            const size_t n = fread(head, 1, sizeof(head), f);
            fclose(f);
            for (size_t i = 12; i + 8 <= n; i++)
                if (!memcmp(head + i, "data", 4))
                {
                    start = i + 8;
                    break;
                }
        }
        it = dataStart.emplace(file, start).first;
    }
    if (!it->second)
        return;
    const uint64_t first = it->second + (offset > it->second ? (offset - it->second + kPacketBytes - 1) / kPacketBytes * kPacketBytes : 0);
    for (uint64_t p = first; p + 64 <= offset + bytes; p += kPacketBytes)
        g_musicPackets.insert(PacketHash(data + (p - offset)));
}
