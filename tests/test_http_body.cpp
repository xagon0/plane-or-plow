#include "../src/http_body.h"
#include "../src/contact_freshness.h"
#include <assert.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

struct Socket {
    uint32_t tick = 0, start = 0;
    std::string bytes;
    std::vector<uint32_t> release;
    size_t position = 0;
    bool keep_open = true, zero_reads = false, failed_reads = false;
    bool negative_eof = false, negative_available = false;
    int available() const {
        if (negative_available || (negative_eof && position == bytes.size())) return -1;
        size_t count = 0;
        while (position + count < bytes.size() &&
               release[position + count] <= (uint32_t)(tick - start)) count++;
        return (int)count;
    }
    int read(uint8_t *out, size_t count) {
        if (failed_reads) return -1;
        if (zero_reads) return 0;
        count = std::min(count, (size_t)available());
        std::copy(bytes.begin() + position, bytes.begin() + position + count, out);
        position += count;
        return (int)count;
    }
    bool connected() const { return keep_open || position < bytes.size(); }
    Socket(std::string data, bool open = true) : bytes(data),
        release(data.size(), 0), keep_open(open) {}
};

static http_body::Result run(Socket &s, int length, std::string &out,
                              size_t cap = 100, uint32_t idle = 5,
                              uint32_t total = 20) {
    return http_body::read(s, length, cap, idle, total,
        [&out](const uint8_t *p, size_t n) { out.append((const char *)p, n); return true; },
        [&s]() { return s.tick; }, [&s]() { s.tick++; });
}

static http_body::Result run_chunked(Socket &s, std::string &out,
                                      size_t cap = 100, uint32_t idle = 5,
                                      uint32_t total = 200) {
    return http_body::read_chunked(s, cap, idle, total,
        [&out](const uint8_t *p, size_t n) { out.append((const char *)p, n); return true; },
        [&s]() { return s.tick; }, [&s]() { s.tick++; });
}

int main() {
    using http_body::Error;
    {
        Socket s("abcEXTRA"); std::string out;
        auto r = run(s, 3, out);
        assert(r.ok() && out == "abc" && s.position == 3);
    }
    {
        Socket s(""); std::string out;
        assert(run(s, 0, out).ok());
    }
    {
        Socket s("abc", false); std::string out;
        assert(run(s, -1, out).ok() && out == "abc");
    }
    {
        Socket s("abc", false); std::string out;
        auto r = run(s, 6, out);
        assert(r.error == Error::Truncated && r.bytes == 3);
    }
    {
        // Regression: HTTPClient::getString() never exits for this peer.
        Socket s("abc"); std::string out;
        auto r = run(s, 6, out);
        assert(r.error == Error::Stalled && r.bytes == 3 && s.tick == 5);
    }
    {
        // Each byte arrives inside the idle window; the total deadline wins.
        Socket s("abcdefghij"); std::string out;
        for (size_t i = 0; i < s.release.size(); i++) s.release[i] = i * 4;
        auto r = run(s, 10, out);
        assert(r.error == Error::Deadline && r.bytes == 5 && s.tick == 20);
    }
    {
        Socket s("abcdef"); std::string out;
        auto r = run(s, 6, out, 5);
        assert(r.error == Error::TooLarge && s.position == 0);
    }
    {
        Socket s("abcdef"); std::string out;
        auto r = run(s, -1, out, 5);
        assert(r.error == Error::TooLarge && out == "abcde");
    }
    {
        Socket s("abc"); s.zero_reads = true; std::string out;
        assert(run(s, 3, out).error == Error::Stalled && s.tick == 5);
    }
    {
        Socket s("abc"); s.failed_reads = true; std::string out;
        assert(run(s, 3, out).error == Error::ReadFailed);
    }
    {
        Socket s(""); std::string out;
        assert(run(s, -2, out).error == Error::InvalidLength);
    }
    {
        Socket s("abc");
        auto r = http_body::read(s, 3, 100, 5, 20,
            [](const uint8_t *, size_t) { return false; },
            [&s]() { return s.tick; }, [&s]() { s.tick++; });
        assert(r.error == Error::SinkRejected);
    }
    {
        Socket s("a"); s.tick = s.start = UINT32_MAX - 2; std::string out;
        auto r = run(s, 2, out, 100, 5, 20);
        assert(r.error == Error::Stalled && (uint32_t)(s.tick - s.start) == 5);
    }
    {
        Socket s(std::string(258105, 'x')); std::string out;
        auto r = run(s, 258105, out, 1024 * 1024, 2500, 15000);
        assert(r.ok() && out.size() == 258105);
    }
    {
        // Arduino WiFiClientSecure::available() reports TLS close as -1.
        Socket s("abc", false); s.negative_eof = true; std::string out;
        assert(run(s, -1, out).ok() && out == "abc");
    }
    {
        Socket s("abc", false); s.negative_eof = true; std::string out;
        assert(run(s, 4, out).error == Error::Truncated);
    }
    {
        Socket s(""); s.negative_available = true; std::string out;
        assert(run(s, -1, out).error == Error::ReadFailed);
    }
    {
        Socket s("4;name=value\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Test: value\r\n\r\n");
        std::string out;
        assert(run_chunked(s, out).ok() && out == "Wikipedia");
        // Socket remains open: completion depends on terminal framing.
        assert(s.connected());
    }
    {
        Socket s("4;name=value\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Test: value\r\n\r\n");
        for (size_t i = 0; i < s.release.size(); i++) s.release[i] = i;
        std::string out;
        assert(run_chunked(s, out).ok() && out == "Wikipedia");
    }
    {
        Socket s("4\r\nWiki\r\n", false); s.negative_eof = true; std::string out;
        assert(run_chunked(s, out).error == Error::Truncated);
    }
    {
        Socket s("0\r\n", false); std::string out;
        assert(run_chunked(s, out).error == Error::Truncated);
    }
    {
        Socket s("1\r\naX\n0\r\n\r\n"); std::string out;
        assert(run_chunked(s, out).error == Error::InvalidChunk);
    }
    {
        Socket s("g\r\n"); std::string out;
        assert(run_chunked(s, out).error == Error::InvalidChunk);
    }
    {
        Socket s("ffffffffffffffffffffffff\r\n"); std::string out;
        assert(run_chunked(s, out).error == Error::TooLarge && out.empty());
    }
    {
        Socket s("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"); std::string out;
        assert(run_chunked(s, out, 8).error == Error::TooLarge && out == "Wiki");
    }
    {
        Socket s("4\r\nab"); std::string out;
        assert(run_chunked(s, out).error == Error::Stalled && out == "ab");
    }
    {
        Socket s("1;abcdefghijklmnopqrstuvwxyz\r\na\r\n0\r\n\r\n");
        for (size_t i = 0; i < s.release.size(); i++) s.release[i] = i * 4;
        std::string out;
        assert(run_chunked(s, out, 100, 5, 20).error == Error::Deadline && s.tick == 20);
    }
    {
        Socket s("0;" + std::string(257, 'x') + "\r\n\r\n"); std::string out;
        assert(run_chunked(s, out).error == Error::InvalidChunk);
    }
    {
        std::string raw = "0\r\n";
        for (int i = 0; i < 70; i++) raw += "X: " + std::string(250, 'x') + "\r\n";
        Socket s(raw); std::string out;
        assert(run_chunked(s, out).error == Error::TooLarge);
    }
    {
        Socket s("0\r\n\r\n"); std::string out;
        assert(run_chunked(s, out).ok() && out.empty());
    }
    {
        struct Contact { uint32_t fix_ms; int id; } contacts[] = {
            {100000, 1}, {280001, 2}, {100001, 3}, {200000, 4}};
        int count = 4;
        expire_contacts(contacts, count, 280000, 180000);
        // Exact boundary expires; stable compaction preserves live IDs.
        // Future-looking timestamp simulates an old timestamp across wrap.
        assert(count == 2 && contacts[0].id == 3 && contacts[1].id == 4);
        expire_contacts(contacts, count, 900000, 600000);
        assert(count == 0);
    }
    {
        struct Contact { uint32_t fix_ms; } contacts[] = {
            {UINT32_MAX - 10}, {UINT32_MAX - 100}};
        int count = 2;
        expire_contacts(contacts, count, 5, 20);
        assert(count == 1 && contacts[0].fix_ms == UINT32_MAX - 10);
    }
    std::cout << "PASS: 30 bounded HTTP body and 2 contact expiry scenarios\n";
}
