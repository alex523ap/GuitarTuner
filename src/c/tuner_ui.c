#include "tuner_ui.h"
#include "audio_mgr.h"
#include "tuner_notes.h"
#include <pebble.h>
#include <stdio.h>

static Layer *s_canvas_layer = NULL;
static Window *s_window = NULL;

static TuningResult s_current_result = {
  .has_signal = false,
  .detected_freq_hz = 0.0f,
  .cents = 0,
  .string_index = 0,
  .status = TUNING_STATUS_SILENCE,
};

static const Instrument *s_instrument = NULL;
static InstrumentId s_instrument_id = INSTRUMENT_GUITAR;
static TunerMode s_mode = TUNER_MODE_AUTO;
static uint8_t s_manual_string_idx = 0;
static time_t s_last_vibe_time = 0;
static bool s_was_in_tune = false;
static GFont s_note_font = NULL;

// One buzz each time a note comes into tune. Re-entering within a few seconds
// stays silent, so a note wobbling on the edge doesn't keep buzzing.
#define BUZZ_GAP_S 3

static void prv_trigger_haptic(void) {
  const time_t now = time(NULL);
  if (!s_was_in_tune && now - s_last_vibe_time >= BUZZ_GAP_S) {
    vibes_short_pulse();
    audio_mgr_ignore_buzz();
    s_last_vibe_time = now;
  }
}

static void prv_format_freq(char *buf, size_t buf_len, const char *prefix, float freq) {
  if (freq < 0.0f) freq = 0.0f;
  int whole = (int)freq;
  int frac = (int)((freq - (float)whole) * 10.0f + 0.5f);
  if (frac >= 10) {
    whole += 1;
    frac -= 10;
  }
  if (prefix) {
    snprintf(buf, buf_len, "%s%d.%d Hz", prefix, whole, frac);
  } else {
    snprintf(buf, buf_len, "%d.%d Hz", whole, frac);
  }
}

// "GUITAR · AUTO", or just "CHROMATIC" (no strings to lock)
static void prv_format_header(char *buf, size_t buf_len) {
  if (tuner_notes_is_chromatic(s_instrument)) {
    snprintf(buf, buf_len, "%s", s_instrument->label);
  } else {
    snprintf(buf, buf_len, "%s · %s", s_instrument->label,
             (s_mode == TUNER_MODE_AUTO) ? "AUTO" : "MANUAL");
  }
}

// String name, or note with octave in chromatic mode ("--" until a note is heard)
static void prv_format_note(char *buf, size_t buf_len) {
  if (tuner_notes_is_chromatic(s_instrument) && !s_current_result.has_signal) {
    snprintf(buf, buf_len, "--");
  } else {
    tuner_notes_format_name(s_instrument, s_current_result.string_index, buf, buf_len);
  }
}

// Detected pitch, or what to aim for while silent
static void prv_format_freq_line(char *buf, size_t buf_len) {
  if (s_current_result.has_signal) {
    prv_format_freq(buf, buf_len, NULL, s_current_result.detected_freq_hz);
  } else if (tuner_notes_is_chromatic(s_instrument)) {
    buf[0] = '\0';
  } else {
    prv_format_freq(buf, buf_len, "Target ",
                    tuner_notes_target_hz(s_instrument, s_current_result.string_index));
  }
}

// Every layout keeps above the OS mic indicator: y 190 on Time 2, y 138 on
// Pebble 2 Duo, and y 212 on Round 2.
typedef struct TextStyle {
  const char *font_key;
  int16_t size;  // nominal height, used to centre the text in its box
} TextStyle;

#define TEXT_14B ((TextStyle) { FONT_KEY_GOTHIC_14_BOLD, 14 })
#define TEXT_14 ((TextStyle) { FONT_KEY_GOTHIC_14, 14 })
#define TEXT_18 ((TextStyle) { FONT_KEY_GOTHIC_18, 18 })
#define TEXT_18B ((TextStyle) { FONT_KEY_GOTHIC_18_BOLD, 18 })
#define TEXT_24B ((TextStyle) { FONT_KEY_GOTHIC_24_BOLD, 24 })
#define TEXT_28B ((TextStyle) { FONT_KEY_GOTHIC_28_BOLD, 28 })

typedef struct Palette {
  GColor bg;
  GColor ink;
  GColor dim;     // secondary text
  GColor accent;  // flat, sharp or neutral
  GColor rail;
  GColor tick;
  GColor cell_bg;  // current string
  GColor cell_fg;
} Palette;

#if defined(PBL_ROUND)
// --- GABBRO (260x260 round) ---
#define HEADER_BOX GRect(40, 40, 180, 20)
#define HEADER_TEXT TEXT_18B
#define REF_BOX GRect(40, 64, 180, 22)
#define REF_TEXT TEXT_18B
#define STRINGS_Y 64
#define STRINGS_H 22
#define STRING_W 26
#define STRING_TEXT TEXT_18B
#define NOTE_BOX GRect(0, 78, 260, 76)
#define TRI_LEFT GRect(40, 110, 18, 32)
#define TRI_RIGHT GRect(202, 110, 18, 32)
#define STATUS_BOX GRect(20, 166, 220, 26)
#define STATUS_TEXT TEXT_24B
#define HZ_BOX GRect(40, 192, 180, 20)
#define HZ_TEXT TEXT_18
#define CHIP_BOX GRect(80, 64, 100, 24)
#define CHIP_TEXT TEXT_18B
#define MSG_BOX GRect(30, 98, 200, 64)
#define MSG_TEXT TEXT_28B
#define ARC_RADIUS 118
#define ARC_SPAN_DEG 60
#define ARC_WIDTH 8
#define ARC_MARK_WIDTH 16
#elif defined(PBL_BW)
// --- FLINT (144x168 black and white) ---
#define HEADER_BOX GRect(0, 0, 144, 16)
#define HEADER_TEXT TEXT_14B
#define REF_BOX GRect(0, 17, 144, 16)
#define REF_TEXT TEXT_14B
#define STRINGS_Y 17
#define STRINGS_H 16
#define STRING_W 20
#define STRING_TEXT TEXT_14B
#define NOTE_BOX GRect(0, 24, 144, 50)
#define TRI_LEFT GRect(8, 47, 12, 24)
#define TRI_RIGHT GRect(124, 47, 12, 24)
#define STATUS_BOX GRect(0, 104, 144, 20)
#define STATUS_TEXT TEXT_18B
#define HZ_BOX GRect(0, 122, 144, 16)
#define HZ_TEXT TEXT_14
#define CHIP_BOX GRect(0, 22, 144, 20)
#define CHIP_TEXT TEXT_14B
#define MSG_BOX GRect(4, 48, 136, 78)
#define MSG_TEXT TEXT_24B
#define METER_X 8
#define METER_W 128
#define RAIL_Y 94
#define RAIL_H 2
#define TICK_Y 90
#define TICK_H 10
#define ZONE_Y 86
#define ZONE_H 18
#define NEEDLE_Y 85
#define NEEDLE_H 20
#define NEEDLE_W 5
#else
// --- EMERY (200x228) ---
#define HEADER_BOX GRect(0, 0, 200, 22)
#define HEADER_TEXT TEXT_18B
#define REF_BOX GRect(0, 24, 200, 22)
#define REF_TEXT TEXT_18B
#define STRINGS_Y 24
#define STRINGS_H 22
#define STRING_W 26
#define STRING_TEXT TEXT_18B
#define NOTE_BOX GRect(0, 36, 200, 72)
#define TRI_LEFT GRect(10, 68, 18, 32)
#define TRI_RIGHT GRect(172, 68, 18, 32)
#define STATUS_BOX GRect(0, 148, 200, 26)
#define STATUS_TEXT TEXT_24B
#define HZ_BOX GRect(0, 172, 200, 18)
#define HZ_TEXT TEXT_18
#define CHIP_BOX GRect(50, 44, 100, 24)
#define CHIP_TEXT TEXT_18B
#define MSG_BOX GRect(10, 76, 180, 64)
#define MSG_TEXT TEXT_28B
#define METER_X 12
#define METER_W 176
#define RAIL_Y 131
#define RAIL_H 6
#define TICK_Y 127
#define TICK_H 14
#define ZONE_Y 121
#define ZONE_H 26
#define NEEDLE_Y 120
#define NEEDLE_H 28
#define NEEDLE_W 6
#endif

#define STRING_GAP 2

static Palette prv_palette(void) {
  if (s_current_result.has_signal && s_current_result.status == TUNING_STATUS_IN_TUNE) {
    const GColor bg = PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite);
    return (Palette) {
      .bg = bg, .ink = GColorBlack, .dim = GColorBlack, .accent = GColorBlack,
      .rail = GColorBlack, .tick = GColorBlack, .cell_bg = GColorBlack, .cell_fg = bg,
    };
  }
  GColor accent = GColorWhite;
  if (s_current_result.has_signal && s_current_result.status == TUNING_STATUS_FLAT) {
    accent = PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorWhite);
  } else if (s_current_result.has_signal && s_current_result.status == TUNING_STATUS_SHARP) {
    accent = PBL_IF_COLOR_ELSE(GColorSunsetOrange, GColorWhite);
  }
  return (Palette) {
    .bg = GColorBlack, .ink = GColorWhite,
    .dim = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite),
    .accent = accent,
    .rail = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite),
    .tick = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite),
    .cell_bg = GColorWhite, .cell_fg = GColorBlack,
  };
}

static void prv_text(GContext *ctx, const char *text, TextStyle style, GRect box, GColor color) {
  // System fonts sit low in their line, so centre on the size, not the line box
  GRect frame = box;
  frame.origin.y += (box.size.h - style.size) / 2 - style.size / 6;
  frame.size.h = style.size + style.size / 2;
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, fonts_get_system_font(style.font_key), frame,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Points right when facing_right, with its flat edge on the far side
static void prv_triangle(GContext *ctx, GRect box, bool facing_right, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 1);
  const int mid = box.origin.y + box.size.h / 2;
  for (int i = 0; i < box.size.w; i++) {
    const int half = (box.size.h / 2) * (box.size.w - i) / box.size.w;
    const int x = facing_right ? box.origin.x + i : box.origin.x + box.size.w - 1 - i;
    graphics_draw_line(ctx, GPoint(x, mid - half), GPoint(x, mid + half));
  }
}

static void prv_string_row(GContext *ctx, const Palette *pal, const GRect *bounds) {
  const int count = s_instrument->num_strings;
  const int total = count * STRING_W + (count - 1) * STRING_GAP;
  const int x0 = (bounds->size.w - total) / 2;
  for (int i = 0; i < count; i++) {
    const GRect cell = GRect(x0 + i * (STRING_W + STRING_GAP), STRINGS_Y, STRING_W, STRINGS_H);
    const bool current = (i == s_current_result.string_index);
    if (current) {
      graphics_context_set_fill_color(ctx, pal->cell_bg);
      graphics_fill_rect(ctx, cell, 0, GCornerNone);
    }
    prv_text(ctx, s_instrument->strings[i].name, STRING_TEXT, cell,
             current ? pal->cell_fg : pal->dim);
  }
}

#if defined(PBL_ROUND)
static GRect prv_arc_rect(const GRect *bounds, int outer_radius) {
  return grect_inset(*bounds, GEdgeInsets(bounds->size.w / 2 - outer_radius));
}

static void prv_arc(GContext *ctx, const GRect *bounds, int from_deg, int to_deg, int width,
                    GColor color) {
  const GRect rect = prv_arc_rect(bounds, ARC_RADIUS + width / 2);
  graphics_context_set_fill_color(ctx, color);
  // Angles must rise from 0 to 360, so split anything crossing 12 o'clock
  if (from_deg < 0) {
    const int end_deg = (to_deg < 0) ? 360 + to_deg : 360;
    graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, width,
                         DEG_TO_TRIGANGLE(360 + from_deg), DEG_TO_TRIGANGLE(end_deg));
    from_deg = 0;
  }
  if (to_deg > from_deg) {
    graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, width, DEG_TO_TRIGANGLE(from_deg),
                         DEG_TO_TRIGANGLE(to_deg));
  }
}

static void prv_meter(GContext *ctx, const Palette *pal, const GRect *bounds) {
  prv_arc(ctx, bounds, -ARC_SPAN_DEG, ARC_SPAN_DEG, ARC_WIDTH, pal->rail);
  prv_arc(ctx, bounds, -31, -29, ARC_MARK_WIDTH, pal->tick);
  prv_arc(ctx, bounds, 29, 31, ARC_MARK_WIDTH, pal->tick);
  prv_arc(ctx, bounds, -6, 6, ARC_MARK_WIDTH, pal->ink);
  if (!s_current_result.has_signal) {
    return;
  }
  const int deg = (s_current_result.cents * ARC_SPAN_DEG) / 50;
  const GPoint needle = gpoint_from_polar(prv_arc_rect(bounds, ARC_RADIUS),
                                          GOvalScaleModeFitCircle,
                                          DEG_TO_TRIGANGLE((360 + deg) % 360));
  graphics_context_set_fill_color(ctx, pal->bg);
  graphics_fill_circle(ctx, needle, 11);
  graphics_context_set_fill_color(ctx, pal->accent);
  graphics_fill_circle(ctx, needle, 9);
}
#else
static void prv_meter(GContext *ctx, const Palette *pal, const GRect *bounds) {
  const int center_x = METER_X + METER_W / 2;
  const int zone_half = (5 * METER_W + 50) / 100;

  graphics_context_set_fill_color(ctx, pal->rail);
  graphics_fill_rect(ctx, GRect(METER_X, RAIL_Y, METER_W, RAIL_H), 0, GCornerNone);

  static const int8_t tick_cents[] = {-50, -25, 25, 50};
  graphics_context_set_fill_color(ctx, pal->tick);
  for (size_t i = 0; i < ARRAY_LENGTH(tick_cents); i++) {
    const int x = center_x + (tick_cents[i] * METER_W) / 100 - 1;
    graphics_fill_rect(ctx, GRect(x, TICK_Y, 2, TICK_H), 0, GCornerNone);
  }

  const GRect zone = GRect(center_x - zone_half, ZONE_Y, 2 * zone_half, ZONE_H);
  graphics_context_set_fill_color(ctx, pal->ink);
  graphics_fill_rect(ctx, zone, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, pal->bg);
  graphics_fill_rect(ctx, grect_inset(zone, GEdgeInsets(2)), 0, GCornerNone);

  if (s_current_result.has_signal) {
    const int x = center_x + (s_current_result.cents * METER_W) / 100 - NEEDLE_W / 2;
    const GRect needle = GRect(x, NEEDLE_Y, NEEDLE_W, NEEDLE_H);
    graphics_context_set_fill_color(ctx, pal->bg);
    graphics_fill_rect(ctx, needle, 0, GCornerNone);
    graphics_context_set_fill_color(ctx, pal->accent);
    graphics_fill_rect(ctx, grect_inset(needle, GEdgeInsets(1)), 0, GCornerNone);
  }
}
#endif

static void prv_draw_error(GContext *ctx, const Palette *pal, const char *message) {
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorSunsetOrange, GColorWhite));
  graphics_fill_rect(ctx, CHIP_BOX, 0, GCornerNone);
  prv_text(ctx, "MIC ERROR", CHIP_TEXT, CHIP_BOX, GColorBlack);
  graphics_context_set_text_color(ctx, pal->ink);
  graphics_draw_text(ctx, message, fonts_get_system_font(MSG_TEXT.font_key), MSG_BOX,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_draw_tuner(GContext *ctx, const Palette *pal, const GRect *bounds) {
  const bool chromatic = tuner_notes_is_chromatic(s_instrument);
  const bool heard = s_current_result.has_signal;
  const bool in_tune = heard && s_current_result.status == TUNING_STATUS_IN_TUNE;
  const bool flat = heard && s_current_result.status == TUNING_STATUS_FLAT;
  const bool sharp = heard && s_current_result.status == TUNING_STATUS_SHARP;

  if (chromatic) {
    prv_text(ctx, "A4 = 440 Hz", REF_TEXT, REF_BOX, pal->dim);
  } else {
    prv_string_row(ctx, pal, bounds);
  }

  char note_buf[8];
  prv_format_note(note_buf, sizeof(note_buf));
  graphics_context_set_text_color(ctx, heard ? (in_tune ? pal->ink : pal->accent) : pal->dim);
  graphics_draw_text(ctx, note_buf, s_note_font, NOTE_BOX, GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  if (flat || in_tune) {
    prv_triangle(ctx, TRI_LEFT, true, pal->accent);
  }
  if (sharp || in_tune) {
    prv_triangle(ctx, TRI_RIGHT, false, pal->accent);
  }

  prv_meter(ctx, pal, bounds);

  char status_buf[24];
  if (in_tune) {
    snprintf(status_buf, sizeof(status_buf), "IN TUNE");
  } else if (heard) {
    snprintf(status_buf, sizeof(status_buf), "%+d cents (%s)", s_current_result.cents,
             flat ? "FLAT" : "SHARP");
  } else if (chromatic) {
    snprintf(status_buf, sizeof(status_buf), "Play a note");
  } else if (s_mode == TUNER_MODE_MANUAL) {
    snprintf(status_buf, sizeof(status_buf), "Pluck string %d",
             tuner_notes_string_number(s_instrument, s_current_result.string_index));
  } else {
    snprintf(status_buf, sizeof(status_buf), "Pluck a string");
  }
  prv_text(ctx, status_buf, STATUS_TEXT, STATUS_BOX, (flat || sharp) ? pal->accent : pal->ink);

  if (heard || !chromatic) {
    char freq_buf[24];
    prv_format_freq_line(freq_buf, sizeof(freq_buf));
    prv_text(ctx, freq_buf, HZ_TEXT, HZ_BOX, heard ? pal->ink : pal->dim);
  }
}

static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  // Fixed layouts, kept clear of the mic indicator, so the full bounds are right
  const GRect bounds = layer_get_bounds(layer);
  const char *mic_error = audio_mgr_get_error_text();
  Palette pal = prv_palette();
  if (mic_error) {
    pal.bg = GColorBlack;
    pal.ink = GColorWhite;
    pal.dim = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  }

  graphics_context_set_fill_color(ctx, pal.bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char header[32];
  prv_format_header(header, sizeof(header));
  prv_text(ctx, header, HEADER_TEXT, HEADER_BOX, pal.dim);

  if (mic_error) {
    prv_draw_error(ctx, &pal, mic_error);
  } else {
    prv_draw_tuner(ctx, &pal, &bounds);
  }
}

void tuner_ui_init(Window *window) {
  s_window = window;
  if (!s_instrument) {
    s_instrument = tuner_notes_get_instrument(s_instrument_id);
  }
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_note_font = fonts_load_custom_font(resource_get_handle(
      PBL_IF_COLOR_ELSE(RESOURCE_ID_FONT_NOTE_74, RESOURCE_ID_FONT_NOTE_54)));
  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);
}

void tuner_ui_deinit(void) {
  if (s_canvas_layer) {
    layer_destroy(s_canvas_layer);
    s_canvas_layer = NULL;
  }
  if (s_note_font) {
    fonts_unload_custom_font(s_note_font);
    s_note_font = NULL;
  }
}

void tuner_ui_update(const TuningResult *result) {
  if (result) {
    s_current_result = *result;
    if (s_current_result.status == TUNING_STATUS_IN_TUNE) {
      prv_trigger_haptic();
      s_was_in_tune = true;
    } else {
      s_was_in_tune = false;
    }
  }
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
}

void tuner_ui_set_mode(TunerMode mode, uint8_t manual_string_idx) {
  s_mode = mode;
  s_manual_string_idx = manual_string_idx;
  s_current_result.string_index = manual_string_idx;
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
}

void tuner_ui_set_instrument(InstrumentId id) {
  s_instrument_id = id;
  s_instrument = tuner_notes_get_instrument(id);
  s_mode = TUNER_MODE_AUTO;
  s_manual_string_idx = 0;
  s_was_in_tune = false;
  s_current_result = (TuningResult) {
    .has_signal = false,
    .status = TUNING_STATUS_SILENCE,
  };
  audio_mgr_set_instrument(id);
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
}

InstrumentId tuner_ui_get_instrument(void) {
  return s_instrument_id;
}

void tuner_ui_handle_select(void) {
  // Chromatic mode has no strings to lock
  if (tuner_notes_is_chromatic(s_instrument)) {
    return;
  }
  // Toggle between Auto and Manual modes
  if (s_mode == TUNER_MODE_AUTO) {
    s_mode = TUNER_MODE_MANUAL;
  } else {
    s_mode = TUNER_MODE_AUTO;
  }
  audio_mgr_set_mode(s_mode);
  tuner_ui_set_mode(s_mode, s_manual_string_idx);
}

void tuner_ui_handle_up(void) {
  if (tuner_notes_is_chromatic(s_instrument)) {
    return;
  }
  // Cycle string (e.g. 6 -> 5 -> 4 -> 3 -> 2 -> 1)
  if (s_mode == TUNER_MODE_AUTO) {
    s_mode = TUNER_MODE_MANUAL;
    audio_mgr_set_mode(s_mode);
  }
  if (s_manual_string_idx > 0) {
    s_manual_string_idx--;
  } else {
    s_manual_string_idx = s_instrument->num_strings - 1;
  }
  audio_mgr_set_manual_string(s_manual_string_idx);
  tuner_ui_set_mode(s_mode, s_manual_string_idx);
}

void tuner_ui_handle_down(void) {
  if (tuner_notes_is_chromatic(s_instrument)) {
    return;
  }
  // Cycle string down
  if (s_mode == TUNER_MODE_AUTO) {
    s_mode = TUNER_MODE_MANUAL;
    audio_mgr_set_mode(s_mode);
  }
  if (s_manual_string_idx < s_instrument->num_strings - 1) {
    s_manual_string_idx++;
  } else {
    s_manual_string_idx = 0;
  }
  audio_mgr_set_manual_string(s_manual_string_idx);
  tuner_ui_set_mode(s_mode, s_manual_string_idx);
}
