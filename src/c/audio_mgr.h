#pragma once

#include "tuner_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*AudioAnalysisCallback)(const TuningResult *result, void *context);

#ifdef TUNER_DEBUG
// While set, raw samples go here instead of to the pitch detector
typedef void (*AudioTapCallback)(const int16_t *samples, size_t count);
void audio_mgr_set_tap(AudioTapCallback tap);
#endif

void audio_mgr_init(AudioAnalysisCallback callback, void *context);
void audio_mgr_deinit(void);

bool audio_mgr_start(void);
void audio_mgr_stop(void);
bool audio_mgr_is_capturing(void);
// Reason the mic is not running, or NULL when capturing or muted by the user
const char *audio_mgr_get_error_text(void);

// Switches instrument; also resets to auto mode and clears the reading
void audio_mgr_set_instrument(InstrumentId id);

// The mic hears the watch's own vibration (about 240 Hz): call when buzzing so
// those frames are skipped instead of read as a note.
void audio_mgr_ignore_buzz(void);

void audio_mgr_set_mode(TunerMode mode);
TunerMode audio_mgr_get_mode(void);

void audio_mgr_set_manual_string(uint8_t string_index);
uint8_t audio_mgr_get_manual_string(void);

const TuningResult* audio_mgr_get_latest_result(void);
