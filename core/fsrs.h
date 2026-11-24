// Port of the inference half of the `fsrs` crate 6.6.1 (as used by Anki 26.08).
// Training/optimisation is left to desktop Anki; the 3DS only needs next_states.
#pragma once
#include <optional>
#include <vector>

namespace anki {

struct MemoryState {
    float stability = 0;
    float difficulty = 0;
    bool operator==(const MemoryState& o) const {
        return stability == o.stability && difficulty == o.difficulty;
    }
};

struct ItemState {
    MemoryState memory;
    float interval = 0;
};

struct NextStates {
    ItemState again, hard, good, easy;
};

extern const float FSRS5_DEFAULT_DECAY;
extern const float FSRS6_DEFAULT_DECAY;

class Fsrs {
public:
    // Accepts 0 (defaults), 17, 19 or 21 parameters, like FSRS::new().
    // Returns false if the parameters are invalid.
    bool init(const std::vector<float>& params);
    NextStates next_states(std::optional<MemoryState> current, float desired_retention,
                           unsigned days_elapsed) const;
    MemoryState step(float delta_t, float rating, MemoryState state, size_t nth) const;
    float init_stability(unsigned rating) const;
    float init_difficulty(unsigned rating) const;
    float next_interval(float stability, float desired_retention) const;
    float retrievability(float days_elapsed, float stability) const;
    const float* w() const { return w_; }

private:
    float w_[21];
};

float decay_from_params(const std::vector<float>& params);

}  // namespace anki
