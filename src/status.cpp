#include "status.h"
#include "config.h"
#include "ambient.h"
#include "weather.h"
#include "backlight.h"
#include "timekeep.h"
#include "diagnostics.h"
#include "touch_input.h"
#include "feed_http.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

SysStats stats;

static WebServer server(80);
static bool server_up = false;

// No fragmentation figure here, deliberately. heap_caps_get_largest_free_block()
// walks and poison-checks every block under a spinlock that disables
// interrupts, which the RGB panel's DMA cannot tolerate — it panicked this
// device with an interrupt watchdog timeout, from the request handler and again
// from loop(), restricting the walk to internal RAM did not help. A status
// endpoint that crashes the thing it reports on is worse than no endpoint.
//
// Everything below is a maintained counter, so polling this is free. Leak and
// fragmentation both show up in `free` and `min_free` trending down over hours,
// which is the question worth answering unattended.

// ---------------------------------------------------------------------------
void status_boot_report() {
    esp_reset_reason_t r = esp_reset_reason();
    switch (r) {
        case ESP_RST_POWERON:   stats.reset_reason = "power-on";           break;
        case ESP_RST_EXT:       stats.reset_reason = "external reset";     break;
        case ESP_RST_SW:        stats.reset_reason = "software restart";   break;
        case ESP_RST_PANIC:     stats.reset_reason = "PANIC / exception";  break;
        case ESP_RST_INT_WDT:   stats.reset_reason = "interrupt watchdog"; break;
        case ESP_RST_TASK_WDT:  stats.reset_reason = "task watchdog";      break;
        case ESP_RST_WDT:       stats.reset_reason = "other watchdog";     break;
        case ESP_RST_BROWNOUT:  stats.reset_reason = "BROWNOUT (power)";   break;
        case ESP_RST_DEEPSLEEP: stats.reset_reason = "deep sleep wake";    break;
        default:                stats.reset_reason = "unknown";           break;
    }
    stats.heap_at_boot = ESP.getFreeHeap();
    Serial.printf("Reset reason: %d (%s)   free heap %u\r\n",
                  (int)r, stats.reset_reason, (unsigned)stats.heap_at_boot);
    diagnostics_begin();
}

void status_mark_settled() {
    stats.heap_settled = ESP.getFreeHeap();
    Serial.printf("Status: heap baseline %u (boot was %u)\r\n",
                  (unsigned)stats.heap_settled, (unsigned)stats.heap_at_boot);
}

// ---------------------------------------------------------------------------
static void nearest_contact(float &km, const char **kind, const char **label) {
    const TrackedVehicle *best = NULL;
    *kind = "none"; *label = ""; km = -1.0f;

    for (int i = 0; i < proximity.aircraft_count; i++) {
        const TrackedVehicle &v = proximity.aircraft[i];
        if (!best || v.dist_km < best->dist_km) { best = &v; *kind = "aircraft"; }
    }
    for (int i = 0; i < proximity.plow_count; i++) {
        const TrackedVehicle &v = proximity.plows[i];
        if (!best || v.dist_km < best->dist_km) { best = &v; *kind = "plow"; }
    }
    if (best) {
        km = best->dist_km;
        *label = (best->label[0] != '\0') ? best->label : best->id;
    }
}

static void handle_status() {
    float near_km; const char *near_kind; const char *near_label;
    nearest_contact(near_km, &near_kind, &near_label);

    uint32_t up = millis() / 1000UL;
    char clock_str[24] = "unset";
    {
        struct tm t;
        if (time_local(&t)) {
            strftime(clock_str, sizeof(clock_str), "%Y-%m-%d %H:%M:%S", &t);
        }
    }
    uint32_t free_heap = ESP.getFreeHeap();

    // ArduinoJson escapes API-supplied labels, quotes, and control characters.
    // A fixed output buffer bounds the response without the extra String copy.
    JsonDocument doc;
    doc["name"] = "Plane or Plow";
    doc["uptime_s"] = up;
    char uptime[32];
    snprintf(uptime, sizeof(uptime), "%luh %lum", (unsigned long)(up / 3600),
             (unsigned long)((up % 3600) / 60));
    doc["uptime"] = uptime;
    doc["reset_reason"] = stats.reset_reason;

    const DiagnosticsStatus &diag = diagnostics_status();
    const DiagnosticBreadcrumb &current = diagnostics_current();
    JsonObject diagnostic = doc["diagnostics"].to<JsonObject>();
    diagnostic["build"] = diagnostics_build();
    char boot_id[9];
    snprintf(boot_id, sizeof(boot_id), "%08lx", (unsigned long)diag.boot_id);
    diagnostic["boot_id"] = boot_id;
    diagnostic["boot_sequence"] = diag.boot_sequence;
    diagnostic["watchdog_active"] = diag.watchdog_active;
    diagnostic["watchdog_timeout_s"] = LOOP_WATCHDOG_SECONDS;
    diagnostic["phase"] = current.phase;
    diagnostic["last_progress_age_ms"] = (uint32_t)(millis() - current.checkpoint_ms);
    diagnostic["previous_run"] = nullptr;
    if (diag.previous_valid) {
        JsonObject previous = diagnostic["previous_run"].to<JsonObject>();
        char previous_id[9];
        snprintf(previous_id, sizeof(previous_id), "%08lx", (unsigned long)diag.previous_boot_id);
        previous["boot_id"] = previous_id;
        previous["phase"] = diag.previous.phase;
        previous["phase_started_ms"] = diag.previous.phase_started_ms;
        previous["phase_started_epoch"] = diag.previous.phase_started_epoch;
        previous["last_progress_ms"] = diag.previous.checkpoint_ms;
        previous["last_progress_epoch"] = diag.previous.checkpoint_epoch;
    }
    diagnostic["breadcrumb_retention"] = "RTC RAM; lost on power-off";

    const FeedHttpStatus feed_status = feed_http_status();
    JsonObject feed = doc["feed_http"].to<JsonObject>();
    feed["ready"] = feed_status.ready;
    feed["busy"] = feed_status.busy;
    feed["label"] = feed_status.label;
    feed["age_ms"] = feed_status.age_ms;
    feed["completion_pending"] = feed_status.completion_pending;
    feed["watchdog_active"] = feed_status.watchdog_active;
    feed["completed"] = feed_status.completed;
    feed["last_elapsed_ms"] = feed_status.last_elapsed_ms;
    feed["worker_stack_free"] = feed_status.worker_stack_free;
    feed["worker_error"] = feed_status.worker_error;

    JsonObject render = doc["render"].to<JsonObject>();
    render["frames"] = ambient_frame_count();
    render["max_frame_gap_ms"] = ambient_max_frame_gap_ms();

    const TouchStats &touch_stats = touch_input_stats();
    JsonObject touch = doc["touch"].to<JsonObject>();
    touch["available"] = touch_stats.available;
    touch["address"] = touch_stats.address;
    touch["frames"] = touch_stats.frames;
    touch["errors"] = touch_stats.errors;
    touch["invalid_frames"] = touch_stats.invalid_frames;
    touch["recoveries"] = touch_stats.recoveries;

    JsonObject clock = doc["clock"].to<JsonObject>();
    clock["valid"] = time_valid();
    clock["local"] = clock_str;
    clock["tz"] = HOME_TZ;
    clock["synced_s_ago"] = time_since_sync_s();
    char schedule[16];
    snprintf(schedule, sizeof(schedule), "%02d:%02d-%02d:%02d",
             SCHEDULE_ON_HOUR, SCHEDULE_ON_MIN, SCHEDULE_OFF_HOUR, SCHEDULE_OFF_MIN);
    clock["schedule"] = schedule;

    JsonObject heap = doc["heap"].to<JsonObject>();
    heap["free"] = free_heap;
    heap["min_free"] = ESP.getMinFreeHeap();
    heap["at_boot"] = stats.heap_at_boot;
    heap["settled"] = stats.heap_settled;
    heap["drift_since_settled"] = stats.heap_settled ? (long)free_heap - (long)stats.heap_settled : 0L;
    doc["psram_free"] = ESP.getFreePsram();
    doc["loop_stack_free"] = uxTaskGetStackHighWaterMark(nullptr);

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ip"] = WiFi.localIP().toString();
    wifi["rssi"] = WiFi.RSSI();
    wifi["reconnects"] = stats.wifi_reconnects;

    JsonObject contacts = doc["contacts"].to<JsonObject>();
    contacts["aircraft"] = proximity.aircraft_count;
    contacts["plows"] = proximity.plow_count;
    contacts["nearest_kind"] = near_kind;
    contacts["nearest_label"] = near_label;
    contacts["nearest_km"] = near_km;

    JsonObject radar = doc["radar"].to<JsonObject>();
    radar["valid"] = weather_valid;
    radar["replaying"] = weather_replaying();
    radar["history_frames"] = weather_history_frames();
    radar["replay_frame"] = weather_replay_frame();
    radar["fetches"] = stats.radar_fetches;
    radar["failures"] = stats.radar_failures;
    radar["last_ms"] = stats.radar_last_ms;
    radar["last_bytes"] = stats.radar_last_bytes;
    radar["inbound"] = weather_inbound;
    radar["inbound_km"] = weather_inbound ? weather_inbound_km : 0.0f;
    radar["inbound_bearing"] = weather_inbound ? weather_inbound_bearing : 0.0f;

    JsonObject polls = doc["polls"].to<JsonObject>();
    polls["aircraft_ok"] = stats.air_ok;
    polls["aircraft_fail"] = stats.air_fail;
    polls["aircraft_filtered_last"] = stats.air_filtered;
    polls["plow_ok"] = stats.plow_ok;
    polls["plow_fail"] = stats.plow_fail;
    polls["aircraft_download_age_s"] = nullptr;
    polls["plow_download_age_s"] = nullptr;
    const uint32_t air_age = millis() - stats.air_last_ok_ms;
    const uint32_t plow_age = millis() - stats.plow_last_ok_ms;
    if (stats.air_ok) polls["aircraft_download_age_s"] = air_age / 1000;
    if (stats.plow_ok) polls["plow_download_age_s"] = plow_age / 1000;
    polls["aircraft_download_stale"] = !stats.air_ok || air_age >= 180000;
    polls["plow_download_stale"] = !stats.plow_ok || plow_age >= 600000;
    // 511 provides no per-position observation time. A fresh download alone
    // cannot establish when a plow last moved or transmitted its location.
    polls["plow_position_timestamp_available"] = false;

    JsonObject backlight = doc["backlight"].to<JsonObject>();
    backlight["duty"] = backlight_duty();
    backlight["percent"] = (backlight_duty() * 100) / 255;
    backlight["manual"] = backlight_manual();
    backlight["daytime"] = backlight_is_daytime();
    backlight["transitions"] = stats.bl_transitions;
    backlight["last_transition"] = stats.bl_last;

    static char b[4096];
    if (doc.overflowed() || measureJson(doc) >= sizeof(b)) {
        server.send(500, "text/plain", "status allocation or buffer overflow");
        return;
    }
    serializeJson(doc, b, sizeof(b));
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", b);
}

// ---------------------------------------------------------------------------
void status_init() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Status: WiFi down, server not started");
        return;
    }
    if (MDNS.begin("planeorplow")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("Status: http://planeorplow.local/status");
    }
    server.on("/status", handle_status);
    server.onNotFound([]() { server.send(404, "text/plain", "try /status\n"); });
    server.begin();
    server_up = true;
    Serial.printf("Status: http://%s/status\r\n", WiFi.localIP().toString().c_str());
}

void status_update() {
    if (!server_up) {
        // Start late if WiFi arrived after boot.
        if (WiFi.status() == WL_CONNECTED) status_init();
        return;
    }
    server.handleClient();
}
