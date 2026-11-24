// Bit-exact port of the parts of Rust's `rand` 0.9 that Anki's scheduler uses:
// StdRng (ChaCha12) seeded with seed_from_u64, random_range for f32 and u32,
// and WeightedIndex<f32>. Anki derives interval fuzz and load-balancer choices
// from these, so the 3DS has to produce the same numbers as desktop Anki.
#pragma once
#include <cstdint>
#include <vector>

namespace anki {

class StdRng {
public:
    static StdRng seed_from_u64(uint64_t state);
    static StdRng from_seed(const uint8_t seed[32]);

    uint32_t next_u32();
    uint64_t next_u64();

    // rng.random_range(low..high) for f32 (rand 0.9 samples the inclusive form).
    float range_f32(float low, float high);
    // rng.random_range(low..high) for u32 (exclusive upper bound).
    uint32_t range_u32(uint32_t low, uint32_t high);

private:
    uint32_t key_[8];
    uint64_t block_pos_ = 0;
    uint32_t results_[64];
    size_t index_ = 64;
    void generate();
};

// rand::distr::weighted::WeightedIndex<f32>. Returns -1 if construction would fail
// (empty, negative, or all-zero weights), mirroring the Rust `.ok()?`.
int weighted_index_sample(const std::vector<float>& weights, StdRng& rng);

}  // namespace anki
