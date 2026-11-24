// Small public-domain-style SHA-1 (for note field checksums and media names).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace anki {

inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    uint64_t bitlen = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t block[64];
    for (size_t off = 0; off < total; off += 64) {
        for (int i = 0; i < 64; i++) {
            size_t p = off + i;
            if (p < len) block[i] = data[p];
            else if (p == len) block[i] = 0x80;
            else if (p >= total - 8) block[i] = (uint8_t)(bitlen >> (8 * (total - 1 - p)));
            else block[i] = 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

inline std::string sha1_hex(const std::string& s) {
    uint8_t d[20];
    sha1((const uint8_t*)s.data(), s.size(), d);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : d) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

}  // namespace anki
