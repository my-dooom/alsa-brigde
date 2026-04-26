#pragma once

#include <cstddef>
#include <memory>

// Diffusion reverb — Geraint Luff ADC21 design, no external dependencies.
// Operates on normalized float samples [-1.0, 1.0].
// Architecture: 4x diffuser stages (8ch Householder) → feedback loop (8ch Householder).
class Reverb {
public:
    // sample_rate : Hz
    // decay_time  : seconds for tail to reach -60 dB
    // wet_mix     : 0 = dry only, 1 = wet only
    explicit Reverb(double sample_rate, float decay_time = 2.0f, float wet_mix = 0.5f);
    ~Reverb();

    void set_decay_time(float seconds);
    void set_wet_mix(float mix);      // clamped to [0, 1]
    void set_damping(float damping);  // 0 = bright (no LP), 1 = very dark; clamped to [0, 1)

    // Process interleaved normalized float samples in place.
    // channels: number of interleaved channels per frame (e.g. 2 for stereo).
    void process_samples_inplace(float* buffer, size_t frames, unsigned channels);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
