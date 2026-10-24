#ifndef FEED_HTTP_H
#define FEED_HTTP_H

#include <stddef.h>
#include <stdint.h>

struct FeedHttpRequest {
    char url[512]{};
    const char *label = "feed";
    bool http10 = true;
    bool xhr = false;
    size_t max_bytes = 0;
    uint32_t body_timeout_ms = 15000;
};

struct FeedHttpResult {
    int code = 0;
    bool body_ok = false;
    const char *data = nullptr;
    size_t size = 0;
    uint32_t elapsed_ms = 0;
};

using FeedHttpCallback = void (*)(const FeedHttpResult &);

// All public calls belong to the Arduino main/loop task. Completion callbacks
// run only from tick(), and data is valid only until the callback returns.
bool feed_http_begin();
bool feed_http_submit(const FeedHttpRequest &request, FeedHttpCallback callback);
void feed_http_tick();
bool feed_http_busy();

struct FeedHttpStatus {
    bool ready;
    bool busy;
    bool completion_pending;
    bool watchdog_active;
    char label[48];
    uint32_t age_ms;
    uint32_t completed;
    uint32_t last_elapsed_ms;
    uint32_t worker_stack_free;
    int worker_error;
};
FeedHttpStatus feed_http_status();

#endif
