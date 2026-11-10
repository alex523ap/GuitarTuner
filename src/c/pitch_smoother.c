#include "pitch_smoother.h"
#include "tuner_notes.h"
#include <math.h>
#include <stdbool.h>

// Median over the last 5 valid readings rejects single-frame outliers (octave errors)
#define MEDIAN_LEN 5
// EMA weight per 40 ms frame: needle settles in ~0.4 s
#define CENTS_EMA_ALPHA 0.25f
// Auto mode switches string only after this many consecutive agreeing frames
#define STRING_SWITCH_FRAMES 5
// Readings needed before a new note is shown, so the pluck attack is skipped
#define MIN_FRAMES_TO_SHOW 3
// A reading this far from the current note is set aside as a likely glitch
// (overtone, sympathetic string, knock)...
#define JUMP_CENTS 100.0f
// ...and only becomes the new note once this many consecutive readings agree
// within CONFIRM_CENTS of each other (a real string change or peg turn)
#define CONFIRM_FRAMES 5
#define CONFIRM_CENTS 50.0f
// Keep showing the last reading this long after the signal drops out
#define HOLD_FRAMES (1000 / PITCH_SMOOTHER_FRAME_MS)
// Must stay within IN_TUNE_ENTER_CENTS this long before reporting IN_TUNE
#define IN_TUNE_FRAMES (320 / PITCH_SMOOTHER_FRAME_MS)
#define IN_TUNE_ENTER_CENTS 5.0f
// Once in tune, stay there until further off, so the full-screen fill doesn't flicker
#define IN_TUNE_EXIT_CENTS 8.0f

static float s_history[MEDIAN_LEN];
static uint8_t s_history_count = 0;
static uint8_t s_history_idx = 0;

static float s_candidates[CONFIRM_FRAMES];
static uint8_t s_candidate_count = 0;

static bool s_active = false;
static float s_ema_cents = 0.0f;
static uint8_t s_string_idx = 0;
static uint8_t s_candidate_idx = 0;
static uint8_t s_candidate_frames = 0;
static uint8_t s_hold_frames = 0;
static uint8_t s_in_tune_frames = 0;
static TuningResult s_last_out = {
  .has_signal = false,
  .status = TUNING_STATUS_SILENCE,
};

void pitch_smoother_reset(void) {
  s_history_count = 0;
  s_history_idx = 0;
  s_candidate_count = 0;
  s_active = false;
  s_candidate_frames = 0;
  s_hold_frames = 0;
  s_in_tune_frames = 0;
  s_last_out.has_signal = false;
  s_last_out.status = TUNING_STATUS_SILENCE;
}

void pitch_smoother_clear(void) {
  pitch_smoother_reset();
  s_string_idx = 0;
  s_candidate_idx = 0;
  s_last_out.string_index = 0;
}

static float prv_median(void) {
  float sorted[MEDIAN_LEN];
  for (uint8_t i = 0; i < s_history_count; i++) {
    sorted[i] = s_history[i];
  }
  // Insertion sort, at most 5 elements
  for (uint8_t i = 1; i < s_history_count; i++) {
    float v = sorted[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && sorted[j] > v) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = v;
  }
  return sorted[s_history_count / 2];
}

static float prv_cents_between(float a_hz, float b_hz) {
  return fabsf(1731.234f * logf(a_hz / b_hz));
}

static float prv_cents(const Instrument *inst, float freq_hz, uint8_t string_idx) {
  // 1200 / ln(2) = 1731.234f
  float cents = 1731.234f * logf(freq_hz / tuner_notes_target_hz(inst, string_idx));
  // Clamp before smoothing so a far-off reading can't fling the needle
  if (cents < -60.0f) cents = -60.0f;
  if (cents > 60.0f) cents = 60.0f;
  return cents;
}

void pitch_smoother_update(const TuningResult *raw, const Instrument *inst, TunerMode mode,
                           uint8_t manual_string_idx, TuningResult *out_result) {
  if (!raw->has_signal) {
    // Hold the last reading briefly so a fading note doesn't flicker to "Pluck"
    if (s_active && s_hold_frames < HOLD_FRAMES) {
      s_hold_frames++;
      *out_result = s_last_out;
      return;
    }
    pitch_smoother_reset();
    out_result->has_signal = false;
    out_result->detected_freq_hz = 0.0f;
    out_result->cents = 0;
    out_result->string_index = (mode == TUNER_MODE_MANUAL) ? manual_string_idx : s_string_idx;
    out_result->status = TUNING_STATUS_SILENCE;
    return;
  }

  s_hold_frames = 0;
  const float raw_freq = raw->detected_freq_hz;
  // A confirmed jump re-picks the string at once; the needle still glides if
  // the string is unchanged (e.g. turning the peg)
  bool note_jumped = false;
  if (s_active && prv_cents_between(raw_freq, prv_median()) > JUMP_CENTS) {
    if (s_candidate_count > 0 &&
        prv_cents_between(raw_freq, s_candidates[s_candidate_count - 1]) > CONFIRM_CENTS) {
      s_candidate_count = 0;
    }
    s_candidates[s_candidate_count++] = raw_freq;
    if (s_candidate_count < CONFIRM_FRAMES) {
      // Not confirmed yet: keep showing the current note
      *out_result = s_last_out;
      return;
    }
    // Confirmed: start over on the new note
    for (uint8_t i = 0; i < CONFIRM_FRAMES; i++) {
      s_history[i] = s_candidates[i];
    }
    s_history_count = CONFIRM_FRAMES;
    s_history_idx = CONFIRM_FRAMES % MEDIAN_LEN;
    s_candidate_count = 0;
    note_jumped = true;
  } else {
    s_candidate_count = 0;
    s_history[s_history_idx] = raw_freq;
    s_history_idx = (s_history_idx + 1) % MEDIAN_LEN;
    if (s_history_count < MEDIAN_LEN) {
      s_history_count++;
    }
  }
  if (!s_active && s_history_count < MIN_FRAMES_TO_SHOW) {
    *out_result = s_last_out;
    out_result->string_index = (mode == TUNER_MODE_MANUAL) ? manual_string_idx : s_string_idx;
    return;
  }
  const float freq_hz = prv_median();

  // String selection: manual is fixed; auto only switches after a run of agreeing frames
  bool string_changed = false;
  if (mode == TUNER_MODE_MANUAL) {
    string_changed = (manual_string_idx != s_string_idx);
    s_string_idx = manual_string_idx;
  } else {
    uint8_t closest = tuner_notes_closest(inst, freq_hz);
    if (!s_active || note_jumped) {
      string_changed = (closest != s_string_idx);
      s_string_idx = closest;
      s_candidate_frames = 0;
    } else if (closest != s_string_idx) {
      if (closest == s_candidate_idx) {
        s_candidate_frames++;
      } else {
        s_candidate_idx = closest;
        s_candidate_frames = 1;
      }
      if (s_candidate_frames >= STRING_SWITCH_FRAMES) {
        s_string_idx = closest;
        s_candidate_frames = 0;
        string_changed = true;
      }
    } else {
      s_candidate_frames = 0;
    }
  }

  const float cents = prv_cents(inst, freq_hz, s_string_idx);
  if (!s_active || string_changed) {
    s_ema_cents = cents;
    s_in_tune_frames = 0;
  } else {
    s_ema_cents += CENTS_EMA_ALPHA * (cents - s_ema_cents);
  }
  s_active = true;

  const float in_tune_limit =
      (s_in_tune_frames >= IN_TUNE_FRAMES) ? IN_TUNE_EXIT_CENTS : IN_TUNE_ENTER_CENTS;
  if (fabsf(s_ema_cents) <= in_tune_limit) {
    if (s_in_tune_frames < IN_TUNE_FRAMES) {
      s_in_tune_frames++;
    }
  } else {
    s_in_tune_frames = 0;
  }

  int16_t display_cents = (int16_t)roundf(s_ema_cents);
  if (display_cents < -50) display_cents = -50;
  if (display_cents > 50) display_cents = 50;

  out_result->has_signal = true;
  out_result->detected_freq_hz = freq_hz;
  out_result->cents = display_cents;
  out_result->string_index = s_string_idx;
  if (s_in_tune_frames >= IN_TUNE_FRAMES) {
    out_result->status = TUNING_STATUS_IN_TUNE;
  } else if (s_ema_cents < 0.0f) {
    out_result->status = TUNING_STATUS_FLAT;
  } else {
    out_result->status = TUNING_STATUS_SHARP;
  }
  s_last_out = *out_result;
}
