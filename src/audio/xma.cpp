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
uint32_t ParsePacket(const uint8_t* packet, uint32_t* starts, uint32_t* lengths, uint32_t maxFrames)
{
    const uint32_t header = (uint32_t(packet[0]) << 24) | (uint32_t(packet[1]) << 16) | (uint32_t(packet[2]) << 8) | packet[3];
    const uint32_t count = std::min<uint32_t>(header >> 26, maxFrames);
    uint32_t pos = 32 + ((header >> 11) & 0x7FFF);
    uint32_t n = 0;
    for (; n < count && pos + 15 <= 16384; n++)
    {
        const uint32_t length = ReadBits(packet, pos, 15);
        if (!length)
            break;
        starts[n] = pos;
        lengths[n] = length;
        pos += length;
    }
    return n;
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

    void Close()
    {
        avcodec_free_context(&av);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    bool Open(uint32_t ch, uint32_t hz)
    {
        Close();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec || !(av = avcodec_alloc_context3(codec)))
            return false;
        av->sample_rate = int(hz);
        av_channel_layout_default(&av->ch_layout, int(ch));
        av->block_align = kPacketBytes;
        // XMA2WAVEFORMATEX (34 bytes): one stream; FFmpeg reads the stream count from it.
        av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        av->extradata_size = 34;
        av->extradata[0] = 1;
        if (avcodec_open2(av, codec, nullptr) < 0)
        {
            avcodec_free_context(&av);
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        channels = ch;
        rate = hz;
        return true;
    }

    // The end of a stream: the codec holds back about a packet of audio until more input
    // comes. Appends it (and silence up to a whole output block) and resets the codec.
    void Drain(size_t block)
    {
        if (!av)
            return;
        const size_t pcmBefore = pcm.size();
        avcodec_send_packet(av, nullptr);
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = 0; i < n; i++)
                    pcm[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
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
        if (reopen)
        {
            // avcodec_flush_buffers does not clear FFmpeg's XMA overlap-add tail (the decoder has
            // no flush callback), so the previous track's last frame bled into the next one
            // (a jarring cut at every song change). Re-opening discards it (as ReXGlue does).
            static LiveSwitch noReopen("PGR4_NO_XMA_REOPEN");
            if (!noReopen)
                Open(channels, rate);
            reopen = false;
            flush = false;
        }
        if (flush)
        {
            avcodec_flush_buffers(av);
            flush = false;
        }
        const size_t pcmBefore = pcm.size();
        packetsDecoded++;
        if (av_new_packet(packet, kPacketBytes) < 0)
            return;
        memcpy(packet->data, data, kPacketBytes);
        const int sent = avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        if (sent < 0)
        {
            flush = true;
            return;
        }
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = 0; i < n; i++)
                    pcm[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
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
    d.pcmRead += perBlock;
    d.framesWritten += perBlock / std::max<uint32_t>(d.channels, 1);
    if (d.pcmRead > (1u << 16))
    {
        d.pcm.erase(d.pcm.begin(), d.pcm.begin() + ptrdiff_t(d.pcmRead));
        d.pcmRead = 0;
    }
    SetField(ctx, 0, 27, 5, (write + 1) % blocks);
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
    // Keep one block free so a full ring never reads as empty.
    if ((read + blocks - write - 1) % blocks == 0)
        return false;

    static const uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
    const uint32_t channels = Field(ctx, 1, 29, 1) ? 2 : 1, rate = kRates[Field(ctx, 1, 27, 2)];
    Decoder& d = g_decoders[ctx];
    if (!d.av || d.channels != channels || d.rate != rate)
        if (!d.Open(channels, rate))
            return false;
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
            d.starved = true;
            return false; // starved: both input buffers consumed
        }
        d.starved = false;
        SetField(ctx, 4, 31, 1, current);
        SetField(ctx, 2, 0, 26, 32);
    }
    const uint32_t packets = current ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12);
    const uint32_t buffer = BufferAddress(LoadBE32(ctx + (5 + current) * 4));
    const uint32_t packet = Field(ctx, 2, 0, 26) / kPacketBits;
    if (packet < packets)
    {
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
        const size_t pcmBefore = d.pcm.size();
        d.Decode(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
        // The loop ends at a frame, not at the end of its packet. The packet holding the loop end
        // also holds what follows it (padding, or the next part of the file): keep only the frames
        // that end at or before it. Left in, every pass of every looping layer ran some tens of
        // milliseconds long, by a different amount each, so layers that should stay aligned (the
        // menu music is 19 of them) drifted apart.
        static LiveSwitch packetLoops("PGR4_XMA_PACKET_LOOPS"); // A/B: the old, packet-accurate loop
        const uint32_t loopsNow = Field(ctx, 0, 12, 8), endNow = Field(ctx, 4, 0, 26);
        if (!packetLoops && loopsNow && endNow / kPacketBits == packet)
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
            const size_t target = pcmBefore + size_t(keep) * 512 * d.channels;
            if (g_traceXma)
                Log("xma: %08X loop end %u in packet %u: keeping %u of %zu frames", ctx, endNow, packet, keep, (d.pcm.size() - pcmBefore) / (512 * d.channels));
            if (d.pcm.size() > target)
                d.pcm.resize(target);
        }
    }

    // Loop: when the packet holding the loop end is done and loops remain, continue from
    // the loop start (packet-accurate).
    const uint32_t loops = Field(ctx, 0, 12, 8), loopEnd = Field(ctx, 4, 0, 26);
    if (loops && loopEnd / kPacketBits == packet)
    {
        if (loops != 255)
            SetField(ctx, 0, 12, 8, loops - 1);
        SetField(ctx, 2, 0, 26, Field(ctx, 3, 0, 26));
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
    }
    else
        SetField(ctx, 2, 0, 26, offset);
    return true;
}

// Clear (register 06A0h+): the context's buffers become invalid and its output ring empty.
void ClearHwContext(uint32_t c)
{
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
    std::lock_guard lock(g_mutex);
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
