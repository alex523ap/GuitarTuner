#pragma once

#include "tuner_types.h"
#include <stdint.h>

// Turns per-frame raw pitch readings into a steady display reading:
// median filter on frequency, EMA on cents, sticky auto string selection,
// a short hold after the note fades, and a debounced in-tune status.
// Call once per analysis frame (PITCH_SMOOTHER_FRAME_MS apart).

#define PITCH_SMOOTHER_FRAME_MS 40

void pitch_smoother_reset(void);
// Also forgets the current string; use when the instrument changes
void pitch_smoother_clear(void);

void pitch_smoother_update(const TuningResult *raw, const Instrument *inst, TunerMode mode,
                           uint8_t manual_string_idx, TuningResult *out_result);
