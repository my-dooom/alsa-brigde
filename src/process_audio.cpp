#include "process_audio.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

static snd_pcm_format_t g_stream_format = SND_PCM_FORMAT_FLOAT_LE;

void set_stream_format(snd_pcm_format_t format) {
    // Store the negotiated PCM sample format selected during device setup.
    g_stream_format = format;
}

void process_samples_inplace(int32_t* buffer_ptr, snd_pcm_uframes_t frames, unsigned channels) {
        (void)buffer_ptr;
        (void)frames;
        (void)channels;
}

// Resolve the first byte pointer from an ALSA mmap channel-area descriptor.
static char* area_byte_ptr(const snd_pcm_channel_area_t* areas,
                           snd_pcm_uframes_t offset) {
    // Convert ALSA channel-area addressing (addr/first/step) to a raw byte pointer.
    return static_cast<char*>(
        static_cast<char*>(areas[0].addr)
        + areas[0].first / 8
        + offset * areas[0].step / 8);
}

// Recover a stream after an xrun (EPIPE) or suspend, then restart if capture.
static bool recover(snd_pcm_t* handle, int err) {
    // Underrun/overrun: prepare resets XRUN state so stream can continue.
    if (err == -EPIPE) {
        err = snd_pcm_prepare(handle);
        if (err < 0) return false;
        // Capture streams need an explicit start after prepare.
        if (snd_pcm_stream(handle) == SND_PCM_STREAM_CAPTURE) {
            err = snd_pcm_start(handle);
            if (err < 0) return false;
        }
        return true;
    }
    // Suspend path (e.g. power events): resume, or fallback to prepare.
    if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN) {}
        if (err < 0) err = snd_pcm_prepare(handle);
        if (err < 0) return false;
        // Capture streams need an explicit start after resume/prepare.
        if (snd_pcm_stream(handle) == SND_PCM_STREAM_CAPTURE) {
            err = snd_pcm_start(handle);
            if (err < 0) return false;
        }
        return true;
    }
    return false;
}

static bool process_block_mmap(
    snd_pcm_t* capture_handle,
    snd_pcm_t* playback_handle,
    snd_pcm_uframes_t frames,
    int& loop_count
) {
    // Tracks whether playback DMA has been started at least once.
    static bool playback_started = false;
    // Counts 1-second waits with no capture readiness.
    static int wait_timeouts = 0;
    int err;

    // If playback was started externally (e.g. via linked streams), reflect it here.
    if (!playback_started && snd_pcm_state(playback_handle) == SND_PCM_STATE_RUNNING) {
        playback_started = true;
    }

    // ---- wait for capture data ----
    // Block until capture has data (or timeout/error).
    err = snd_pcm_wait(capture_handle, 1000);
    if (err == 0) {
        // Timeout: collect periodic diagnostics to debug stalled paths.
        wait_timeouts++;
        if (wait_timeouts % 10 == 0) {
            snd_pcm_state_t cap_state = snd_pcm_state(capture_handle);
            snd_pcm_state_t play_state = snd_pcm_state(playback_handle);
            snd_pcm_sframes_t cap_avail_dbg = snd_pcm_avail_update(capture_handle);
            snd_pcm_sframes_t play_avail_dbg = snd_pcm_avail_update(playback_handle);
            std::cerr << "capture wait timeout x" << wait_timeouts
                      << " cap_state=" << snd_pcm_state_name(cap_state)
                      << " play_state=" << snd_pcm_state_name(play_state)
                      << " cap_avail=" << cap_avail_dbg
                      << " play_avail=" << play_avail_dbg << "\n";
        }
        return false;
    }
    if (err < 0) {
        std::cerr << "capture wait: " << snd_strerror(err) << ", recovering...\n";
        // Recover capture stream and skip this cycle.
        recover(capture_handle, err);
        return false;
    }

    // Query capture frames currently available for mmap transfer.
    snd_pcm_sframes_t cap_avail = snd_pcm_avail_update(capture_handle);
    if (cap_avail < 0) {
        recover(capture_handle, static_cast<int>(cap_avail));
        return false;
    }

    // Start with capture-available frames; then clamp as needed.
    snd_pcm_uframes_t cap_u = static_cast<snd_pcm_uframes_t>(cap_avail);
    snd_pcm_uframes_t play_u = frames;

    // Once playback is running, also constrain by playback free space.
    if (playback_started) {
        snd_pcm_sframes_t play_avail = snd_pcm_avail_update(playback_handle);
        if (play_avail < 0) {
            // Playback stream broke; recover and force re-start path.
            recover(playback_handle, static_cast<int>(play_avail));
            playback_started = false;
            return false;
        }
        play_u = static_cast<snd_pcm_uframes_t>(play_avail);
    }

    // If capture is running ahead of playback space, drop a small chunk to prevent overrun.
    if (playback_started && cap_u > play_u + frames * 2) {
        // Drop at most one block so control stays smooth.
        const snd_pcm_uframes_t drop = (cap_u - (play_u + frames));
        const snd_pcm_uframes_t max_drop = frames;
        snd_pcm_uframes_t to_drop = drop < max_drop ? drop : max_drop;

        // Map and commit capture frames without copying to playback (intentional drop).
        const snd_pcm_channel_area_t* drop_areas;
        snd_pcm_uframes_t drop_offset;
        snd_pcm_uframes_t drop_frames = to_drop;
        err = snd_pcm_mmap_begin(capture_handle, &drop_areas, &drop_offset, &drop_frames);
        if (err >= 0 && drop_frames > 0) {
            snd_pcm_mmap_commit(capture_handle, drop_offset, drop_frames);
            // Reflect dropped frames in local availability estimate.
            cap_u = (cap_u > drop_frames) ? (cap_u - drop_frames) : 0;
        }
    }

    // Choose the block size both sides can accept right now.
    snd_pcm_uframes_t target = frames;
    if (cap_u < target) target = cap_u;
    if (playback_started && play_u < target) target = play_u;
    if (target == 0) return false;
    frames = target;

    // ---- map capture DMA region ----
    const snd_pcm_channel_area_t* cap_areas;
    snd_pcm_uframes_t cap_offset;
    snd_pcm_uframes_t cap_frames = frames;

    // Reserve capture DMA region for this block.
    err = snd_pcm_mmap_begin(capture_handle, &cap_areas, &cap_offset, &cap_frames);
    if (err < 0) {
        std::cerr << "capture mmap_begin: " << snd_strerror(err) << "\n";
        recover(capture_handle, err);
        return false;
    }
    // Nothing to process from capture this cycle.
    if (cap_frames == 0) {
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        return false;
    }

    // ---- map playback DMA region ----
    const snd_pcm_channel_area_t* play_areas;
    snd_pcm_uframes_t play_offset;
    snd_pcm_uframes_t play_frames = cap_frames;

    // Reserve playback DMA region sized from capture side availability.
    err = snd_pcm_mmap_begin(playback_handle, &play_areas, &play_offset, &play_frames);
    if (err < 0) {
        std::cerr << "playback mmap_begin: " << snd_strerror(err) << "\n";
        // Release capture reservation if playback reservation fails.
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        recover(playback_handle, err);
        playback_started = false;
        return false;
    }

    // Transfer only what both mapped regions can hold.
    snd_pcm_uframes_t n = (cap_frames < play_frames) ? cap_frames : play_frames;
    if (n == 0) {
        // Commit zero to release both reservations cleanly.
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        snd_pcm_mmap_commit(playback_handle, play_offset, 0);
        return false;
    }

    // ---- copy capture → playback DMA buffer, then process in-place for S32_LE ----
    // bytes_per_frame comes from ALSA channel area stride for interleaved data.
    const unsigned bytes_per_frame = static_cast<unsigned>(cap_areas[0].step / 8);
    if (bytes_per_frame == 0U) {
        // Invalid stride: release both reservations and skip.
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        snd_pcm_mmap_commit(playback_handle, play_offset, 0);
        return false;
    }

    // Resolve byte pointers for capture and playback mapped regions.
    char* cap_ptr  = area_byte_ptr(cap_areas, cap_offset);
    char* play_ptr = area_byte_ptr(play_areas, play_offset);

    // Copy raw PCM bytes directly from capture DMA region to playback DMA region.
    std::memcpy(play_ptr, cap_ptr, n * bytes_per_frame);
    if (g_stream_format == SND_PCM_FORMAT_S32_LE) {
        process_samples_inplace(reinterpret_cast<int32_t*>(play_ptr), n, 2U);
    }


    // Periodic signal meter for S32 path to verify non-silent input.
    if (loop_count % 200 == 0) {
        if (g_stream_format == SND_PCM_FORMAT_S32_LE) {
            const auto* s = reinterpret_cast<const int32_t*>(cap_ptr);
            const size_t sample_count = static_cast<size_t>(n) * 2U;
            int32_t peak = 0;
            // Compute absolute peak over this block.
            for (size_t i = 0; i < sample_count; ++i) {
                int32_t v = s[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            std::cout << "S32 peak=" << peak << "\n";
        }
    }

    // ---- commit both regions ----
    snd_pcm_sframes_t r;

    // Release capture reservation and advance capture hardware pointer.
    r = snd_pcm_mmap_commit(capture_handle, cap_offset, n);
    if (r < 0 || static_cast<snd_pcm_uframes_t>(r) != n) {
        std::cerr << "capture commit error\n";
        recover(capture_handle, static_cast<int>(r));
        return false;
    }

    // Release playback reservation and queue samples for playback DMA.
    r = snd_pcm_mmap_commit(playback_handle, play_offset, n);
    if (r < 0 || static_cast<snd_pcm_uframes_t>(r) != n) {
        std::cerr << "playback commit error\n";
        recover(playback_handle, static_cast<int>(r));
        playback_started = false;
        return false;
    }

    // Start the playback DMA engine after the first successful commit.
    // mmap_commit does not auto-start the stream like writei does.
    if (!playback_started) {
        err = snd_pcm_start(playback_handle);
        if (err < 0) {
            // If start fails, recover and retry on a later block.
            recover(playback_handle, err);
            playback_started = false;
            std::cerr << "playback start: " << snd_strerror(err) << "\n";
            return false;
        }
        // Mark playback as running so subsequent cycles can clamp by play_avail.
        playback_started = true;
    }

    // Progress heartbeat.
    loop_count++;
    if (loop_count % 1000 == 0) {
        std::cout << "Processed " << loop_count << " blocks\n";
    }

    return true;
}

bool process_block(
    snd_pcm_t* capture_handle,
    snd_pcm_t* playback_handle,
    snd_pcm_uframes_t frames,
    int& loop_count
) {
    // mmap DMA is the only supported implementation.
    return process_block_mmap(capture_handle, playback_handle, frames, loop_count);
}