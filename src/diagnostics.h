#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stdint.h>

static constexpr uint32_t LOOP_WATCHDOG_SECONDS = 60;
static constexpr unsigned DIAGNOSTIC_PHASE_SIZE = 40;

struct DiagnosticBreadcrumb {
    char phase[DIAGNOSTIC_PHASE_SIZE];
    uint32_t phase_started_ms;
    int64_t phase_started_epoch;  // UTC seconds, or 0 until SNTP has set time
    uint32_t checkpoint_ms;
    int64_t checkpoint_epoch;
};

struct DiagnosticsStatus {
    uint32_t boot_id;
    uint32_t boot_sequence;      // retained across resets, not loss of power
    bool watchdog_active;
    bool previous_valid;
    uint32_t previous_boot_id;
    DiagnosticBreadcrumb previous;
};

// Call once, early in setup() on the Arduino loop task. RTC RAM survives a
// watchdog/software restart, but is not a disk log and cannot survive power-off.
void diagnostics_begin();
const DiagnosticsStatus &diagnostics_status();
const DiagnosticBreadcrumb &diagnostics_current();
const char *diagnostics_build();

// These hooks are loop-task only. A phase change records context but NEVER
// feeds the watchdog; a stalled transfer must still time out.
void diagnostics_phase(const char *phase);
void diagnostics_checkpoint(); // only after a bounded operation makes progress
void diagnostics_update();     // periodic serial health and optional self-test

class DiagnosticsPhase {
public:
    explicit DiagnosticsPhase(const char *phase);
    ~DiagnosticsPhase();
    DiagnosticsPhase(const DiagnosticsPhase &) = delete;
    DiagnosticsPhase &operator=(const DiagnosticsPhase &) = delete;
private:
    char previous_[DIAGNOSTIC_PHASE_SIZE];
};

#endif
