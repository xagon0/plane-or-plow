#ifndef POLL_SCHEDULE_H
#define POLL_SCHEDULE_H

#include <stdint.h>

// A busy shared HTTP slot must retry before the next full polling interval.
// Otherwise two equal-period timers can collide on every tick indefinitely.
struct PollSchedule {
    uint32_t cycle = 0;
    uint32_t delay_ms = 250;
    bool request_pending = false;
    bool next_is_plow() const { return cycle % 4 == 3; }
    void submission(bool accepted, uint32_t regular_period_ms) {
        delay_ms = accepted ? regular_period_ms : 250;
        if (accepted) {
            ++cycle;
            request_pending = true;
        }
    }
    void completion(uint32_t regular_period_ms) {
        request_pending = false;
        delay_ms = regular_period_ms;
    }
};

#endif // POLL_SCHEDULE_H
