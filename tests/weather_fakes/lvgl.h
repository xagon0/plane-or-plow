#pragma once
#include <stdint.h>
struct lv_timer_t {
    void (*callback)(lv_timer_t *);
    uint32_t period;
    bool active;
    uint32_t last_reset_ms;
    unsigned resets;
};
lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *), uint32_t period, void *);
void lv_timer_del(lv_timer_t *timer);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void lv_timer_reset(lv_timer_t *timer);
