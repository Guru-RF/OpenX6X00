/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 WAV files for test mode and tools
 */

#include "wav_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace x6100 {
namespace js8 {

// WAV fields are little-endian whatever the machine.
static unsigned get_u16(const unsigned char *b) { return b[0] | (unsigned)b[1] << 8; }
static std::uint32_t get_u32(const unsigned char *b) { return get_u16(b) | (std::uint32_t)get_u16(b + 2) << 16; }
static void set_le(unsigned char *b, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) b[i] = (unsigned char)(value >> (8 * i));
}

using File = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;

static bool fail(std::string &error, const std::string &why) {
    error = why;
    return false;
}

// An hour at 48 kHz: longer than any recording the decoders take
static constexpr std::size_t MAX_FRAMES = 48000u * 3600u;

bool read_wav(const std::string &file, WavAudio &out, std::string &error) {
    const File f(std::fopen(file.c_str(), "rb"), &std::fclose);
    if (f == nullptr) return fail(error, "cannot open " + file);

    unsigned char riff[12];
    if (std::fread(riff, 1, sizeof riff, f.get()) != sizeof riff || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0)
        return fail(error, "not a RIFF/WAVE file");

    int  channels = 0;
    bool got_fmt  = false;

    // Chunk by chunk: 4-byte id, 4-byte size, the body, a pad byte if the size is odd.
    unsigned char head[8];
    while (std::fread(head, 1, sizeof head, f.get()) == sizeof head) {
        const std::uint32_t size = get_u32(head + 4);
        const std::uint32_t pad  = size & 1;

        if (std::memcmp(head, "data", 4) == 0) {
            if (!got_fmt) return fail(error, "data chunk before fmt chunk");
            // The chunk's length is the file's word: bounded by what the file holds, or a 44-byte
            // file claiming 4 GB of samples asked for them (std::bad_alloc, nothing caught it).
            const std::size_t frame_bytes = 2u * channels;
            const long        here        = std::ftell(f.get());
            std::size_t       frames      = size / frame_bytes;
            if (here >= 0 && std::fseek(f.get(), 0, SEEK_END) == 0) {
                const long end = std::ftell(f.get());
                std::fseek(f.get(), here, SEEK_SET);
                if (end >= here) frames = std::min<std::size_t>(frames, (std::size_t)(end - here) / frame_bytes);
            }
            if (frames > MAX_FRAMES) return fail(error, "longer than an hour");
            std::vector<std::int16_t> interleaved;
            try {
                interleaved.resize(frames * channels);
            } catch (const std::bad_alloc &) {
                return fail(error, "not enough memory for the file");
            }
            const std::size_t read = std::fread(interleaved.data(), frame_bytes, frames, f.get());
            out.samples.resize(read);
            for (std::size_t i = 0; i < read; ++i) out.samples[i] = interleaved[i * channels] / 32768.0f;
            return true;
        }

        if (std::memcmp(head, "fmt ", 4) != 0) {
            std::fseek(f.get(), (long)(size + pad), SEEK_CUR);
            continue;
        }

        unsigned char fmt[16];
        if (size < sizeof fmt || std::fread(fmt, 1, sizeof fmt, f.get()) != sizeof fmt)
            return fail(error, "bad fmt chunk");
        const unsigned tag  = get_u16(fmt);
        channels            = (int)get_u16(fmt + 2);
        out.rate            = (int)get_u32(fmt + 4);
        const unsigned bits = get_u16(fmt + 14);
        // 0xFFFE is WAVE_FORMAT_EXTENSIBLE, which also carries PCM (its
        // subformat comes after these 16 bytes and is not checked).
        if (tag != 1 && tag != 0xFFFE) return fail(error, "only PCM WAV is supported");
        if (bits != 16 || channels < 1 || out.rate <= 0) return fail(error, "need 16-bit PCM");
        got_fmt = true;
        std::fseek(f.get(), (long)(size - 16 + pad), SEEK_CUR);
    }

    return fail(error, "no data chunk");
}

bool write_wav(const std::string &file, int rate, const std::vector<float> &samples) {
    const File f(std::fopen(file.c_str(), "wb"), &std::fclose);
    if (f == nullptr) return false;

    // The canonical 44-byte header: RIFF, a 16-byte fmt chunk, then data.
    const std::uint32_t data_size = (std::uint32_t)(2 * samples.size());
    unsigned char       header[44];
    std::memcpy(header, "RIFF", 4);
    set_le(header + 4, 36 + data_size, 4);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    set_le(header + 16, 16, 4);                    // fmt body size
    set_le(header + 20, 1, 2);                     // integer PCM
    set_le(header + 22, 1, 2);                     // one channel
    set_le(header + 24, (std::uint32_t)rate, 4);   // samples per second
    set_le(header + 28, (std::uint32_t)rate * 2, 4); // bytes per second
    set_le(header + 32, 2, 2);                     // bytes per sample
    set_le(header + 34, 16, 2);                    // bits per sample
    std::memcpy(header + 36, "data", 4);
    set_le(header + 40, data_size, 4);
    if (std::fwrite(header, 1, sizeof header, f.get()) != sizeof header) return false;

    // Scaled by 32768 like read_wav(), so a file read and written again is unchanged.
    std::vector<std::int16_t> body(samples.size());
    std::transform(samples.begin(), samples.end(), body.begin(), [](float x) {
        return (std::int16_t)std::clamp<long>(std::lrintf(32768.0f * x), -32768, 32767);
    });
    return std::fwrite(body.data(), 2, body.size(), f.get()) == body.size();
}

} // namespace js8
} // namespace x6100
