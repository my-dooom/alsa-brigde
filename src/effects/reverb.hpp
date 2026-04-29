#pragma once

#include <cstddef>
#include <memory>

// MVerb reverb — Martin Eastwood (GPLv3).
// Operates on normalized float samples [-1.0, 1.0].
class Reverb {
public:
    // sample_rate : Hz
    // decay_time  : seconds for tail to reach -60 dB
    // wet_mix     : 0 = dry only, 1 = wet only
    explicit Reverb(double sample_rate, float decay_time = 2.0f, float wet_mix = 0.5f);
    ~Reverb();

    void set_decay_time(float seconds);
    void set_wet_mix(float mix);      // clamped to [0, 1]
    void set_damping(float damping);  // 0 = bright (no LP), 1 = very dark [0, 1)
    void set_bandwidth(float bandwidth);  // input bandwidth filter center freq [0, 1]

    // Process interleaved normalized float samples in place.
    // channels: number of interleaved channels per frame (e.g. 2 for stereo).
    void process_samples_inplace(float* buffer, size_t frames, unsigned channels);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
