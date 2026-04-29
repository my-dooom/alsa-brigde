#include "loudness_meter.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef ENABLE_LINK_SYNC
#include "link_sync.hpp"
#endif

void LoudnessMeter::render_if_due(int loop_count, const int32_t* interleaved_stereo, size_t frame_count) {
    // Throttle meter updates and guard against invalid input.
    if (loop_count % 5 != 0 || interleaved_stereo == nullptr || frame_count == 0) {
        return;
    }

    double sum_sq_l = 0.0;
    double sum_sq_r = 0.0;

    // Compute per-channel RMS energy from interleaved S32 stereo frames.
    for (size_t i = 0; i < frame_count; ++i) {
        const double l = static_cast<double>(interleaved_stereo[i * 2]) / 2147483648.0;
        const double r = static_cast<double>(interleaved_stereo[i * 2 + 1]) / 2147483648.0;
        sum_sq_l += l * l;
        sum_sq_r += r * r;
    }

    // Convert RMS to dBFS.
    const double rms_l = std::sqrt(sum_sq_l / static_cast<double>(frame_count));
    const double rms_r = std::sqrt(sum_sq_r / static_cast<double>(frame_count));

    const double floor = 1e-9;
    double db_l = 20.0 * std::log10(rms_l > floor ? rms_l : floor);
    double db_r = 20.0 * std::log10(rms_r > floor ? rms_r : floor);

    // Clamp display range so bar scaling remains stable.
    if (db_l < -60.0) db_l = -60.0;
    if (db_r < -60.0) db_r = -60.0;
    if (db_l > 0.0) db_l = 0.0;
    if (db_r > 0.0) db_r = 0.0;

    // Translate dB range [-60, 0] into bar lengths.
    constexpr int meter_width = 30;
    const int bars_l = static_cast<int>(((db_l + 60.0) / 60.0) * meter_width);
    const int bars_r = static_cast<int>(((db_r + 60.0) / 60.0) * meter_width);

    // Redraw in-place: 2 meter lines + optional Link line.
    // Cursor rests at the end of the last printed line (no trailing \n).
    // \r moves to column 0 of that line; each \033[1A moves up one line.
    // 2-line layout: after L's \n cursor is on line N+2; \033[2A moves up 2 to R.
    // 3-line layout: Link has no trailing \n so cursor stays on line N+2; \033[1A\033[1A lands on R.
    if (initialized_) {
#ifdef ENABLE_LINK_SYNC
        std::cout << "\r\033[1A\033[1A";
#else
        std::cout << "\r\033[2A";
#endif
    } else {
        initialized_ = true;
    }

    // Clear line before drawing to avoid stale characters from longer previous output.
    std::cout << "\033[2KR [";
    for (int i = 0; i < meter_width; ++i) std::cout << (i < bars_r ? '#' : ' ');
    std::cout << "] " << db_r << " dBFS\n";

    std::cout << "\033[2KL [";
    for (int i = 0; i < meter_width; ++i) std::cout << (i < bars_l ? '#' : ' ');
    std::cout << "] " << db_l << " dBFS\n";

#ifdef ENABLE_LINK_SYNC
    {
        const float bpm = link_sync_get_bpm();
        std::cout << "\033[2KLink: ";
        if (bpm > 0.0f) {
            std::cout << std::fixed << std::setprecision(2) << bpm << " BPM";
        } else {
            std::cout << (link_sync_connected() ? "connected, no peers" : "not connected");
        }
    }
#endif
    std::cout << std::flush;
}
