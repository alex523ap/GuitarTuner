#include "tuner_notes.h"
#include <math.h>
#include <stdio.h>

// Standard guitar strings: 6 (low E) to 1 (high E)
static const TunerString s_guitar_strings[] = {
  { .name = "E", .number = 6, .target_freq_hz = 82.41f  },
  { .name = "A", .number = 5, .target_freq_hz = 110.00f },
  { .name = "D", .number = 4, .target_freq_hz = 146.83f },
  { .name = "G", .number = 3, .target_freq_hz = 196.00f },
  { .name = "B", .number = 2, .target_freq_hz = 246.94f },
  { .name = "E", .number = 1, .target_freq_hz = 329.63f },
};

// 4-string bass: 4 (low E1) to 1 (G2)
static const TunerString s_bass_strings[] = {
  { .name = "E", .number = 4, .target_freq_hz = 41.20f },
  { .name = "A", .number = 3, .target_freq_hz = 55.00f },
  { .name = "D", .number = 2, .target_freq_hz = 73.42f },
  { .name = "G", .number = 1, .target_freq_hz = 98.00f },
};

// Ukulele, standard re-entrant tuning (high G4): 4 (G) to 1 (A)
static const TunerString s_ukulele_strings[] = {
  { .name = "G", .number = 4, .target_freq_hz = 392.00f },
  { .name = "C", .number = 3, .target_freq_hz = 261.63f },
  { .name = "E", .number = 2, .target_freq_hz = 329.63f },
  { .name = "A", .number = 1, .target_freq_hz = 440.00f },
};

// Lag ranges (16 kHz / lag = Hz) leave headroom around each instrument's
// strings for strings that are far out of tune. The largest lag_max must stay
// within PITCH_LAG_LIMIT.
static const Instrument s_instruments[INSTRUMENT_COUNT] = {
  [INSTRUMENT_GUITAR] = {
    .name = "Guitar", .label = "GUITAR", .tuning = "E A D G B E",
    .strings = s_guitar_strings, .num_strings = 6,
    .lag_min = 35, .lag_max = 225,   // 457 .. 71 Hz
  },
  [INSTRUMENT_BASS] = {
    .name = "Bass", .label = "BASS", .tuning = "E A D G",
    .strings = s_bass_strings, .num_strings = 4,
    .lag_min = 123, .lag_max = 421,  // 130 .. 38 Hz
  },
  [INSTRUMENT_UKULELE] = {
    .name = "Ukulele", .label = "UKULELE", .tuning = "G C E A",
    .strings = s_ukulele_strings, .num_strings = 4,
    .lag_min = 27, .lag_max = 90,    // 593 .. 178 Hz
  },
  [INSTRUMENT_CHROMATIC] = {
    .name = "Chromatic", .label = "CHROMATIC", .tuning = "All notes · A4 = 440 Hz",
    .strings = NULL, .num_strings = 0,
    .lag_min = 12, .lag_max = 410,   // 1333 .. 39 Hz
  },
};

static const char *const s_note_names[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

const Instrument *tuner_notes_get_instrument(InstrumentId id) {
  if (id >= INSTRUMENT_COUNT) {
    id = INSTRUMENT_GUITAR;
  }
  return &s_instruments[id];
}

uint8_t tuner_notes_closest(const Instrument *inst, float freq_hz) {
  if (tuner_notes_is_chromatic(inst)) {
    // MIDI 69 = A4 = 440 Hz; 12 / ln(2) = 17.31234
    int note = (int)floorf(69.5f + 17.31234f * logf(freq_hz / 440.0f));
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return (uint8_t)note;
  }
  uint8_t best = 0;
  float best_dist = 100000.0f;
  for (uint8_t i = 0; i < inst->num_strings; i++) {
    float dist = fabsf(logf(freq_hz / inst->strings[i].target_freq_hz));
    if (dist < best_dist) {
      best_dist = dist;
      best = i;
    }
  }
  return best;
}

float tuner_notes_target_hz(const Instrument *inst, uint8_t index) {
  if (tuner_notes_is_chromatic(inst)) {
    // ln(2) / 12 = 0.05776227
    return 440.0f * expf(((float)index - 69.0f) * 0.05776227f);
  }
  if (index >= inst->num_strings) {
    index = 0;
  }
  return inst->strings[index].target_freq_hz;
}

void tuner_notes_format_name(const Instrument *inst, uint8_t index, char *buf, size_t buf_len) {
  if (tuner_notes_is_chromatic(inst)) {
    snprintf(buf, buf_len, "%s%d", s_note_names[index % 12], (int)(index / 12) - 1);
    return;
  }
  if (index >= inst->num_strings) {
    index = 0;
  }
  snprintf(buf, buf_len, "%s", inst->strings[index].name);
}

uint8_t tuner_notes_string_number(const Instrument *inst, uint8_t index) {
  if (tuner_notes_is_chromatic(inst)) {
    return 0;
  }
  if (index >= inst->num_strings) {
    index = 0;
  }
  return inst->strings[index].number;
}
