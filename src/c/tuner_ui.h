#pragma once

#include "tuner_types.h"
#include <pebble.h>

void tuner_ui_init(Window *window);
void tuner_ui_deinit(void);

void tuner_ui_update(const TuningResult *result);
void tuner_ui_set_mode(TunerMode mode, uint8_t manual_string_idx);
// Switches instrument (also resets to auto mode); call after audio_mgr_init
void tuner_ui_set_instrument(InstrumentId id);
InstrumentId tuner_ui_get_instrument(void);

// Interaction callbacks triggered by UI / touch
void tuner_ui_handle_select(void);
void tuner_ui_handle_up(void);
void tuner_ui_handle_down(void);
