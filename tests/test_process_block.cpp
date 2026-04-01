#include "process_audio.hpp"
#include "verifier.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels = 2;
constexpr snd_pcm_uframes_t kFrames = 1024;

av::AudioBuffer make_sine_buffer() {
	av::AudioBuffer buf;
	buf.sample_rate = kSampleRate;
	buf.channels = kChannels;
	buf.total_frames = static_cast<uint32_t>(kFrames);
	buf.samples.resize(static_cast<size_t>(kFrames) * kChannels);

	for (size_t i = 0; i < kFrames; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
		const float s = 0.25f * std::sin(2.0f * 3.1415926535f * 440.0f * t);
		buf.samples[i * kChannels] = s;
		buf.samples[i * kChannels + 1] = s;
	}

	return buf;
}

} // namespace

int main() {
	av::AudioBuffer reference = make_sine_buffer();
	av::AudioBuffer processed = reference;

	process_samples_inplace(processed.samples.data(), kFrames, kChannels);

	av::Config cfg;
	cfg.mode = av::Mode::WavCompare;
	cfg.snr_threshold = 100.0;
	cfg.mse_threshold = 1e-10;
	cfg.corr_threshold = 0.999999;

	const av::VerifyResult result = av::verify_compare(reference, processed, cfg);
	if (!result.passed) {
		std::cerr << "verify_compare failed\n";
		for (const auto& err : result.errors) {
			std::cerr << "  error: " << err << '\n';
		}
		return 1;
	}

	if (result.metrics.snr_db < cfg.snr_threshold ||
		result.metrics.mse > cfg.mse_threshold ||
		result.metrics.cross_corr < cfg.corr_threshold) {
		std::cerr << "metric threshold check failed\n";
		std::cerr << "  snr_db: " << result.metrics.snr_db << '\n';
		std::cerr << "  mse: " << result.metrics.mse << '\n';
		std::cerr << "  cross_corr: " << result.metrics.cross_corr << '\n';
		return 1;
	}

	std::cout << "process block quality test passed\n";
	return 0;
}
