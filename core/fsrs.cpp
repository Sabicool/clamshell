#include "fsrs.h"

#include <algorithm>
#include <cmath>

namespace anki {

const float FSRS5_DEFAULT_DECAY = 0.5f;
const float FSRS6_DEFAULT_DECAY = 0.1542f;

static const float DEFAULT_PARAMETERS[21] = {
    0.212f,  1.2931f, 2.3065f, 8.2956f, 6.4133f, 0.8334f, 3.0194f,
    0.001f,  1.8722f, 0.1666f, 0.796f,  1.4835f, 0.0614f, 0.2629f,
    1.6483f, 0.6014f, 1.8729f, 0.5425f, 0.0912f, 0.0658f, 0.1542f};

static const float S_MIN = 0.001f;
static const float S_MAX = 36500.0f;
static const float D_MIN = 1.0f;
static const float D_MAX = 10.0f;
static const float INIT_S_MAX = 100.0f;

static inline float clampf(float v, float lo, float hi) {
    // Rust f32::clamp
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

float decay_from_params(const std::vector<float>& params) {
    if (params.empty()) return FSRS6_DEFAULT_DECAY;
    if (params.size() < 21) return FSRS5_DEFAULT_DECAY;
    return params[20];
}

bool Fsrs::init(const std::vector<float>& in) {
    std::vector<float> p;
    switch (in.size()) {
        case 0:
            p.assign(DEFAULT_PARAMETERS, DEFAULT_PARAMETERS + 21);
            break;
        case 17:
            p = in;
            p[4] = std::fma(p[5], 2.0f, p[4]);
            p[5] = std::log(std::fma(p[5], 3.0f, 1.0f)) / 3.0f;
            p[6] += 0.5f;
            p.insert(p.end(), {0.0f, 0.0f, 0.0f, FSRS5_DEFAULT_DECAY});
            break;
        case 19:
            p = in;
            p.insert(p.end(), {0.0f, FSRS5_DEFAULT_DECAY});
            break;
        case 21:
            p = in;
            break;
        default:
            return false;
    }
    for (float v : p)
        if (!std::isfinite(v)) return false;
    // clip_parameters(num_relearning_steps = 1, enable_short_term = false)
    const float w17_w18_ceiling = 2.0f;
    const float w19_floor = 0.0f;
    const float clamps[21][2] = {
        {S_MIN, INIT_S_MAX}, {S_MIN, INIT_S_MAX}, {S_MIN, INIT_S_MAX}, {S_MIN, INIT_S_MAX},
        {D_MIN, D_MAX},      {0.001f, 4.0f},      {0.001f, 4.0f},      {0.001f, 0.75f},
        {0.0f, 4.5f},        {0.0f, 0.8f},        {0.001f, 3.5f},      {0.001f, 5.0f},
        {0.001f, 0.25f},     {0.001f, 0.9f},      {0.0f, 4.0f},        {0.0f, 1.0f},
        {1.0f, 6.0f},        {0.0f, w17_w18_ceiling}, {0.0f, w17_w18_ceiling},
        {w19_floor, 0.8f},   {0.1f, 0.8f}};
    for (int i = 0; i < 21; i++) w_[i] = clampf(p[i], clamps[i][0], clamps[i][1]);
    return true;
}

static float power_forgetting_curve(const float* w, float t, float s) {
    float decay = -w[20];
    float factor = std::exp(std::log(0.9f) / decay) - 1.0f;
    return std::pow(t / s * factor + 1.0f, decay);
}

float Fsrs::retrievability(float t, float s) const { return power_forgetting_curve(w_, t, s); }

float Fsrs::next_interval(float stability, float desired_retention) const {
    float decay = -w_[20];
    float factor = std::exp(std::log(0.9f) / decay) - 1.0f;
    return stability / factor * (std::pow(desired_retention, 1.0f / decay) - 1.0f);
}

static float init_stability_w(const float* w, size_t rating) {
    size_t idx = rating == 0 ? 0 : rating - 1;
    if (idx > 3) idx = 3;
    return w[idx];
}

static float init_difficulty_w(const float* w, size_t rating) {
    float r = (float)(rating == 0 ? 0 : rating - 1);
    return w[4] - std::exp(w[5] * r) + 1.0f;
}

float Fsrs::init_stability(unsigned rating) const { return init_stability_w(w_, rating); }
float Fsrs::init_difficulty(unsigned rating) const { return init_difficulty_w(w_, rating); }

static float mean_reversion(const float* w, float new_d) {
    return w[7] * (init_difficulty_w(w, 4) - new_d) + new_d;
}

static float linear_damping(float delta_d, float old_d) { return (10.0f - old_d) * delta_d / 9.0f; }

static float next_difficulty(const float* w, float d, float rating) {
    float delta_d = -w[6] * (rating - 3.0f);
    return d + linear_damping(delta_d, d);
}

static float stability_after_success(const float* w, float s, float d, float r, float rating) {
    float hard_penalty = rating == 2.0f ? w[15] : 1.0f;
    float easy_bonus = rating == 4.0f ? w[16] : 1.0f;
    return s * (std::exp(w[8]) * (11.0f - d) * std::pow(s, -w[9]) *
                    (std::exp((1.0f - r) * w[10]) - 1.0f) * hard_penalty * easy_bonus +
                1.0f);
}

static float stability_after_failure(const float* w, float s, float d, float r) {
    float new_s = w[11] * std::pow(d, -w[12]) * (std::pow(s + 1.0f, w[13]) - 1.0f) *
                  std::exp((1.0f - r) * w[14]);
    float new_s_min = s / std::exp(w[17] * w[18]);
    return std::min(new_s, new_s_min);
}

static float stability_short_term(const float* w, float s, float rating) {
    float sinc = std::exp(w[17] * (rating - 3.0f + w[18])) * std::pow(s, -w[19]);
    return s * (rating >= 2.0f ? std::max(sinc, 1.0f) : sinc);
}

MemoryState Fsrs::step(float delta_t, float rating, MemoryState state, size_t nth) const {
    const float* w = w_;
    float last_s = clampf(state.stability, S_MIN, S_MAX);
    float last_d = clampf(state.difficulty, D_MIN, D_MAX);
    float r = power_forgetting_curve(w, delta_t, last_s);
    float sas = stability_after_success(w, last_s, last_d, r, rating);
    float saf = stability_after_failure(w, last_s, last_d, r);
    float sst = stability_short_term(w, last_s, rating);
    float new_s = rating == 1.0f ? saf : sas;
    if (delta_t == 0.0f) new_s = sst;
    float new_d = next_difficulty(w, last_d, rating);
    new_d = clampf(mean_reversion(w, new_d), D_MIN, D_MAX);
    if (nth == 0 && state.stability == 0.0f) {
        unsigned ir = (unsigned)rating;
        if (ir < 1) ir = 1;
        if (ir > 4) ir = 4;
        new_s = init_stability_w(w, ir);
        new_d = clampf(init_difficulty_w(w, ir), D_MIN, D_MAX);
    }
    if (rating == 0.0f) {
        new_s = last_s;
        new_d = last_d;
    }
    return MemoryState{clampf(new_s, S_MIN, S_MAX), new_d};
}

NextStates Fsrs::next_states(std::optional<MemoryState> current, float desired_retention,
                             unsigned days_elapsed) const {
    MemoryState st{0, 0};
    size_t nth = 0;
    if (current) {
        st = *current;
        nth = 1;
    }
    NextStates out;
    ItemState* slots[4] = {&out.again, &out.hard, &out.good, &out.easy};
    for (int rating = 1; rating <= 4; rating++) {
        MemoryState m = step((float)days_elapsed, (float)rating, st, nth);
        slots[rating - 1]->memory = m;
        slots[rating - 1]->interval = next_interval(m.stability, desired_retention);
    }
    return out;
}

}  // namespace anki
