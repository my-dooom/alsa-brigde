#include "reverb.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------
static constexpr int   kN               = 8;       // reverb channel count
static constexpr int   kDiffuserStages  = 4;
static constexpr float kDiffuserMaxMs   = 0.020f;  // 20 ms max diffuser delay
static constexpr float kFeedbackMinSec  = 0.10f;   // 100 ms
static constexpr float kFeedbackMaxSec  = 0.20f;   // 200 ms
static constexpr float kTargetGain      = 1e-6f;   // -60 dB
static constexpr float kAvgFeedbackDelay =
    0.5f * (kFeedbackMinSec + kFeedbackMaxSec);

// ---------------------------------------------------------------------------
// Delay line
// ---------------------------------------------------------------------------
struct Delay {
    std::vector<float> buf;
    int pos = 0;

    explicit Delay(int num_samples) : buf(num_samples, 0.0f) {
        assert(num_samples > 0);
    }

    float read() const { return buf[pos]; }
    void  write(float v) { buf[pos] = v; }
    void  advance() { pos = (pos + 1) % static_cast<int>(buf.size()); }
};

// ---------------------------------------------------------------------------
// Householder matrix: H = I - 2*v*vT  (random unit vector v)
// ---------------------------------------------------------------------------
static std::array<float, kN * kN> make_householder(std::mt19937& rng) {
    std::normal_distribution<float> dist;
    std::array<float, kN> v;
    float norm_sq = 0.0f;
    for (int i = 0; i < kN; ++i) {
        v[i] = dist(rng);
        norm_sq += v[i] * v[i];
    }
    const float inv_norm = 1.0f / std::sqrt(norm_sq);
    for (auto& x : v) x *= inv_norm;

    std::array<float, kN * kN> H;
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j)
            H[i * kN + j] = (i == j ? 1.0f : 0.0f) - 2.0f * v[i] * v[j];
    return H;
}

// ---------------------------------------------------------------------------
// Diffuser stage: randomized delays + polarity flips + Householder mix
// ---------------------------------------------------------------------------
struct Diffuser {
    std::array<std::unique_ptr<Delay>, kN> delays;
    std::array<float, kN>       flip_polarity{};
    std::array<float, kN * kN>  mix_matrix{};

    Diffuser(double sample_rate, std::mt19937& rng) {
        std::uniform_int_distribution<int> binary(0, 1);
        for (int ch = 0; ch < kN; ++ch) {
            const float lo = kDiffuserMaxMs / kN * ch;
            const float hi = kDiffuserMaxMs / kN * (ch + 1);
            const float t  = std::uniform_real_distribution<float>(lo, hi)(rng);
            const int   n  = std::max(1, static_cast<int>(t * static_cast<float>(sample_rate)));
            delays[ch]     = std::make_unique<Delay>(n);
            flip_polarity[ch] = binary(rng) * 2.0f - 1.0f;
        }
        mix_matrix = make_householder(rng);
    }

    void process(std::array<float, kN>& slice) {
        std::array<float, kN> tmp;
        for (int ch = 0; ch < kN; ++ch) {
            tmp[ch] = delays[ch]->read() * flip_polarity[ch];
            delays[ch]->write(slice[ch]);
            delays[ch]->advance();
        }

        std::array<float, kN> mixed{};
        for (int row = 0; row < kN; ++row)
            for (int col = 0; col < kN; ++col)
                mixed[row] += tmp[col] * mix_matrix[row * kN + col];

        slice = mixed;
    }
};

// ---------------------------------------------------------------------------
// Feedback loop: randomized delays + Householder mix + decay
// ---------------------------------------------------------------------------
struct FeedbackLoop {
    std::array<std::unique_ptr<Delay>, kN> delays;
    std::array<float, kN * kN> mix_matrix{};
    std::array<float, kN> lp_state{};  // 1-pole LP state per channel (damping)

    FeedbackLoop(double sample_rate, std::mt19937& rng) {
        std::uniform_real_distribution<float> dist(kFeedbackMinSec, kFeedbackMaxSec);
        for (int ch = 0; ch < kN; ++ch) {
            const int n = std::max(1, static_cast<int>(dist(rng) * static_cast<float>(sample_rate)));
            delays[ch]  = std::make_unique<Delay>(n);
        }
        mix_matrix = make_householder(rng);
        lp_state.fill(0.0f);
    }

    // damping_coeff: 0 = bypass (bright), approaches 1 = heavy LP (dark).
    void process(std::array<float, kN>& slice, float decay_gain, float damping_coeff) {
        std::array<float, kN> out;
        std::array<float, kN> decayed;
        for (int ch = 0; ch < kN; ++ch) {
            out[ch] = delays[ch]->read();
            // 1-pole LP applied per feedback pass: y += coeff * (x - y)
            lp_state[ch] += damping_coeff * (out[ch] - lp_state[ch]);
            decayed[ch] = lp_state[ch] * decay_gain;
        }

        std::array<float, kN> mixed{};
        for (int row = 0; row < kN; ++row)
            for (int col = 0; col < kN; ++col)
                mixed[row] += decayed[col] * mix_matrix[row * kN + col];

        for (int ch = 0; ch < kN; ++ch) {
            delays[ch]->write(mixed[ch] + slice[ch]);
            delays[ch]->advance();
            slice[ch] = out[ch];
        }
    }
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct Reverb::Impl {
    double sample_rate;
    float  decay_time;
    float  wet_mix;
    float  damping;  // [0, 1): LP coeff per feedback pass (0=bright, →1=dark)

    std::array<Diffuser, kDiffuserStages> diffusers;
    FeedbackLoop feedback;

    Impl(double sr, float dt, float wm, std::mt19937& rng)
        : sample_rate(sr)
        , decay_time(dt)
        , wet_mix(std::clamp(wm, 0.0f, 1.0f))
        , damping(0.95f)  // default: slightly warm
        , diffusers{ Diffuser(sr, rng), Diffuser(sr, rng),
                     Diffuser(sr, rng), Diffuser(sr, rng) }
        , feedback(sr, rng)
    {}

    float decay_gain() const {
        const float inv_iters = kAvgFeedbackDelay / decay_time;
        return std::pow(kTargetGain, inv_iters);
    }
};

// ---------------------------------------------------------------------------
// Reverb public API
// ---------------------------------------------------------------------------
Reverb::Reverb(double sample_rate, float decay_time, float wet_mix) {
    const auto seed = static_cast<unsigned>(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::mt19937 rng(seed);
    m_impl = std::make_unique<Impl>(sample_rate, decay_time, wet_mix, rng);
}

Reverb::~Reverb() = default;

void Reverb::set_decay_time(float seconds) {
    m_impl->decay_time = std::max(0.01f, seconds);
}

void Reverb::set_wet_mix(float mix) {
    m_impl->wet_mix = std::clamp(mix, 0.0f, 1.0f);
}

void Reverb::set_damping(float damping) {
    // Clamp to [0, 0.9999]: coeff=1 would freeze the LP state.
    m_impl->damping = std::clamp(damping, 0.0f, 0.9999f);
}

void Reverb::process_samples_inplace(float* buffer, size_t frames, unsigned channels) {
    if (!buffer || frames == 0 || channels == 0) return;

    Impl& im = *m_impl;
    const float dg  = im.decay_gain();
    const float wet = im.wet_mix;
    const float dry = 1.0f - wet;

    for (size_t f = 0; f < frames; ++f) {
        float* frame_ptr = buffer + f * channels;

        // Mix all input channels to mono, then fan out to kN reverb channels.
        float mono = 0.0f;
        for (unsigned ch = 0; ch < channels; ++ch)
            mono += frame_ptr[ch];
        mono /= static_cast<float>(channels);

        std::array<float, kN> slice;
        slice.fill(mono);

        for (auto& diffuser : im.diffusers)
            diffuser.process(slice);
        im.feedback.process(slice, dg, im.damping);

        // Collapse reverb channels back to mono wet signal.
        float wet_out = 0.0f;
        for (int ch = 0; ch < kN; ++ch)
            wet_out += slice[ch];
        wet_out /= static_cast<float>(kN);

        for (unsigned ch = 0; ch < channels; ++ch)
            frame_ptr[ch] = dry * frame_ptr[ch] + wet * wet_out;
    }
}
