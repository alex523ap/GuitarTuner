#include "audio_mgr.h"
#include "pitch_detector.h"
#include "pitch_smoother.h"
#include "tuner_notes.h"
#include <pebble.h>
#include <stdio.h>
#include <string.h>

static AudioAnalysisCallback s_client_callback = NULL;
static void *s_client_context = NULL;

static int16_t s_ring_buffer[PITCH_BUFFER_SIZE];
static size_t s_samples_received_total = 0;
static size_t s_write_idx = 0;
// Linear copy of the ring buffer for analysis (static: 2 KB is a lot of stack)
static int16_t s_linear_buf[PITCH_BUFFER_SIZE];
// Mic delivers 20 ms chunks; analyse every other one (PITCH_SMOOTHER_FRAME_MS)
static uint8_t s_chunk_counter = 0;
// Frames to skip after a buzz: measured bursts last up to 240 ms, plus the 64 ms
// the analysis buffer takes to clear
#define BUZZ_IGNORE_FRAMES 8
static uint8_t s_ignore_frames = 0;

// Human-readable reason the mic is not running, or NULL when OK / muted by user
static const char *s_error_text = NULL;

static const char *prv_start_error_text(MicCaptureStartResult res) {
  switch (res) {
    case MicCaptureStartErrBusy: return "Mic busy";
    case MicCaptureStartErrNotForeground: return "Mic: not foreground";
    case MicCaptureStartErrUnavailable: return "Mic unavailable";
    case MicCaptureStartErrPermissionDenied: return "Mic permission denied";
    case MicCaptureStartErrNoMemory: return "Mic: no memory";
    default: return "Mic error";
  }
}

static const Instrument *s_instrument = NULL;
static TunerMode s_tuner_mode = TUNER_MODE_AUTO;
static uint8_t s_manual_string_idx = 0;
static TuningResult s_latest_result = {
  .has_signal = false,
  .detected_freq_hz = 0.0f,
  .cents = 0,
  .string_index = 0,
  .status = TUNING_STATUS_SILENCE,
};

#ifdef TUNER_DEBUG
static AudioTapCallback s_tap = NULL;

void audio_mgr_set_tap(AudioTapCallback tap) {
  s_tap = tap;
}

// Logs every analysis frame, batched so the log link keeps up:
// TUNE|level,raw Hz x10,shown Hz x10,string,cents,status;...
#define TRACE_BATCH 8
static char s_trace[8 * 32];
static size_t s_trace_len = 0;
static uint8_t s_trace_count = 0;

static void prv_trace(const int16_t *buffer, const TuningResult *raw, const TuningResult *out) {
  int32_t sum = 0;
  for (size_t i = 0; i < PITCH_WINDOW_SIZE; i++) {
    sum += (buffer[i] < 0) ? -buffer[i] : buffer[i];
  }
  s_trace_len += snprintf(&s_trace[s_trace_len], sizeof(s_trace) - s_trace_len, "%d,%d,%d,%d,%d,%d;",
                          (int)(sum / PITCH_WINDOW_SIZE),
                          raw->has_signal ? (int)(raw->detected_freq_hz * 10.0f) : 0,
                          out->has_signal ? (int)(out->detected_freq_hz * 10.0f) : 0,
                          out->string_index, out->cents, (int)out->status);
  if (++s_trace_count >= TRACE_BATCH) {
    APP_LOG(APP_LOG_LEVEL_INFO, "TUNE|%s", s_trace);
    s_trace_len = 0;
    s_trace_count = 0;
  }
}
#endif

static void prv_on_audio_data(const int16_t *samples, size_t count, void *context) {
  if (!samples || count == 0) return;

#ifdef TUNER_DEBUG
  if (s_tap) {
    s_tap(samples, count);
    return;
  }
#endif

  // Insert samples into circular ring buffer
  for (size_t i = 0; i < count; i++) {
    s_ring_buffer[s_write_idx] = samples[i];
    s_write_idx = (s_write_idx + 1) % PITCH_BUFFER_SIZE;
  }
  s_samples_received_total += count;

  s_chunk_counter++;

  // Wait until we have accumulated at least one full buffer
  if (s_samples_received_total >= PITCH_BUFFER_SIZE &&
      s_chunk_counter * 20 >= PITCH_SMOOTHER_FRAME_MS) {
    s_chunk_counter = 0;
    // Linearize the circular buffer so index 0 is oldest and index 1023 is newest
    size_t oldest_idx = s_write_idx;
    size_t first_part_len = PITCH_BUFFER_SIZE - oldest_idx;
    memcpy(&s_linear_buf[0], &s_ring_buffer[oldest_idx], first_part_len * sizeof(int16_t));
    if (oldest_idx > 0) {
      memcpy(&s_linear_buf[first_part_len], &s_ring_buffer[0], oldest_idx * sizeof(int16_t));
    }

    TuningResult raw = {
      .has_signal = false,
      .status = TUNING_STATUS_SILENCE,
    };
    if (s_ignore_frames > 0) {
      s_ignore_frames--;
    } else {
      pitch_detector_analyze(s_linear_buf, PITCH_BUFFER_SIZE, s_instrument->lag_min,
                             s_instrument->lag_max, &raw);
    }
    pitch_smoother_update(&raw, s_instrument, s_tuner_mode, s_manual_string_idx,
                          &s_latest_result);
#ifdef TUNER_DEBUG
    prv_trace(s_linear_buf, &raw, &s_latest_result);
#endif

    if (s_client_callback) {
      s_client_callback(&s_latest_result, s_client_context);
    }
  }
}

static void prv_on_capture_stopped(MicCaptureStopReason reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Mic capture stopped involuntarily: reason %d", (int)reason);
  s_error_text = (reason == MicCaptureStopReasonPermissionRevoked) ? "Mic permission revoked"
                                                                   : "Mic stopped";
  s_latest_result.has_signal = false;
  s_latest_result.status = TUNING_STATUS_SILENCE;
  if (s_client_callback) {
    s_client_callback(&s_latest_result, s_client_context);
  }
}

void audio_mgr_init(AudioAnalysisCallback callback, void *context) {
  s_client_callback = callback;
  s_client_context = context;
  if (!s_instrument) {
    s_instrument = tuner_notes_get_instrument(INSTRUMENT_GUITAR);
  }
  pitch_detector_init();
  pitch_smoother_reset();
  s_chunk_counter = 0;
  s_ignore_frames = 0;
  memset(s_ring_buffer, 0, sizeof(s_ring_buffer));
  s_samples_received_total = 0;
  s_write_idx = 0;
}

void audio_mgr_deinit(void) {
  audio_mgr_stop();
  s_client_callback = NULL;
  s_client_context = NULL;
}

bool audio_mgr_start(void) {
  if (mic_capture_is_active()) {
    return true;
  }

  MicCaptureHandlers handlers = {
    .data = prv_on_audio_data,
    .stopped = prv_on_capture_stopped,
  };

  MicCaptureStartResult res = mic_capture_start(handlers, NULL);
  if (res == MicCaptureStartOk) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Mic capture started successfully");
    pitch_smoother_reset();
    s_error_text = NULL;
    return true;
  }

  APP_LOG(APP_LOG_LEVEL_WARNING, "Mic capture start failed with code %d", (int)res);
  s_error_text = prv_start_error_text(res);
  return false;
}

void audio_mgr_stop(void) {
  mic_capture_stop();
  s_error_text = NULL;
  s_latest_result.has_signal = false;
  s_latest_result.status = TUNING_STATUS_SILENCE;
}

bool audio_mgr_is_capturing(void) {
  return mic_capture_is_active();
}

const char *audio_mgr_get_error_text(void) {
  return s_error_text;
}

void audio_mgr_set_instrument(InstrumentId id) {
  s_instrument = tuner_notes_get_instrument(id);
  s_tuner_mode = TUNER_MODE_AUTO;
  s_manual_string_idx = 0;
  pitch_smoother_clear();
  s_latest_result.has_signal = false;
  s_latest_result.detected_freq_hz = 0.0f;
  s_latest_result.cents = 0;
  s_latest_result.string_index = 0;
  s_latest_result.status = TUNING_STATUS_SILENCE;
}

void audio_mgr_ignore_buzz(void) {
  s_ignore_frames = BUZZ_IGNORE_FRAMES;
}

void audio_mgr_set_mode(TunerMode mode) {
  s_tuner_mode = mode;
}

TunerMode audio_mgr_get_mode(void) {
  return s_tuner_mode;
}

void audio_mgr_set_manual_string(uint8_t string_index) {
  if (string_index < s_instrument->num_strings) {
    s_manual_string_idx = string_index;
    s_latest_result.string_index = string_index;
  }
}

uint8_t audio_mgr_get_manual_string(void) {
  return s_manual_string_idx;
}

const TuningResult* audio_mgr_get_latest_result(void) {
  return &s_latest_result;
}
