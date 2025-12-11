/*
 * MIT License
 *
 * Copyright (c) 2025 Elliot Sayes
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "verbal_clock_face.h"
#include "watch.h"
#include "watch_utility.h"
#include "watch_common_display.h"

// 2.4 volts seems to offer adequate warning of a low battery condition?
// refined based on user reports and personal observations; may need further adjustment.
#ifndef CLOCK_FACE_LOW_BATTERY_VOLTAGE_THRESHOLD
#define CLOCK_FACE_LOW_BATTERY_VOLTAGE_THRESHOLD 2400
#endif

static const char *words[12] = {
    "   ",
    "FIV",
    "TEN",
    "QTR",
    "TWY",
    "TW5",
    "HLF",
    // unused
    "35",
    "40",
    "45",
    "50",
    "55",
};

// TODO: use these
static const char *words_fallback[12] = {
    "   ",
    "FV ",
    "TN ",
    "QR ",
    "TY ",
    "T5 ",
    "HL ",
    // unused
    "35",
    "40",
    "45",
    "50",
    "55",
};

static const char *past_word = " P";
static const char *to_word = "to";
static const char *oclock_super = "oc";
static const char *oclock_inline = "OC";

enum OC_MODE {
    NEVER = 0,
    INLINE,
    SUPER,
};

typedef struct {
    char word[6 + 1];
    enum OC_MODE oc_mode;
} hour_data_t;

static const hour_data_t hours_data[24] = {
  { "}}th  ", NEVER }, // lilith
  { " ONE  ", INLINE },
  { " TuuO ", SUPER },
  { " Three", SUPER },
  { "Four  ", INLINE },
  { "F1VE  ", INLINE },
  { "  S,][", SUPER },
  { " SEVeN", SUPER },
  { "E1GHT ", SUPER },
  { "N1NE  ", INLINE },
  { " TeN  ", INLINE },
  { "ELEVeN", SUPER },
  { "Noon  ", NEVER },
  { " ONE  ", INLINE },
  { " Tuu0 ", SUPER },
  { " Three", SUPER },
  { "Four  ", INLINE },
  { "F1VE  ", INLINE },
  { "  S,][", SUPER },
  { " SEVeN", SUPER },
  { "E1GHT ", SUPER },
  { "N1NE  ", INLINE },
  { " TeN  ", INLINE },
  { "ELEVeN", SUPER },
};

// sets when in the five minute period we switch
// from "X past HH" to  "X to HH+1"
static const int hour_switch_index = 7;

static void clock_stop_tick_tock_animation(void) {
    if (watch_sleep_animation_is_running()) {
        watch_stop_sleep_animation();
        watch_stop_blink();
    }
}

static void clock_indicate(watch_indicator_t indicator, bool on) {
    if (on) {
        watch_set_indicator(indicator);
    } else {
        watch_clear_indicator(indicator);
    }
}

void verbal_clock_face_setup(uint8_t watch_face_index, void ** context_ptr) {
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(verbal_clock_state_t));
        memset(*context_ptr, 0, sizeof(verbal_clock_state_t));
    }
}

void verbal_clock_face_activate(void *context) {
    verbal_clock_state_t *state = (verbal_clock_state_t *)context;

    clock_stop_tick_tock_animation();

    clock_indicate(WATCH_INDICATOR_BELL, movement_alarm_enabled());
    // We don't support 24H mode yet (ever?)
    clock_indicate(WATCH_INDICATOR_24H, false);

    // this ensures that none of the five_minute_periods will match, so we always rerender when the face activates
    state->prev_five_minute_period = -1;
    state->prev_min_checked = -1;
}

static void clock_check_battery_periodically(verbal_clock_state_t *state) {
    // If the battery is  low, skip the check. We have already indicated it.
    if (state->battery_low) {
        return;
    }

    watch_date_time_t date_time = movement_get_local_date_time();

    if (date_time.unit.day == state->last_battery_check) { return; }

    state->last_battery_check = date_time.unit.day;

    uint16_t voltage = watch_get_vcc_voltage();

    state->battery_low = voltage < CLOCK_FACE_LOW_BATTERY_VOLTAGE_THRESHOLD;

    if (watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM) {
        // interlocking arrows imply "exchange" the battery.
        clock_indicate(WATCH_INDICATOR_ARROWS, state->battery_low);
    } else {
        // LAP indicator on classic LCD is an adequate fallback.
        clock_indicate(WATCH_INDICATOR_LAP, state->battery_low);
    }
}

bool verbal_clock_face_loop(movement_event_t event, void *context) {
    verbal_clock_state_t *state = (verbal_clock_state_t *)context;
    watch_date_time_t date_time;
    bool show_next_hour = false;
    int prev_five_minute_period;
    int prev_min_checked;
    int verbal_clock_hour;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
        case EVENT_TICK:
        case EVENT_LOW_ENERGY_UPDATE:
            date_time = movement_get_local_date_time();
            prev_five_minute_period = state->prev_five_minute_period;
            prev_min_checked = state->prev_min_checked;

            // check the battery voltage once a day...
            clock_check_battery_periodically(state);

            // same minute, skip update
            if (date_time.unit.minute == prev_min_checked) {
                break;
            } else {
                state->prev_min_checked = date_time.unit.minute;
            }

            int five_minute_period = (date_time.unit.minute / 5) % 12;

            // Move to next five minute period if we are above 50% through the current five minute period (we are only checking the remainder)
            if (fmodf(date_time.unit.minute / 5.0f, 1.0f) > 0.5f) {
                // If we are on the last 5 interval and moving to the next period we need to display the next hour
                if (five_minute_period == 11) {
                    show_next_hour = true;
                }

                five_minute_period = (five_minute_period + 1) % 12;
            }

            // same five_minute_period, skip update
            if (five_minute_period == prev_five_minute_period) {
                break;
            }

            int clock_hour = date_time.unit.hour;
            verbal_clock_hour = clock_hour;

            // move from "MM P HH" to "MM 2 HH+1"
            if (five_minute_period >= hour_switch_index || show_next_hour) {
                verbal_clock_hour = (verbal_clock_hour + 1) % 24;
                show_next_hour = true;
            }

            if (clock_hour < 12) {
                watch_clear_indicator(WATCH_INDICATOR_PM);
            } else {
                watch_set_indicator(WATCH_INDICATOR_PM);
            }

            hour_data_t hour_data = hours_data[verbal_clock_hour];

            char top_mid[3 + 1] = { 0 };
            char top_right[2 + 1] = { 0 };
            char bottom[6 + 1] = { 0 };
            if (five_minute_period == 0) { // "  HH OC",
                sprintf(top_mid, "   ");
                if (hour_data.oc_mode == SUPER) {
                    strncpy(top_right, oclock_super, 3);
                } else {
                    sprintf(top_right, "  ");
                }
                if (hour_data.oc_mode == INLINE) {
                    strncpy(bottom, hour_data.word, 4);
                    strncpy(bottom + 4, oclock_inline, 3);
                } else {
                    strncpy(bottom, hour_data.word, 7);
                }
            } else { // "MM P HH" or "MM 2 HH+1"
                int words_length = sizeof(words) / sizeof(words[0]);

                strncpy(
                    top_mid,
                    show_next_hour ?
                        words[words_length - five_minute_period] :
                        words[five_minute_period],
                    4
                );
                strncpy(
                    top_right,
                    show_next_hour ? to_word : past_word,
                    3
                );
                strncpy(bottom, hour_data.word, 7);
            }

            watch_display_text_with_fallback(
                WATCH_POSITION_TOP_LEFT,
                top_mid, top_mid
            );

            watch_display_text(
                WATCH_POSITION_TOP_RIGHT,
                top_right
            );

            watch_display_text(
                WATCH_POSITION_BOTTOM,
                bottom
            );

            state->prev_five_minute_period = five_minute_period;
            break;

        default:
            return movement_default_loop_handler(event);
    }

    return true;
}

void verbal_clock_face_resign(void *context) {
    (void) context;
}

