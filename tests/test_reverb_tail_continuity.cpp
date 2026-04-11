#include "process_audio.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

constexpr snd_pcm_uframes_t kFramesPerBlock = 128;
constexpr unsigned kChannels = 2;
constexpr float kS32Scale = 2147483647.0f;

bool has_nonzero_sample(const std::vector<int32_t>& buffer) {
    for (int32_t sample : buffer) {
        if (sample != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    // Use a clearly audible reverb setting where tail should persist across blocks.
    set_effect_target_params(EffectParams{0.90f, 0.95f, 24});

    std::vector<int32_t> block_a(static_cast<size_t>(kFramesPerBlock) * kChannels, 0);
    std::vector<int32_t> block_b(static_cast<size_t>(kFramesPerBlock) * kChannels, 0);

    // One-sample impulse on both channels.
    const int32_t impulse = static_cast<int32_t>(0.8f * kS32Scale);
    block_a[0] = impulse;
    block_a[1] = impulse;

    process_samples_inplace(block_a.data(), kFramesPerBlock, kChannels);

    // Retune delay length to emulate live knob movement; tail should not reset to silence.
    set_effect_target_params(EffectParams{0.90f, 0.95f, 64});
    process_samples_inplace(block_b.data(), kFramesPerBlock, kChannels);

    if (!has_nonzero_sample(block_a)) {
        std::cerr << "unexpected: first processed block is all zeros\n";
        return 1;
    }

    if (!has_nonzero_sample(block_b)) {
        std::cerr << "reverb tail continuity failed after delay retune: second block is all zeros\n";
        return 1;
    }

    std::cout << "reverb tail continuity test passed\n";
    return 0;
}
