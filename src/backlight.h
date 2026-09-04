#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdint.h>

void backlight_init();
void backlight_update();
void backlight_on_tap();

// Current PWM duty and schedule state, for /status.
uint8_t backlight_duty();
// True while a manual level is overriding the schedule.
bool    backlight_manual();
bool    backlight_is_daytime();

#endif // BACKLIGHT_H
