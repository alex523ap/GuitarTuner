#pragma once

#include "tuner_types.h"
#include <stddef.h>

const Instrument *tuner_notes_get_instrument(InstrumentId id);

static inline bool tuner_notes_is_chromatic(const Instrument *inst) {
  return inst->num_strings == 0;
}

// Closest string index, or closest MIDI note number in chromatic mode
uint8_t tuner_notes_closest(const Instrument *inst, float freq_hz);

// Target pitch of a string index, or of a MIDI note number in chromatic mode
float tuner_notes_target_hz(const Instrument *inst, uint8_t index);

// String name ("E"), or note name with octave ("C#4") in chromatic mode
void tuner_notes_format_name(const Instrument *inst, uint8_t index, char *buf, size_t buf_len);

// String number on the instrument (6 = low E on guitar); 0 in chromatic mode
uint8_t tuner_notes_string_number(const Instrument *inst, uint8_t index);
