#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using BaseType_t = int;
using TickType_t = uint32_t;
using esp_err_t = int;
constexpr int pdTRUE = 1, pdPASS = 1, ESP_OK = 0, ESP_FAIL = -1;
constexpr int ESP_ERR_NO_MEM = 0x101, ESP_ERR_NOT_FOUND = 0x105;
constexpr int HTTP_CODE_OK = 200, HTTPC_ERROR_NOT_CONNECTED = -4, HTTPC_ERROR_TOO_LESS_RAM = -8;
#define pdMS_TO_TICKS(value) (value)
struct TestTaskStop {};
struct FakeQueue {
    size_t capacity, item_size;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<uint8_t>> items;
};
struct FakeTask { std::thread thread; std::atomic<bool> stopped{false}; };
using TaskHandle_t = FakeTask *;
using QueueHandle_t = FakeQueue *;
extern thread_local FakeTask *current_fake_task;
struct FeedFakeRuntime {
    std::vector<FakeQueue *> queues;
    std::vector<FakeTask *> tasks;
    std::mutex watchdog_mutex, payload_mutex, http_mutex;
    std::set<FakeTask *> watched;
    std::set<void *> payloads;
    std::condition_variable http_changed;
    std::atomic<bool> fail_queue{false}, fail_task{false}, fail_watchdog{false}, fail_send{false};
    std::atomic<bool> block_http{true}, entered_http{false}, body_ok{true};
    std::atomic<int> code{200}, http_clients{0}, tls_clients{0}, watchdog_feeds{0}, feeds_during_http{0};
    std::string received_url;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};
extern FeedFakeRuntime feed_fake;

inline uint32_t millis() {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - feed_fake.started).count());
}
inline void vTaskDelay(TickType_t ticks) {
    if (current_fake_task && current_fake_task->stopped) throw TestTaskStop{};
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
    if (current_fake_task && current_fake_task->stopped) throw TestTaskStop{};
}
inline QueueHandle_t xQueueCreate(unsigned capacity, size_t item_size) {
    if (feed_fake.fail_queue.exchange(false)) return nullptr;
    auto *queue = new FakeQueue;
    queue->capacity = capacity; queue->item_size = item_size;
    feed_fake.queues.push_back(queue);
    return queue;
}
inline void vQueueDelete(QueueHandle_t queue) {
    auto &queues = feed_fake.queues;
    queues.erase(std::remove(queues.begin(), queues.end(), queue), queues.end());
    delete queue;
}
inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t) {
    if (feed_fake.fail_send.exchange(false)) return 0;
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.size() == queue->capacity) return 0;
    const auto *bytes = static_cast<const uint8_t *>(item);
    queue->items.emplace_back(bytes, bytes + queue->item_size);
    queue->changed.notify_all();
    return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->changed.wait_for(lock, std::chrono::milliseconds(ticks), [&]() {
        return !queue->items.empty() || (current_fake_task && current_fake_task->stopped);
    });
    if (current_fake_task && current_fake_task->stopped) throw TestTaskStop{};
    if (queue->items.empty()) return 0;
    std::memcpy(item, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdTRUE;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t queue) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    return queue->items.size();
}
inline BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *,
        uint32_t stack, void *argument, unsigned priority, TaskHandle_t *handle, int core) {
    assert(stack == 16384 && priority == 1 && core == 0);
    if (feed_fake.fail_task.exchange(false)) return 0;
    auto *task = new FakeTask;
    feed_fake.tasks.push_back(task);
    *handle = task;
    task->thread = std::thread([=]() {
        current_fake_task = task;
        try { entry(argument); } catch (const TestTaskStop &) {}
    });
    return pdPASS;
}
inline void vTaskDelete(TaskHandle_t task) {
    task->stopped = true;
    for (auto *queue : feed_fake.queues) queue->changed.notify_all();
    task->thread.join();
    auto &tasks = feed_fake.tasks;
    tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end());
    delete task;
}
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 8192; }
inline esp_err_t esp_task_wdt_status(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(feed_fake.watchdog_mutex);
    return feed_fake.watched.count(task) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
inline esp_err_t esp_task_wdt_add(TaskHandle_t task) {
    if (feed_fake.fail_watchdog.exchange(false)) return ESP_FAIL;
    std::lock_guard<std::mutex> lock(feed_fake.watchdog_mutex);
    feed_fake.watched.insert(task);
    return ESP_OK;
}
inline esp_err_t esp_task_wdt_delete(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(feed_fake.watchdog_mutex);
    feed_fake.watched.erase(task);
    return ESP_OK;
}
inline esp_err_t esp_task_wdt_reset() {
    assert(esp_task_wdt_status(current_fake_task) == ESP_OK);
    ++feed_fake.watchdog_feeds;
    if (feed_fake.entered_http && feed_fake.block_http) ++feed_fake.feeds_during_http;
    return ESP_OK;
}
inline const char *esp_err_to_name(int) { return "mock error"; }
struct FeedFakeSerial { void printf(const char *, ...) {} void println(const char *) {} };
extern FeedFakeSerial Serial;
struct WiFiClientSecure {
    WiFiClientSecure() { ++feed_fake.tls_clients; }
    ~WiFiClientSecure() { --feed_fake.tls_clients; }
    void setInsecure() {}
};
struct HTTPClient {
    HTTPClient() { ++feed_fake.http_clients; }
    ~HTTPClient() { --feed_fake.http_clients; }
    bool begin(WiFiClientSecure &, const char *url) { feed_fake.received_url = url; return true; }
    void setUserAgent(const char *) {}
    void addHeader(const char *, const char *) {}
    void end() {}
    int GET() {
        std::unique_lock<std::mutex> lock(feed_fake.http_mutex);
        feed_fake.entered_http = true;
        feed_fake.http_changed.notify_all();
        feed_fake.http_changed.wait(lock, []() { return !feed_fake.block_http; });
        return feed_fake.code;
    }
};
inline void feed_test_free(void *pointer) {
    if (!pointer) return;
    std::lock_guard<std::mutex> lock(feed_fake.payload_mutex);
    assert(feed_fake.payloads.erase(pointer) == 1); // detects double-free/foreign ownership
    std::free(pointer);
}
struct HttpBodyBuffer {
    char *data = nullptr;
    size_t size = 0;
    ~HttpBodyBuffer() { feed_test_free(data); }
};
inline void configure_http(HTTPClient &, WiFiClientSecure &, bool) {}
inline bool read_http_body(HTTPClient &, HttpBodyBuffer &body, size_t, uint32_t) {
    body.data = static_cast<char *>(std::malloc(4));
    std::memcpy(body.data, "abc", 4); body.size = 3;
    std::lock_guard<std::mutex> lock(feed_fake.payload_mutex);
    feed_fake.payloads.insert(body.data);
    return feed_fake.body_ok;
}

// Compile the production worker unchanged, replacing only platform/transport
// dependencies. No ARDUINO macro means http_body.h's real Arduino adapter is
// excluded; bounded transfer behavior has its own separate regression tests.
#define CONFIG_H
#define HTTP_USER_AGENT "host-regression-test"
#define free feed_test_free
