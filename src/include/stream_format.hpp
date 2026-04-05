#pragma once

#include <alsa/asoundlib.h>

// Returns the currently negotiated PCM format used by the processing path.
snd_pcm_format_t get_stream_format();

// Stores the negotiated PCM format during device configuration.
void set_stream_format_internal(snd_pcm_format_t format);
