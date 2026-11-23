#pragma once
#include <stdbool.h>

typedef enum {
    BTN_SHORT,       // 200 ms – 3 s, single
    BTN_DOUBLE,      // two presses < 350 ms within 500 ms
    BTN_RESTART,     // 4 – 8 s
    BTN_RESET_PAIR,  // 10 – 15 s
} btn_event_t;

typedef void (*btn_cb_t)(btn_event_t ev);

void io_init(bool charger_on, btn_cb_t cb);
void io_set_charger(bool on);
void io_set_led(int sr_bit, bool on);
void io_door_pulse(void);
// Returns NAN if the reading is unusable.
float io_read_temperature_c(void);
