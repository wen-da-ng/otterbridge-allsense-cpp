// The shared client scenarios (allsense-conformance, from allsense-testkit) for the C++ client:
//
//     allsense-conformance -- build/conformance
//
// Called as `conformance <link> <scenario>`; prints PASS when the scenario passed.
#include <allsense/allsense.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using allsense::Error;

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

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// Tells the driver to act on the phone, then waits for its GO; returns what follows GO.
std::string wait_for(const std::string& action) {
    std::cout << "WAIT " << action << std::endl;
    std::string line;
    std::getline(std::cin, line);
    CHECK(line.rfind("GO", 0) == 0);
    return line.size() > 3 ? line.substr(3) : "";
}

double now_unix() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Waits up to `timeout` for the subscription's state to become `state`.
bool state_becomes(const allsense::Subscription& sub, const std::string& state, std::chrono::milliseconds timeout) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (sub.state() == state) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

void unit() {
    auto l = allsense::parse_link("http://192.168.1.20:8765/?token=abcd");
    CHECK(l.host == "192.168.1.20" && l.port == 8765 && l.token == "abcd");
    CHECK(l.ws_url() == "ws://192.168.1.20:8765/ws?token=abcd");
    l = allsense::parse_link("10.0.0.5", "tok");
    CHECK(l.host == "10.0.0.5" && l.port == 8765 && l.token == "tok");
    l = allsense::parse_link("localhost:8766", "tok");
    CHECK(l.host == "localhost" && l.port == 8766);
    l = allsense::parse_link("ws://[fe80::1]:9000/ws?catalog=none&token=a%2Bb");
    CHECK(l.host == "fe80::1" && l.port == 9000 && l.token == "a+b");
    CHECK(l.http_url("/camera/back.jpg", "size=640x480") == "http://[fe80::1]:9000/camera/back.jpg?token=a%2Bb&size=640x480");
    bool token_error = false;
    try {
        allsense::parse_link("http://192.168.1.20:8765/");
    } catch (const allsense::TokenError&) {
        token_error = true;
    }
    CHECK(token_error);
    CHECK(contains(throws([] { allsense::parse_link("https://192.168.1.20:8765/?token=a"); }), "TLS"));
    CHECK(contains(throws([] { allsense::parse_link("wss://192.168.1.20:8765/ws?token=a"); }), "TLS"));
    CHECK(allsense::parse_link("http://h/?token=a%zz%4").token == "a%zz%4");  // bad escapes stay as they are
}

void basics(const std::string& link) {
    allsense::Options opt;
    opt.name = "conformance";
    auto phone = allsense::connect(link, opt);
    CHECK(phone.device() == "rover");

    // Catalog: Basic hides vendor streams, all=true shows them.
    auto basic = phone.streams();
    auto all = phone.streams(true);
    CHECK(all.size() > basic.size());
    for (auto& s : basic) CHECK(s.name != "locked");
    auto accel_info = phone.find("accelerometer");
    CHECK(accel_info.fields.size() == 3 && accel_info.fields[2] == "z");
    CHECK(accel_info.unit == "m/s²" && accel_info.sensor_type == 1 && accel_info.continuous());
    CHECK(phone.find("sensors.accelerometer").topic == accel_info.topic);
    CHECK(phone.find(accel_info.topic).id == accel_info.id);
    CHECK(phone.find(accel_info.id).name == "accelerometer");
    auto pos_info = phone.find("position");
    CHECK(!pos_info.sensor_type && pos_info.units.size() == 7 && pos_info.units[0] == "°");
    CHECK(contains(throws([&] { phone.find("acelerometer"); }), "Did you mean accelerometer"));

    // Samples: the fake phone's z axis is 9.81, time is on this computer's clock.
    auto accel = phone.subscribe("accelerometer", 100);
    auto first = accel.get(3s);
    CHECK(first.has_value());
    CHECK(std::fabs((*first)["z"] - 9.81) < 1e-9 && std::fabs((*first)[2] - 9.81) < 1e-9);
    CHECK(first->fields && first->fields->size() == 3);
    CHECK(std::fabs(first->time - now_unix()) < 2.0);
    CHECK(!throws([&] { (void)(*first)["w"]; }).empty());
    auto second = accel.get();
    CHECK(second.seq > first->seq && second.t >= first->t);  // the fake phone's clock ticks every ~15 ms on Windows
    std::this_thread::sleep_for(300ms);
    CHECK(accel.latest().has_value());
    auto batch = accel.read();
    CHECK(batch.size() >= 5);  // ~30 at 100 Hz; busy CI machines deliver fewer
    CHECK(accel.state() == "streaming");
    accel.set_rate(10);
    std::this_thread::sleep_for(300ms);
    accel.read();
    std::this_thread::sleep_for(1000ms);
    auto slow = accel.read().size();
    CHECK(slow >= 3 && slow <= 16);  // ~10 at 10 Hz

    // Several at once, and a callback.
    auto pair = phone.subscribe(std::vector<std::string>{"gyroscope", "magnetic_field"}, 50);
    CHECK(pair.size() == 2);
    CHECK(pair[0].get(3s).has_value() && pair[1].get(3s).has_value());
    CHECK(std::fabs(pair[0].get()["y"] + 0.2) < 1e-9);
    std::atomic<int> calls{0};
    auto light = phone.subscribe("light", 0, [&](const allsense::Sample& s) {
        if (!std::isnan(s[0])) ++calls;
    });
    std::this_thread::sleep_for(500ms);
    CHECK(calls > 0);

    // States: refused and not shared.
    auto locked = phone.subscribe("locked");
    CHECK(state_becomes(locked, "refused", 2s));
    CHECK(contains(locked.status(), "Android"));
    CHECK(!locked.get(300ms).has_value());
    auto hr = phone.subscribe("heart_rate");
    CHECK(state_becomes(hr, "not_shared", 2s));

    // Null values are NaN.
    auto pos = phone.subscribe("position");
    auto fix = pos.get(3s);
    CHECK(fix.has_value());
    CHECK(std::fabs((*fix)["latitude"] - 51.477928123456) < 1e-12);  // full double precision
    CHECK(std::isnan((*fix)["bearing"]));

    // Close stops a subscription.
    pair[1].close();
    CHECK(pair[1].closed());
    pair[1].read();  // samples queued before close() can still be read
    CHECK(!throws([&] { pair[1].get(); }).empty());

    // Commands.
    auto r = phone.command("vibrate", {{"ms", 50}});
    CHECK(r.value("ok", false));
    auto v = phone.command("volume");
    CHECK(v.contains("level"));
    CHECK(contains(throws([&] { phone.command("fly"); }), "fly"));

    // Clock sync: the first burst runs within a second.
    std::this_thread::sleep_for(1200ms);
    CHECK(phone.round_trip_ms().has_value() && *phone.round_trip_ms() >= 0);

    // Media.
    CHECK(phone.cameras().size() >= 1 && phone.microphones().size() >= 1);
    auto jpeg = phone.snapshot("back");
    CHECK(jpeg.size() > 4 && (unsigned char)jpeg[0] == 0xFF && (unsigned char)jpeg[1] == 0xD8);
    CHECK(contains(throws([&] { phone.snapshot("accelerometer"); }), "isn't a camera"));
    allsense::CameraOptions co;
    co.codec = "h264";
    co.raw = true;
    auto url = phone.camera_url("back", co);
    CHECK(contains(url, "/camera/back.h264?token=testtoken") && contains(url, "raw=1"));
    CHECK(contains(phone.camera_url("back"), "/camera/back.mjpg?token=testtoken&size=640x480&fps=15"));
    CHECK(contains(phone.microphone_url("mic", 16000), "/mic/mic.wav?token=testtoken&rate=16000"));

    CHECK(light.dropped() == 0);
    phone.close();
    CHECK(phone.closed() && accel.closed());
    accel.read();
    CHECK(!throws([&] { accel.get(); }).empty());
}

void share(const std::string& link) {
    auto phone = allsense::connect(link);
    auto hr = phone.subscribe("heart_rate");
    CHECK(state_becomes(hr, "not_shared", 2s));
    wait_for("share heart_rate");
    auto s = hr.get(3s);  // starts by itself, no resubscribe
    CHECK(s.has_value());
    // The first sample can arrive just before `subscribed`; the state follows that message.
    CHECK(state_becomes(hr, "streaming", 2s));
    CHECK(hr.status().empty());
}

void reconnect(const std::string& link) {
    auto phone = allsense::connect(link);
    auto accel = phone.subscribe("accelerometer", 50);
    CHECK(accel.get(3s).has_value());
    wait_for("restart");
    accel.read();
    auto end = std::chrono::steady_clock::now() + 15s;
    std::optional<allsense::Sample> s;
    while (!s && std::chrono::steady_clock::now() < end) s = accel.get(500ms);
    CHECK(s.has_value());
    CHECK(!phone.closed());
}

void stall(const std::string& link) {
    auto phone = allsense::connect(link);
    auto accel = phone.subscribe("accelerometer", 50);
    CHECK(accel.get(3s).has_value());
    wait_for("freeze");  // silent for 8 s; the 6 s watchdog reconnects
    accel.read();
    auto end = std::chrono::steady_clock::now() + 20s;
    std::optional<allsense::Sample> s;
    while (!s && std::chrono::steady_clock::now() < end) s = accel.get(500ms);
    CHECK(s.has_value());
}

void ping(const std::string& link) {
    auto phone = allsense::connect(link);
    auto rtt = wait_for("ping");
    CHECK(!rtt.empty() && rtt != "none");
}

void token(const std::string& link) {
    auto start = std::chrono::steady_clock::now();
    bool token_error = false;
    try {
        allsense::connect(link);
    } catch (const allsense::TokenError&) {
        token_error = true;
    }
    CHECK(token_error);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}

int main(int argc, char** argv) {
    if (argc < 3) return std::puts("usage: conformance <link> <scenario>"), 2;
    std::string link = argv[1], scenario = argv[2];
    try {
        if (scenario == "unit") unit();
        else if (scenario == "basics") basics(link);
        else if (scenario == "share") share(link);
        else if (scenario == "reconnect") reconnect(link);
        else if (scenario == "stall") stall(link);
        else if (scenario == "ping") ping(link);
        else if (scenario == "token") token(link);
        else return std::cout << "unknown scenario " << scenario << std::endl, 2;
    } catch (const std::exception& e) {
        std::cout << "FAILED with " << e.what() << std::endl;
        return 1;
    }
    std::cout << "PASS" << std::endl;
    return 0;
}
