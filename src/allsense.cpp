#include "allsense/allsense.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include "net.hpp"

namespace allsense {

using namespace std::chrono;

namespace {

const char* kTokenRejected = "The phone rejected the pairing token";
const std::size_t kMaxQueue = 1000;

std::int64_t mono_ns() { return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(); }
std::int64_t unix_ns() { return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count(); }

std::string str(const json& j, const char* key, const std::string& fallback = "") {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

std::vector<std::string> strings(const json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it != j.end() && it->is_array())
        for (auto& v : *it) out.push_back(v.is_string() ? v.get<std::string>() : "");
    return out;
}

std::string url_encode(const std::string& s) {
    std::string out;
    char hex[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diag = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}

std::string fmt_double(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

// ---- Link ----

Link parse_link(const std::string& link_in, const std::string& token) {
    std::string link = link_in;
    auto scheme = link.find("://");
    if (scheme != std::string::npos) link = link.substr(scheme + 3);
    std::string query;
    auto q = link.find('?');
    if (q != std::string::npos) {
        query = link.substr(q + 1);
        link = link.substr(0, q);
    }
    std::string hostport = link.substr(0, link.find('/'));
    Link out;
    if (!hostport.empty() && hostport[0] == '[') {  // IPv6 literal: [fe80::1]:8765
        auto close = hostport.find(']');
        out.host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == ':') out.port = std::atoi(hostport.c_str() + close + 2);
    } else {
        auto colon = hostport.rfind(':');
        out.host = hostport.substr(0, colon);
        if (colon != std::string::npos) out.port = std::atoi(hostport.c_str() + colon + 1);
    }
    if (out.host.empty()) out.host = "localhost";
    if (out.port <= 0) out.port = 8765;
    out.token = token;
    std::size_t start = 0;
    while (out.token.empty() && start <= query.size()) {
        auto end = query.find('&', start);
        std::string kv = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (kv.rfind("token=", 0) == 0) out.token = url_decode(kv.substr(6));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (out.token.empty())
        throw TokenError("A pairing token is needed: copy the full link from the app (Home → Connect), or set Options::token");
    return out;
}

static std::string host_part(const Link& l) {
    bool v6 = l.host.find(':') != std::string::npos;
    return (v6 ? "[" + l.host + "]" : l.host) + ":" + std::to_string(l.port);
}

std::string Link::ws_url() const { return "ws://" + host_part(*this) + "/ws?token=" + url_encode(token); }

std::string Link::http_url(const std::string& path, const std::string& query) const {
    return "http://" + host_part(*this) + path + "?token=" + url_encode(token) + (query.empty() ? "" : "&" + query);
}

// ---- StreamInfo, Sample ----

StreamInfo StreamInfo::from_json(const json& j) {
    StreamInfo s;
    s.topic = str(j, "topic");
    s.name = str(j, "name", s.topic.substr(s.topic.rfind('.') + 1));
    s.id = str(j, "id");
    s.category = str(j, "category");
    s.kind = str(j, "kind");
    s.type = str(j, "type");
    if (j.contains("sensor_type") && j["sensor_type"].is_number_integer()) {
        s.sensor_type = j["sensor_type"].get<int>();
    } else if (s.id.rfind("sensors/", 0) == 0) {  // older app versions: "sensors/<type>/…"
        int t = std::atoi(s.id.c_str() + 8);
        if (t > 0) s.sensor_type = t;
    }
    s.stream_type = str(j, "stream_type");
    s.hardware = str(j, "hardware");
    s.vendor = str(j, "vendor");
    s.fields = strings(j, "fields");
    s.unit = str(j, "unit");
    s.units = strings(j, "units");
    if (s.units.empty()) s.units.assign(s.fields.size(), s.unit);
    s.reporting = str(j, "reporting", "continuous");
    if (j.contains("max_rate_hz") && j["max_rate_hz"].is_number()) s.max_rate_hz = j["max_rate_hz"].get<double>();
    s.wake_up = j.value("wake_up", false);
    s.basic = j.value("basic", true);
    s.shared = j.value("shared", true);
    s.raw = j;
    return s;
}

double Sample::operator[](const std::string& field) const {
    if (fields) {
        auto it = std::find(fields->begin(), fields->end(), field);
        if (it != fields->end()) return values.at(static_cast<std::size_t>(it - fields->begin()));
    }
    throw Error("No field \"" + field + "\" in " + topic);
}

// ---- internals ----

namespace detail {

struct SubState {
    std::weak_ptr<PhoneImpl> phone;
    StreamInfo info;
    std::shared_ptr<const std::vector<std::string>> fields;
    double rate = 0;
    std::function<void(const Sample&)> callback;

    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<Sample> queue;
    std::optional<Sample> latest;
    std::optional<std::int64_t> last_seq;
    std::uint64_t dropped = 0;
    std::string state, status;
    bool closed = false;
};

// NTP-style estimate of (phone boot clock − this computer's steady clock): keeps the recent
// exchange with the smallest round trip, which has the least error.
struct ClockSync {
    std::mutex m;
    std::deque<std::pair<std::int64_t, std::int64_t>> samples;  // (rtt, offset) in ns
    std::optional<std::int64_t> boot_to_unix_ns;
    std::int64_t mono_to_unix_ns = unix_ns() - mono_ns();

    void add(std::int64_t t0, std::int64_t t1, std::int64_t t2, std::int64_t t3) {
        std::int64_t rtt = (t3 - t0) - (t2 - t1);
        if (rtt < 0) return;
        std::lock_guard<std::mutex> lock(m);
        samples.emplace_back(rtt, ((t1 - t0) + (t2 - t3)) / 2);
        if (samples.size() > 16) samples.pop_front();
    }
    std::optional<double> round_trip_ms() {
        std::lock_guard<std::mutex> lock(m);
        if (samples.empty()) return std::nullopt;
        return std::min_element(samples.begin(), samples.end())->first / 1e6;
    }
    double to_unix(std::int64_t phone_ns) {
        std::lock_guard<std::mutex> lock(m);
        if (!samples.empty()) {
            auto offset = std::min_element(samples.begin(), samples.end())->second;
            return static_cast<double>(phone_ns - offset + mono_to_unix_ns) / 1e9;
        }
        if (boot_to_unix_ns) return static_cast<double>(phone_ns + *boot_to_unix_ns) / 1e9;
        return static_cast<double>(unix_ns()) / 1e9;
    }
};

struct PhoneImpl : std::enable_shared_from_this<PhoneImpl> {
    Link link;
    Options opt;
    ClockSync clock;

    mutable std::mutex m;  // guards everything below
    std::condition_variable cv;
    std::shared_ptr<WebSocket> ws;
    std::string device, app_version, last_error;
    std::vector<StreamInfo> catalog;
    std::map<std::string, std::shared_ptr<SubState>> subs;  // by topic
    std::map<std::int64_t, std::optional<json>> commands;   // id -> answer
    std::int64_t next_command = 1;
    bool ready = false;
    std::atomic<bool> closed{false};
    std::atomic<std::int64_t> last_rx{0};
    std::thread reader, syncer;

    void open() {
        auto sock = std::make_shared<WebSocket>();
        int status = 0;
        try {
            sock->open(link.host, link.port, "/ws?token=" + url_encode(link.token),
                       static_cast<int>(opt.timeout.count()), &status);
        } catch (const std::exception& e) {
            if (status == 401) throw TokenError(kTokenRejected);
            if (status) throw Error(std::string("Connection refused by the phone (") + e.what() + "). Check the token in the link.");
            throw Error("Can't reach the phone at " + link.host + ":" + std::to_string(link.port) + ": " + e.what() +
                        ". Is streaming started, and are you on the same network?");
        }
        {
            std::lock_guard<std::mutex> lock(m);
            ws = sock;
        }
        last_rx = mono_ns();
        send({{"op", "hello"}, {"client", opt.name}});
    }

    void send(const json& msg) {
        std::shared_ptr<WebSocket> w;
        {
            std::lock_guard<std::mutex> lock(m);
            w = ws;
        }
        if (!w || closed) return;
        try {
            w->send_text(msg.dump());
        } catch (const std::exception&) {
            // the reader thread notices and reconnects
        }
    }

    void subscribe_msg(const std::string& topic, double rate) {
        json msg = {{"op", "subscribe"}, {"topics", {topic}}};
        if (rate > 0) msg["rate_hz"] = rate;
        send(msg);
    }

    // Sleeps, but wakes at once on close(). Returns false if closed.
    bool sleep(milliseconds d) {
        std::unique_lock<std::mutex> lock(m);
        return !cv.wait_for(lock, d, [&] { return closed.load(); });
    }

    void wake_all_subs() {
        std::vector<std::shared_ptr<SubState>> all;
        {
            std::lock_guard<std::mutex> lock(m);
            for (auto& kv : subs) all.push_back(kv.second);
        }
        for (auto& s : all) {
            std::lock_guard<std::mutex> lock(s->m);
            s->cv.notify_all();
        }
    }

    void read_loop() {
        const int backoff_ms[] = {500, 1000, 2000, 5000};
        int attempt = 0;
        while (!closed) {
            std::shared_ptr<WebSocket> w;
            {
                std::lock_guard<std::mutex> lock(m);
                w = ws;
            }
            std::string msg;
            for (;;) {
                auto r = w->read(msg);
                if (r == WebSocket::Read::Closed || closed) break;
                if (r == WebSocket::Read::Timeout) continue;
                last_rx = mono_ns();
                try {
                    handle(json::parse(msg));
                } catch (const std::exception&) {
                    // a message we can't read: skip it
                }
            }
            if (w->close_code() == 1008) {
                std::lock_guard<std::mutex> lock(m);
                last_error = kTokenRejected;
                break;
            }
            if (closed || !opt.reconnect) break;
            // Reconnect and resume every subscription (Wi-Fi on a moving robot drops now and then).
            while (!closed) {
                int delay = backoff_ms[std::min(attempt++, 3)];
                std::fprintf(stderr, "allsense: lost the phone; reconnecting in %.1f s\n", delay / 1000.0);
                if (!sleep(milliseconds(delay))) break;
                try {
                    open();
                } catch (const TokenError&) {
                    std::lock_guard<std::mutex> lock(m);
                    last_error = kTokenRejected;
                    closed = true;
                    break;
                } catch (const Error&) {
                    continue;
                }
                std::vector<std::shared_ptr<SubState>> all;
                {
                    std::lock_guard<std::mutex> lock(m);
                    for (auto& kv : subs) all.push_back(kv.second);
                }
                for (auto& s : all) subscribe_msg(s->info.topic, s->rate);
                attempt = 0;
                std::fprintf(stderr, "allsense: reconnected\n");
                break;
            }
        }
        closed = true;
        cv.notify_all();
        wake_all_subs();
    }

    void sync_loop() {
        // A quick burst first for a good initial estimate, then every 2 s.
        auto every = std::min(milliseconds(2000), opt.stall_timeout / 3);
        for (int i = 0; sleep(i < 5 ? milliseconds(200) : every); ++i) {
            send({{"op", "time_sync"}, {"t0", mono_ns()}});
            // Wi-Fi that vanishes doesn't close the socket, so nothing at all for a while means a dead link.
            if (opt.reconnect && mono_ns() - last_rx > duration_cast<nanoseconds>(opt.stall_timeout).count()) {
                std::fprintf(stderr, "allsense: nothing from the phone for %.0f s; reconnecting\n",
                             opt.stall_timeout.count() / 1000.0);
                last_rx = mono_ns();
                std::lock_guard<std::mutex> lock(m);
                if (ws) ws->shutdown();
            }
        }
    }

    void handle(const json& j) {
        std::string op = str(j, "op");
        if (op == "data") {
            std::shared_ptr<SubState> s;
            {
                std::lock_guard<std::mutex> lock(m);
                auto it = subs.find(str(j, "topic"));
                if (it == subs.end()) return;
                s = it->second;
            }
            Sample sample;
            sample.topic = s->info.topic;
            sample.seq = j.value("seq", std::int64_t{0});
            sample.t = j.value("t", std::int64_t{0});
            sample.time = clock.to_unix(sample.t);
            sample.accuracy = j.value("acc", 0);
            if (j.contains("v") && j["v"].is_array())
                for (auto& v : j["v"])
                    sample.values.push_back(v.is_number() ? v.get<double>() : std::numeric_limits<double>::quiet_NaN());
            std::function<void(const Sample&)> cb;
            {
                std::lock_guard<std::mutex> lock(s->m);
                sample.fields = s->fields;
                if (s->last_seq && sample.seq > *s->last_seq + 1) s->dropped += sample.seq - *s->last_seq - 1;
                s->last_seq = sample.seq;
                s->latest = sample;
                // A sample means the stream runs, unless the phone has since said it stopped: a sample can still be
                // on its way after a not_shared or refused status. Only `subscribed` undoes those.
                if (s->state != "not_shared" && s->state != "refused") s->state = "streaming";
                if (s->queue.size() == kMaxQueue) {
                    s->queue.pop_front();
                    ++s->dropped;
                }
                s->queue.push_back(sample);
                s->cv.notify_all();
                cb = s->callback;
            }
            if (cb) {
                try {
                    cb(sample);
                } catch (const std::exception& e) {  // never let user code kill the reader thread
                    std::fprintf(stderr, "allsense: error in callback for %s: %s\n", sample.topic.c_str(), e.what());
                }
            }
        } else if (op == "time_sync") {
            auto t3 = mono_ns();
            if (j.contains("t0") && j["t0"].is_number() && j.contains("t1") && j.contains("t2"))
                clock.add(j["t0"].get<std::int64_t>(), j["t1"].get<std::int64_t>(), j["t2"].get<std::int64_t>(), t3);
        } else if (op == "hello") {
            std::lock_guard<std::mutex> lock(m);
            device = str(j, "device");
            app_version = str(j, "app_version");
            if (j.contains("boot_to_unix_ns") && j["boot_to_unix_ns"].is_number()) {
                std::lock_guard<std::mutex> clock_lock(clock.m);
                clock.boot_to_unix_ns = j["boot_to_unix_ns"].get<std::int64_t>();
            }
            if (j.contains("protocol") && j["protocol"] != 0)
                std::fprintf(stderr, "allsense: the phone speaks protocol %s; this library knows 0\n", j["protocol"].dump().c_str());
        } else if (op == "catalog") {
            std::vector<StreamInfo> fresh;
            for (auto& s : j.value("streams", json::array())) fresh.push_back(StreamInfo::from_json(s));
            std::lock_guard<std::mutex> lock(m);
            catalog = fresh;
            // Sent again when the user changes names or sharing: keep subscriptions' info current.
            for (auto& kv : subs) {
                for (auto& f : catalog) {
                    if (!f.id.empty() && f.id == kv.second->info.id && f.topic == kv.second->info.topic) {
                        std::lock_guard<std::mutex> sl(kv.second->m);
                        kv.second->info = f;
                    }
                }
            }
            device = str(j, "device", device);
            ready = true;
            cv.notify_all();
        } else if (op == "status" || op == "subscribed") {
            std::shared_ptr<SubState> s;
            {
                std::lock_guard<std::mutex> lock(m);
                auto it = subs.find(str(j, "topic"));
                if (it == subs.end()) return;
                s = it->second;
            }
            std::lock_guard<std::mutex> lock(s->m);
            if (op == "subscribed") {  // also sent when the user shares a stream that was waiting
                s->state = "streaming";
                s->status.clear();
            } else {
                s->state = str(j, "state");
                s->status = str(j, "reason", s->state);
                if (s->state == "refused" || s->state == "not_shared")
                    std::fprintf(stderr, "allsense: %s: %s\n", s->info.name.c_str(), s->status.c_str());
            }
        } else if (op == "command_result") {
            std::lock_guard<std::mutex> lock(m);
            auto it = commands.find(j.value("id", std::int64_t{-1}));
            if (it != commands.end()) {
                it->second = j;
                cv.notify_all();
            }
        } else if (op == "ping") {
            // Link test (the protocol's "Link test"): answer at once with the same id.
            send({{"op", "pong"}, {"id", j.contains("id") ? j["id"] : json()}});
        } else if (op == "error") {
            std::lock_guard<std::mutex> lock(m);
            last_error = str(j, "message");
            std::fprintf(stderr, "allsense: the phone says: %s\n", last_error.c_str());
        }
        // "fill" (link test padding) and unknown ops need no action.
    }

    void close() {
        if (closed.exchange(true)) {
            join();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m);
            if (ws) ws->shutdown();
        }
        cv.notify_all();
        wake_all_subs();
        join();
    }

    void join() {
        if (reader.joinable() && reader.get_id() != std::this_thread::get_id()) reader.join();
        if (syncer.joinable() && syncer.get_id() != std::this_thread::get_id()) syncer.join();
    }

    void unsubscribe(const std::shared_ptr<SubState>& s) {
        bool was = false;
        {
            std::lock_guard<std::mutex> lock(m);
            auto it = subs.find(s->info.topic);
            if (it != subs.end() && it->second == s) {
                subs.erase(it);
                was = true;
            }
        }
        if (was) send({{"op", "unsubscribe"}, {"topics", {s->info.topic}}});
    }
};

}  // namespace detail

// ---- Subscription ----

static detail::SubState& need(const std::shared_ptr<detail::SubState>& s) {
    if (!s) throw Error("Empty subscription");
    return *s;
}

static bool phone_closed(const detail::SubState& s) {
    auto p = s.phone.lock();
    return !p || p->closed;
}

std::optional<Sample> Subscription::get(milliseconds timeout) {
    auto& s = need(s_);
    std::unique_lock<std::mutex> lock(s.m);
    s.cv.wait_for(lock, timeout, [&] { return !s.queue.empty() || s.closed || phone_closed(s); });
    if (s.queue.empty()) return std::nullopt;
    Sample out = std::move(s.queue.front());
    s.queue.pop_front();
    return out;
}

Sample Subscription::get() {
    auto& s = need(s_);
    for (;;) {
        if (auto out = get(milliseconds(500))) return *out;
        std::lock_guard<std::mutex> lock(s.m);
        if (s.closed || phone_closed(s)) throw Error("Subscription to " + s.info.name + " is closed");
    }
}

std::optional<Sample> Subscription::latest() const {
    auto& s = need(s_);
    std::lock_guard<std::mutex> lock(s.m);
    return s.latest;
}

std::vector<Sample> Subscription::read(std::size_t max) {
    auto& s = need(s_);
    std::lock_guard<std::mutex> lock(s.m);
    std::size_t n = std::min(max, s.queue.size());
    std::vector<Sample> out(std::make_move_iterator(s.queue.begin()), std::make_move_iterator(s.queue.begin() + n));
    s.queue.erase(s.queue.begin(), s.queue.begin() + n);
    return out;
}

void Subscription::set_rate(double hz) {
    auto& s = need(s_);
    s.rate = hz;
    if (auto p = s.phone.lock()) p->subscribe_msg(s.info.topic, hz);
}

void Subscription::close() {
    auto& s = need(s_);
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.closed) return;
        s.closed = true;
        s.cv.notify_all();
    }
    if (auto p = s.phone.lock()) p->unsubscribe(s_);
}

const StreamInfo& Subscription::info() const { return need(s_).info; }

std::uint64_t Subscription::dropped() const {
    auto& s = need(s_);
    std::lock_guard<std::mutex> lock(s.m);
    return s.dropped;
}

std::string Subscription::state() const {
    auto& s = need(s_);
    std::lock_guard<std::mutex> lock(s.m);
    return s.state;
}

std::string Subscription::status() const {
    auto& s = need(s_);
    std::lock_guard<std::mutex> lock(s.m);
    return s.status;
}

bool Subscription::closed() const {
    auto& s = need(s_);
    std::lock_guard<std::mutex> lock(s.m);
    return s.closed || phone_closed(s);
}

// ---- Phone ----

static std::shared_ptr<detail::PhoneImpl> open_phone(const std::string& link, const Options& options) {
    auto p = std::make_shared<detail::PhoneImpl>();
    p->link = parse_link(link, options.token);
    p->opt = options;
    // Phones on Wi-Fi power save can miss the first attempt; try a few times.
    for (int attempt = 0;; ++attempt) {
        try {
            p->open();
            break;
        } catch (const TokenError&) {
            throw;
        } catch (const Error&) {
            if (attempt == 2) throw;
            std::this_thread::sleep_for(milliseconds(500));
        }
    }
    p->reader = std::thread([raw = p.get()] { raw->read_loop(); });
    {
        std::unique_lock<std::mutex> lock(p->m);
        p->cv.wait_for(lock, options.timeout, [&] { return p->ready || p->closed; });
    }
    if (!p->ready) {
        std::string reason = p->last_error.empty()
                                 ? "Connected, but the phone didn't answer. Check that the link's token is current "
                                   "(the app can change it) and that this is an AllSense phone."
                                 : p->last_error;
        p->close();
        if (reason == kTokenRejected) throw TokenError(reason);
        throw Error(reason);
    }
    p->syncer = std::thread([raw = p.get()] { raw->sync_loop(); });
    return p;
}

Phone::Phone(const std::string& link, const Options& options) {
    // A huge wait (e.g. milliseconds::max() for a robot that starts before the phone) means forever.
    auto deadline = options.wait >= hours(24 * 365 * 100) ? steady_clock::time_point::max() : steady_clock::now() + options.wait;
    for (;;) {
        try {
            p_ = open_phone(link, options);
            return;
        } catch (const TokenError&) {
            throw;
        } catch (const Error& e) {
            if (steady_clock::now() >= deadline) throw;
            std::fprintf(stderr, "allsense: %s Retrying…\n", e.what());
            std::this_thread::sleep_for(std::min(milliseconds(2000),
                duration_cast<milliseconds>(deadline - steady_clock::now()) + milliseconds(1)));
        }
    }
}

Phone::~Phone() { close(); }
Phone::Phone(Phone&&) noexcept = default;
Phone& Phone::operator=(Phone&& other) noexcept {
    if (this != &other) {
        close();
        p_ = std::move(other.p_);
    }
    return *this;
}

static detail::PhoneImpl& need(const std::shared_ptr<detail::PhoneImpl>& p) {
    if (!p) throw Error("Phone was moved from");
    return *p;
}

std::string Phone::device() const {
    auto& p = need(p_);
    std::lock_guard<std::mutex> lock(p.m);
    return p.device;
}

std::vector<StreamInfo> Phone::catalog() const {
    auto& p = need(p_);
    std::lock_guard<std::mutex> lock(p.m);
    return p.catalog;
}

std::vector<StreamInfo> Phone::streams(bool all) const {
    std::vector<StreamInfo> out;
    for (auto& s : catalog())
        if (all || s.basic) out.push_back(s);
    return out;
}

StreamInfo Phone::find(const std::string& name) const {
    auto list = catalog();
    for (auto& s : list)
        if (name == s.name || name == s.category + "." + s.name || name == s.topic || name == s.id) return s;
    std::vector<std::pair<std::size_t, std::string>> close;
    for (auto& s : list) {
        auto d = edit_distance(name, s.name);
        if (d <= std::max<std::size_t>(2, s.name.size() / 3)) close.emplace_back(d, s.name);
    }
    std::sort(close.begin(), close.end());
    close.erase(std::unique(close.begin(), close.end()), close.end());
    std::string hint = " See phone.streams() for names.";
    if (!close.empty()) {
        hint = " Did you mean ";
        for (std::size_t i = 0; i < close.size() && i < 3; ++i) hint += (i ? ", " : "") + close[i].second;
        hint += "?";
    }
    throw Error("No stream called \"" + name + "\" on " + device() + "." + hint);
}

Subscription Phone::subscribe(const std::string& name, double rate, std::function<void(const Sample&)> callback) {
    auto& p = need(p_);
    if (p.closed) throw Error("The connection to the phone is closed");
    auto info = find(name);
    auto s = std::make_shared<detail::SubState>();
    s->phone = p_;
    s->info = info;
    s->fields = std::make_shared<const std::vector<std::string>>(info.fields);
    s->rate = rate;
    s->callback = std::move(callback);
    if (!info.shared)
        std::fprintf(stderr, "allsense: %s isn't shared over the network; it starts once you share it in the app "
                             "(Home → Connect → Browser → Streams it may send)\n", info.name.c_str());
    std::shared_ptr<detail::SubState> old;
    {
        std::lock_guard<std::mutex> lock(p.m);
        auto& slot = p.subs[info.topic];
        old = slot;
        slot = s;
    }
    if (old) {
        std::lock_guard<std::mutex> lock(old->m);
        old->closed = true;
        old->cv.notify_all();
    }
    p.subscribe_msg(info.topic, rate);
    return Subscription(s);
}

std::vector<Subscription> Phone::subscribe(const std::vector<std::string>& names, double rate) {
    std::vector<Subscription> out;
    for (auto& n : names) out.push_back(subscribe(n, rate));
    return out;
}

json Phone::command(const std::string& name, const json& args) {
    auto& p = need(p_);
    json msg = args.is_object() ? args : json::object();
    std::int64_t id;
    {
        std::lock_guard<std::mutex> lock(p.m);
        id = p.next_command++;
        p.commands[id] = std::nullopt;
    }
    msg["op"] = "command";
    msg["id"] = id;
    msg["command"] = name;
    p.send(msg);
    std::optional<json> result;
    {
        std::unique_lock<std::mutex> lock(p.m);
        p.cv.wait_for(lock, p.opt.timeout, [&] { return p.commands[id].has_value() || p.closed; });
        result = p.commands[id];
        p.commands.erase(id);
    }
    if (!result) throw Error("No answer to the \"" + name + "\" command (older app without commands?)");
    if (!result->value("ok", false)) throw Error(name + ": " + str(*result, "error", "failed"));
    return *result;
}

std::vector<StreamInfo> Phone::cameras() const {
    std::vector<StreamInfo> out;
    for (auto& s : catalog())
        if (s.kind == "image") out.push_back(s);
    return out;
}

std::vector<StreamInfo> Phone::microphones() const {
    std::vector<StreamInfo> out;
    for (auto& s : catalog())
        if (s.kind == "audio") out.push_back(s);
    return out;
}

static std::string media_path(const Phone& phone, const std::string& name, const std::string& kind) {
    auto info = phone.find(name);
    if (info.kind != kind) {
        std::string list;
        for (auto& s : kind == "image" ? phone.cameras() : phone.microphones()) list += (list.empty() ? "" : ", ") + s.name;
        throw Error(info.name + (kind == "image" ? " isn't a camera; cameras: " : " isn't a microphone; microphones: ") +
                    (list.empty() ? "none" : list));
    }
    std::string url = str(info.raw, "url");
    if (url.empty()) url = (kind == "image" ? "/camera/" : "/mic/") + info.name + (kind == "image" ? ".mjpg" : ".wav");
    return url;
}

static void replace_ext(std::string& path, const std::string& from, const std::string& to) {
    if (path.size() >= from.size() && path.compare(path.size() - from.size(), from.size(), from) == 0)
        path.replace(path.size() - from.size(), from.size(), to);
}

std::string Phone::snapshot(const std::string& camera, const std::string& size, std::optional<int> quality,
                            std::optional<double> zoom) {
    auto& p = need(p_);
    std::string path = media_path(*this, camera, "image");
    replace_ext(path, ".mjpg", ".jpg");
    std::string query = "token=" + url_encode(p.link.token) + "&size=" + url_encode(size);
    if (quality) query += "&quality=" + std::to_string(*quality);
    if (zoom) query += "&zoom=" + fmt_double(*zoom);
    detail::HttpResponse r;
    try {
        r = detail::http_get(p.link.host, p.link.port, path + "?" + query, static_cast<int>(p.opt.timeout.count()) + 8000);
    } catch (const std::exception& e) {
        throw Error(std::string("Snapshot from ") + camera + " failed: " + e.what());
    }
    if (r.status == 401) throw TokenError(kTokenRejected);
    if (r.status != 200) throw Error("Snapshot from " + camera + ": HTTP " + std::to_string(r.status) + " " + r.body);
    return r.body;
}

std::string Phone::camera_url(const std::string& camera, const CameraOptions& o) const {
    auto& p = need(p_);
    std::string path = media_path(*this, camera, "image");
    if (o.codec == "h264") {
        std::string h264 = str(find(camera).raw, "h264_url");
        if (!h264.empty()) path = h264;
        else replace_ext(path, ".mjpg", ".h264");
    } else if (o.codec != "mjpeg") {
        throw Error("codec is \"mjpeg\" or \"h264\", not \"" + o.codec + "\"");
    }
    std::string q = "size=" + url_encode(o.size) + "&fps=" + std::to_string(o.fps);
    if (o.quality) q += "&quality=" + std::to_string(*o.quality);
    if (o.zoom) q += "&zoom=" + fmt_double(*o.zoom);
    if (o.bitrate) q += "&bitrate=" + std::to_string(*o.bitrate);
    if (o.raw && o.codec == "h264") q += "&raw=1";
    return p.link.http_url(path, q);
}

std::string Phone::microphone_url(const std::string& mic, int rate) const {
    return need(p_).link.http_url(media_path(*this, mic, "audio"), "rate=" + std::to_string(rate));
}

std::vector<StreamInfo> Phone::refresh_catalog() {
    auto& p = need(p_);
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.ready = false;
    }
    p.send({{"op", "catalog"}});
    std::unique_lock<std::mutex> lock(p.m);
    p.cv.wait_for(lock, p.opt.timeout, [&] { return p.ready || p.closed; });
    return p.catalog;
}

std::optional<double> Phone::round_trip_ms() const { return need(p_).clock.round_trip_ms(); }

std::string Phone::last_error() const {
    auto& p = need(p_);
    std::lock_guard<std::mutex> lock(p.m);
    return p.last_error;
}

bool Phone::closed() const { return !p_ || p_->closed; }

void Phone::close() {
    if (p_) p_->close();
}

}  // namespace allsense
