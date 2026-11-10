#include "instrument_menu.h"
#include "tuner_notes.h"
#include <pebble.h>
#ifdef TUNER_DEBUG
#include "clip_recorder.h"
#define MENU_NUM_ROWS (INSTRUMENT_COUNT + 1)
#else
#define MENU_NUM_ROWS INSTRUMENT_COUNT
#endif

// Every instrument fits on one screen, clear of the mic indicator
#if defined(PBL_ROUND)
#define MENU_Y 42
#define ROW_H 44
#define ROW_X 30
#define ROW_W 200
#define NAME_Y (-4)
#define TUNING_Y 20
#define ROW_ALIGN GTextAlignmentCenter
#define NAME_FONT FONT_KEY_GOTHIC_24_BOLD
#define TUNING_FONT FONT_KEY_GOTHIC_18
#elif defined(PBL_BW)
#define MENU_Y 0
#define ROW_H 34
#define ROW_X 6
#define ROW_W 132
#define NAME_Y (-3)
#define TUNING_Y 15
#define ROW_ALIGN GTextAlignmentLeft
#define NAME_FONT FONT_KEY_GOTHIC_18_BOLD
#define TUNING_FONT FONT_KEY_GOTHIC_14
#else
#define MENU_Y 0
#define ROW_H 47
#define ROW_X 10
#define ROW_W 180
#define NAME_Y (-2)
#define TUNING_Y 22
#define ROW_ALIGN GTextAlignmentLeft
#define NAME_FONT FONT_KEY_GOTHIC_24_BOLD
#define TUNING_FONT FONT_KEY_GOTHIC_18
#endif

static Window *s_window = NULL;
static MenuLayer *s_menu_layer = NULL;
static InstrumentId s_current = INSTRUMENT_GUITAR;
static InstrumentMenuCallback s_callback = NULL;

static uint16_t prv_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return MENU_NUM_ROWS;
}

static int16_t prv_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return ROW_H;
}

static int16_t prv_get_separator_height(MenuLayer *menu_layer, MenuIndex *cell_index,
                                        void *context) {
  return 0;
}

static void prv_draw_cell(GContext *ctx, const Layer *cell_layer, const char *name,
                          const char *tuning) {
  const bool selected = menu_cell_layer_is_highlighted(cell_layer);
  graphics_context_set_text_color(ctx, selected ? GColorBlack : GColorWhite);
  graphics_draw_text(ctx, name, fonts_get_system_font(NAME_FONT),
                     GRect(ROW_X, NAME_Y, ROW_W, ROW_H), GTextOverflowModeTrailingEllipsis,
                     ROW_ALIGN, NULL);
  graphics_context_set_text_color(
      ctx, selected ? GColorBlack : PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  graphics_draw_text(ctx, tuning, fonts_get_system_font(TUNING_FONT),
                     GRect(ROW_X, TUNING_Y, ROW_W, ROW_H - TUNING_Y),
                     GTextOverflowModeTrailingEllipsis, ROW_ALIGN, NULL);
}

static void prv_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                         void *context) {
#ifdef TUNER_DEBUG
  if (cell_index->row == INSTRUMENT_COUNT) {
    prv_draw_cell(ctx, cell_layer, "Record clip", "Debug");
    return;
  }
#endif
  const Instrument *inst = tuner_notes_get_instrument((InstrumentId)cell_index->row);
  prv_draw_cell(ctx, cell_layer, inst->name, inst->tuning);
}

static void prv_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
#ifdef TUNER_DEBUG
  if (cell_index->row == INSTRUMENT_COUNT) {
    clip_recorder_show();
    window_stack_remove(s_window, false);
    return;
  }
#endif
  if (s_callback) {
    s_callback((InstrumentId)cell_index->row);
  }
  window_stack_remove(s_window, true);
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect frame = layer_get_bounds(window_layer);
  frame.origin.y += MENU_Y;
  frame.size.h -= MENU_Y;
  s_menu_layer = menu_layer_create(frame);
#if defined(PBL_ROUND)
  menu_layer_set_center_focused(s_menu_layer, false);
#endif
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_get_num_rows,
    .get_cell_height = prv_get_cell_height,
    .get_separator_height = prv_get_separator_height,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_normal_colors(s_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_selected_index(s_menu_layer, MenuIndex(0, s_current), MenuRowAlignNone, false);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void prv_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
  window_destroy(s_window);
  s_window = NULL;
  s_callback = NULL;
}

void instrument_menu_show(InstrumentId current, InstrumentMenuCallback callback) {
  if (s_window) {
    return;
  }
  s_current = current;
  s_callback = callback;
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
