#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mydoom {

class Vibeverb {
public:
    Vibeverb(float decay, size_t delay_len, size_t channels = 2);

    void set_decay(float decay);
    void set_delay_length(size_t delay_len);
    void set_mix(float wet_mix);
    void set_diffusion(float diffusion);
    void set_diffusion_b(float diffusion);
    void reset();

    void process_interleaved(int32_t* buffer, size_t frames, size_t channels);

private:
    void rebuild_state();
    static float process_allpass(std::vector<float>& line, size_t& index, float input, float coeff);

    float decay_;
    float mix_;
    float diffusion_a_;
    float diffusion_b_;
    size_t base_delay_len_;
    size_t channels_;

    std::vector<std::vector<float>> predelay_lines_;
    std::vector<size_t> predelay_index_;

    std::vector<std::vector<float>> diffuser_a_lines_;
    std::vector<size_t> diffuser_a_index_;
    std::vector<std::vector<float>> diffuser_b_lines_;
    std::vector<size_t> diffuser_b_index_;

    std::vector<std::vector<std::vector<float>>> comb_lines_;
    std::vector<std::vector<size_t>> comb_index_;
    std::vector<std::vector<float>> comb_damp_state_;

    std::vector<float> crossfeed_state_;
};

} // namespace mydoom