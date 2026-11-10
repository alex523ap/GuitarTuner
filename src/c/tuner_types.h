#pragma once

#include <pebble.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum TunerMode {
  TUNER_MODE_AUTO = 0,
  TUNER_MODE_MANUAL,
} TunerMode;

typedef enum TuningStatus {
  TUNING_STATUS_SILENCE = 0,
  TUNING_STATUS_FLAT,
  TUNING_STATUS_IN_TUNE,
  TUNING_STATUS_SHARP,
} TuningStatus;

typedef enum InstrumentId {
  INSTRUMENT_GUITAR = 0,
  INSTRUMENT_BASS,
  INSTRUMENT_UKULELE,
  INSTRUMENT_CHROMATIC,
  INSTRUMENT_COUNT,
} InstrumentId;

typedef struct TunerString {
  const char *name;         // "E", "A", "D", "G", "B", "E"
  uint8_t number;           // 6, 5, 4, 3, 2, 1
  float target_freq_hz;     // 82.41, 110.00, etc.
} TunerString;

typedef struct Instrument {
  const char *name;           // "Guitar" (menu)
  const char *label;          // "GUITAR" (header)
  const char *tuning;         // "E A D G B E" (menu subtitle)
  const TunerString *strings; // NULL in chromatic mode
  uint8_t num_strings;        // 0 in chromatic mode
  uint16_t lag_min;           // pitch search range, in samples at 16 kHz
  uint16_t lag_max;
} Instrument;

typedef struct TuningResult {
  bool has_signal;
  float detected_freq_hz;
  int16_t cents;            // -50 to +50
  uint8_t string_index;     // string index, or MIDI note number in chromatic mode
  TuningStatus status;
} TuningResult;
