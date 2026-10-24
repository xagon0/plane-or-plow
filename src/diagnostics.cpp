#include "diagnostics.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>

namespace {
constexpr uint32_t RETAINED_MAGIC = 0x504c5732; // PLW2; change for layout changes
struct RetainedDiagnostics {
    uint32_t magic;
    uint32_t boot_id;
    uint32_t boot_sequence;
    bool watchdog_self_test_done;
    DiagnosticBreadcrumb breadcrumb;
};

// Deliberately RTC RAM, never Preferences/NVS: frequent breadcrumbs cannot
// wear out flash. Power-on/brownout records are discarded below.
RTC_NOINIT_ATTR RetainedDiagnostics retained;
DiagnosticsStatus state = {};
uint32_t last_health_ms = 0;

int64_t epoch_now() {
    time_t now = time(nullptr);
    return now > 1700000000 ? (int64_t)now : 0;
}
}

const char *diagnostics_build() { return "freeze-recovery " __DATE__ " " __TIME__; }
const DiagnosticsStatus &diagnostics_status() { return state; }
const DiagnosticBreadcrumb &diagnostics_current() { return retained.breadcrumb; }

void diagnostics_begin() {
    const esp_reset_reason_t reason = esp_reset_reason();
    state.previous_valid = reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT &&
        retained.magic == RETAINED_MAGIC && retained.boot_sequence != 0 &&
        retained.breadcrumb.phase[DIAGNOSTIC_PHASE_SIZE - 1] == '\0';
    bool self_test_done = false;
    if (state.previous_valid) {
        state.previous = retained.breadcrumb;
        state.previous_boot_id = retained.boot_id;
        state.boot_sequence = retained.boot_sequence;
        self_test_done = retained.watchdog_self_test_done;
        Serial.printf("Previous run: boot=%08lx phase=%s entered_ms=%lu entered_epoch=%lld "
                      "checkpoint_ms=%lu checkpoint_epoch=%lld\r\n",
                      (unsigned long)state.previous_boot_id, state.previous.phase,
                      (unsigned long)state.previous.phase_started_ms,
                      (long long)state.previous.phase_started_epoch,
                      (unsigned long)state.previous.checkpoint_ms,
                      (long long)state.previous.checkpoint_epoch);
    } else {
        Serial.println("Previous run: RTC breadcrumb unavailable (power loss or first boot)");
    }
    ++state.boot_sequence;
    if (!state.boot_sequence) state.boot_sequence = 1;
    state.boot_id = esp_random();
    retained = {};
    retained.magic = RETAINED_MAGIC;
    retained.boot_id = state.boot_id;
    retained.boot_sequence = state.boot_sequence;
    retained.watchdog_self_test_done = self_test_done;
    diagnostics_phase("setup");

    // IDF 4.4 explicitly supports updating the initialized TWDT this way. Do
    // not deinit it: that would discard protection of existing idle tasks.
    // Arduino's default loopTaskWDTEnabled is false. Subscribe the actual
    // loop task here and feed explicitly only at progress checkpoints.
    esp_err_t err = esp_task_wdt_init(LOOP_WATCHDOG_SECONDS, true);
    if (err == ESP_OK && esp_task_wdt_status(nullptr) != ESP_OK) {
        err = esp_task_wdt_add(nullptr);
    }
    state.watchdog_active = err == ESP_OK && esp_task_wdt_status(nullptr) == ESP_OK;
    Serial.printf("Build: %s | boot=%08lx sequence=%lu\r\n", diagnostics_build(),
                  (unsigned long)state.boot_id, (unsigned long)state.boot_sequence);
    Serial.printf("Loop watchdog: %s timeout=%lus panic=on result=%s\r\n",
                  state.watchdog_active ? "armed" : "FAILED",
                  (unsigned long)LOOP_WATCHDOG_SECONDS, esp_err_to_name(err));
    diagnostics_checkpoint();
}

void diagnostics_phase(const char *phase) {
    if (!phase || strcmp(retained.breadcrumb.phase, phase) == 0) return;
    strlcpy(retained.breadcrumb.phase, phase, sizeof(retained.breadcrumb.phase));
    retained.breadcrumb.phase_started_ms = millis();
    retained.breadcrumb.phase_started_epoch = epoch_now();
}

void diagnostics_checkpoint() {
    retained.breadcrumb.checkpoint_ms = millis();
    retained.breadcrumb.checkpoint_epoch = epoch_now();
    if (state.watchdog_active) {
        esp_err_t err = esp_task_wdt_reset();
        if (err != ESP_OK) {
            state.watchdog_active = false;
            Serial.printf("Loop watchdog feed FAILED: %s\r\n", esp_err_to_name(err));
        }
    }
}

DiagnosticsPhase::DiagnosticsPhase(const char *phase) {
    strlcpy(previous_, retained.breadcrumb.phase, sizeof(previous_));
    diagnostics_phase(phase);
}
DiagnosticsPhase::~DiagnosticsPhase() { diagnostics_phase(previous_); }

void diagnostics_update() {
#if defined(DIAGNOSTICS_WATCHDOG_SELF_TEST) && DIAGNOSTICS_WATCHDOG_SELF_TEST
    // Temporary bench build only. Yielding reproduces the old HTTP hang:
    // idle tasks remain healthy while the subscribed loop task stops feeding.
    // RTC one-shot lets the next boot report the failure and run normally.
    if (state.watchdog_active && !retained.watchdog_self_test_done && millis() >= 15000) {
        retained.watchdog_self_test_done = true;
        diagnostics_phase("watchdog-self-test");
        diagnostics_checkpoint();
        Serial.println("WATCHDOG SELF-TEST: loop stalled deliberately; expect reboot in ~60s");
        Serial.flush();
        while (true) delay(1000);
    }
#endif
    const uint32_t now = millis();
    if (now - last_health_ms < 60000) return;
    last_health_ms = now;
    Serial.printf("Health: boot=%08lx up_s=%lu epoch=%lld phase=%s heap=%u min_heap=%u "
                  "psram=%u loop_stack_free=%u wifi=%d rssi=%d watchdog=%s\r\n",
                  (unsigned long)state.boot_id, (unsigned long)(now / 1000),
                  (long long)epoch_now(), retained.breadcrumb.phase,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getFreePsram(), (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                  (int)WiFi.status(), (int)WiFi.RSSI(), state.watchdog_active ? "armed" : "OFF");
}
