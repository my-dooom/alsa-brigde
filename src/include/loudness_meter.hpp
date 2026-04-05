#pragma once

#include <cstddef>
#include <cstdint>

// Small stateful renderer for a terminal loudness meter.
class LoudnessMeter {
public:
    // Draws/refreshes the meter only when the configured update cadence is reached.
    void render_if_due(int loop_count, const int32_t* interleaved_stereo, size_t frame_count);

private:
    // Tracks first draw so subsequent updates can rewrite in place.
    bool initialized_ = false;
};
