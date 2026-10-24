#include "feed_fakes/runtime.h"
#include "../src/feed_http.cpp"

thread_local FakeTask *current_fake_task = nullptr;
FeedFakeRuntime feed_fake;
FeedFakeSerial Serial;
static std::thread::id main_thread = std::this_thread::get_id();
static unsigned callbacks = 0;
static int expected_code = 200;
static bool expected_body = true;
static FeedHttpRequest nested_request;

template<class Predicate>
static void wait_for(Predicate predicate) {
    const uint32_t started = millis();
    while (!predicate()) {
        assert(millis() - started < 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void receive(const FeedHttpResult &result) {
    assert(std::this_thread::get_id() == main_thread);
    assert(feed_http_busy());
    assert(!feed_http_submit(nested_request, receive)); // busy through callback
    assert(feed_fake.http_clients == 0 && feed_fake.tls_clients == 0);
    assert(result.code == expected_code && result.body_ok == expected_body);
    if (result.code == 200) {
        assert(result.size == 3 && std::memcmp(result.data, "abc", 3) == 0);
        std::lock_guard<std::mutex> lock(feed_fake.payload_mutex);
        assert(feed_fake.payloads.count(const_cast<char *>(result.data)) == 1);
    } else assert(result.data == nullptr && result.size == 0);
    ++callbacks;
}
static void unblock_http() {
    feed_fake.block_http = false;
    feed_fake.http_changed.notify_all();
}
static void deliver() {
    wait_for([]() { return feed_http_status().completion_pending; });
    assert(feed_http_busy());
    feed_http_tick();
    assert(!feed_http_busy());
    std::lock_guard<std::mutex> lock(feed_fake.payload_mutex);
    assert(feed_fake.payloads.empty());
}

int main() {
    FeedHttpRequest request;
    std::strcpy(request.url, "https://example.test/feed");
    request.max_bytes = 1024;
    nested_request = request;
    assert(!feed_http_submit(request, receive));

    feed_fake.fail_queue = true;
    assert(!feed_http_begin() && feed_fake.queues.empty());
    feed_fake.fail_task = true;
    assert(!feed_http_begin() && feed_fake.queues.empty());
    feed_fake.fail_watchdog = true;
    assert(!feed_http_begin() && feed_fake.queues.empty() && feed_fake.tasks.empty());
    assert(feed_http_begin() && feed_http_begin());
    assert(feed_http_status().ready && feed_http_status().watchdog_active);
    assert(feed_fake.tasks.size() == 1 && feed_fake.queues.size() == 2);

    feed_fake.fail_send = true;
    assert(!feed_http_submit(request, receive) && !feed_http_busy());
    char label[] = "test-feed";
    request.label = label;
    assert(feed_http_submit(request, receive));
    std::strcpy(request.url, "changed"); label[0] = 'X';
    wait_for([]() { return feed_fake.entered_http.load(); });
    assert(feed_fake.received_url == "https://example.test/feed");
    assert(std::strcmp(feed_http_status().label, "test-feed") == 0);
    assert(!feed_http_submit(nested_request, receive));
    unsigned display_ticks = 0;
    for (; display_ticks < 40; ++display_ticks) {
        feed_http_tick(); // the main task keeps rendering while GET is blocked
        assert(callbacks == 0 && feed_http_busy());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(feed_http_status().age_ms >= 30 && feed_fake.feeds_during_http == 0);
    unblock_http(); deliver();
    assert(callbacks == 1 && feed_http_status().completed == 1);

    expected_code = 429; expected_body = false; feed_fake.code = 429;
    assert(feed_http_submit(nested_request, receive)); deliver();
    expected_code = 200; feed_fake.code = 200; feed_fake.body_ok = false;
    assert(feed_http_submit(nested_request, receive)); deliver(); // partial failure freed

    feed_fake.block_http = true; feed_fake.entered_http = false;
    assert(feed_http_submit(nested_request, receive));
    wait_for([]() { return feed_fake.entered_http.load(); });
    feed_fake.fail_send = true; // fail completion enqueue, not the initial job
    expected_code = HTTPC_ERROR_TOO_LESS_RAM;
    unblock_http();
    wait_for([]() { return !feed_http_status().ready; });
    wait_for([]() { feed_http_tick(); return callbacks == 4; });
    assert(callbacks == 4 && !feed_http_busy() && feed_fake.payloads.empty());

    for (auto *task : std::vector<FakeTask *>(feed_fake.tasks)) {
        esp_task_wdt_delete(task); vTaskDelete(task);
    }
    for (auto *queue : std::vector<FakeQueue *>(feed_fake.queues)) vQueueDelete(queue);
    std::puts("PASS: feed worker ownership, main-thread callbacks, blocked-transport responsiveness, watchdog discipline and failure cleanup");
}
