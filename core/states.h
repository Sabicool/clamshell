// Port of rslib/src/scheduler/states/* from Anki 26.08: the card state machine
// that decides what Again/Hard/Good/Easy do. Kept structurally close to the Rust
// so it can be diffed against upstream when Anki changes.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "fsrs.h"

namespace anki {

enum class IntervalUnit { Secs, Days };
struct IntervalKind {
    IntervalUnit unit;
    uint32_t value;
    IntervalKind maybe_as_days(uint32_t secs_until_rollover) const;
    uint32_t as_seconds() const;
    int32_t as_revlog_interval() const;
};

enum class RevlogKind : uint8_t { Learning = 0, Review = 1, Relearning = 2, Filtered = 3, Manual = 4, Rescheduled = 5 };

struct LearnState {
    uint32_t remaining_steps = 0;
    uint32_t scheduled_secs = 0;
    uint32_t elapsed_secs = 0;
    std::optional<MemoryState> memory_state;
};

struct ReviewState {
    uint32_t scheduled_days = 0;
    uint32_t elapsed_days = 0;
    float ease_factor = 2.5f;
    uint32_t lapses = 0;
    bool leeched = false;
    std::optional<MemoryState> memory_state;
    int32_t days_late() const { return (int32_t)elapsed_days - (int32_t)scheduled_days; }
};

enum class NormalKind { New, Learning, Review, Relearning };
struct NormalState {
    NormalKind kind = NormalKind::New;
    uint32_t new_position = 0;  // New
    LearnState learning;        // Learning, Relearning
    ReviewState review;         // Review, Relearning
    bool leeched() const;
    std::optional<ReviewState> review_state() const;
};

enum class CardStateKind { Normal, Preview, Rescheduling };
struct CardState {
    CardStateKind kind = CardStateKind::Normal;
    NormalState normal;  // Normal, or original_state for Rescheduling
    uint32_t preview_scheduled_secs = 0;
    bool preview_finished = false;

    static CardState from_normal(const NormalState& n) { CardState c; c.normal = n; return c; }
    IntervalKind interval_kind() const;
    RevlogKind revlog_kind() const;
    bool leeched() const;
    std::optional<uint32_t> new_position() const;
};

struct SchedulingStates {
    CardState current, again, hard, good, easy;
};

class LearningSteps {
public:
    explicit LearningSteps(const std::vector<float>* steps) : steps_(steps) {}
    std::optional<uint32_t> again_delay_secs_learn() const;
    std::optional<uint32_t> hard_delay_secs(uint32_t remaining) const;
    std::optional<uint32_t> good_delay_secs(uint32_t remaining) const;
    uint32_t current_delay_secs(uint32_t remaining) const;
    uint32_t remaining_for_good(uint32_t remaining) const;
    uint32_t remaining_for_failed() const { return (uint32_t)steps_->size(); }
    bool is_empty() const { return steps_->empty(); }

private:
    const std::vector<float>* steps_;
    size_t get_index(uint32_t remaining) const;
    std::optional<uint32_t> secs_at_index(size_t i) const;
    uint32_t hard_delay_secs_for_first_step(uint32_t again_secs) const;
};

// Returns Some(interval) when the load balancer picks one, None to fall back to fuzz.
using LoadBalancerFn = std::function<std::optional<uint32_t>(float interval, uint32_t minimum, uint32_t maximum)>;

struct StateContext {
    std::optional<float> fuzz_factor;
    std::optional<NextStates> fsrs_next_states;
    bool fsrs_short_term_with_steps_enabled = false;
    bool fsrs_allow_short_term = false;
    const std::vector<float>* learn_steps = nullptr;
    uint32_t graduating_interval_good = 1;
    uint32_t graduating_interval_easy = 4;
    float initial_ease_factor = 2.5f;
    float hard_multiplier = 1.2f;
    float easy_multiplier = 1.3f;
    float interval_multiplier = 1.0f;
    uint32_t maximum_review_interval = 36500;
    uint32_t leech_threshold = 8;
    LoadBalancerFn load_balancer;  // empty if disabled
    const std::vector<float>* relearn_steps = nullptr;
    float lapse_multiplier = 0.0f;
    uint32_t minimum_lapse_interval = 1;
    bool in_filtered_deck = false;
    uint32_t preview_again = 0, preview_hard = 0, preview_good = 0;

    LearningSteps steps() const { return LearningSteps(learn_steps); }
    LearningSteps relearn() const { return LearningSteps(relearn_steps); }
    std::pair<uint32_t, uint32_t> min_and_max_review_intervals(uint32_t minimum) const;
    uint32_t with_review_fuzz(float interval, uint32_t minimum, uint32_t maximum) const;
};

SchedulingStates next_states(const CardState& current, const StateContext& ctx);

// fuzz.rs free functions
uint32_t with_review_fuzz(std::optional<float> fuzz_factor, float interval, uint32_t minimum, uint32_t maximum);
std::pair<uint32_t, uint32_t> constrained_fuzz_bounds(float interval, uint32_t minimum, uint32_t maximum);

// Rust `as u32` on f32: saturating, NaN -> 0.
inline uint32_t f2u(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 4294967295.0f) return 4294967295u;
    return (uint32_t)v;
}

}  // namespace anki
