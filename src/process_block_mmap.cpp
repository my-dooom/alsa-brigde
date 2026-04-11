#include "process_block_mmap.hpp"

#include "loudness_meter.hpp"
#include "process_audio.hpp"
#include "stream_format.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

// Convert ALSA area metadata (base/bit offset/stride) into a byte pointer.
static char* area_byte_ptr(const snd_pcm_channel_area_t* areas,
                           snd_pcm_uframes_t offset) {
    return static_cast<char*>(
        static_cast<char*>(areas[0].addr)
        + areas[0].first / 8
        + offset * areas[0].step / 8);
}

// Recover stream state for XRUN/suspend and restart capture streams when required.
static bool recover(snd_pcm_t* handle, int err) {
    if (err == -EPIPE) {
        err = snd_pcm_prepare(handle);
        if (err < 0) return false;
        if (snd_pcm_stream(handle) == SND_PCM_STREAM_CAPTURE) {
            err = snd_pcm_start(handle);
            if (err < 0) return false;
        }
        return true;
    }

    if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN) {}
        if (err < 0) err = snd_pcm_prepare(handle);
        if (err < 0) return false;
        if (snd_pcm_stream(handle) == SND_PCM_STREAM_CAPTURE) {
            err = snd_pcm_start(handle);
            if (err < 0) return false;
        }
        return true;
    }

    return false;
}

bool process_block_mmap_impl(
    snd_pcm_t* capture_handle,
    snd_pcm_t* playback_handle,
    snd_pcm_uframes_t frames,
    int& loop_count
) {
    // Persist runtime state between calls while the stream loop is active.
    static bool playback_started = false;
    static int wait_timeouts = 0;
#ifdef NDEBUG
    static LoudnessMeter meter;
#endif
    int err;

    // If playback is already running (e.g. externally started), sync internal state.
    if (!playback_started && snd_pcm_state(playback_handle) == SND_PCM_STATE_RUNNING) {
        playback_started = true;
    }

    // Wait for capture readiness with a timeout to avoid busy spinning.
    err = snd_pcm_wait(capture_handle, 1000);
    if (err == 0) {
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
        recover(capture_handle, err);
        return false;
    }

    // Query capture-side available frames for this cycle.
    snd_pcm_sframes_t cap_avail = snd_pcm_avail_update(capture_handle);
    if (cap_avail < 0) {
        recover(capture_handle, static_cast<int>(cap_avail));
        return false;
    }

    // Start from capture availability; optionally clamp against playback free space.
    snd_pcm_uframes_t cap_u = static_cast<snd_pcm_uframes_t>(cap_avail);
    snd_pcm_uframes_t play_u = frames;

    if (playback_started) {
        snd_pcm_sframes_t play_avail = snd_pcm_avail_update(playback_handle);
        if (play_avail < 0) {
            recover(playback_handle, static_cast<int>(play_avail));
            playback_started = false;
            return false;
        }
        play_u = static_cast<snd_pcm_uframes_t>(play_avail);
    }

    // Drop a bounded chunk when capture runs far ahead to limit latency growth.
    if (playback_started && cap_u > play_u + frames * 2) {
        const snd_pcm_uframes_t drop = (cap_u - (play_u + frames));
        const snd_pcm_uframes_t max_drop = frames;
        snd_pcm_uframes_t to_drop = drop < max_drop ? drop : max_drop;

        const snd_pcm_channel_area_t* drop_areas;
        snd_pcm_uframes_t drop_offset;
        snd_pcm_uframes_t drop_frames = to_drop;
        err = snd_pcm_mmap_begin(capture_handle, &drop_areas, &drop_offset, &drop_frames);
        if (err >= 0 && drop_frames > 0) {
            snd_pcm_mmap_commit(capture_handle, drop_offset, drop_frames);
            cap_u = (cap_u > drop_frames) ? (cap_u - drop_frames) : 0;
        }
    }

    // Determine transferable frame count accepted by both endpoints.
    snd_pcm_uframes_t target = frames;
    if (cap_u < target) target = cap_u;
    if (playback_started && play_u < target) target = play_u;
    if (target == 0) return false;
    frames = target;

    // Map capture DMA window.
    const snd_pcm_channel_area_t* cap_areas;
    snd_pcm_uframes_t cap_offset;
    snd_pcm_uframes_t cap_frames = frames;

    err = snd_pcm_mmap_begin(capture_handle, &cap_areas, &cap_offset, &cap_frames);
    if (err < 0) {
        std::cerr << "capture mmap_begin: " << snd_strerror(err) << "\n";
        recover(capture_handle, err);
        return false;
    }
    if (cap_frames == 0) {
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        return false;
    }

    // Map playback DMA window sized from capture availability.
    const snd_pcm_channel_area_t* play_areas;
    snd_pcm_uframes_t play_offset;
    snd_pcm_uframes_t play_frames = cap_frames;

    err = snd_pcm_mmap_begin(playback_handle, &play_areas, &play_offset, &play_frames);
    if (err < 0) {
        std::cerr << "playback mmap_begin: " << snd_strerror(err) << "\n";
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        recover(playback_handle, err);
        playback_started = false;
        return false;
    }

    // Only process frames that both mapped windows can exchange.
    snd_pcm_uframes_t n = (cap_frames < play_frames) ? cap_frames : play_frames;
    if (n == 0) {
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        snd_pcm_mmap_commit(playback_handle, play_offset, 0);
        return false;
    }

    // Derive interleaved frame size from ALSA stride metadata.
    const unsigned bytes_per_frame = static_cast<unsigned>(cap_areas[0].step / 8);
    if (bytes_per_frame == 0U) {
        snd_pcm_mmap_commit(capture_handle, cap_offset, 0);
        snd_pcm_mmap_commit(playback_handle, play_offset, 0);
        return false;
    }

    char* cap_ptr = area_byte_ptr(cap_areas, cap_offset);
    char* play_ptr = area_byte_ptr(play_areas, play_offset);

    // Copy captured bytes directly into playback mapped region.
    std::memcpy(play_ptr, cap_ptr, n * bytes_per_frame);

    // Apply optional sample processing and update live meter for S32 stream format.
    if (get_stream_format() == SND_PCM_FORMAT_S32_LE) {
        process_samples_inplace(reinterpret_cast<int32_t*>(play_ptr), n, 2U);
#ifdef NDEBUG
        meter.render_if_due(loop_count, reinterpret_cast<const int32_t*>(cap_ptr), static_cast<size_t>(n));
#endif
    }

    // Commit capture and playback windows; failure means stream recovery is needed.
    snd_pcm_sframes_t r = snd_pcm_mmap_commit(capture_handle, cap_offset, n);
    if (r < 0 || static_cast<snd_pcm_uframes_t>(r) != n) {
        std::cerr << "capture commit error\n";
        recover(capture_handle, static_cast<int>(r));
        return false;
    }

    r = snd_pcm_mmap_commit(playback_handle, play_offset, n);
    if (r < 0 || static_cast<snd_pcm_uframes_t>(r) != n) {
        std::cerr << "playback commit error\n";
        recover(playback_handle, static_cast<int>(r));
        playback_started = false;
        return false;
    }

    // Start playback only after first successful commit so DMA has queued data.
    if (!playback_started) {
        err = snd_pcm_start(playback_handle);
        if (err < 0) {
            recover(playback_handle, err);
            playback_started = false;
            std::cerr << "playback start: " << snd_strerror(err) << "\n";
            return false;
        }
        playback_started = true;
    }

    // Count successful loop iterations to pace periodic telemetry.
    loop_count++;
    return true;
}
