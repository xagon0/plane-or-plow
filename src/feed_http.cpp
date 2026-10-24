#include "feed_http.h"
#include "http_body.h"
#include "config.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <stdlib.h>
#include <string.h>

namespace {
struct Job {
    FeedHttpRequest request;
    FeedHttpCallback callback;
    char label[48];
};
struct Completion {
    FeedHttpResult result;
    FeedHttpCallback callback;
};

QueueHandle_t requests = nullptr, completions = nullptr;
TaskHandle_t worker = nullptr;
std::atomic<bool> ready{false};
std::atomic<bool> watchdog_active{false};
std::atomic<bool> completion_lost{false};
std::atomic<int> worker_error{ESP_OK};
std::atomic<uint32_t> worker_stack_free{0};

// These fields are exclusively owned by the main task. In particular busy
// stays set while the completion callback parses its response, so TLS and a
// second payload can never allocate concurrently with that parser.
bool busy = false;
char active_label[48] = "idle";
uint32_t submitted_ms = 0, completed = 0, last_elapsed_ms = 0;
FeedHttpCallback active_callback = nullptr;

bool feed_worker_watchdog() {
    const esp_err_t error = esp_task_wdt_reset();
    if (error == ESP_OK) return true;
    worker_error.store(error);
    watchdog_active.store(false);
    ready.store(false);
    Serial.printf("Feed worker: watchdog feed failed: %s\r\n", esp_err_to_name(error));
    return false;
}

Completion perform_request(const Job &job) {
    Completion completion;
    completion.callback = job.callback;
    const uint32_t started = millis();
    // Destruction occurs before this function returns, so the TLS client and
    // HTTP connection are gone before the main task can receive the buffer.
    WiFiClientSecure client;
    client.setInsecure(); // preserve the existing feed TLS policy
    HTTPClient http;
    HttpBodyBuffer body;
    completion.result.code = HTTPC_ERROR_NOT_CONNECTED;
    if (http.begin(client, job.request.url)) {
        configure_http(http, client, job.request.http10);
        http.setUserAgent(HTTP_USER_AGENT);
        if (job.request.xhr) {
            http.addHeader("X-Requested-With", "XMLHttpRequest");
            http.addHeader("Accept", "application/json");
        }
        completion.result.code = http.GET();
        if (completion.result.code == HTTP_CODE_OK) {
            completion.result.body_ok = read_http_body(http, body,
                job.request.max_bytes, job.request.body_timeout_ms);
        }
    }
    http.end();
    completion.result.data = body.data;
    completion.result.size = body.size;
    completion.result.elapsed_ms = millis() - started;
    body.data = nullptr; // ownership moves through the completion queue
    return completion;
}

void worker_loop(void *) {
    // begin() registers this task with the already configured TWDT by handle.
    // Keep it parked until registration succeeds; failure can safely delete
    // this task before any queue, TLS, or payload ownership has begun.
    while (!ready.load()) vTaskDelay(1);
    for (;;) {
        if (!ready.load()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!feed_worker_watchdog()) continue;
        Job job;
        if (xQueueReceive(requests, &job, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        // No feeds during a request. A stuck yielding TLS/HTTP operation is
        // watched independently of the still-responsive display task.
        Completion completion = perform_request(job);
        worker_stack_free.store(uxTaskGetStackHighWaterMark(nullptr));
        if (xQueueSend(completions, &completion, 0) != pdTRUE) {
            free(const_cast<char *>(completion.result.data));
            worker_error.store(ESP_FAIL);
            ready.store(false);
            completion_lost.store(true);
            Serial.println("Feed worker: completion queue failed; worker disabled");
        }
    }
}

void cleanup_startup() {
    // Used only before ready=true: the worker is still in its startup gate.
    if (worker) {
        if (esp_task_wdt_status(worker) == ESP_OK) esp_task_wdt_delete(worker);
        vTaskDelete(worker);
        worker = nullptr;
    }
    if (requests) { vQueueDelete(requests); requests = nullptr; }
    if (completions) { vQueueDelete(completions); completions = nullptr; }
    watchdog_active.store(false);
    ready.store(false);
}

void finish_completion(const Completion &completion) {
    // A nested submit from inside the callback must still see busy=true.
    if (completion.callback) completion.callback(completion.result);
    free(const_cast<char *>(completion.result.data));
    ++completed;
    last_elapsed_ms = completion.result.elapsed_ms;
    active_callback = nullptr;
    strcpy(active_label, "idle");
    busy = false;
}
}

bool feed_http_begin() {
    if (worker) return ready.load();
    worker_error.store(ESP_OK);
    completion_lost.store(false);
    requests = xQueueCreate(1, sizeof(Job));
    completions = xQueueCreate(1, sizeof(Completion));
    if (!requests || !completions) {
        worker_error.store(ESP_ERR_NO_MEM);
        cleanup_startup();
        Serial.println("Feed worker: queue allocation failed");
        return false;
    }
    // ESP-IDF's stack argument is bytes, unlike vanilla FreeRTOS words.
    if (xTaskCreatePinnedToCore(worker_loop, "feed-http", 16384, nullptr, 1,
                                &worker, 0) != pdPASS) {
        worker_error.store(ESP_ERR_NO_MEM);
        cleanup_startup();
        Serial.println("Feed worker: task allocation failed");
        return false;
    }
    esp_err_t error = esp_task_wdt_status(worker);
    if (error != ESP_OK) error = esp_task_wdt_add(worker);
    if (error != ESP_OK) {
        worker_error.store(error);
        cleanup_startup();
        Serial.printf("Feed worker: watchdog registration failed: %s\r\n", esp_err_to_name(error));
        return false;
    }
    watchdog_active.store(true);
    ready.store(true);
    Serial.println("Feed worker: core 0, stack 16384, watchdog armed");
    return true;
}

bool feed_http_submit(const FeedHttpRequest &request, FeedHttpCallback callback) {
    if (!ready.load() || busy || !callback || request.max_bytes == 0 ||
        request.max_bytes > 1024 * 1024 || request.body_timeout_ms == 0 ||
        request.body_timeout_ms > 30000 || !request.url[0] ||
        !memchr(request.url, '\0', sizeof(request.url))) return false;
    Job job;
    job.request = request;
    job.callback = callback;
    strlcpy(job.label, request.label ? request.label : "feed", sizeof(job.label));
    job.request.label = nullptr; // queued job owns its label, never caller RAM
    busy = true;
    submitted_ms = millis();
    active_callback = callback;
    strlcpy(active_label, job.label, sizeof(active_label));
    if (xQueueSend(requests, &job, 0) == pdTRUE) return true;
    active_callback = nullptr;
    strcpy(active_label, "idle");
    busy = false;
    return false;
}

void feed_http_tick() {
    if (!busy) return;
    Completion completion;
    if (xQueueReceive(completions, &completion, 0) == pdTRUE) {
        finish_completion(completion);
    } else if (completion_lost.exchange(false)) {
        completion.callback = active_callback;
        completion.result.code = HTTPC_ERROR_TOO_LESS_RAM;
        completion.result.elapsed_ms = millis() - submitted_ms;
        finish_completion(completion);
    }
}

bool feed_http_busy() { return busy; }

FeedHttpStatus feed_http_status() {
    FeedHttpStatus status{};
    status.ready = ready.load();
    status.busy = busy;
    status.completion_pending = completions && uxQueueMessagesWaiting(completions) != 0;
    status.watchdog_active = watchdog_active.load();
    strlcpy(status.label, active_label, sizeof(status.label));
    status.age_ms = busy ? uint32_t(millis() - submitted_ms) : 0;
    status.completed = completed;
    status.last_elapsed_ms = last_elapsed_ms;
    status.worker_stack_free = worker_stack_free.load();
    status.worker_error = worker_error.load();
    return status;
}
