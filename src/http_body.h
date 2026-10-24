#ifndef HTTP_BODY_H
#define HTTP_BODY_H

#include <stddef.h>
#include <stdint.h>

// Arduino 2.0.17 HTTPClient::getString() waits forever when a peer leaves a
// response body incomplete and its TCP connection open. Its socket timeout
// never runs in the available()==0 branch. Keep the policy independent of
// Arduino so the actual reader can be exercised with stalled fake sockets.
namespace http_body {

enum class Error { None, InvalidLength, TooLarge, Deadline, Stalled,
                   Truncated, ReadFailed, SinkRejected, InvalidChunk };
struct Result {
    Error error;
    size_t bytes;
    bool ok() const { return error == Error::None; }
};

inline const char *error_name(Error e) {
    switch (e) {
        case Error::None: return "ok";
        case Error::InvalidLength: return "invalid length";
        case Error::TooLarge: return "body too large";
        case Error::Deadline: return "body deadline";
        case Error::Stalled: return "body stalled";
        case Error::Truncated: return "truncated body";
        case Error::ReadFailed: return "socket read failed";
        case Error::SinkRejected: return "body storage failed";
        case Error::InvalidChunk: return "invalid chunk framing";
    }
    return "unknown";
}

template <class Client, class Sink, class Clock, class Yield, class Done>
Result read_until(Client &client, int content_length, size_t max_bytes,
                  uint32_t idle_ms, uint32_t total_ms, Sink sink, Clock now,
                  Yield yield, Done done) {
    if (content_length < -1) return {Error::InvalidLength, 0};
    if (content_length >= 0 && (size_t)content_length > max_bytes)
        return {Error::TooLarge, 0};

    const uint32_t started = now();
    uint32_t last_data = started;
    size_t total = 0;
    uint8_t buffer[512];
    for (;;) {
        if (done()) return {Error::None, total};
        if (content_length >= 0 && total == (size_t)content_length)
            return {Error::None, total};
        if ((uint32_t)(now() - started) >= total_ms)
            return {Error::Deadline, total};

        int available = client.available();
        if (available < 0) {
            // WiFiClientSecure reports TLS close through available()==-1 and
            // calls stop(). A close-delimited body ends here; a declared body
            // must still contain every promised byte.
            if (!client.connected())
                return {content_length < 0 ? Error::None : Error::Truncated, total};
            return {Error::ReadFailed, total};
        }
        if (available > 0) {
            if (total == max_bytes) return {Error::TooLarge, total};
            size_t count = (size_t)available;
            if (count > sizeof(buffer)) count = sizeof(buffer);
            if (count > max_bytes - total) count = max_bytes - total;
            if (content_length >= 0 && count > (size_t)content_length - total)
                count = (size_t)content_length - total;
            // Only request bytes already available; never enter readBytes()'s
            // separate timeout loop or count a zero-byte read as progress.
            int got = client.read(buffer, count);
            if (got < 0 || (size_t)got > count) return {Error::ReadFailed, total};
            if (got > 0) {
                if (!sink(buffer, (size_t)got)) return {Error::SinkRejected, total};
                total += (size_t)got;
                last_data = now();
                yield();
                continue;
            }
        } else if (!client.connected()) {
            return {content_length < 0 ? Error::None : Error::Truncated, total};
        }
        if ((uint32_t)(now() - last_data) >= idle_ms)
            return {Error::Stalled, total};
        yield();
    }
}

template <class Client, class Sink, class Clock, class Yield>
Result read(Client &client, int content_length, size_t max_bytes,
            uint32_t idle_ms, uint32_t total_ms, Sink sink, Clock now, Yield yield) {
    return read_until(client, content_length, max_bytes, idle_ms, total_ms,
                      sink, now, yield, []() { return false; });
}

// HTTP/1.1 feeds can use chunks. Decode framing under the SAME transport
// deadline as the body: chunk sizes, extensions and trailers do not get a
// fresh timeout, and an open keep-alive socket need not close after 0\r\n\r\n.
template <class Sink>
class ChunkDecoder {
public:
    static constexpr size_t metadata_limit = 16384;
    explicit ChunkDecoder(Sink sink, size_t limit) : sink_(sink), limit_(limit) {}
    Error error = Error::None;
    size_t bytes = 0;
    bool done() const { return state_ == Done; }
    bool feed(const uint8_t *data, size_t size) {
        size_t pos = 0;
        while (pos < size && !done()) {
            if (state_ == Data) {
                size_t n = size - pos;
                if (n > remaining_) n = remaining_;
                if (!sink_(data + pos, n)) return fail(Error::SinkRejected);
                pos += n; bytes += n; remaining_ -= n;
                if (!remaining_) state_ = DataCR;
                continue;
            }
            if (++metadata_ > metadata_limit) return fail(Error::TooLarge);
            uint8_t c = data[pos++];
            switch (state_) {
                case Size:
                    if (++line_length_ > 256) return fail(Error::InvalidChunk);
                    if (c == '\r') {
                        if (!digits_) return fail(Error::InvalidChunk);
                        state_ = SizeLF;
                    } else if (extension_) {
                        if (c < 32 || c > 126) return fail(Error::InvalidChunk);
                    } else if (c == ';' && digits_) {
                        extension_ = true;
                    } else {
                        int digit = c >= '0' && c <= '9' ? c - '0' :
                            c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                        if (digit < 0) return fail(Error::InvalidChunk);
                        // Check before multiply so oversized hexadecimal sizes
                        // cannot wrap and bypass the decoded payload limit.
                        size_t left = limit_ - bytes;
                        if (chunk_size_ > left / 16 ||
                            (chunk_size_ == left / 16 && (size_t)digit > left % 16))
                            return fail(Error::TooLarge);
                        chunk_size_ = chunk_size_ * 16 + (size_t)digit;
                        digits_ = true;
                    }
                    break;
                case SizeLF:
                    if (c != '\n') return fail(Error::InvalidChunk);
                    remaining_ = chunk_size_;
                    state_ = remaining_ ? Data : Trailer;
                    chunk_size_ = line_length_ = 0;
                    digits_ = extension_ = false;
                    break;
                case DataCR:
                    if (c != '\r') return fail(Error::InvalidChunk);
                    state_ = DataLF;
                    break;
                case DataLF:
                    if (c != '\n') return fail(Error::InvalidChunk);
                    state_ = Size;
                    break;
                case Trailer:
                    if (c == '\r') state_ = TrailerLF;
                    else {
                        if (c < 32 || c > 126 || ++line_length_ > 256)
                            return fail(Error::InvalidChunk);
                    }
                    break;
                case TrailerLF:
                    if (c != '\n') return fail(Error::InvalidChunk);
                    state_ = line_length_ ? Trailer : Done;
                    line_length_ = 0;
                    break;
                default: return fail(Error::InvalidChunk);
            }
        }
        return true;
    }
private:
    enum State { Size, SizeLF, Data, DataCR, DataLF, Trailer, TrailerLF, Done };
    State state_ = Size;
    Sink sink_;
    size_t limit_, chunk_size_ = 0, remaining_ = 0, line_length_ = 0, metadata_ = 0;
    bool digits_ = false, extension_ = false;
    bool fail(Error e) { error = e; return false; }
};

template <class Client, class Sink, class Clock, class Yield>
Result read_chunked(Client &client, size_t max_bytes, uint32_t idle_ms,
                    uint32_t total_ms, Sink sink, Clock now, Yield yield) {
    ChunkDecoder<Sink> decoder(sink, max_bytes);
    auto result = read_until(client, -1, max_bytes + decoder.metadata_limit,
        idle_ms, total_ms,
        [&decoder](const uint8_t *p, size_t n) { return decoder.feed(p, n); },
        now, yield, [&decoder]() { return decoder.done(); });
    if (decoder.error != Error::None) result.error = decoder.error;
    else if (result.ok() && !decoder.done()) result.error = Error::Truncated;
    result.bytes = decoder.bytes;
    return result;
}

} // namespace http_body

#ifdef ARDUINO
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

// WiFiClientSecure's handshake timeout is in seconds and is independent of
// HTTPClient's connect/header timeout (otherwise it defaults to 120 seconds).
inline void configure_http(HTTPClient &http, WiFiClientSecure &client, bool http10 = true) {
    client.setHandshakeTimeout(8);
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.setReuse(false);
    http.useHTTP10(http10);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(2);
    static const char *headers[] = {"Transfer-Encoding", "Content-Encoding"};
    http.collectHeaders(headers, 2);
}

struct HttpBodyBuffer {
    char *data = nullptr;
    size_t size = 0;
    HttpBodyBuffer() = default;
    HttpBodyBuffer(const HttpBodyBuffer &) = delete;
    HttpBodyBuffer &operator=(const HttpBodyBuffer &) = delete;
    ~HttpBodyBuffer() { free(data); }
};

inline bool read_http_body(HTTPClient &http, HttpBodyBuffer &body,
                           size_t max_bytes, uint32_t total_ms = 15000) {
    String transfer = http.header("Transfer-Encoding");
    String encoding = http.header("Content-Encoding");
    transfer.trim();
    encoding.trim();
    bool chunked = transfer.equalsIgnoreCase("chunked");
    if ((transfer.length() && !transfer.equalsIgnoreCase("identity") && !chunked) ||
        (encoding.length() && !encoding.equalsIgnoreCase("identity"))) {
        Serial.printf("HTTP: unsupported encoding transfer=%s content=%s\r\n",
                      transfer.c_str(), encoding.c_str());
        return false;
    }
    int length = chunked ? -1 : http.getSize();
    if (length < -1 || (length >= 0 && (size_t)length > max_bytes)) {
        Serial.printf("HTTP: rejected body length %d (limit %u)\r\n",
                      length, (unsigned)max_bytes);
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("HTTP: connection closed before response body");
        return false;
    }
    size_t capacity = length >= 0 ? (size_t)length : max_bytes;
    // The 511 response includes ~250 KB of unused base64 icons. Keep the
    // complete wire response out of internal RAM and filter JSON before it
    // allocates its document. The board requires PSRAM for its display too.
    body.data = (char *)heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM);
    if (!body.data) {
        Serial.printf("HTTP: PSRAM allocation failed (%u bytes)\r\n",
                      (unsigned)(capacity + 1));
        return false;
    }
    auto sink = [&body](const uint8_t *data, size_t n) {
        memcpy(body.data + body.size, data, n);
        body.size += n;
        return true;
    };
    auto now = []() { return millis(); };
    auto yield = []() { delay(1); };
    auto result = chunked ?
        http_body::read_chunked(*stream, capacity, 2500, total_ms, sink, now, yield) :
        http_body::read(*stream, length, capacity, 2500, total_ms, sink, now, yield);
    body.data[body.size] = '\0';
    if (!result.ok()) {
        Serial.printf("HTTP: %s after %u bytes\r\n",
                      http_body::error_name(result.error), (unsigned)result.bytes);
    }
    return result.ok() && body.size > 0;
}
#endif // ARDUINO
#endif // HTTP_BODY_H
