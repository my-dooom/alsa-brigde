#include "verifier.hpp"

#include <alsa/asoundlib.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr unsigned kSampleRate = 48000;
constexpr unsigned kChannels = 2;
constexpr snd_pcm_uframes_t kPeriodFrames = 512;
constexpr unsigned kBufferPeriods = 4;
constexpr unsigned kSignalSeconds = 2;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kS32Scale = 2147483647.0f;
constexpr snd_pcm_format_t kFormat = SND_PCM_FORMAT_S32_LE;

static bool configure_stream(snd_pcm_t* handle, const char* role) {
	snd_pcm_hw_params_t* hw;
	snd_pcm_hw_params_alloca(&hw);

	int err = snd_pcm_hw_params_any(handle, hw);
	if (err < 0) {
		std::cerr << role << " hw_params_any failed: " << snd_strerror(err) << "\n";
		return false;
	}

	err = snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED);
	if (err < 0) {
		std::cerr << role << " set_access(MMAP_INTERLEAVED) failed: "
				  << snd_strerror(err) << "\n";
		return false;
	}

	err = snd_pcm_hw_params_set_format(handle, hw, kFormat);
	if (err < 0) {
		std::cerr << role << " set_format(S32_LE) failed: " << snd_strerror(err) << "\n";
		return false;
	}

	unsigned rate = kSampleRate;
	err = snd_pcm_hw_params_set_rate_near(handle, hw, &rate, nullptr);
	if (err < 0 || rate != kSampleRate) {
		std::cerr << role << " set_rate_near(" << kSampleRate << ") failed, got " << rate
				  << "\n";
		return false;
	}

	err = snd_pcm_hw_params_set_channels(handle, hw, kChannels);
	if (err < 0) {
		std::cerr << role << " set_channels failed: " << snd_strerror(err) << "\n";
		return false;
	}

	snd_pcm_uframes_t period = kPeriodFrames;
	err = snd_pcm_hw_params_set_period_size_near(handle, hw, &period, nullptr);
	if (err < 0) {
		std::cerr << role << " set_period_size_near failed: " << snd_strerror(err) << "\n";
		return false;
	}

	unsigned periods = kBufferPeriods;
	err = snd_pcm_hw_params_set_periods_near(handle, hw, &periods, nullptr);
	if (err < 0) {
		std::cerr << role << " set_periods_near failed: " << snd_strerror(err) << "\n";
		return false;
	}

	err = snd_pcm_hw_params(handle, hw);
	if (err < 0) {
		std::cerr << role << " hw_params failed: " << snd_strerror(err) << "\n";
		return false;
	}

	return true;
}

static bool write_block_mmap(snd_pcm_t* playback, const int32_t* interleaved, snd_pcm_uframes_t frames) {
	snd_pcm_uframes_t done = 0;
	while (done < frames) {
		snd_pcm_sframes_t w = snd_pcm_mmap_writei(
			playback,
			interleaved + done * kChannels,
			frames - done
		);
		if (w == -EAGAIN) {
			continue;
		}
		if (w < 0) {
			int err = snd_pcm_recover(playback, static_cast<int>(w), 1);
			if (err < 0) {
				std::cerr << "playback recover failed: " << snd_strerror(err) << "\n";
				return false;
			}
			continue;
		}
		done += static_cast<snd_pcm_uframes_t>(w);
	}
	return true;
}

static bool read_block_mmap(snd_pcm_t* capture, int32_t* interleaved, snd_pcm_uframes_t frames) {
	snd_pcm_uframes_t done = 0;
	while (done < frames) {
		snd_pcm_sframes_t r = snd_pcm_mmap_readi(
			capture,
			interleaved + done * kChannels,
			frames - done
		);
		if (r == -EAGAIN) {
			continue;
		}
		if (r < 0) {
			int err = snd_pcm_recover(capture, static_cast<int>(r), 1);
			if (err < 0) {
				std::cerr << "capture recover failed: " << snd_strerror(err) << "\n";
				return false;
			}
			continue;
		}
		done += static_cast<snd_pcm_uframes_t>(r);
	}
	return true;
}

static std::vector<int32_t> make_sine_block(size_t start_frame, snd_pcm_uframes_t frames, float freq_hz) {
	std::vector<int32_t> data(static_cast<size_t>(frames) * kChannels);

	for (snd_pcm_uframes_t i = 0; i < frames; ++i) {
		const size_t abs_frame = start_frame + static_cast<size_t>(i);
		const float t = static_cast<float>(abs_frame) / static_cast<float>(kSampleRate);
		const float s = 0.4f * std::sin(kTwoPi * freq_hz * t);
		const int32_t v = static_cast<int32_t>(s * kS32Scale);
		data[static_cast<size_t>(i) * kChannels] = v;
		data[static_cast<size_t>(i) * kChannels + 1] = v;
	}

	return data;
}

static av::AudioBuffer to_audio_buffer(const std::vector<int32_t>& s32, uint32_t frames) {
	av::AudioBuffer out;
	out.sample_rate = kSampleRate;
	out.channels = kChannels;
	out.total_frames = frames;
	out.samples.resize(static_cast<size_t>(frames) * kChannels);
	for (size_t i = 0; i < out.samples.size(); ++i) {
		out.samples[i] = static_cast<float>(s32[i]) / kS32Scale;
	}
	return out;
}

static int best_delay_frames(const std::vector<int32_t>& ref, const std::vector<int32_t>& cap,
							 size_t ref_frames, size_t cap_frames, int max_delay) {
	int best_delay = 0;
	double best_score = -1.0;

	for (int d = 0; d <= max_delay; ++d) {
		if (static_cast<size_t>(d) + ref_frames > cap_frames) {
			break;
		}
		double score = 0.0;
		for (size_t f = 0; f < ref_frames; ++f) {
			const float x = static_cast<float>(ref[f * kChannels]) / kS32Scale;
			const float y = static_cast<float>(cap[(f + static_cast<size_t>(d)) * kChannels]) / kS32Scale;
			score += static_cast<double>(x) * static_cast<double>(y);
		}
		if (score > best_score) {
			best_score = score;
			best_delay = d;
		}
	}
	return best_delay;
}

} // namespace

int main() {
	const char* enabled = std::getenv("RUN_DMA_E2E");
	if (!enabled || std::string(enabled) != "1") {
		std::cout << "Skipping DMA e2e test (set RUN_DMA_E2E=1 to enable)\n";
		return 0;
	}

	const std::string capture_dev = std::getenv("DMA_CAPTURE_DEV")
		? std::getenv("DMA_CAPTURE_DEV") : "hw:0,0";
	const std::string playback_dev = std::getenv("DMA_PLAYBACK_DEV")
		? std::getenv("DMA_PLAYBACK_DEV") : "hw:1,0";
	const float sine_hz = std::getenv("DMA_TEST_FREQ_HZ")
		? std::stof(std::getenv("DMA_TEST_FREQ_HZ")) : 1000.0f;
	const double snr_threshold = std::getenv("DMA_SNR_THRESHOLD_DB")
		? std::stod(std::getenv("DMA_SNR_THRESHOLD_DB")) : 20.0;
	const double max_clip_ratio = std::getenv("DMA_MAX_CLIP_RATIO")
		? std::stod(std::getenv("DMA_MAX_CLIP_RATIO")) : 0.0;

	snd_pcm_t* capture = nullptr;
	snd_pcm_t* playback = nullptr;

	int err = snd_pcm_open(&capture, capture_dev.c_str(), SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		std::cerr << "open capture failed: " << snd_strerror(err) << "\n";
		return 1;
	}
	err = snd_pcm_open(&playback, playback_dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		std::cerr << "open playback failed: " << snd_strerror(err) << "\n";
		snd_pcm_close(capture);
		return 1;
	}

	if (!configure_stream(capture, "capture") || !configure_stream(playback, "playback")) {
		snd_pcm_close(capture);
		snd_pcm_close(playback);
		return 1;
	}

	err = snd_pcm_prepare(capture);
	if (err < 0) {
		std::cerr << "capture prepare failed: " << snd_strerror(err) << "\n";
		snd_pcm_close(capture);
		snd_pcm_close(playback);
		return 1;
	}
	err = snd_pcm_prepare(playback);
	if (err < 0) {
		std::cerr << "playback prepare failed: " << snd_strerror(err) << "\n";
		snd_pcm_close(capture);
		snd_pcm_close(playback);
		return 1;
	}

	err = snd_pcm_start(capture);
	if (err < 0) {
		std::cerr << "capture start failed: " << snd_strerror(err) << "\n";
		snd_pcm_close(capture);
		snd_pcm_close(playback);
		return 1;
	}

	const size_t signal_frames = static_cast<size_t>(kSignalSeconds) * kSampleRate;
	const size_t tail_frames = kSampleRate / 2;
	const size_t total_capture_frames = signal_frames + tail_frames;

	std::vector<int32_t> sent(signal_frames * kChannels, 0);
	std::vector<int32_t> captured(total_capture_frames * kChannels, 0);

	size_t sent_frames = 0;
	size_t cap_frames = 0;

	while (sent_frames < signal_frames || cap_frames < total_capture_frames) {
		const bool sending_signal = sent_frames < signal_frames;
		const size_t remaining_send = sending_signal ? (signal_frames - sent_frames) : 0;
		const snd_pcm_uframes_t tx_frames = static_cast<snd_pcm_uframes_t>(
			remaining_send > kPeriodFrames ? kPeriodFrames : remaining_send
		);

		std::vector<int32_t> tx_block;
		if (tx_frames > 0) {
			tx_block = make_sine_block(sent_frames, tx_frames, sine_hz);
			for (snd_pcm_uframes_t i = 0; i < tx_frames; ++i) {
				sent[(sent_frames + static_cast<size_t>(i)) * kChannels] = tx_block[static_cast<size_t>(i) * kChannels];
				sent[(sent_frames + static_cast<size_t>(i)) * kChannels + 1] = tx_block[static_cast<size_t>(i) * kChannels + 1];
			}
		} else {
			tx_block.assign(static_cast<size_t>(kPeriodFrames) * kChannels, 0);
		}

		const snd_pcm_uframes_t block_tx_frames = tx_frames > 0 ? tx_frames : kPeriodFrames;
		if (!write_block_mmap(playback, tx_block.data(), block_tx_frames)) {
			snd_pcm_close(capture);
			snd_pcm_close(playback);
			return 1;
		}
		sent_frames += static_cast<size_t>(tx_frames);

		const size_t remaining_cap = total_capture_frames - cap_frames;
		const snd_pcm_uframes_t block_cap_frames = static_cast<snd_pcm_uframes_t>(
			remaining_cap > block_tx_frames ? block_tx_frames : remaining_cap
		);
		if (block_cap_frames == 0) {
			continue;
		}

		std::vector<int32_t> cap_block(static_cast<size_t>(block_cap_frames) * kChannels, 0);
		if (!read_block_mmap(capture, cap_block.data(), block_cap_frames)) {
			snd_pcm_close(capture);
			snd_pcm_close(playback);
			return 1;
		}

		for (snd_pcm_uframes_t i = 0; i < block_cap_frames; ++i) {
			captured[(cap_frames + static_cast<size_t>(i)) * kChannels] = cap_block[static_cast<size_t>(i) * kChannels];
			captured[(cap_frames + static_cast<size_t>(i)) * kChannels + 1] = cap_block[static_cast<size_t>(i) * kChannels + 1];
		}
		cap_frames += static_cast<size_t>(block_cap_frames);
	}

	snd_pcm_close(capture);
	snd_pcm_close(playback);

	const int delay = best_delay_frames(sent, captured, signal_frames, total_capture_frames, static_cast<int>(tail_frames));
	std::vector<int32_t> aligned(signal_frames * kChannels, 0);
	for (size_t f = 0; f < signal_frames; ++f) {
		aligned[f * kChannels] = captured[(f + static_cast<size_t>(delay)) * kChannels];
		aligned[f * kChannels + 1] = captured[(f + static_cast<size_t>(delay)) * kChannels + 1];
	}

	const av::AudioBuffer reference = to_audio_buffer(sent, static_cast<uint32_t>(signal_frames));
	const av::AudioBuffer processed = to_audio_buffer(aligned, static_cast<uint32_t>(signal_frames));

	av::Config cfg;
	cfg.mode = av::Mode::WavCompare;
	cfg.snr_threshold = snr_threshold;
	cfg.mse_threshold = 0.05;
	cfg.corr_threshold = 0.80;
	cfg.verbose = true;

	const av::VerifyResult result = av::verify_compare(reference, processed, cfg);
	const double processed_clip_ratio = av::clipping_ratio(
		processed.samples.data(),
		processed.samples.size()
	);
	std::cout << "e2e delay(frames)=" << delay
			  << " snr_db=" << result.metrics.snr_db
			  << " mse=" << result.metrics.mse
			  << " corr=" << result.metrics.cross_corr
			  << " clip_ratio=" << processed_clip_ratio << "\n";

	if (processed_clip_ratio > max_clip_ratio) {
		std::cerr << "DMA e2e clipping check failed: clip_ratio="
			      << processed_clip_ratio
			      << " > max=" << max_clip_ratio << "\n";
		return 1;
	}

	if (!result.passed) {
		std::cerr << "DMA e2e compare failed\n";
		for (const auto& err_msg : result.errors) {
			std::cerr << "  error: " << err_msg << '\n';
		}
		return 1;
	}

	std::cout << "DMA e2e DeciBeliever test passed\n";
	return 0;
}
