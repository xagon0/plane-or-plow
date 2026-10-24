#include "network.h"
#include "config.h"
#include "ambient.h"
#include "status.h"
#include "feed_http.h"
#include "diagnostics.h"
#include "contact_freshness.h"
#include "poll_schedule.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Haversine distance (km)
// ---------------------------------------------------------------------------
static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0; // Earth radius in km
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dLon / 2.0) * sin(dLon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

// ---------------------------------------------------------------------------
// WiFi management
// ---------------------------------------------------------------------------
static bool wifi_connected = false;
static unsigned long last_reconnect_ms = 0;
#define RECONNECT_INTERVAL_MS 15000  // Wait 15s between reconnect attempts

static void wifi_ensure() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifi_connected) {
            wifi_connected = true;
            Serial.printf("WiFi connected: %s\r\n", WiFi.localIP().toString().c_str());
        }
        return;
    }

    wifi_connected = false;

    // Don't spam reconnect — wait between attempts
    unsigned long now = millis();
    if (now - last_reconnect_ms < RECONNECT_INTERVAL_MS) return;
    last_reconnect_ms = now;

    stats.wifi_reconnects++;
    Serial.printf("WiFi reconnecting (status=%d)...\r\n", WiFi.status());
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ---------------------------------------------------------------------------
// Boost polling state (declared early so poll functions can cancel on 429)
// ---------------------------------------------------------------------------
static lv_timer_t *poll_timer = NULL;
static unsigned long boost_deadline = 0;
static PollSchedule poll_schedule;

static void cancel_boost() {
    if (!boost_deadline) return;
    boost_deadline = 0;
    lv_timer_set_period(poll_timer, API_POLL_MS);
}

static void contact_request_finished() {
    // Start the next interval when our request finishes. If a slow request
    // outlives the timer, retrying our own slot every 250ms could beat a waiting
    // radar request to the slot after every completion and starve it forever.
    poll_schedule.completion(boost_deadline ? BOOST_POLL_MS : API_POLL_MS);
    if (poll_timer) {
        lv_timer_set_period(poll_timer, poll_schedule.delay_ms);
        lv_timer_reset(poll_timer);
    }
}

// ---------------------------------------------------------------------------
// Tracked vehicle helpers
// ---------------------------------------------------------------------------
#define AGE_OUT_POLLS 5  // remove after 5 missed polls

// Temp struct for incoming poll results before merging into TrackedVehicle[]
struct IncomingVehicle {
    char  id[10];
    char  label[10];
    float lat;
    float lon;
    float heading_deg;   // <0 = unknown
    float speed_kts;     // 0 = unknown
    int32_t alt_ft;      // <0 = unknown / on ground
    float dist_km;
};

// `seed_trail` is true for vehicles the renderer does not interpolate (plows):
// their trail advances one point per fix. Aircraft sample their own trail from
// the dead-reckoned position instead, so pushing here would double up.
static void update_tracked_vehicles(TrackedVehicle *tracked, int &count,
                                     const IncomingVehicle *incoming, int n_incoming,
                                     bool seed_trail) {
    // 1. Increment age of all existing tracked vehicles
    for (int i = 0; i < count; i++) {
        tracked[i].age++;
    }

    // 2. Match incoming vehicles by ID
    for (int j = 0; j < n_incoming; j++) {
        const IncomingVehicle &inc = incoming[j];
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(tracked[i].id, inc.id) == 0) {
                found = i;
                break;
            }
        }

        bool is_new = false;
        if (found < 0) {
            if (count >= MAX_VEHICLES) continue;
            // New contact — seed the trail with its first fix.
            TrackedVehicle &tv = tracked[count];
            memset(&tv, 0, sizeof(TrackedVehicle));
            strncpy(tv.id, inc.id, sizeof(tv.id) - 1);
            trail_push(tv, inc.lat, inc.lon);
            found = count;
            count++;
            is_new = true;
        }

        TrackedVehicle &tv = tracked[found];
        strncpy(tv.label, inc.label, sizeof(tv.label) - 1);
        tv.label[sizeof(tv.label) - 1] = '\0';
        tv.lat         = inc.lat;
        tv.lon         = inc.lon;
        tv.heading_deg = inc.heading_deg;
        tv.speed_kts   = inc.speed_kts;
        tv.alt_ft      = inc.alt_ft;
        tv.dist_km     = inc.dist_km;
        tv.fix_ms      = millis();
        tv.age         = 0;

        if (seed_trail && !is_new) trail_push(tv, inc.lat, inc.lon);
    }

    // 3. Age-out vehicles not seen for AGE_OUT_POLLS — compact in-place
    int write = 0;
    for (int i = 0; i < count; i++) {
        if (tracked[i].age < AGE_OUT_POLLS) {
            if (write != i) tracked[write] = tracked[i];
            write++;
        }
    }
    count = write;
}

// Callsigns come back padded ("WJA231  "); trim so the readout sits tight.
static void copy_trimmed(char *dst, size_t n, const char *src) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o < n - 1; i++) {
        if (src[i] == ' ' && o == 0) continue;   // leading
        dst[o++] = src[i];
    }
    while (o > 0 && dst[o - 1] == ' ') o--;      // trailing
    dst[o] = '\0';
}

// ---------------------------------------------------------------------------
// Airplane polling
// ---------------------------------------------------------------------------
// Log every dropped contact for the first few minutes after boot, so a scope
// that looks empty can be explained rather than guessed at.
static bool wx_verbose = true;

static void handle_airplanes(const FeedHttpResult &result) {
    contact_request_finished();
    DiagnosticsPhase phase("aircraft_parse");
    if (wx_verbose && millis() > 300000UL) wx_verbose = false;

    int code = result.code;
    if (code == 429) {
        Serial.println("Airplane API HTTP 429 — cancelling boost");
        stats.air_fail++;
        cancel_boost();
        return;
    }
    if (code != 200) {
        Serial.printf("Airplane API HTTP %d\r\n", code);
        stats.air_fail++;
        return;
    }

    if (!result.body_ok) { stats.air_fail++; return; }

    JsonDocument filter;
    JsonObject fields = filter["ac"][0].to<JsonObject>();
    for (const char *key : {"lat", "lon", "hex", "category", "alt_baro",
                            "flight", "track", "gs"}) fields[key] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, result.data,
        result.size, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("Airplane JSON error: %s\r\n", err.c_str());
        stats.air_fail++;
        return;
    }
    if (!doc["ac"].is<JsonArray>()) { stats.air_fail++; return; }

    JsonArray ac = doc["ac"];

    IncomingVehicle incoming[MAX_VEHICLES];
    int n_incoming = 0;
    int n_surface = 0, n_ground = 0;

    for (JsonObject a : ac) {
        if (n_incoming >= MAX_VEHICLES) break;
        if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
        if (!a["hex"].is<const char *>()) continue;

        const char *cat = a["category"].is<const char *>()
                        ? a["category"].as<const char *>() : NULL;
        bool on_ground = a["alt_baro"].is<const char *>() &&
                         strcmp(a["alt_baro"].as<const char *>(), "ground") == 0;

#if FILTER_SURFACE_VEHICLES
        // C1/C2 are surface vehicles, C3-C5 obstacles. C0 is "no category
        // information" and real aircraft do report it, so it must NOT be swept
        // up here — doing so empties the scope.
        if (cat && (cat[0] == 'C' || cat[0] == 'c') &&
            cat[1] >= '1' && cat[1] <= '7') {
            n_surface++;
            if (wx_verbose) Serial.printf("  drop %s cat=%s (surface)\r\n",
                                          a["hex"].as<const char *>(), cat);
            continue;
        }
#endif
#if FILTER_ON_GROUND
        if (on_ground) {
            n_ground++;
            if (wx_verbose) Serial.printf("  drop %s cat=%s (on ground)\r\n",
                                          a["hex"].as<const char *>(),
                                          cat ? cat : "-");
            continue;
        }
#endif

        float lat = a["lat"];
        float lon = a["lon"];
        float dist = (float)haversine_km(HOME_LAT, HOME_LON, lat, lon);
        if (dist > radius_km) continue;

        IncomingVehicle &iv = incoming[n_incoming];
        memset(&iv, 0, sizeof(iv));
        strncpy(iv.id, a["hex"].as<const char *>(), sizeof(iv.id) - 1);
        iv.id[sizeof(iv.id) - 1] = '\0';

        if (a["flight"].is<const char *>()) {
            copy_trimmed(iv.label, sizeof(iv.label), a["flight"].as<const char *>());
        }

        iv.lat = lat;
        iv.lon = lon;
        iv.dist_km = dist;
        // `track` is the true ground track; without it the renderer falls back
        // to inferring heading from the trail.
        iv.heading_deg = a["track"].is<float>() ? (float)a["track"] : -1.0f;
        iv.speed_kts   = a["gs"].is<float>()    ? (float)a["gs"]    :  0.0f;
        // alt_baro is the string "ground" for taxiing aircraft.
        iv.alt_ft      = a["alt_baro"].is<int>() ? (int32_t)a["alt_baro"] : -1;

        n_incoming++;
    }

    update_tracked_vehicles(proximity.aircraft, proximity.aircraft_count,
                            incoming, n_incoming, false);

    stats.air_ok++;
    stats.air_last_ok_ms = millis();
    stats.air_filtered = n_surface + n_ground;
    Serial.printf("Airplanes: %d in API, %d surface, %d on-ground, "
                  "%d in range, %d tracked\r\n",
                  (int)ac.size(), n_surface, n_ground,
                  n_incoming, proximity.aircraft_count);
    proximity.airplane_nearby = proximity.aircraft_count > 0;
}

static bool poll_airplanes() {
    FeedHttpRequest request;
    request.label = "aircraft_fetch";
    request.http10 = false; // adsb.lol requires HTTP/1.1
    request.max_bytes = 256 * 1024;
    int radius_nm = (int)ceilf(radius_km / 1.852f);
    snprintf(request.url, sizeof(request.url), "%s/%.4f/%.4f/%d",
             AIRPLANE_API_HOST, (double)HOME_LAT, (double)HOME_LON, radius_nm);
    return feed_http_submit(request, handle_airplanes);
}

// ---------------------------------------------------------------------------
// Snowplow polling
// ---------------------------------------------------------------------------
static void handle_snowplows(const FeedHttpResult &result) {
    contact_request_finished();
    DiagnosticsPhase phase("plow_parse");
    int code = result.code;
    if (code == 429) {
        Serial.println("Plow API HTTP 429 — cancelling boost");
        stats.plow_fail++;
        cancel_boost();
        return;
    }
    if (code != 200) {
        Serial.printf("Plow API HTTP %d\r\n", code);
        stats.plow_fail++;
        return;
    }

    if (!result.body_ok) { stats.plow_fail++; return; }

    JsonDocument filter;
    filter["item2"][0]["itemId"] = true;
    filter["item2"][0]["location"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, result.data,
        result.size, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("Plow JSON error: %s\r\n", err.c_str());
        stats.plow_fail++;
        return;
    }
    if (!doc["item2"].is<JsonArray>()) { stats.plow_fail++; return; }

    JsonArray vehicles = doc["item2"];
    int total = vehicles.size();

    IncomingVehicle incoming[MAX_VEHICLES];
    int n_incoming = 0;

    for (JsonObject v : vehicles) {
        if (n_incoming >= MAX_VEHICLES) break;

        JsonArray loc = v["location"];
        if (loc.size() < 2) continue;

        float lat = loc[0];
        float lon = loc[1];
        float dist = (float)haversine_km(HOME_LAT, HOME_LON, lat, lon);
        if (dist > radius_km) continue;

        IncomingVehicle &iv = incoming[n_incoming];
        memset(&iv, 0, sizeof(iv));
        // itemId is an integer — convert to string
        snprintf(iv.id, sizeof(iv.id), "%d", v["itemId"].as<int>());
        iv.lat = lat;
        iv.lon = lon;
        iv.dist_km = dist;
        // The 511 feed carries no course or speed, so plows are never
        // extrapolated; the renderer infers a heading from their trail.
        iv.heading_deg = -1.0f;
        iv.speed_kts   = 0.0f;
        iv.alt_ft      = -1;

        n_incoming++;
    }

    update_tracked_vehicles(proximity.plows, proximity.plow_count,
                            incoming, n_incoming, true);

    stats.plow_ok++;
    stats.plow_last_ok_ms = millis();
    Serial.printf("Snowplows: %d total, %d in range, %d tracked\r\n",
                  total, n_incoming, proximity.plow_count);
    proximity.snowplow_nearby = proximity.plow_count > 0;
}

static bool poll_snowplows() {
    FeedHttpRequest request;
    request.label = "plow_fetch";
    request.xhr = true;
    request.max_bytes = 1024 * 1024;
    snprintf(request.url, sizeof(request.url), "%s", PLOW_API_URL);
    return feed_http_submit(request, handle_snowplows);
}

// ---------------------------------------------------------------------------
// Poll timer.
//
// Aircraft are polled three ticks out of four: they are the fast-moving thing
// on screen, and the shorter the gap between fixes the less the renderer has
// to extrapolate. Plows move at plow speed and are happy with every 4th tick.
// ---------------------------------------------------------------------------
static void poll_tick_cb(lv_timer_t *timer) {
    // Check if boost period has expired
    if (boost_deadline && millis() > boost_deadline) {
        boost_deadline = 0;
        lv_timer_set_period(poll_timer, API_POLL_MS);
        Serial.println("Boost polling: expired");
    }

    // One shared transport slot avoids simultaneous TLS allocations. Do not
    // disconnect/reconfigure WiFi while a weather/contact request owns it.
    const uint32_t regular_period = boost_deadline ? BOOST_POLL_MS : API_POLL_MS;
    if (poll_schedule.request_pending) {
        lv_timer_set_period(timer, regular_period);
        return;
    }
    if (feed_http_busy()) {
        poll_schedule.submission(false, regular_period);
        lv_timer_set_period(timer, poll_schedule.delay_ms);
        return;
    }
    wifi_ensure();
    if (!wifi_connected) {
        proximity.airplane_nearby = false;
        proximity.snowplow_nearby = false;
        proximity.aircraft_count = 0;
        proximity.plow_count = 0;
        lv_timer_set_period(timer, regular_period);
        return;
    }

    bool accepted = poll_schedule.next_is_plow() ? poll_snowplows() : poll_airplanes();
    // Busy/rejected submissions do not consume an aircraft/plow turn.
    poll_schedule.submission(accepted, regular_period);
    lv_timer_set_period(timer, poll_schedule.delay_ms);
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------
void network_tick() {
    // Failed polls never called update_tracked_vehicles(), so its poll-age
    // counter could keep old contacts visible for hours. Expire by elapsed
    // time even when every request fails or the connection stays offline.
    const uint32_t now = millis();
    expire_contacts(proximity.aircraft, proximity.aircraft_count, now, 180000UL);
    expire_contacts(proximity.plows, proximity.plow_count, now, 600000UL);
    proximity.airplane_nearby = proximity.aircraft_count > 0;
    proximity.snowplow_nearby = proximity.plow_count > 0;
}

void network_init() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    delay(100);

    Serial.printf("WiFi connecting to %s...\r\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Block up to 15s for initial connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.printf("WiFi connected: %s (RSSI %d)\r\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.printf("WiFi not connected (status=%d), will retry in background\r\n",
                      WiFi.status());
    }

    poll_timer = lv_timer_create(poll_tick_cb, API_POLL_MS, NULL);

    Serial.println("Network initialized");
}

// ---------------------------------------------------------------------------
// Boost polling — faster ticks for a few minutes after a tap
// ---------------------------------------------------------------------------
void network_boost_polling() {
    if (!poll_timer) return;
    bool already_boosting = (boost_deadline != 0);
    boost_deadline = millis() + BOOST_DURATION_MS;
    lv_timer_set_period(poll_timer, BOOST_POLL_MS);
    if (!already_boosting) {
        lv_timer_ready(poll_timer);  // immediate poll only on first tap
        Serial.println("Boost polling: ON");
    }
}
