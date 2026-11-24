#include "rng.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace anki {

static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d)            \
    a += b; d ^= a; d = rotl(d, 16); \
    c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8);  \
    c += d; b ^= c; b = rotl(b, 7);

StdRng StdRng::from_seed(const uint8_t seed[32]) {
    StdRng r;
    for (int i = 0; i < 8; i++) {
        r.key_[i] = (uint32_t)seed[i * 4] | ((uint32_t)seed[i * 4 + 1] << 8) |
                    ((uint32_t)seed[i * 4 + 2] << 16) | ((uint32_t)seed[i * 4 + 3] << 24);
    }
    r.block_pos_ = 0;
    r.index_ = 64;
    return r;
}

StdRng StdRng::seed_from_u64(uint64_t state) {
    // rand_core 0.9 SeedableRng::seed_from_u64: PCG32 fills the 32-byte seed.
    uint8_t seed[32];
    for (int chunk = 0; chunk < 8; chunk++) {
        const uint64_t MUL = 6364136223846793005ULL;
        const uint64_t INC = 11634580027462260723ULL;
        state = state * MUL + INC;
        uint32_t xorshifted = (uint32_t)(((state >> 18) ^ state) >> 27);
        uint32_t rot = (uint32_t)(state >> 59);
        uint32_t x = (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
        seed[chunk * 4 + 0] = x & 0xff;
        seed[chunk * 4 + 1] = (x >> 8) & 0xff;
        seed[chunk * 4 + 2] = (x >> 16) & 0xff;
        seed[chunk * 4 + 3] = (x >> 24) & 0xff;
    }
    return from_seed(seed);
}

// rand_chacha fills a 64-word buffer with four consecutive ChaCha12 blocks,
// using a 64-bit block counter in words 12-13 and a zero nonce in words 14-15.
void StdRng::generate() {
    for (int blk = 0; blk < 4; blk++) {
        uint64_t pos = block_pos_ + blk;
        uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                          key_[0], key_[1], key_[2], key_[3],
                          key_[4], key_[5], key_[6], key_[7],
                          (uint32_t)pos, (uint32_t)(pos >> 32), 0, 0};
        uint32_t x[16];
        memcpy(x, s, sizeof x);
        for (int i = 0; i < 6; i++) {  // 12 rounds = 6 double rounds
            QR(x[0], x[4], x[8], x[12]);
            QR(x[1], x[5], x[9], x[13]);
            QR(x[2], x[6], x[10], x[14]);
            QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]);
            QR(x[1], x[6], x[11], x[12]);
            QR(x[2], x[7], x[8], x[13]);
            QR(x[3], x[4], x[9], x[14]);
        }
        for (int i = 0; i < 16; i++) results_[blk * 16 + i] = x[i] + s[i];
    }
    block_pos_ += 4;
}

uint32_t StdRng::next_u32() {
    if (index_ >= 64) {
        generate();
        index_ = 0;
    }
    return results_[index_++];
}

uint64_t StdRng::next_u64() {
    const size_t len = 64;
    if (index_ < len - 1) {
        uint64_t v = ((uint64_t)results_[index_ + 1] << 32) | results_[index_];
        index_ += 2;
        return v;
    } else if (index_ >= len) {
        generate();
        index_ = 2;
        return ((uint64_t)results_[1] << 32) | results_[0];
    } else {
        uint64_t x = results_[len - 1];
        generate();
        index_ = 1;
        uint64_t y = results_[0];
        return (y << 32) | x;
    }
}

static inline float unit_f32_from(uint32_t r) {
    // (u32 >> 9).into_float_with_exponent(0) - 1.0
    uint32_t bits = (127u << 23) | (r >> 9);
    float v;
    memcpy(&v, &bits, 4);
    return v - 1.0f;
}

float StdRng::range_f32(float low, float high) {
    float scale = high - low;
    float value0_1 = unit_f32_from(next_u32());
    return value0_1 * scale + low;
}

uint32_t StdRng::range_u32(uint32_t low, uint32_t high) {
    // sample_single(low, high) -> sample_single_inclusive(low, high - 1), "biased" variant.
    uint32_t hi_incl = high - 1;
    uint32_t range = hi_incl - low + 1;
    if (range == 0) return next_u32();
    uint64_t m = (uint64_t)next_u32() * (uint64_t)range;
    uint32_t result = (uint32_t)(m >> 32);
    uint32_t lo_order = (uint32_t)m;
    if (lo_order > (uint32_t)(0u - range)) {
        uint64_t m2 = (uint64_t)next_u32() * (uint64_t)range;
        uint32_t new_hi_order = (uint32_t)(m2 >> 32);
        bool overflow = (uint64_t)lo_order + (uint64_t)new_hi_order > 0xffffffffULL;
        result += overflow ? 1 : 0;
    }
    return low + result;
}

int weighted_index_sample(const std::vector<float>& weights, StdRng& rng) {
    if (weights.empty()) return -1;
    float total = weights[0];
    if (!(total >= 0.0f)) return -1;
    std::vector<float> cumulative;
    cumulative.reserve(weights.size());
    for (size_t i = 1; i < weights.size(); i++) {
        float w = weights[i];
        if (!(w >= 0.0f)) return -1;
        cumulative.push_back(total);
        total += w;
        if (!std::isfinite(total)) return -1;
    }
    if (total == 0.0f) return -1;
    // UniformFloat::new(0, total) -> new_bounded: shrink scale until
    // scale * (1 - EPS) + low <= high.
    float low = 0.0f, high = total, scale = high - low;
    const float max_rand = 1.0f - std::numeric_limits<float>::epsilon();
    while (scale * max_rand + low > high) {
        uint32_t b;
        memcpy(&b, &scale, 4);
        b -= 1;
        memcpy(&scale, &b, 4);
    }
    float chosen = unit_f32_from(rng.next_u32()) * scale + low;
    // partition_point(|w| w <= chosen)
    size_t idx = 0;
    while (idx < cumulative.size() && cumulative[idx] <= chosen) idx++;
    return (int)idx;
}

}  // namespace anki
