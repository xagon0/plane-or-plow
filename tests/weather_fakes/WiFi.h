#pragma once
constexpr int WL_CONNECTED = 3;
struct WeatherFakeWiFi {
    int connection = WL_CONNECTED;
    int status() const { return connection; }
};
extern WeatherFakeWiFi WiFi;
