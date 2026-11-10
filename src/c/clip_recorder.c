#ifdef TUNER_DEBUG

#include "clip_recorder.h"
#include "audio_mgr.h"
#include "tuner_notes.h"
#include "tuner_ui.h"
#include <ctype.h>
#include <pebble.h>
#include <string.h>

#define CLIP_MAX_SAMPLES (6 * MIC_CAPTURE_SAMPLE_RATE)
// Every sample costs at most 3 bytes
#define CLIP_MAX_BYTES (3 * CLIP_MAX_SAMPLES)
// Left free for the window and AppMessage
#define HEAP_RESERVE 8192
#define MIN_BUFFER 16384
// Long enough that the start buzz isn't in the clip
#define SETTLE_MS 600
// Ends a recording if the mic stops feeding it (e.g. a notification took focus)
#define WATCHDOG_MS 7000
// Small enough that a base64 chunk survives as one log line
#define CHUNK_BYTES 900
#define RETRY_MS 300
#define MAX_RETRIES 20

typedef enum ClipState {
  CLIP_IDLE = 0,
  CLIP_SETTLE,
  CLIP_RECORDING,
  CLIP_SENDING,
  CLIP_FAILED,
} ClipState;

typedef enum SendStep {
  SEND_HEADER = 0,
  SEND_DATA,
  SEND_END,
} SendStep;

static Window *s_window = NULL;
static TextLayer *s_title_layer = NULL;
static TextLayer *s_body_layer = NULL;
static char s_title[32];
static char s_body[64];

static const Instrument *s_instrument = NULL;
// A string index; one past the last string is room silence
static uint8_t s_target = 0;
static ClipState s_state = CLIP_IDLE;
static AppTimer *s_timer = NULL;

static uint8_t *s_buffer = NULL;
static size_t s_capacity = 0;
static size_t s_length = 0;
static uint32_t s_samples = 0;
static int16_t s_previous = 0;
static int32_t s_peak = 0;
// Loudest mean level of one mic frame, the figure to compare with the silence gate
static int32_t s_level = 0;
static uint32_t s_clip_id = 0;

static SendStep s_step = SEND_HEADER;
static size_t s_sent = 0;
static uint32_t s_seq = 0;
static uint32_t s_sum = 0;
static uint8_t s_retries = 0;

static void prv_start_recording(void *context);

static void prv_cancel_timer(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
}

static void prv_free_buffer(void) {
  free(s_buffer);
  s_buffer = NULL;
}

static void prv_format_label(char *buf, size_t buf_len) {
  char name[12];
  size_t i = 0;
  for (; s_instrument->label[i] && i < sizeof(name) - 1; i++) {
    name[i] = (char)tolower((unsigned char)s_instrument->label[i]);
  }
  name[i] = '\0';
  if (s_target >= s_instrument->num_strings) {
    snprintf(buf, buf_len, "%s-%s", name, tuner_notes_is_chromatic(s_instrument) ? "note" : "room");
  } else {
    char note[8];
    tuner_notes_format_name(s_instrument, s_target, note, sizeof(note));
    snprintf(buf, buf_len, "%s-%d-%s", name, tuner_notes_string_number(s_instrument, s_target), note);
  }
}

static void prv_show(const char *title, const char *body) {
  snprintf(s_title, sizeof(s_title), "%s", title);
  snprintf(s_body, sizeof(s_body), "%s", body);
  text_layer_set_text(s_title_layer, s_title);
  text_layer_set_text(s_body_layer, s_body);
}

static void prv_show_idle(const char *result) {
  char title[24];
  if (tuner_notes_is_chromatic(s_instrument)) {
    snprintf(title, sizeof(title), "Any note");
  } else if (s_target >= s_instrument->num_strings) {
    snprintf(title, sizeof(title), "Room silence");
  } else {
    char note[8];
    tuner_notes_format_name(s_instrument, s_target, note, sizeof(note));
    snprintf(title, sizeof(title), "String %d (%s)",
             tuner_notes_string_number(s_instrument, s_target), note);
  }
  prv_show(title, result ? result : "Select: record\nUp/Down: change");
}

static void prv_fail(const char *reason) {
  prv_cancel_timer();
  audio_mgr_set_tap(NULL);
  prv_free_buffer();
  s_state = CLIP_FAILED;
  prv_show("Failed", reason);
}

// --- Sending ---

static void prv_send_next(void *context);

static void prv_retry(void) {
  if (++s_retries > MAX_RETRIES) {
    prv_fail("Phone not responding");
    return;
  }
  s_timer = app_timer_register(RETRY_MS, prv_send_next, NULL);
}

static void prv_send_finished(void) {
  char result[48];
  snprintf(result, sizeof(result), "Saved: peak %d, level %d", (int)s_peak, (int)s_level);
  prv_free_buffer();
  s_state = CLIP_IDLE;
  s_target = (s_target >= s_instrument->num_strings) ? 0 : s_target + 1;
  vibes_short_pulse();
  prv_show_idle(result);
}

static void prv_send_next(void *context) {
  s_timer = NULL;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    prv_retry();
    return;
  }
  dict_write_uint32(iter, MESSAGE_KEY_CLIP_ID, s_clip_id);
  if (s_step == SEND_HEADER) {
    char label[24];
    prv_format_label(label, sizeof(label));
    dict_write_cstring(iter, MESSAGE_KEY_CLIP_LABEL, label);
    dict_write_uint32(iter, MESSAGE_KEY_CLIP_SAMPLES, s_samples);
    dict_write_uint32(iter, MESSAGE_KEY_CLIP_TOTAL, s_length);
    dict_write_int32(iter, MESSAGE_KEY_CLIP_PEAK, s_peak);
    dict_write_int32(iter, MESSAGE_KEY_CLIP_LEVEL, s_level);
  } else if (s_step == SEND_DATA) {
    size_t size = s_length - s_sent;
    if (size > CHUNK_BYTES) {
      size = CHUNK_BYTES;
    }
    dict_write_uint32(iter, MESSAGE_KEY_CLIP_SEQ, s_seq);
    dict_write_data(iter, MESSAGE_KEY_CLIP_DATA, &s_buffer[s_sent], size);
  } else {
    dict_write_uint32(iter, MESSAGE_KEY_CLIP_SUM, s_sum);
  }
  if (app_message_outbox_send() != APP_MSG_OK) {
    prv_retry();
  }
}

static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  if (s_state != CLIP_SENDING) {
    return;
  }
  s_retries = 0;
  if (s_step == SEND_HEADER) {
    s_step = SEND_DATA;
  } else if (s_step == SEND_DATA) {
    size_t size = s_length - s_sent;
    if (size > CHUNK_BYTES) {
      size = CHUNK_BYTES;
    }
    s_sent += size;
    s_seq++;
    if (s_sent >= s_length) {
      s_step = SEND_END;
    }
    char title[24];
    snprintf(title, sizeof(title), "Sending %d%%", (int)(s_sent * 100 / s_length));
    snprintf(s_title, sizeof(s_title), "%s", title);
    text_layer_set_text(s_title_layer, s_title);
  } else {
    prv_send_finished();
    return;
  }
  prv_send_next(NULL);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  if (s_state == CLIP_SENDING) {
    prv_retry();
  }
}

// --- Recording ---

static void prv_finish_recording(void) {
  prv_cancel_timer();
  audio_mgr_set_tap(NULL);
  vibes_double_pulse();
  if (s_samples == 0) {
    prv_fail("No audio: is the mic on?");
    return;
  }
  for (size_t i = 0; i < s_length; i++) {
    s_sum += s_buffer[i];
  }
  char body[48];
  snprintf(body, sizeof(body), "Peak %d, level %d", (int)s_peak, (int)s_level);
  s_state = CLIP_SENDING;
  s_step = SEND_HEADER;
  prv_show("Sending 0%", body);
  prv_send_next(NULL);
}

static void prv_on_samples(const int16_t *samples, size_t count) {
  if (s_state != CLIP_RECORDING) {
    return;
  }
  int32_t frame_sum = 0;
  for (size_t i = 0; i < count; i++) {
    if (s_samples >= CLIP_MAX_SAMPLES || s_length + 3 > s_capacity) {
      prv_finish_recording();
      return;
    }
    const int16_t sample = samples[i];
    const int32_t delta = (int32_t)sample - s_previous;
    if (delta >= -127 && delta <= 127) {
      s_buffer[s_length++] = (uint8_t)(int8_t)delta;
    } else {
      s_buffer[s_length++] = 0x80;
      s_buffer[s_length++] = (uint8_t)(sample & 0xff);
      s_buffer[s_length++] = (uint8_t)((sample >> 8) & 0xff);
    }
    s_previous = sample;
    s_samples++;
    const int32_t magnitude = (sample < 0) ? -(int32_t)sample : sample;
    frame_sum += magnitude;
    if (magnitude > s_peak) {
      s_peak = magnitude;
    }
  }
  const int32_t frame_level = frame_sum / (int32_t)count;
  if (frame_level > s_level) {
    s_level = frame_level;
  }
}

static void prv_watchdog(void *context) {
  s_timer = NULL;
  prv_finish_recording();
}

static void prv_start_recording(void *context) {
  s_timer = NULL;
  s_state = CLIP_RECORDING;
  prv_show("Recording", "Pluck now");
  s_timer = app_timer_register(WATCHDOG_MS, prv_watchdog, NULL);
  audio_mgr_set_tap(prv_on_samples);
}

static bool prv_alloc_buffer(void) {
  size_t size = heap_bytes_free();
  size = (size > HEAP_RESERVE) ? size - HEAP_RESERVE : 0;
  if (size > CLIP_MAX_BYTES) {
    size = CLIP_MAX_BYTES;
  }
  // The largest free block can be smaller than the total free
  while (size >= MIN_BUFFER) {
    s_buffer = malloc(size);
    if (s_buffer) {
      s_capacity = size;
      APP_LOG(APP_LOG_LEVEL_INFO, "Clip buffer %d bytes", (int)size);
      return true;
    }
    size -= 4096;
  }
  return false;
}

static void prv_begin(void) {
  if (!audio_mgr_is_capturing()) {
    prv_fail("Mic is off");
    return;
  }
  if (!prv_alloc_buffer()) {
    prv_fail("Not enough memory");
    return;
  }
  s_length = 0;
  s_samples = 0;
  s_previous = 0;
  s_peak = 0;
  s_level = 0;
  s_sent = 0;
  s_seq = 0;
  s_sum = 0;
  s_retries = 0;
  s_clip_id = (uint32_t)time(NULL);
  s_state = CLIP_SETTLE;
  prv_show("Get ready", "");
  vibes_short_pulse();
  s_timer = app_timer_register(SETTLE_MS, prv_start_recording, NULL);
}

// --- Window ---

static bool prv_can_start(void) {
  return s_state == CLIP_IDLE || s_state == CLIP_FAILED;
}

static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  if (prv_can_start()) {
    prv_begin();
  }
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  if (prv_can_start()) {
    s_state = CLIP_IDLE;
    s_target = (s_target == 0) ? s_instrument->num_strings : s_target - 1;
    prv_show_idle(NULL);
  }
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  if (prv_can_start()) {
    s_state = CLIP_IDLE;
    s_target = (s_target >= s_instrument->num_strings) ? 0 : s_target + 1;
    prv_show_idle(NULL);
  }
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds(root);
  const int mid = bounds.size.h / 2;

  s_title_layer = text_layer_create(GRect(16, mid - 62, bounds.size.w - 32, 36));
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  s_body_layer = text_layer_create(GRect(24, mid - 18, bounds.size.w - 48, 66));
  text_layer_set_font(s_body_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  TextLayer *layers[] = { s_title_layer, s_body_layer };
  for (size_t i = 0; i < 2; i++) {
    text_layer_set_background_color(layers[i], GColorClear);
    text_layer_set_text_color(layers[i], GColorWhite);
    text_layer_set_text_alignment(layers[i], GTextAlignmentCenter);
    layer_add_child(root, text_layer_get_layer(layers[i]));
  }
  prv_show_idle(NULL);
}

static void prv_window_unload(Window *window) {
  prv_cancel_timer();
  audio_mgr_set_tap(NULL);
  prv_free_buffer();
  s_state = CLIP_IDLE;
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_body_layer);
  window_destroy(s_window);
  s_window = NULL;
}

void clip_recorder_show(void) {
  static bool s_app_message_open = false;
  if (s_window) {
    return;
  }
  if (!s_app_message_open) {
    app_message_register_outbox_sent(prv_outbox_sent);
    app_message_register_outbox_failed(prv_outbox_failed);
    app_message_open(64, CHUNK_BYTES + 64);
    s_app_message_open = true;
  }
  s_instrument = tuner_notes_get_instrument(tuner_ui_get_instrument());
  s_target = 0;
  s_state = CLIP_IDLE;
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, prv_click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}

#endif
