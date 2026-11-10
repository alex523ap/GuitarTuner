#pragma once

#include "tuner_types.h"
#include <stddef.h>
#include <stdint.h>

#define PITCH_WINDOW_SIZE 512
#define PITCH_BUFFER_SIZE 1024

// Largest lag any instrument searches (Instrument.lag_max): 421 samples = 38 Hz,
// for a flat bass low E. Window + lag must fit in the buffer.
#define PITCH_LAG_LIMIT 421
_Static_assert(PITCH_WINDOW_SIZE + PITCH_LAG_LIMIT < PITCH_BUFFER_SIZE, "lag range too long for buffer");

// Silence threshold (mean absolute amplitude of the filtered signal). The watch
// mic is quiet: rooms measure about 7 and a soft high E only 15-45, so gate just
// above the room and let the confidence check reject sound that has no pitch.
#define PITCH_SILENCE_THRESHOLD 12

void pitch_detector_init(void);

// Analyze a buffer of 1024 samples (16-bit signed PCM @ 16 kHz), searching
// periods from lag_min to lag_max samples. Sets has_signal and
// detected_freq_hz; string matching and cents are left to the smoother.
void pitch_detector_analyze(const int16_t *buffer, size_t count, uint16_t lag_min,
                            uint16_t lag_max, TuningResult *out_result);
