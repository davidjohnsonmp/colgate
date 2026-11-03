// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Just enough WAV reading and writing for the command-line tools.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace colgate_tools
{
struct Audio
{
    int rate = 0;
    int channels = 0;
    std::vector<std::vector<float>> samples;
};

inline uint32_t read32 (const unsigned char* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t read16 (const unsigned char* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

inline bool readWav (const char* path, Audio& audio)
{
    FILE* file = std::fopen (path, "rb");
    if (file == nullptr)
        return false;
    std::fseek (file, 0, SEEK_END);
    const long size = std::ftell (file);
    std::fseek (file, 0, SEEK_SET);
    std::vector<unsigned char> data ((size_t) size);
    const size_t got = std::fread (data.data(), 1, (size_t) size, file);
    std::fclose (file);
    if (got != (size_t) size || size < 44 || std::memcmp (data.data(), "RIFF", 4) != 0 || std::memcmp (data.data() + 8, "WAVE", 4) != 0)
        return false;

    int format = 0, bits = 0;
    size_t offset = 12;
    while (offset + 8 <= data.size())
    {
        const uint32_t chunk = read32 (data.data() + offset + 4);
        const unsigned char* body = data.data() + offset + 8;
        if (std::memcmp (data.data() + offset, "fmt ", 4) == 0)
        {
            format = read16 (body);
            audio.channels = read16 (body + 2);
            audio.rate = (int) read32 (body + 4);
            bits = read16 (body + 14);
            if (format == 0xFFFE && chunk >= 26)
                format = read16 (body + 24);
        }
        else if (std::memcmp (data.data() + offset, "data", 4) == 0 && audio.channels > 0)
        {
            const size_t available = std::min ((size_t) chunk, data.size() - (offset + 8));
            const int bytes = bits / 8;
            const size_t frames = available / (size_t) (bytes * audio.channels);
            audio.samples.assign ((size_t) audio.channels, std::vector<float> (frames));
            for (size_t n = 0; n < frames; ++n)
            {
                for (int c = 0; c < audio.channels; ++c)
                {
                    const unsigned char* p = body + n * (size_t) (bytes * audio.channels) + (size_t) (c * bytes);
                    double v = 0.0;
                    if (format == 3 && bits == 32)
                    {
                        float f;
                        std::memcpy (&f, p, 4);
                        v = f;
                    }
                    else if (format == 1 && bits == 16)
                        v = (int16_t) read16 (p) / 32768.0;
                    else if (format == 1 && bits == 24)
                        v = (int32_t) ((uint32_t) (p[0] << 8) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 24)) / 2147483648.0;
                    else if (format == 1 && bits == 32)
                        v = (int32_t) read32 (p) / 2147483648.0;
                    else
                        return false;
                    audio.samples[(size_t) c][n] = (float) v;
                }
            }
            return true;
        }
        offset += 8 + (size_t) chunk + (chunk & 1);
    }
    return false;
}

inline bool writeWav (const char* path, const Audio& audio)
{
    FILE* file = std::fopen (path, "wb");
    if (file == nullptr)
        return false;
    const auto put32 = [file] (uint32_t v) { const unsigned char b[4] = { (unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24) }; std::fwrite (b, 1, 4, file); };
    const auto put16 = [file] (uint16_t v) { const unsigned char b[2] = { (unsigned char) v, (unsigned char) (v >> 8) }; std::fwrite (b, 1, 2, file); };
    const uint32_t frames = (uint32_t) audio.samples[0].size();
    const uint32_t bytes = frames * (uint32_t) audio.channels * 4;
    std::fwrite ("RIFF", 1, 4, file);
    put32 (36 + bytes);
    std::fwrite ("WAVEfmt ", 1, 8, file);
    put32 (16);
    put16 (3);
    put16 ((uint16_t) audio.channels);
    put32 ((uint32_t) audio.rate);
    put32 ((uint32_t) audio.rate * (uint32_t) audio.channels * 4);
    put16 ((uint16_t) (audio.channels * 4));
    put16 (32);
    std::fwrite ("data", 1, 4, file);
    put32 (bytes);
    for (uint32_t n = 0; n < frames; ++n)
        for (int c = 0; c < audio.channels; ++c)
            std::fwrite (&audio.samples[(size_t) c][n], 4, 1, file);
    std::fclose (file);
    return true;
}
} // namespace colgate_tools
