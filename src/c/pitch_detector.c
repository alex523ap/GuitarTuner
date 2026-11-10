#include "pitch_detector.h"
#include <stdbool.h>

// Moving average over 0.5 ms: keeps every note in range and steadies faint high strings
#define FILTER_LEN 8

static uint32_t s_diff_buffer[PITCH_LAG_LIMIT + 1];
static int16_t s_filtered[PITCH_BUFFER_SIZE];

void pitch_detector_init(void) {
  // Reset any internal state
}

static inline int32_t prv_abs(int32_t val) {
  return (val < 0) ? -val : val;
}

// Removes DC and smooths the signal. Returns the filtered copy.
static const int16_t *prv_filter(const int16_t *buffer) {
  int32_t mean = 0;
  for (size_t i = 0; i < PITCH_BUFFER_SIZE; i++) {
    mean += buffer[i];
  }
  mean /= PITCH_BUFFER_SIZE;

  int32_t sum = 0;
  for (size_t i = 0; i < PITCH_BUFFER_SIZE; i++) {
    sum += buffer[i] - mean;
    if (i >= FILTER_LEN) {
      sum -= buffer[i - FILTER_LEN] - mean;
    }
    s_filtered[i] = (int16_t)(sum / FILTER_LEN);
  }
  return s_filtered;
}

void pitch_detector_analyze(const int16_t *buffer, size_t count, uint16_t lag_min,
                            uint16_t lag_max, TuningResult *out_result) {
  if (lag_max > PITCH_LAG_LIMIT) {
    lag_max = PITCH_LAG_LIMIT;
  }
  if (!buffer || count < PITCH_BUFFER_SIZE || !out_result) {
    if (out_result) {
      out_result->has_signal = false;
      out_result->status = TUNING_STATUS_SILENCE;
    }
    return;
  }

  buffer = prv_filter(buffer);

  // 1. Energy check across the analysis window
  int64_t sum_abs = 0;
  for (size_t i = 0; i < PITCH_WINDOW_SIZE; i++) {
    sum_abs += prv_abs((int32_t)buffer[i]);
  }
  const int32_t mean_abs = (int32_t)(sum_abs / PITCH_WINDOW_SIZE);

  if (mean_abs < PITCH_SILENCE_THRESHOLD) {
    out_result->has_signal = false;
    out_result->detected_freq_hz = 0.0f;
    out_result->cents = 0;
    out_result->status = TUNING_STATUS_SILENCE;
    return;
  }

  // Keep as much of the signal as 32 bits allow: a fixed scale rounds quiet notes
  // away. Differences stay under 4096, so 256 squares fit in a uint32_t.
  int32_t peak = 0;
  for (size_t i = 0; i < (size_t)PITCH_WINDOW_SIZE + lag_max; i++) {
    const int32_t magnitude = prv_abs((int32_t)buffer[i]);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  uint8_t shift = 0;
  while (((2 * peak) >> shift) >= 4096) {
    shift++;
  }

  // 2. Compute difference function: d(tau) = sum_j (x[j] - x[j + tau])^2
  uint32_t min_val = 0xFFFFFFFF;
  size_t best_tau = 0;

  for (size_t tau = lag_min; tau <= lag_max; tau++) {
    uint32_t diff_sum = 0;
    for (size_t j = 0; j < PITCH_WINDOW_SIZE; j += 2) {
      int32_t d = ((int32_t)buffer[j] - (int32_t)buffer[j + tau]) >> shift;
      diff_sum += (uint32_t)(d * d);
    }
    s_diff_buffer[tau] = diff_sum;
  }

  // 3. Find the first prominent local minimum (avoiding octave/multiple errors)
  // Find overall minimum and mean first
  uint64_t diff_total = 0;
  for (size_t tau = lag_min; tau <= lag_max; tau++) {
    diff_total += s_diff_buffer[tau];
    if (s_diff_buffer[tau] < min_val) {
      min_val = s_diff_buffer[tau];
    }
  }
  const uint32_t mean_val = (uint32_t)(diff_total / (lag_max - lag_min + 1));

  // Confidence: a periodic signal has a dip far below the mean; noise or a
  // strummed/attack transient doesn't. Treat those frames as no reading.
  // On the watch, plucked notes measure min/mean 0-4% and a fading high E up to
  // about 15%; knocks and taps 20-24%.
  if (min_val > (uint32_t)(((uint64_t)mean_val * 3) / 16)) {
    out_result->has_signal = false;
    out_result->detected_freq_hz = 0.0f;
    out_result->cents = 0;
    out_result->status = TUNING_STATUS_SILENCE;
    return;
  }

  // Threshold for first valid dip (like YIN dip threshold), relative to the
  // signal level rather than the minimum: a clean tone's deepest dip is ~0, so
  // a min-relative threshold rejects the true period and picks the octave below.
  const uint32_t threshold = min_val + ((mean_val - min_val) >> 3);

  for (size_t tau = lag_min + 1; tau < lag_max; tau++) {
    if (s_diff_buffer[tau] < s_diff_buffer[tau - 1] &&
        s_diff_buffer[tau] < s_diff_buffer[tau + 1] &&
        s_diff_buffer[tau] <= threshold) {
      best_tau = tau;
      break;
    }
  }

  if (best_tau == 0) {
    // Fall back to absolute minimum if no local valley met the threshold
    for (size_t tau = lag_min; tau <= lag_max; tau++) {
      if (s_diff_buffer[tau] == min_val) {
        best_tau = tau;
        break;
      }
    }
  }

  // Overtone check: a strong 2nd/3rd harmonic (common on guitar, especially low
  // strings through the watch mic) passes the threshold at a fraction of the true
  // period. The true period also dips at its multiples, so if a multiple dips much
  // deeper while this dip isn't near-perfect, the multiple is the real note.
  // Repeat from the new period: a high harmonic can lead to a lower one first
  // (4th -> 2nd -> fundamental on a bass through the watch mic).
  bool moved = true;
  while (moved && s_diff_buffer[best_tau] > (mean_val >> 5)) {
    moved = false;
    for (size_t k = 2; k <= 3; k++) {
      const size_t center = best_tau * k;
      if (center + 2 > lag_max) {
        break;
      }
      size_t deep_tau = center - 2;
      for (size_t tau = center - 1; tau <= center + 2; tau++) {
        if (s_diff_buffer[tau] < s_diff_buffer[deep_tau]) {
          deep_tau = tau;
        }
      }
      if (s_diff_buffer[deep_tau] * 2 < s_diff_buffer[best_tau] &&
          deep_tau > lag_min && deep_tau < lag_max) {
        best_tau = deep_tau;
        moved = true;
        break;
      }
    }
  }

  // A "pitch" on the edge of the search range isn't a real dip (seen on the
  // watch as 457 Hz from knocks/taps), so report no reading
  if (best_tau <= lag_min || best_tau >= lag_max) {
    out_result->has_signal = false;
    out_result->detected_freq_hz = 0.0f;
    out_result->cents = 0;
    out_result->status = TUNING_STATUS_SILENCE;
    return;
  }

  // 4. Parabolic interpolation around best_tau
  float refined_tau = (float)best_tau;
  if (best_tau > lag_min && best_tau < lag_max) {
    float y1 = (float)s_diff_buffer[best_tau - 1];
    float y2 = (float)s_diff_buffer[best_tau];
    float y3 = (float)s_diff_buffer[best_tau + 1];

    float denom = 2.0f * (y1 - 2.0f * y2 + y3);
    if (denom > 0.0001f || denom < -0.0001f) {
      float delta = (y1 - y3) / denom;
      if (delta >= -1.0f && delta <= 1.0f) {
        refined_tau = (float)best_tau + delta;
      }
    }
  }

  if (refined_tau <= 0.0f) {
    out_result->has_signal = false;
    out_result->status = TUNING_STATUS_SILENCE;
    return;
  }

  out_result->has_signal = true;
  out_result->detected_freq_hz = 16000.0f / refined_tau;
  out_result->cents = 0;
  out_result->status = TUNING_STATUS_SILENCE;
}
