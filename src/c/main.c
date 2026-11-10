#include <pebble.h>
#include "audio_mgr.h"
#include "instrument_menu.h"
#include "tuner_ui.h"

// Last chosen instrument, restored on launch
#define PERSIST_KEY_INSTRUMENT 1

static Window *s_main_window = NULL;

static void prv_audio_analysis_callback(const TuningResult *result, void *context) {
  // Skip while the instrument menu is open, so no in-tune buzz over the menu
  if (window_stack_get_top_window() != s_main_window) {
    return;
  }
  tuner_ui_update(result);
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  tuner_ui_handle_select();
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  tuner_ui_handle_up();
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  tuner_ui_handle_down();
}

static void prv_instrument_chosen(InstrumentId id) {
  tuner_ui_set_instrument(id);
  persist_write_int(PERSIST_KEY_INSTRUMENT, id);
}

static void prv_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  instrument_menu_show(tuner_ui_get_instrument(), prv_instrument_chosen);
}

// The OS stops capture when a notification, dictation, etc. takes focus;
// restart it once the tuner is back in front (also retries after a phone-side grant)
static void prv_did_focus(bool in_focus) {
  if (in_focus && !audio_mgr_is_capturing()) {
    audio_mgr_start();
    tuner_ui_update(NULL);
  }
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, prv_select_long_click_handler, NULL);
}

static void prv_window_load(Window *window) {
  tuner_ui_init(window);
}

static void prv_window_unload(Window *window) {
  tuner_ui_deinit();
}

static void prv_init(void) {
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);

  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });

  window_set_click_config_provider(s_main_window, prv_click_config_provider);
  window_stack_push(s_main_window, true);

  // Initialize audio and pitch detection pipeline
  audio_mgr_init(prv_audio_analysis_callback, NULL);
  InstrumentId instrument = INSTRUMENT_GUITAR;
  if (persist_exists(PERSIST_KEY_INSTRUMENT)) {
    int32_t saved = persist_read_int(PERSIST_KEY_INSTRUMENT);
    if (saved >= 0 && saved < INSTRUMENT_COUNT) {
      instrument = (InstrumentId)saved;
    }
  }
  tuner_ui_set_instrument(instrument);
  audio_mgr_start();
  tuner_ui_update(NULL);

  app_focus_service_subscribe_handlers((AppFocusHandlers) {
    .did_focus = prv_did_focus,
  });
}

static void prv_deinit(void) {
  app_focus_service_unsubscribe();
  audio_mgr_deinit();
  if (s_main_window) {
    window_destroy(s_main_window);
    s_main_window = NULL;
  }
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
  return 0;
}
