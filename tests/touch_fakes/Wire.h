#pragma once
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <deque>
#include <vector>

struct FakeWire {
    uint16_t timeout = 0;
    unsigned requests = 0, transmissions = 0;
    std::deque<std::vector<int>> responses;
    std::deque<uint8_t> transmit_results;
    std::vector<int> current;
    size_t index = 0;
    bool begin(int, int, uint32_t) { return true; }
    void setTimeOut(uint16_t value) { timeout = value; }
    void beginTransmission(uint8_t) {}
    size_t write(const uint8_t *, size_t size) { return size; }
    uint8_t endTransmission(bool) {
        ++transmissions;
        if (transmit_results.empty()) return 0;
        const uint8_t result = transmit_results.front();
        transmit_results.pop_front();
        return result;
    }
    size_t requestFrom(uint8_t, size_t, bool) {
        ++requests;
        assert(!responses.empty());
        current = responses.front();
        responses.pop_front();
        index = 0;
        return current.size();
    }
    int read() { return index < current.size() ? current[index++] : -1; }
};
extern FakeWire Wire;
