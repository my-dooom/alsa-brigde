#include "stream_format.hpp"

namespace {

// Module-local storage for the negotiated PCM stream format.
snd_pcm_format_t g_stream_format = SND_PCM_FORMAT_FLOAT_LE;

} // namespace

snd_pcm_format_t get_stream_format() {
    // Return the currently selected runtime format.
    return g_stream_format;
}

void set_stream_format_internal(snd_pcm_format_t format) {
    // Update runtime format after ALSA negotiation in the setup phase.
    g_stream_format = format;
}
