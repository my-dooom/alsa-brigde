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
    void reset();

    void process_interleaved(int32_t* buffer, size_t frames, size_t channels);

private:
    void rebuild_state();
    static void hadamard_in_place(std::vector<float>& values);

    float decay_;
    float mix_;
    size_t base_delay_len_;
    size_t channels_;

    std::vector<std::vector<float>> delay_lines_;
    std::vector<size_t> write_index_;
    std::vector<size_t> tap_offset_;
    std::vector<size_t> shuffle_index_;
    std::vector<float> polarity_;
    std::vector<float> stage_;
    std::vector<float> hadamard_buffer_;
    std::vector<float> diffusion_state_;
};

} // namespace mydoom