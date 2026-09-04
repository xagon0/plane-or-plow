#include "status.h"
#include "config.h"
#include "ambient.h"
#include "weather.h"
#include "backlight.h"
#include "timekeep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

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

    static char b[1800];
    int n = snprintf(b, sizeof(b),
      "{\n"
      "  \"name\": \"Plane or Plow\",\n"
      "  \"uptime_s\": %lu,\n"
      "  \"uptime\": \"%luh %lum\",\n"
      "  \"reset_reason\": \"%s\",\n"
      "  \"clock\": { \"valid\": %s, \"local\": \"%s\", \"tz\": \"%s\",\n"
      "              \"synced_s_ago\": %lu, \"schedule\": \"%02d:%02d-%02d:%02d\" },\n"
      "  \"heap\": {\n"
      "    \"free\": %u,\n"
      "    \"min_free\": %u,\n"
      "    \"at_boot\": %u,\n"
      "    \"settled\": %u,\n"
      "    \"drift_since_settled\": %ld\n"
      "  },\n"
      "  \"psram_free\": %u,\n"
      "  \"loop_stack_free\": %u,\n"
      "  \"wifi\": { \"ip\": \"%s\", \"rssi\": %d, \"reconnects\": %lu },\n"
      "  \"contacts\": {\n"
      "    \"aircraft\": %d, \"plows\": %d,\n"
      "    \"nearest_kind\": \"%s\", \"nearest_label\": \"%s\", \"nearest_km\": %.1f\n"
      "  },\n"
      "  \"radar\": {\n"
      "    \"valid\": %s, \"fetches\": %lu, \"failures\": %lu,\n"
      "    \"last_ms\": %lu, \"last_bytes\": %lu,\n"
      "    \"inbound\": %s, \"inbound_km\": %.0f, \"inbound_bearing\": %.0f\n"
      "  },\n"
      "  \"polls\": {\n"
      "    \"aircraft_ok\": %lu, \"aircraft_fail\": %lu,\n"
      "    \"aircraft_filtered_last\": %lu,\n"
      "    \"plow_ok\": %lu, \"plow_fail\": %lu\n"
      "  },\n"
      "  \"backlight\": { \"duty\": %u, \"percent\": %u, \"manual\": %s,\n"
      "                  \"daytime\": %s,\n"
      "                  \"transitions\": %lu, \"last_transition\": \"%s\" }\n"
      "}\n",
      (unsigned long)up, (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
      stats.reset_reason,
      time_valid() ? "true" : "false", clock_str, HOME_TZ,
      (unsigned long)time_since_sync_s(),
      SCHEDULE_ON_HOUR, SCHEDULE_ON_MIN, SCHEDULE_OFF_HOUR, SCHEDULE_OFF_MIN,
      (unsigned)free_heap,
      (unsigned)ESP.getMinFreeHeap(),
      (unsigned)stats.heap_at_boot,
      (unsigned)stats.heap_settled,
      stats.heap_settled ? (long)free_heap - (long)stats.heap_settled : 0L,
      (unsigned)ESP.getFreePsram(),
      (unsigned)uxTaskGetStackHighWaterMark(NULL),
      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
      (unsigned long)stats.wifi_reconnects,
      proximity.aircraft_count, proximity.plow_count,
      near_kind, near_label, near_km,
      weather_valid ? "true" : "false",
      (unsigned long)stats.radar_fetches, (unsigned long)stats.radar_failures,
      (unsigned long)stats.radar_last_ms, (unsigned long)stats.radar_last_bytes,
      weather_inbound ? "true" : "false",
      weather_inbound ? weather_inbound_km : 0.0f,
      weather_inbound ? weather_inbound_bearing : 0.0f,
      (unsigned long)stats.air_ok, (unsigned long)stats.air_fail,
      (unsigned long)stats.air_filtered,
      (unsigned long)stats.plow_ok, (unsigned long)stats.plow_fail,
      (unsigned)backlight_duty(), (unsigned)((backlight_duty() * 100) / 255),
      backlight_manual() ? "true" : "false",
      backlight_is_daytime() ? "true" : "false",
      (unsigned long)stats.bl_transitions, stats.bl_last);

    if (n < 0 || n >= (int)sizeof(b)) {
        server.send(500, "text/plain", "status buffer overflow");
        return;
    }
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
