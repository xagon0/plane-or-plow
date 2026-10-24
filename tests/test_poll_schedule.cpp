#include "../src/poll_schedule.h"
#include <assert.h>
#include <iostream>
#include <vector>

int main() {
    {
        // Weather owns the slot for the first 2s of every 20s. Contact polling
        // starts 500ms later. Waiting another 20s after rejection would collide
        // forever; the actual scheduler must recover and preserve A,A,A,P.
        PollSchedule schedule;
        uint32_t due = 500;
        std::vector<uint32_t> accepted_at;
        std::vector<bool> plows;
        while (due < 65000) {
            bool available = due % 20000 >= 2000;
            bool is_plow = schedule.next_is_plow();
            schedule.submission(available, 20000);
            if (available) {
                accepted_at.push_back(due);
                plows.push_back(is_plow);
                assert(schedule.request_pending);
                schedule.completion(20000);
            }
            due += schedule.delay_ms;
        }
        assert(accepted_at.size() == 4 && accepted_at[0] == 2000);
        assert(!plows[0] && !plows[1] && !plows[2] && plows[3]);
        assert(schedule.cycle == 4 && schedule.delay_ms == 20000);
    }
    {
        PollSchedule schedule;
        for (int i = 0; i < 3; ++i) {
            schedule.submission(true, 8000);
            schedule.completion(8000);
        }
        assert(schedule.next_is_plow() && schedule.delay_ms == 8000);
        for (int i = 0; i < 20; ++i) schedule.submission(false, 8000);
        assert(schedule.next_is_plow() && schedule.cycle == 3 && schedule.delay_ms == 250);
        schedule.submission(true, 8000);
        assert(!schedule.next_is_plow() && schedule.delay_ms == 8000);
        assert(schedule.request_pending);
        schedule.completion(8000);
        schedule.submission(true, 20000);
        assert(schedule.delay_ms == 20000);
    }
    {
        // Always visit contacts before weather, as LVGL may do. Downloads
        // outlive each producer's normal/boost period. Neither source may
        // repeatedly take the released slot while the other waits.
        for (uint32_t period : {8000U, 20000U}) {
            PollSchedule schedules[2];
            uint32_t due[2] = {0, 0};
            int owner = -1;
            uint32_t finishes = 0;
            std::vector<int> starts;
            for (uint32_t now = 0; now < 200000; now += 250) {
                for (int i = 0; i < 2; ++i) {
                    if (now < due[i]) continue;
                    if (schedules[i].request_pending) {
                        due[i] = now + period;
                        continue;
                    }
                    bool accepted = owner < 0;
                    schedules[i].submission(accepted, period);
                    due[i] = now + schedules[i].delay_ms;
                    if (accepted) {
                        owner = i;
                        finishes = now + period + 5000;
                        starts.push_back(i);
                    }
                }
                // Main applies completions after LVGL timer callbacks.
                if (owner >= 0 && now >= finishes) {
                    schedules[owner].completion(period);
                    due[owner] = now + schedules[owner].delay_ms;
                    owner = -1;
                }
            }
            assert(starts.size() >= 6);
            for (size_t i = 0; i < starts.size(); ++i) assert(starts[i] == (int)(i % 2));
        }
    }
    std::cout << "PASS: shared-slot collision, boost scheduling, and slow-job fairness\n";
}
