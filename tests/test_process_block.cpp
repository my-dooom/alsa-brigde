#include "process_audio.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels = 2;
constexpr snd_pcm_uframes_t kFrames = kSampleRate * 2; // one full AM period
constexpr float kPi = 3.14159265358979323846f;

std::vector<int32_t> make_s32_sine_buffer() {
	std::vector<int32_t> buf(static_cast<size_t>(kFrames) * kChannels);
	constexpr float kS32Scale = 2147483647.0f;

	for (size_t i = 0; i < kFrames; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
		const float s = 0.25f * std::sin(2.0f * kPi * 440.0f * t);
		const int32_t v = static_cast<int32_t>(s * kS32Scale);
		buf[i * kChannels] = v;
		buf[i * kChannels + 1] = v;
	}

	return buf;
}

float expected_gain(size_t frame_index) {
	constexpr float kTwoPi = 6.28318530717958647692f;
	const float phase = kTwoPi * static_cast<float>(frame_index)
		/ (static_cast<float>(kSampleRate) * 2.0f);
	return 0.5f * (1.0f + std::cos(phase));
}

bool roughly_equal(float a, float b, float tol) {
	return std::fabs(a - b) <= tol;
}

} // namespace

int main() {
	std::vector<int32_t> reference = make_s32_sine_buffer();
	std::vector<int32_t> processed = reference;

	process_samples_inplace(processed.data(), kFrames, kChannels);

	constexpr float kS32Scale = 2147483647.0f;
	const size_t idx_0s = 0;
	const size_t idx_05s = static_cast<size_t>(kSampleRate / 2);
	const size_t idx_1s = static_cast<size_t>(kSampleRate);

	auto norm = [](int32_t v) {
		return static_cast<float>(v) / kS32Scale;
	};

	const float in_05 = norm(reference[idx_05s * kChannels]);
	const float out_05 = norm(processed[idx_05s * kChannels]);
	const float exp_05 = in_05 * expected_gain(idx_05s);

	const float out_1s = norm(processed[idx_1s * kChannels]);
	const float exp_1s = norm(reference[idx_1s * kChannels]) * expected_gain(idx_1s);

	if (!roughly_equal(norm(processed[idx_0s * kChannels]), norm(reference[idx_0s * kChannels]), 1e-6f)) {
		std::cerr << "gain at t=0 is not near 1.0\n";
		return 1;
	}

	if (!roughly_equal(out_05, exp_05, 2e-4f)) {
		std::cerr << "unexpected gain at t=0.5s"
		          << " expected=" << exp_05
		          << " actual=" << out_05 << "\n";
		return 1;
	}

	if (!roughly_equal(out_1s, exp_1s, 1e-5f)) {
		std::cerr << "unexpected gain at t=1.0s"
		          << " expected=" << exp_1s
		          << " actual=" << out_1s << "\n";
		return 1;
	}

	std::cout << "S32_LE AM processing test passed\n";
	return 0;
}
