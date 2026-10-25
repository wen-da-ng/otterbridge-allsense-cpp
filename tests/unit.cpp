// Unit tests of the client's internals against a loopback server playing a (misbehaving) phone:
// message and body limits, the WebSocket handshake, JSON depth, link parsing, catalog media urls.
//
//     ctest --test-dir build    (or run build/unit_tests)
//
// Prints one line per test and exits non-zero on the first failure.
#include <allsense/allsense.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "internal.hpp"
#include "net.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void close_sock(sock_t s) { ::close(s); }
#endif

using namespace allsense;
using namespace std::chrono_literals;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cout << "FAILED " << __FILE__ << ":" << __LINE__ << ": " #cond << std::endl; \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

template <class F>
std::string throws(F f) {  // the error's message, or "" if it didn't throw
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

// ---- a one-connection loopback server ----

struct Server {
    sock_t listener;
    int port = 0;
    std::thread thread;

    explicit Server(std::function<void(sock_t)> handle) {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        CHECK(::listen(listener, 1) == 0);
        socklen_t len = sizeof a;
        getsockname(listener, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        thread = std::thread([this, handle] {
            sock_t c = ::accept(listener, nullptr, nullptr);
            handle(c);
            close_sock(c);
        });
    }
    ~Server() {
        thread.join();
        close_sock(listener);
    }
};

void send_raw(sock_t s, const std::string& data) {  // errors ignored: the client may hang up first
    std::size_t done = 0;
    while (done < data.size()) {
        int n = ::send(s, data.data() + done, static_cast<int>(data.size() - done), 0);
        if (n <= 0) return;
        done += static_cast<std::size_t>(n);
    }
}

std::string read_request(sock_t s) {
    std::string req;
    char c;
    while (req.find("\r\n\r\n") == std::string::npos && ::recv(s, &c, 1, 0) == 1) req += c;
    return req;
}

// Answers the upgrade; `accept` overrides the correct Sec-WebSocket-Accept.
void handshake(sock_t s, const char* accept = nullptr) {
    std::string req = read_request(s);
    auto p = req.find("Sec-WebSocket-Key: ") + 19;
    std::string key = req.substr(p, req.find("\r\n", p) - p);
    send_raw(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                    (accept ? std::string(accept) : detail::websocket_accept(key)) + "\r\n\r\n");
}

// An unmasked server frame; `declared` overrides the length in the header.
std::string frame(int opcode, bool fin, const std::string& payload, std::uint64_t declared = ~0ull) {
    std::uint64_t n = declared == ~0ull ? payload.size() : declared;
    std::string f(1, static_cast<char>((fin ? 0x80 : 0) | opcode));
    if (n < 126) {
        f += static_cast<char>(n);
    } else if (n < 65536) {
        f += static_cast<char>(126);
        f += static_cast<char>(n >> 8);
        f += static_cast<char>(n & 0xFF);
    } else {
        f += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) f += static_cast<char>((n >> (8 * i)) & 0xFF);
    }
    return f + payload;
}

detail::WebSocket::Read read_message(detail::WebSocket& ws, std::string& out) {
    auto end = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        auto r = ws.read(out);
        if (r != detail::WebSocket::Read::Timeout || std::chrono::steady_clock::now() > end) return r;
    }
}

std::string deep(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

// ---- tests ----

void test_accept_key() {
    // RFC 6455 §1.3's example; key + GUID is 60 bytes, so SHA-1 runs over two blocks.
    CHECK(detail::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

void test_handshake() {
    {
        Server srv([](sock_t s) {
            handshake(s);
            send_raw(s, frame(0x1, true, "{\"op\":\"hello\"}"));
        });
        detail::WebSocket ws;
        ws.open("127.0.0.1", srv.port, "/ws", 3000, nullptr);
        std::string msg;
        CHECK(read_message(ws, msg) == detail::WebSocket::Read::Message && msg == "{\"op\":\"hello\"}");
    }
    {
        Server srv([](sock_t s) { handshake(s, "c29tZXRoaW5nIGVsc2U="); });
        detail::WebSocket ws;
        CHECK(throws([&] { ws.open("127.0.0.1", srv.port, "/ws", 3000, nullptr); }).find("Sec-WebSocket-Accept") !=
              std::string::npos);
    }
    {
        Server srv([](sock_t s) {
            read_request(s);
            send_raw(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n");  // no accept at all
        });
        detail::WebSocket ws;
        CHECK(!throws([&] { ws.open("127.0.0.1", srv.port, "/ws", 3000, nullptr); }).empty());
    }
}

void test_message_limit() {
    const std::string mib(1 << 20, 'x');
    {   // 3 MiB in fragments is fine; then 5 fragments of 1 MiB end the link.
        Server srv([&](sock_t s) {
            handshake(s);
            send_raw(s, frame(0x1, false, mib) + frame(0x0, false, mib) + frame(0x0, true, mib));
            for (int i = 0; i < 5; ++i) send_raw(s, frame(i ? 0x0 : 0x1, false, mib));
        });
        detail::WebSocket ws;
        ws.open("127.0.0.1", srv.port, "/ws", 3000, nullptr);
        std::string msg;
        CHECK(read_message(ws, msg) == detail::WebSocket::Read::Message && msg.size() == 3u << 20);
        CHECK(read_message(ws, msg) == detail::WebSocket::Read::Closed);
    }
    {   // a single frame that only claims to be big is refused before anything is buffered
        Server srv([](sock_t s) {
            handshake(s);
            send_raw(s, frame(0x1, true, "", detail::kMaxMessage + 1));
            send_raw(s, frame(0x1, true, "", 1ull << 62));
        });
        detail::WebSocket ws;
        ws.open("127.0.0.1", srv.port, "/ws", 3000, nullptr);
        std::string msg;
        CHECK(read_message(ws, msg) == detail::WebSocket::Read::Closed);
    }
}

void test_http_limit() {
    {
        Server srv([](sock_t s) {
            read_request(s);
            send_raw(s, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
        });
        auto r = detail::http_get("127.0.0.1", srv.port, "/x", 3000);
        CHECK(r.status == 200 && r.body == "hello");
    }
    {   // a Content-Length over the limit fails at once
        Server srv([](sock_t s) {
            read_request(s);
            send_raw(s, "HTTP/1.1 200 OK\r\nContent-Length: 40000000000\r\n\r\n");
        });
        CHECK(throws([&] { detail::http_get("127.0.0.1", srv.port, "/x", 3000); }).find("bigger") != std::string::npos);
    }
    {   // no Content-Length: reading until close stops at the limit
        Server srv([](sock_t s) {
            read_request(s);
            send_raw(s, "HTTP/1.1 200 OK\r\n\r\n");
            std::string chunk(64 * 1024, 'x');
            for (int i = 0; i < 64; ++i) send_raw(s, chunk);  // 4 MiB, the client gives up after 1 MiB
        });
        CHECK(throws([&] { detail::http_get("127.0.0.1", srv.port, "/x", 3000, 1u << 20); }).find("bigger") !=
              std::string::npos);
    }
}

void test_json_depth() {
    CHECK(detail::parse_message(deep(detail::kMaxJsonDepth))[0].is_array());
    CHECK(!throws([] { detail::parse_message(deep(detail::kMaxJsonDepth + 2)); }).empty());
    CHECK(!throws([] { detail::parse_message(deep(100000)); }).empty());  // used to overflow the stack
    CHECK(!throws([] { detail::parse_message("{\"a\":" + deep(100000) + "}"); }).empty());
}

void test_parse_link() {
    for (const char* l : {"https://1.2.3.4/?token=a", "wss://1.2.3.4/ws?token=a", "HTTPS://1.2.3.4/?token=a"})
        CHECK(throws([&] { parse_link(l); }).find("TLS") != std::string::npos);
    CHECK(throws([] { parse_link("ftp://1.2.3.4/?token=a"); }).find("scheme") != std::string::npos);
    CHECK(parse_link("WS://1.2.3.4/?token=a").host == "1.2.3.4");
    // Bad % escapes stay as they are instead of throwing.
    CHECK(parse_link("http://h/?token=a%zzb").token == "a%zzb");
    CHECK(parse_link("http://h/?token=ab%4").token == "ab%4");
    CHECK(parse_link("http://h/?token=ab%").token == "ab%");
    CHECK(parse_link("http://h/?token=a%2Bb%41").token == "a+bA");
}

void test_media_paths() {
    CHECK(detail::safe_media_path("/camera/back.mjpg"));
    CHECK(detail::safe_media_path("//x/y"));  // still only a path after http://host:port
    for (const char* bad : {"", "camera/back.mjpg", "@evil.example/x", "/x@evil.example", "\\\\evil\\x", "/a\r\nHost: x"})
        CHECK(!detail::safe_media_path(bad));
}

// A whole client against a phone that sends a bad media url and a deeply nested message.
void test_phone() {
    Server srv([](sock_t s) {
        handshake(s);
        std::string catalog =
            R"({"op":"catalog","device":"rover","streams":[)"
            R"({"topic":"allsense.rover.camera.back","name":"back","id":"camera/0","kind":"image","url":"@evil.example/x"},)"
            R"({"topic":"allsense.rover.camera.front","name":"front","id":"camera/1","kind":"image","url":"/camera/front.mjpg","h264_url":"/x\r\nHost: evil"},)"
            R"({"topic":"allsense.rover.mic.mic","name":"mic","id":"mic/1","kind":"audio","url":"/mic/mic.wav"}]})";
        send_raw(s, frame(0x1, true, catalog));
        send_raw(s, frame(0x1, true, "{\"op\":\"ping\",\"id\":" + deep(100000) + "}"));
        send_raw(s, frame(0x1, true, "{\"op\":\"error\",\"message\":\"still here\"}"));
        std::this_thread::sleep_for(1500ms);
    });
    Options opt;
    opt.reconnect = false;
    auto phone = connect("http://127.0.0.1:" + std::to_string(srv.port) + "/?token=t", opt);
    CHECK(phone.device() == "rover");
    CHECK(throws([&] { phone.camera_url("back"); }).find("usable") != std::string::npos);
    CHECK(throws([&] { phone.snapshot("back"); }).find("usable") != std::string::npos);
    CHECK(!phone.camera_url("front").empty());
    CHECK(throws([&] {
              CameraOptions o;
              o.codec = "h264";
              phone.camera_url("front", o);
          }).find("usable") != std::string::npos);
    CHECK(phone.microphone_url("mic").find("/mic/mic.wav?token=t") != std::string::npos);
    auto end = std::chrono::steady_clock::now() + 5s;
    while (phone.last_error() != "still here" && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(20ms);
    CHECK(phone.last_error() == "still here");  // the deep message was skipped, the client lives on
}

int main() {
#ifdef _WIN32
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const std::pair<const char*, void (*)()> tests[] = {
        {"accept_key", test_accept_key},   {"handshake", test_handshake},   {"message_limit", test_message_limit},
        {"http_limit", test_http_limit},   {"json_depth", test_json_depth}, {"parse_link", test_parse_link},
        {"media_paths", test_media_paths}, {"phone", test_phone},
    };
    for (auto& t : tests) {
        t.second();
        std::cout << "ok " << t.first << std::endl;
    }
    std::cout << "PASS " << sizeof tests / sizeof tests[0] << " tests" << std::endl;
    return 0;
}
