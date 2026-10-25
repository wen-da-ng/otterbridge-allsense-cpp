#include "net.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kInvalid = INVALID_SOCKET;
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kInvalid = -1;
#endif

namespace allsense::detail {
namespace {

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { WSACleanup(); }
};
void ensure_init() { static WinsockInit init; }
int last_error() { return WSAGetLastError(); }
bool timed_out(int e) { return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
std::string error_text(int e) {
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, sizeof buf, nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s.empty() ? "error " + std::to_string(e) : s;
}
void close_sock(sock_t s) { closesocket(s); }
#else
void ensure_init() {}
int last_error() { return errno; }
bool timed_out(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
std::string error_text(int e) { return std::strerror(e); }
void close_sock(sock_t s) { ::close(s); }
#endif

sock_t S(std::intptr_t fd) { return static_cast<sock_t>(fd); }

std::string random_bytes(std::size_t n) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::string out(n, '\0');
    for (auto& c : out) c = static_cast<char>(rng() & 0xFF);
    return out;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += {t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], t[v & 63]};
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += {t[v >> 18], t[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += {t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], '='};
    }
    return out;
}

std::string host_header(const std::string& host, int port) {
    bool v6 = host.find(':') != std::string::npos;
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

// Reads until the end of the HTTP headers; returns them and leaves anything after in `rest`.
std::string read_headers(Socket& sock, std::string& rest) {
    std::string data;
    char buf[2048];
    for (;;) {
        auto end = data.find("\r\n\r\n");
        if (end != std::string::npos) {
            rest = data.substr(end + 4);
            return data.substr(0, end + 2);
        }
        if (data.size() > 64 * 1024) throw std::runtime_error("HTTP headers too long");
        long n = sock.recv_some(buf, sizeof buf);
        if (n == 0) throw std::runtime_error("the phone closed the connection");
        if (n < 0) throw std::runtime_error("no answer from the phone");
        data.append(buf, static_cast<std::size_t>(n));
    }
}

int status_of(const std::string& headers) {
    // "HTTP/1.1 101 Switching Protocols"
    auto sp = headers.find(' ');
    return sp == std::string::npos ? 0 : std::atoi(headers.c_str() + sp + 1);
}

long content_length(const std::string& headers) {
    std::string lower;
    for (char c : headers) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto p = lower.find("\r\ncontent-length:");
    return p == std::string::npos ? -1 : std::atol(lower.c_str() + p + 17);
}

}  // namespace

// ---- Socket ----

Socket::~Socket() { close(); }

void Socket::connect(const std::string& host, int port, int timeout_ms) {
    ensure_init();
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res) throw std::runtime_error("can't resolve " + host);
    std::string err = "no address";
    for (addrinfo* a = res; a; a = a->ai_next) {
        sock_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kInvalid) continue;
        // Non-blocking connect, so an unreachable phone fails after timeout_ms, not the OS's ~20 s.
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
#else
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
        rc = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
        bool ok = rc == 0;
        if (!ok) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select(static_cast<int>(s) + 1, nullptr, &wr, &ex, &tv) > 0) {
                int so = 0;
                socklen_t len = sizeof so;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
                ok = so == 0;
                if (!ok) err = error_text(so);
            } else {
                err = "timed out";
            }
        }
        if (ok) {
#ifdef _WIN32
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
#else
            fcntl(s, F_SETFL, flags);
#endif
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE  // macOS has no MSG_NOSIGNAL: a write to a dropped link must not kill the program
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            fd_ = static_cast<std::intptr_t>(s);
            freeaddrinfo(res);
            return;
        }
        close_sock(s);
    }
    freeaddrinfo(res);
    throw std::runtime_error(err);
}

void Socket::set_timeouts(int recv_ms, int send_ms) {
#ifdef _WIN32
    DWORD r = recv_ms, w = send_ms;
#else
    timeval r{recv_ms / 1000, (recv_ms % 1000) * 1000}, w{send_ms / 1000, (send_ms % 1000) * 1000};
#endif
    setsockopt(S(fd_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&r), sizeof r);
    setsockopt(S(fd_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&w), sizeof w);
}

void Socket::send_all(const void* data, std::size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
#ifdef MSG_NOSIGNAL
        long k = ::send(S(fd_), p, n, MSG_NOSIGNAL);
#else
        long k = ::send(S(fd_), p, static_cast<int>(n), 0);
#endif
        if (k <= 0) throw std::runtime_error("send failed: " + error_text(last_error()));
        p += k;
        n -= static_cast<std::size_t>(k);
    }
}

long Socket::recv_some(void* buf, std::size_t n) {
    if (fd_ == -1) return 0;
    long k = ::recv(S(fd_), static_cast<char*>(buf), static_cast<int>(n), 0);
    if (k >= 0) return k;
    int e = last_error();
    if (timed_out(e)) return -1;
    throw std::runtime_error(error_text(e));
}

void Socket::shutdown() {
    if (fd_ == -1) return;
#ifdef _WIN32
    ::shutdown(S(fd_), SD_BOTH);
#else
    ::shutdown(S(fd_), SHUT_RDWR);
#endif
}

void Socket::close() {
    if (fd_ == -1) return;
    close_sock(S(fd_));
    fd_ = -1;
}

// ---- WebSocket ----

void WebSocket::open(const std::string& host, int port, const std::string& path, int timeout_ms, int* http_status) {
    sock_.connect(host, port, timeout_ms);
    sock_.set_timeouts(timeout_ms, timeout_ms);
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host_header(host, port) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(random_bytes(16)) +
                      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    sock_.send_all(req.data(), req.size());
    std::string headers = read_headers(sock_, buf_);
    int status = status_of(headers);
    if (status != 101) {
        if (http_status) *http_status = status;
        throw std::runtime_error("the phone answered HTTP " + std::to_string(status));
    }
    // Short read timeouts let the reader thread notice close() and stalls; sends stay patient.
    sock_.set_timeouts(500, 5000);
}

void WebSocket::send_frame(int opcode, const std::string& payload) {
    std::string f;
    f += static_cast<char>(0x80 | opcode);
    std::size_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(0x80 | n);
    } else if (n < 65536) {
        f += static_cast<char>(0x80 | 126);
        f += static_cast<char>(n >> 8);
        f += static_cast<char>(n & 0xFF);
    } else {
        f += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF);
    }
    std::string mask = random_bytes(4);  // clients must mask every frame
    f += mask;
    std::size_t start = f.size();
    f += payload;
    for (std::size_t i = 0; i < n; ++i) f[start + i] ^= mask[i & 3];
    std::lock_guard<std::mutex> lock(send_mutex_);
    sock_.send_all(f.data(), f.size());
}

void WebSocket::send_text(const std::string& text) { send_frame(0x1, text); }

bool WebSocket::fill(std::size_t n) {
    char tmp[16384];
    while (buf_.size() < n) {
        long k = sock_.recv_some(tmp, sizeof tmp);
        if (k == 0) throw std::runtime_error("closed");
        if (k < 0) return false;
        buf_.append(tmp, static_cast<std::size_t>(k));
    }
    return true;
}

WebSocket::Read WebSocket::read(std::string& out) {
    if (closed_) return Read::Closed;
    try {
        for (;;) {
            if (!fill(2)) return Read::Timeout;
            auto b0 = static_cast<unsigned char>(buf_[0]), b1 = static_cast<unsigned char>(buf_[1]);
            bool fin = b0 & 0x80, masked = b1 & 0x80;
            int opcode = b0 & 0x0F;
            std::size_t head = 2;
            std::uint64_t len = b1 & 0x7F;
            if (len == 126) {
                if (!fill(4)) return Read::Timeout;
                len = static_cast<unsigned char>(buf_[2]) << 8 | static_cast<unsigned char>(buf_[3]);
                head = 4;
            } else if (len == 127) {
                if (!fill(10)) return Read::Timeout;
                len = 0;
                for (int i = 0; i < 8; ++i) len = len << 8 | static_cast<unsigned char>(buf_[2 + i]);
                head = 10;
            }
            if (len > (64u << 20)) throw std::runtime_error("message too big");
            std::size_t mask_at = head;
            if (masked) head += 4;
            if (!fill(head + len)) return Read::Timeout;
            std::string payload = buf_.substr(head, len);
            if (masked)
                for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= buf_[mask_at + (i & 3)];
            buf_.erase(0, head + len);

            switch (opcode) {
                case 0x0:  // continuation
                case 0x1:  // text
                case 0x2:  // binary (not used by the phone; passed on like text)
                    partial_ += payload;
                    if (fin) {
                        out.swap(partial_);
                        partial_.clear();
                        return Read::Message;
                    }
                    break;
                case 0x8:  // close
                    close_code_ = payload.size() >= 2
                                      ? static_cast<unsigned char>(payload[0]) << 8 | static_cast<unsigned char>(payload[1])
                                      : 0;
                    try {
                        send_frame(0x8, payload.substr(0, 2));
                    } catch (...) {
                    }
                    closed_ = true;
                    sock_.shutdown();  // finish the closing handshake: the server waits for our FIN
                    return Read::Closed;
                case 0x9:  // ping
                    send_frame(0xA, payload);
                    break;
                default:  // pong and anything else
                    break;
            }
        }
    } catch (const std::exception&) {
        closed_ = true;
        sock_.shutdown();
        return Read::Closed;
    }
}

// ---- HTTP ----

HttpResponse http_get(const std::string& host, int port, const std::string& path_and_query, int timeout_ms) {
    Socket sock;
    sock.connect(host, port, timeout_ms);
    sock.set_timeouts(timeout_ms, timeout_ms);
    std::string req = "GET " + path_and_query + " HTTP/1.1\r\nHost: " + host_header(host, port) +
                      "\r\nConnection: close\r\n\r\n";
    sock.send_all(req.data(), req.size());
    HttpResponse r;
    std::string headers = read_headers(sock, r.body);
    r.status = status_of(headers);
    long want = content_length(headers);
    char buf[16384];
    while (want < 0 || static_cast<long>(r.body.size()) < want) {
        long n = sock.recv_some(buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) throw std::runtime_error("the phone stopped answering");
        r.body.append(buf, static_cast<std::size_t>(n));
    }
    if (want >= 0 && static_cast<long>(r.body.size()) > want) r.body.resize(static_cast<std::size_t>(want));
    return r;
}

}  // namespace allsense::detail
