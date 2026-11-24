#include "states.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace anki {

static const uint32_t DAY = 86400;
static const float MINIMUM_EASE_FACTOR = 1.3f;
static const float EASE_FACTOR_AGAIN_DELTA = -0.2f;
static const float EASE_FACTOR_HARD_DELTA = -0.15f;
static const float EASE_FACTOR_EASY_DELTA = 0.15f;

// ---------------------------------------------------------------- interval_kind.rs

IntervalKind IntervalKind::maybe_as_days(uint32_t secs_until_rollover) const {
    if (unit == IntervalUnit::Secs && value >= secs_until_rollover)
        return {IntervalUnit::Days, ((value - secs_until_rollover) / 86400) + 1};
    return *this;
}

uint32_t IntervalKind::as_seconds() const {
    if (unit == IntervalUnit::Secs) return value;
    uint64_t v = (uint64_t)value * 86400;
    return v > 0xffffffffULL ? 0xffffffffu : (uint32_t)v;
}

int32_t IntervalKind::as_revlog_interval() const {
    if (unit == IntervalUnit::Days) return (int32_t)value;
    return value > (uint32_t)INT32_MAX ? -INT32_MAX : -(int32_t)value;
}

// ---------------------------------------------------------------- steps.rs

static uint32_t to_secs(float v) { return f2u(v * 60.0f); }

static uint32_t maybe_round_in_days(uint32_t secs) {
    if (secs > DAY) {
        uint64_t d = (uint64_t)f2u(std::round((float)secs / (float)DAY)) * DAY;
        return d > 0xffffffffULL ? 0xffffffffu : (uint32_t)d;
    }
    return secs;
}

size_t LearningSteps::get_index(uint32_t remaining) const {
    size_t total = steps_->size();
    size_t r = remaining % 1000;
    size_t a = total > r ? total - r : 0;
    size_t b = total > 0 ? total - 1 : 0;
    return std::min(a, b);
}

std::optional<uint32_t> LearningSteps::secs_at_index(size_t i) const {
    if (i < steps_->size()) return to_secs((*steps_)[i]);
    return std::nullopt;
}

std::optional<uint32_t> LearningSteps::again_delay_secs_learn() const { return secs_at_index(0); }

uint32_t LearningSteps::hard_delay_secs_for_first_step(uint32_t again_secs) const {
    if (auto next = secs_at_index(1)) {
        uint64_t sum = (uint64_t)again_secs + *next;
        if (sum > 0xffffffffULL) sum = 0xffffffffULL;
        return maybe_round_in_days((uint32_t)(sum / 2));
    }
    uint64_t a = (uint64_t)again_secs * 3;
    if (a > 0xffffffffULL) a = 0xffffffffULL;
    uint64_t b = (uint64_t)again_secs + DAY;
    if (b > 0xffffffffULL) b = 0xffffffffULL;
    uint32_t secs = (uint32_t)std::min(a / 2, b);
    return maybe_round_in_days(secs);
}

std::optional<uint32_t> LearningSteps::hard_delay_secs(uint32_t remaining) const {
    size_t idx = get_index(remaining);
    std::optional<uint32_t> cur = secs_at_index(idx);
    if (!cur && !steps_->empty()) cur = to_secs((*steps_)[0]);
    if (!cur) return std::nullopt;
    return idx == 0 ? hard_delay_secs_for_first_step(*cur) : *cur;
}

std::optional<uint32_t> LearningSteps::good_delay_secs(uint32_t remaining) const {
    return secs_at_index(get_index(remaining) + 1);
}

uint32_t LearningSteps::current_delay_secs(uint32_t remaining) const {
    return secs_at_index(get_index(remaining)).value_or(0);
}

uint32_t LearningSteps::remaining_for_good(uint32_t remaining) const {
    size_t idx = get_index(remaining);
    size_t total = steps_->size();
    return (uint32_t)(total > idx + 1 ? total - (idx + 1) : 0);
}

// ---------------------------------------------------------------- fuzz.rs

struct FuzzRange { float start, end, factor; };
static const FuzzRange FUZZ_RANGES[3] = {
    {2.5f, 7.0f, 0.15f},
    {7.0f, 20.0f, 0.1f},
    {20.0f, std::numeric_limits<float>::max(), 0.05f},
};

static float fuzz_delta(float interval) {
    if (interval < 2.5f) return 0.0f;
    float delta = 1.0f;
    for (const auto& r : FUZZ_RANGES)
        delta = delta + r.factor * std::max(std::min(interval, r.end) - r.start, 0.0f);
    return delta;
}

static std::pair<uint32_t, uint32_t> fuzz_bounds(float interval) {
    float delta = fuzz_delta(interval);
    return {f2u(std::round(interval - delta)), f2u(std::round(interval + delta))};
}

std::pair<uint32_t, uint32_t> constrained_fuzz_bounds(float interval, uint32_t minimum, uint32_t maximum) {
    minimum = std::min(minimum, maximum);
    float iv = interval;
    if (iv < (float)minimum) iv = (float)minimum;
    if (iv > (float)maximum) iv = (float)maximum;
    auto [lower, upper] = fuzz_bounds(iv);
    lower = std::clamp<uint32_t>(lower, minimum, maximum);
    upper = std::clamp<uint32_t>(upper, minimum, maximum);
    if (upper == lower && upper > 2 && upper < maximum) upper = lower + 1;
    return {lower, upper};
}

uint32_t with_review_fuzz(std::optional<float> fuzz_factor, float interval, uint32_t minimum, uint32_t maximum) {
    if (fuzz_factor) {
        auto [lower, upper] = constrained_fuzz_bounds(interval, minimum, maximum);
        return f2u(std::floor((float)lower + *fuzz_factor * (float)(1 + upper - lower)));
    }
    return std::clamp<uint32_t>(f2u(std::round(interval)), minimum, maximum);
}

static uint32_t minimum_review_fuzz_interval(float interval, uint32_t previous_interval, uint32_t maximum_interval) {
    uint32_t rounded = f2u(std::round(interval));
    uint32_t upper = constrained_fuzz_bounds(interval, 1, maximum_interval).second;
    if (rounded > previous_interval) return previous_interval + 1;
    if (previous_interval <= upper) return previous_interval;
    return 0;
}

std::pair<uint32_t, uint32_t> StateContext::min_and_max_review_intervals(uint32_t minimum) const {
    uint32_t maximum = std::max<uint32_t>(maximum_review_interval, 1u);
    minimum = std::clamp<uint32_t>(minimum, 1u, maximum);
    return {minimum, maximum};
}

uint32_t StateContext::with_review_fuzz(float interval, uint32_t minimum, uint32_t maximum) const {
    if (load_balancer) {
        if (auto v = load_balancer(interval, minimum, maximum)) return *v;
    }
    return anki::with_review_fuzz(fuzz_factor, interval, minimum, maximum);
}

// ---------------------------------------------------------------- helpers

static CardState from_learn(const LearnState& l) {
    NormalState n;
    n.kind = NormalKind::Learning;
    n.learning = l;
    return CardState::from_normal(n);
}
static CardState from_review(const ReviewState& r) {
    NormalState n;
    n.kind = NormalKind::Review;
    n.review = r;
    return CardState::from_normal(n);
}
static CardState from_relearn(const LearnState& l, const ReviewState& r) {
    NormalState n;
    n.kind = NormalKind::Relearning;
    n.learning = l;
    n.review = r;
    return CardState::from_normal(n);
}

static std::optional<MemoryState> mem(const StateContext& ctx, int which) {
    if (!ctx.fsrs_next_states) return std::nullopt;
    const NextStates& s = *ctx.fsrs_next_states;
    switch (which) {
        case 1: return s.again.memory;
        case 2: return s.hard.memory;
        case 3: return s.good.memory;
        default: return s.easy.memory;
    }
}

static float fsrs_interval(const NextStates& s, int which) {
    switch (which) {
        case 1: return s.again.interval;
        case 2: return s.hard.interval;
        case 3: return s.good.interval;
        default: return s.easy.interval;
    }
}

// ---------------------------------------------------------------- learning.rs

static CardState learn_answer_passfail(const LearnState& self, const StateContext& ctx, int which,
                                       std::optional<uint32_t> step_delay, bool keep_remaining,
                                       uint32_t new_remaining) {
    auto memory_state = mem(ctx, which);
    if (step_delay) {
        LearnState l;
        l.remaining_steps = keep_remaining ? self.remaining_steps : new_remaining;
        l.scheduled_secs = *step_delay;
        l.elapsed_secs = 0;
        l.memory_state = memory_state;
        return from_learn(l);
    }
    auto [minimum, maximum] = ctx.min_and_max_review_intervals(1);
    float interval;
    bool short_term;
    if (ctx.fsrs_next_states) {
        interval = fsrs_interval(*ctx.fsrs_next_states, which);
        short_term = ctx.fsrs_allow_short_term &&
                     (ctx.fsrs_short_term_with_steps_enabled || ctx.steps().is_empty()) &&
                     interval < 0.5f;
    } else {
        interval = (float)ctx.graduating_interval_good;
        short_term = false;
    }
    if (short_term) {
        LearnState l;
        // again resets remaining steps; hard/good keep `..self`
        l.remaining_steps = which == 1 ? ctx.steps().remaining_for_failed() : self.remaining_steps;
        l.scheduled_secs = f2u(interval * 86400.0f);
        l.elapsed_secs = 0;
        l.memory_state = memory_state;
        return from_learn(l);
    }
    ReviewState r;
    r.scheduled_days = ctx.with_review_fuzz(std::max(std::round(interval), 1.0f), minimum, maximum);
    r.ease_factor = ctx.initial_ease_factor;
    r.memory_state = memory_state;
    return from_review(r);
}

static SchedulingStates learn_next_states(const LearnState& self, const StateContext& ctx) {
    SchedulingStates s;
    s.current = from_learn(self);
    auto steps = ctx.steps();
    s.again = learn_answer_passfail(self, ctx, 1, steps.again_delay_secs_learn(), false,
                                    steps.remaining_for_failed());
    s.hard = learn_answer_passfail(self, ctx, 2, steps.hard_delay_secs(self.remaining_steps), true, 0);
    s.good = learn_answer_passfail(self, ctx, 3, steps.good_delay_secs(self.remaining_steps), false,
                                   steps.remaining_for_good(self.remaining_steps));
    // easy
    auto [minimum, maximum] = ctx.min_and_max_review_intervals(1);
    uint32_t interval;
    if (ctx.fsrs_next_states) {
        uint32_t good = ctx.with_review_fuzz(ctx.fsrs_next_states->good.interval, minimum, maximum);
        minimum = good + 1;
        interval = f2u(std::max(std::round(ctx.fsrs_next_states->easy.interval), 1.0f));
    } else {
        interval = ctx.graduating_interval_easy;
    }
    ReviewState r;
    r.scheduled_days = ctx.with_review_fuzz((float)interval, minimum, maximum);
    r.ease_factor = ctx.initial_ease_factor;
    r.memory_state = mem(ctx, 4);
    s.easy = from_review(r);
    return s;
}

// ---------------------------------------------------------------- review.rs

static bool leech_threshold_met(uint32_t lapses, uint32_t threshold) {
    if (threshold > 0) {
        uint32_t half = f2u(std::max(std::ceil((float)threshold / 2.0f), 1.0f));
        return lapses >= threshold && (lapses - threshold) % half == 0;
    }
    return false;
}

static uint32_t constrain_passing_interval(const StateContext& ctx, float interval, uint32_t minimum, bool fuzz) {
    if (!ctx.fsrs_next_states) interval = interval * ctx.interval_multiplier;
    auto [mn, mx] = ctx.min_and_max_review_intervals(minimum);
    if (fuzz) return ctx.with_review_fuzz(interval, mn, mx);
    return std::clamp<uint32_t>(f2u(std::round(interval)), mn, mx);
}

static std::pair<float, std::optional<MemoryState>> failing_review_interval(const ReviewState& self,
                                                                            const StateContext& ctx) {
    if (ctx.fsrs_next_states) return {ctx.fsrs_next_states->again.interval, ctx.fsrs_next_states->again.memory};
    auto [mn, mx] = ctx.min_and_max_review_intervals(ctx.minimum_lapse_interval);
    uint32_t iv = ctx.with_review_fuzz(std::max((float)self.scheduled_days, 1.0f) * ctx.lapse_multiplier, mn, mx);
    return {(float)iv, std::nullopt};
}

static void passing_review_intervals(const ReviewState& self, const StateContext& ctx, uint32_t& hard,
                                     uint32_t& good, uint32_t& easy) {
    if (ctx.fsrs_next_states) {
        const NextStates& st = *ctx.fsrs_next_states;
        hard = constrain_passing_interval(
            ctx, st.hard.interval,
            std::max<uint32_t>(minimum_review_fuzz_interval(st.hard.interval, self.scheduled_days, ctx.maximum_review_interval), 1u),
            true);
        good = constrain_passing_interval(
            ctx, st.good.interval,
            std::max<uint32_t>(minimum_review_fuzz_interval(st.good.interval, self.scheduled_days, ctx.maximum_review_interval), hard + 1),
            true);
        easy = constrain_passing_interval(
            ctx, st.easy.interval,
            std::max<uint32_t>(minimum_review_fuzz_interval(st.easy.interval, self.scheduled_days, ctx.maximum_review_interval), good + 1),
            true);
    } else if (self.days_late() < 0) {
        float scheduled = std::max((float)self.scheduled_days, 1.0f);
        float elapsed = (float)self.elapsed_days;
        float factor = ctx.hard_multiplier;
        float half_usual = factor / 2.0f;
        hard = constrain_passing_interval(ctx, std::max(elapsed * factor, scheduled * half_usual), 0, false);
        good = constrain_passing_interval(ctx, std::max(elapsed * self.ease_factor, scheduled), 0, false);
        float reduced_bonus = ctx.easy_multiplier - (ctx.easy_multiplier - 1.0f) / 2.0f;
        easy = constrain_passing_interval(ctx, std::max(elapsed * self.ease_factor, scheduled) * reduced_bonus, 0, false);
    } else {
        float current_interval = std::max((float)self.scheduled_days, 1.0f);
        float days_late = (float)std::max<int32_t>(self.days_late(), 0);
        float hard_factor = ctx.hard_multiplier;
        uint32_t hard_minimum = hard_factor <= 1.0f ? 0 : self.scheduled_days + 1;
        hard = constrain_passing_interval(ctx, current_interval * hard_factor, hard_minimum, true);
        uint32_t good_minimum = hard_factor <= 1.0f ? self.scheduled_days + 1 : hard + 1;
        good = constrain_passing_interval(ctx, (current_interval + days_late / 2.0f) * self.ease_factor, good_minimum, true);
        easy = constrain_passing_interval(ctx, (current_interval + days_late) * self.ease_factor * ctx.easy_multiplier,
                                          good + 1, true);
    }
}

static SchedulingStates review_next_states(const ReviewState& self, const StateContext& ctx) {
    uint32_t hard_iv, good_iv, easy_iv;
    passing_review_intervals(self, ctx, hard_iv, good_iv, easy_iv);
    SchedulingStates s;
    s.current = from_review(self);
    // again
    {
        uint32_t lapses = self.lapses + 1;
        bool leeched = leech_threshold_met(lapses, ctx.leech_threshold);
        auto [scheduled_days, memory_state] = failing_review_interval(self, ctx);
        ReviewState again_review;
        again_review.scheduled_days = f2u(std::max(std::round(scheduled_days), 1.0f));
        again_review.elapsed_days = 0;
        again_review.ease_factor = std::max(self.ease_factor + EASE_FACTOR_AGAIN_DELTA, MINIMUM_EASE_FACTOR);
        again_review.lapses = lapses;
        again_review.leeched = leeched;
        again_review.memory_state = memory_state;
        auto relearn = ctx.relearn();
        if (auto again_delay = relearn.again_delay_secs_learn()) {
            LearnState l;
            l.remaining_steps = relearn.remaining_for_failed();
            l.scheduled_secs = *again_delay;
            l.memory_state = memory_state;
            s.again = from_relearn(l, again_review);
        } else if (ctx.fsrs_allow_short_term &&
                   (ctx.fsrs_short_term_with_steps_enabled || relearn.is_empty()) && scheduled_days < 0.5f) {
            LearnState l;
            l.remaining_steps = relearn.remaining_for_failed();
            l.scheduled_secs = f2u(scheduled_days * 86400.0f);
            l.memory_state = memory_state;
            s.again = from_relearn(l, again_review);
        } else {
            s.again = from_review(again_review);
        }
    }
    ReviewState h = self;
    h.scheduled_days = hard_iv;
    h.elapsed_days = 0;
    h.ease_factor = std::max(self.ease_factor + EASE_FACTOR_HARD_DELTA, MINIMUM_EASE_FACTOR);
    h.memory_state = mem(ctx, 2);
    s.hard = from_review(h);
    ReviewState g = self;
    g.scheduled_days = good_iv;
    g.elapsed_days = 0;
    g.memory_state = mem(ctx, 3);
    s.good = from_review(g);
    ReviewState e = self;
    e.scheduled_days = easy_iv;
    e.elapsed_days = 0;
    e.ease_factor = self.ease_factor + EASE_FACTOR_EASY_DELTA;
    e.memory_state = mem(ctx, 4);
    s.easy = from_review(e);
    return s;
}

// ---------------------------------------------------------------- relearning.rs

static SchedulingStates relearn_next_states(const LearnState& learning, const ReviewState& review,
                                            const StateContext& ctx) {
    SchedulingStates s;
    s.current = from_relearn(learning, review);
    auto relearn = ctx.relearn();
    // again
    {
        auto [scheduled_days, memory_state] = failing_review_interval(review, ctx);
        if (auto again_delay = relearn.again_delay_secs_learn()) {
            LearnState l;
            l.remaining_steps = relearn.remaining_for_failed();
            l.scheduled_secs = *again_delay;
            l.memory_state = memory_state;
            ReviewState r = review;
            r.scheduled_days = f2u(std::max(std::round(scheduled_days), 1.0f));
            r.elapsed_days = 0;
            r.memory_state = memory_state;
            s.again = from_relearn(l, r);
        } else if (ctx.fsrs_next_states) {
            auto [mn, mx] = ctx.min_and_max_review_intervals(1);
            float interval = ctx.fsrs_next_states->again.interval;
            ReviewState r = review;
            r.scheduled_days = ctx.with_review_fuzz(std::max(std::round(interval), 1.0f), mn, mx);
            r.memory_state = memory_state;
            if (ctx.fsrs_allow_short_term &&
                (ctx.fsrs_short_term_with_steps_enabled || relearn.is_empty()) && interval < 0.5f) {
                LearnState l;
                l.remaining_steps = relearn.remaining_for_failed();
                l.scheduled_secs = f2u(interval * 86400.0f);
                l.memory_state = memory_state;
                s.again = from_relearn(l, r);
            } else {
                s.again = from_review(r);
            }
        } else {
            s.again = from_review(review);
        }
    }
    // hard
    {
        auto memory_state = mem(ctx, 2);
        if (auto hard_delay = relearn.hard_delay_secs(learning.remaining_steps)) {
            LearnState l = learning;
            l.scheduled_secs = *hard_delay;
            l.memory_state = memory_state;
            ReviewState r = review;
            r.elapsed_days = 0;
            r.memory_state = memory_state;
            s.hard = from_relearn(l, r);
        } else if (ctx.fsrs_next_states) {
            auto [mn, mx] = ctx.min_and_max_review_intervals(1);
            float interval = ctx.fsrs_next_states->hard.interval;
            ReviewState r = review;
            r.scheduled_days = ctx.with_review_fuzz(std::max(std::round(interval), 1.0f), mn, mx);
            r.memory_state = memory_state;
            if (ctx.fsrs_allow_short_term &&
                (ctx.fsrs_short_term_with_steps_enabled || relearn.is_empty()) && interval < 0.5f) {
                LearnState l = learning;
                l.scheduled_secs = f2u(interval * 86400.0f);
                l.memory_state = memory_state;
                s.hard = from_relearn(l, r);
            } else {
                s.hard = from_review(r);
            }
        } else {
            s.hard = from_review(review);
        }
    }
    // good
    {
        auto memory_state = mem(ctx, 3);
        if (auto good_delay = relearn.good_delay_secs(learning.remaining_steps)) {
            LearnState l;
            l.scheduled_secs = *good_delay;
            l.remaining_steps = relearn.remaining_for_good(learning.remaining_steps);
            l.elapsed_secs = 0;
            l.memory_state = memory_state;
            ReviewState r = review;
            r.elapsed_days = 0;
            r.memory_state = memory_state;
            s.good = from_relearn(l, r);
        } else if (ctx.fsrs_next_states) {
            auto [mn, mx] = ctx.min_and_max_review_intervals(1);
            float interval = ctx.fsrs_next_states->good.interval;
            ReviewState r = review;
            r.scheduled_days = ctx.with_review_fuzz(std::max(std::round(interval), 1.0f), mn, mx);
            r.memory_state = memory_state;
            if (ctx.fsrs_allow_short_term &&
                (ctx.fsrs_short_term_with_steps_enabled || relearn.is_empty()) && interval < 0.5f) {
                LearnState l = learning;
                l.scheduled_secs = f2u(interval * 86400.0f);
                l.remaining_steps = relearn.remaining_for_good(learning.remaining_steps);
                l.memory_state = memory_state;
                s.good = from_relearn(l, r);
            } else {
                s.good = from_review(r);
            }
        } else {
            s.good = from_review(review);
        }
    }
    // easy
    {
        uint32_t scheduled_days;
        if (ctx.fsrs_next_states) {
            auto [mn, mx] = ctx.min_and_max_review_intervals(1);
            uint32_t good = ctx.with_review_fuzz(ctx.fsrs_next_states->good.interval, mn, mx);
            mn = good + 1;
            scheduled_days = ctx.with_review_fuzz(std::max(std::round(ctx.fsrs_next_states->easy.interval), 1.0f), mn, mx);
        } else {
            scheduled_days = review.scheduled_days + 1;
        }
        ReviewState r = review;
        r.scheduled_days = scheduled_days;
        r.elapsed_days = 0;
        r.memory_state = mem(ctx, 4);
        s.easy = from_review(r);
    }
    return s;
}

// ---------------------------------------------------------------- normal / filtered

static SchedulingStates normal_next_states(const NormalState& n, const StateContext& ctx) {
    switch (n.kind) {
        case NormalKind::New: {
            LearnState l;
            l.remaining_steps = ctx.steps().remaining_for_failed();
            SchedulingStates s = learn_next_states(l, ctx);
            s.current = CardState::from_normal(n);
            return s;
        }
        case NormalKind::Learning:
            return learn_next_states(n.learning, ctx);
        case NormalKind::Review:
            return review_next_states(n.review, ctx);
        case NormalKind::Relearning:
            return relearn_next_states(n.learning, n.review, ctx);
    }
    return {};
}

static CardState preview_delay_or_return(uint32_t seconds) {
    CardState c;
    c.kind = CardStateKind::Preview;
    c.preview_scheduled_secs = seconds;
    c.preview_finished = seconds == 0;
    return c;
}

static CardState maybe_wrap(const CardState& st) {
    if (st.normal.kind == NormalKind::Review) return st;
    CardState c = st;
    c.kind = CardStateKind::Rescheduling;
    return c;
}

SchedulingStates next_states(const CardState& current, const StateContext& ctx) {
    switch (current.kind) {
        case CardStateKind::Normal:
            return normal_next_states(current.normal, ctx);
        case CardStateKind::Preview: {
            SchedulingStates s;
            s.current = current;
            s.again = preview_delay_or_return(ctx.preview_again);
            s.hard = preview_delay_or_return(ctx.preview_hard);
            s.good = preview_delay_or_return(ctx.preview_good);
            s.easy = preview_delay_or_return(0);
            return s;
        }
        case CardStateKind::Rescheduling: {
            SchedulingStates n = normal_next_states(current.normal, ctx);
            if (!ctx.in_filtered_deck) return n;
            n.current = current;
            n.again = maybe_wrap(n.again);
            n.hard = maybe_wrap(n.hard);
            n.good = maybe_wrap(n.good);
            n.easy = maybe_wrap(n.easy);
            return n;
        }
    }
    return {};
}

// ---------------------------------------------------------------- CardState accessors

std::optional<ReviewState> NormalState::review_state() const {
    if (kind == NormalKind::Review || kind == NormalKind::Relearning) return review;
    return std::nullopt;
}

bool NormalState::leeched() const {
    auto r = review_state();
    return r && r->leeched;
}

bool CardState::leeched() const {
    if (kind == CardStateKind::Preview) return false;
    return normal.leeched();
}

IntervalKind CardState::interval_kind() const {
    if (kind == CardStateKind::Preview) return {IntervalUnit::Secs, preview_scheduled_secs};
    switch (normal.kind) {
        case NormalKind::New: return {IntervalUnit::Secs, 0};
        case NormalKind::Learning:
        case NormalKind::Relearning: return {IntervalUnit::Secs, normal.learning.scheduled_secs};
        case NormalKind::Review: return {IntervalUnit::Days, normal.review.scheduled_days};
    }
    return {IntervalUnit::Secs, 0};
}

RevlogKind CardState::revlog_kind() const {
    if (kind == CardStateKind::Preview) return RevlogKind::Filtered;
    switch (normal.kind) {
        case NormalKind::New:
        case NormalKind::Learning: return RevlogKind::Learning;
        case NormalKind::Relearning: return RevlogKind::Relearning;
        case NormalKind::Review: return normal.review.days_late() < 0 ? RevlogKind::Filtered : RevlogKind::Review;
    }
    return RevlogKind::Learning;
}

std::optional<uint32_t> CardState::new_position() const {
    if ((kind == CardStateKind::Normal || kind == CardStateKind::Rescheduling) && normal.kind == NormalKind::New)
        return normal.new_position;
    return std::nullopt;
}

}  // namespace anki
