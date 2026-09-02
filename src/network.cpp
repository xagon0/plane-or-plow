#include "network.h"
#include "config.h"
#include "ambient.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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

static void cancel_boost() {
    if (!boost_deadline) return;
    boost_deadline = 0;
    lv_timer_set_period(poll_timer, API_POLL_MS);
}

// ---------------------------------------------------------------------------
// Tracked vehicle helpers
// ---------------------------------------------------------------------------
#define AGE_OUT_POLLS 5  // remove after 5 missed polls (~7.5 min normal)

// Temp struct for incoming poll results before merging into TrackedVehicle[]
struct IncomingVehicle {
    char id[12];
    float lat;
    float lon;
    float dist_km;
};

static void update_tracked_vehicles(TrackedVehicle *tracked, int &count,
                                     const IncomingVehicle *incoming, int n_incoming) {
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

        if (found >= 0) {
            // Existing vehicle — append position to ring buffer, reset age
            TrackedVehicle &tv = tracked[found];
            tv.dist_km = inc.dist_km;
            tv.age = 0;
            tv.trail_head = (tv.trail_head + 1) % MAX_TRAIL_PTS;
            tv.trail[tv.trail_head].lat = inc.lat;
            tv.trail[tv.trail_head].lon = inc.lon;
            if (tv.trail_count < MAX_TRAIL_PTS) tv.trail_count++;
        } else if (count < MAX_VEHICLES) {
            // New vehicle — create entry
            TrackedVehicle &tv = tracked[count];
            memset(&tv, 0, sizeof(TrackedVehicle));
            strncpy(tv.id, inc.id, sizeof(tv.id) - 1);
            tv.id[sizeof(tv.id) - 1] = '\0';
            tv.dist_km = inc.dist_km;
            tv.age = 0;
            tv.trail_head = 0;
            tv.trail[0].lat = inc.lat;
            tv.trail[0].lon = inc.lon;
            tv.trail_count = 1;
            count++;
        }
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

// ---------------------------------------------------------------------------
// Airplane polling
// ---------------------------------------------------------------------------
static bool poll_airplanes() {
    WiFiClientSecure client;
    client.setInsecure();

    // Build URL with radius in nautical miles (km / 1.852)
    int radius_nm = (int)ceilf(radius_km / 1.852f);
    char url[128];
    snprintf(url, sizeof(url), "%s%d", AIRPLANE_API_BASE, radius_nm);

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code = http.GET();
    if (code == 429) {
        Serial.println("Airplane API HTTP 429 — cancelling boost");
        cancel_boost();
        http.end();
        return false;
    }
    if (code != 200) {
        Serial.printf("Airplane API HTTP %d\r\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("Airplane JSON error: %s\r\n", err.c_str());
        return false;
    }

    JsonArray ac = doc["ac"];

    // Collect incoming vehicles into temp array
    IncomingVehicle incoming[MAX_VEHICLES];
    int n_incoming = 0;

    for (JsonObject a : ac) {
        if (n_incoming >= MAX_VEHICLES) break;
        if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
        if (!a["hex"].is<const char *>()) continue;

        float lat = a["lat"];
        float lon = a["lon"];
        float dist = (float)haversine_km(HOME_LAT, HOME_LON, lat, lon);

        if (dist <= radius_km) {
            IncomingVehicle &iv = incoming[n_incoming];
            strncpy(iv.id, a["hex"].as<const char *>(), sizeof(iv.id) - 1);
            iv.id[sizeof(iv.id) - 1] = '\0';
            iv.lat = lat;
            iv.lon = lon;
            iv.dist_km = dist;
            n_incoming++;
        }
    }

    update_tracked_vehicles(proximity.aircraft, proximity.aircraft_count,
                            incoming, n_incoming);

    Serial.printf("Airplanes: %d in API, %d incoming, %d tracked\r\n",
                  (int)ac.size(), n_incoming, proximity.aircraft_count);
    return proximity.aircraft_count > 0;
}

// ---------------------------------------------------------------------------
// Snowplow polling
// ---------------------------------------------------------------------------
static bool poll_snowplows() {
    WiFiClientSecure client;
    client.setInsecure(); // Skip cert verification

    HTTPClient http;
    http.begin(client, PLOW_API_URL);
    http.setTimeout(10000);
    http.useHTTP10(true);  // HTTP/1.0 prevents gzip and chunked encoding
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "ESP32");

    int code = http.GET();
    if (code == 429) {
        Serial.println("Plow API HTTP 429 — cancelling boost");
        cancel_boost();
        http.end();
        return false;
    }
    if (code != 200) {
        Serial.printf("Plow API HTTP %d\r\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    Serial.printf("Plow response: %d bytes, starts: %.80s\r\n", payload.length(), payload.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("Plow JSON error: %s\r\n", err.c_str());
        return false;
    }

    JsonArray vehicles = doc["item2"];
    int total = vehicles.size();

    // Collect incoming vehicles into temp array
    IncomingVehicle incoming[MAX_VEHICLES];
    int n_incoming = 0;

    for (JsonObject v : vehicles) {
        if (n_incoming >= MAX_VEHICLES) break;

        JsonArray loc = v["location"];
        if (loc.size() < 2) continue;

        float lat = loc[0];
        float lon = loc[1];
        float dist = (float)haversine_km(HOME_LAT, HOME_LON, lat, lon);

        if (dist <= radius_km) {
            IncomingVehicle &iv = incoming[n_incoming];
            // itemId is an integer — convert to string
            snprintf(iv.id, sizeof(iv.id), "%d", v["itemId"].as<int>());
            iv.lat = lat;
            iv.lon = lon;
            iv.dist_km = dist;
            n_incoming++;
        }
    }

    update_tracked_vehicles(proximity.plows, proximity.plow_count,
                            incoming, n_incoming);

    Serial.printf("Snowplows: %d total, %d incoming, %d tracked\r\n",
                  total, n_incoming, proximity.plow_count);
    return proximity.plow_count > 0;
}

// ---------------------------------------------------------------------------
// Poll timer — alternates airplane / snowplow each cycle
// ---------------------------------------------------------------------------
static uint32_t poll_cycle = 0;

static void poll_tick_cb(lv_timer_t *timer) {
    // Check if boost period has expired
    if (boost_deadline && millis() > boost_deadline) {
        boost_deadline = 0;
        lv_timer_set_period(poll_timer, API_POLL_MS);
        Serial.println("Boost polling: expired, back to 45s");
    }

    wifi_ensure();
    if (!wifi_connected) {
        // No WiFi → clear everything
        proximity.airplane_nearby = false;
        proximity.snowplow_nearby = false;
        proximity.aircraft_count = 0;
        proximity.plow_count = 0;
        return;
    }

    if (poll_cycle % 2 == 0) {
        proximity.airplane_nearby = poll_airplanes();
    } else {
        proximity.snowplow_nearby = poll_snowplows();
    }

    poll_cycle++;

    Serial.printf("State: airplane=%s, snowplow=%s\r\n",
                  proximity.airplane_nearby ? "YES" : "no",
                  proximity.snowplow_nearby ? "YES" : "no");
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------
void network_init() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    delay(100);

    // Quick scan to verify network is visible
    Serial.println("Scanning WiFi networks...");
    int n = WiFi.scanNetworks();
    Serial.printf("Found %d networks:\r\n", n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        Serial.printf("  [%d] %s (RSSI %d, ch %d)\r\n", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
        if (WiFi.SSID(i) == WIFI_SSID) found = true;
    }
    WiFi.scanDelete();

    if (!found) {
        Serial.printf("WARNING: SSID '%s' not found in scan!\r\n", WIFI_SSID);
    }

    Serial.printf("WiFi connecting to %s...\r\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Block up to 15s for initial connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.printf(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.printf("WiFi connected: %s (RSSI %d)\r\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.printf("WiFi not connected (status=%d), will retry in background\r\n", WiFi.status());
    }

    // Start poll timer
    poll_timer = lv_timer_create(poll_tick_cb, API_POLL_MS, NULL);

    Serial.println("Network initialized");
}

// ---------------------------------------------------------------------------
// Boost polling — 30s per source for 3 minutes after user interaction
// ---------------------------------------------------------------------------
void network_boost_polling() {
    if (!poll_timer) return;
    bool already_boosting = (boost_deadline != 0);
    boost_deadline = millis() + BOOST_DURATION_MS;
    lv_timer_set_period(poll_timer, BOOST_POLL_MS);
    if (!already_boosting) {
        lv_timer_ready(poll_timer);  // immediate poll only on first tap
        Serial.println("Boost polling: ON (15s ticks for 180s)");
    } else {
        Serial.println("Boost polling: extended");
    }
}
