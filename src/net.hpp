// Just enough networking for the phone: TCP sockets (Winsock or POSIX), a WebSocket client
// (RFC 6455, text frames, no TLS: the phone serves plain ws://) and an HTTP GET.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace allsense::detail {

class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Throws std::runtime_error with a readable reason.
    void connect(const std::string& host, int port, int timeout_ms);
    void set_timeouts(int recv_ms, int send_ms);
    void send_all(const void* data, std::size_t n);  // throws on failure
    // > 0 bytes read, 0 = closed by the peer, -1 = timed out. Throws on errors.
    long recv_some(void* buf, std::size_t n);
    void shutdown();  // unblocks a reader in another thread
    void close();

private:
    std::intptr_t fd_ = -1;
};

class WebSocket {
public:
    enum class Read { Message, Timeout, Closed };

    // Throws std::runtime_error; *http_status is set when the server answered the upgrade with an error.
    void open(const std::string& host, int port, const std::string& path, int timeout_ms, int* http_status);
    void send_text(const std::string& text);  // thread-safe; throws once the link is gone
    // Reads the next text message (answers pings itself). Timeout after the socket's ~0.5 s read
    // timeout keeps partial frames for the next call. On Closed, close_code() says why (0 = no code).
    Read read(std::string& out);
    int close_code() const { return close_code_; }
    void shutdown() { sock_.shutdown(); }

private:
    void send_frame(int opcode, const std::string& payload);
    bool fill(std::size_t n);  // false = timed out; throws when closed

    Socket sock_;
    std::mutex send_mutex_;
    std::string buf_;      // bytes received but not parsed yet
    std::string partial_;  // a fragmented message so far
    int close_code_ = 0;
    bool closed_ = false;
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_get(const std::string& host, int port, const std::string& path_and_query, int timeout_ms);

}  // namespace allsense::detail
