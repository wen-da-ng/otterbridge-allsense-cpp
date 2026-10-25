// AllSense client for C++17: stream an Android phone's sensors into your program.
//
//     auto phone = allsense::connect("http://192.168.1.20:8765/?token=abcd");  // the link from the app
//     auto accel = phone.subscribe("accelerometer", 100);
//     for (;;) {
//         allsense::Sample s = accel.get();
//         std::printf("%f %f %f\n", s["x"], s["y"], s["z"]);
//     }
//
// A background thread reads from the phone, keeps the clock in sync and reconnects if the link
// drops; your code just takes samples. Protocol: https://docs.allsense.otterbridge.org/PROTOCOL/. MIT licensed.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace allsense {

using json = nlohmann::json;

/// Something went wrong talking to the phone (bad link, unreachable, unknown stream, closed…).
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The link has no pairing token, or the phone rejected it. Retrying won't help: get the current link.
class TokenError : public Error {
public:
    using Error::Error;
};

/// Where the phone is: parsed from the app's link (http://ip:port/?token=…), a ws:// URL, "ip" or "ip:port".
struct Link {
    std::string host;
    int port = 8765;
    std::string token;

    std::string ws_url() const;                                         // ws://host:port/ws?token=…
    std::string http_url(const std::string& path, const std::string& query = "") const;  // http://host:port<path>?token=…&query
};

/// Throws TokenError if neither the link nor `token` has a token.
Link parse_link(const std::string& link, const std::string& token = "");

/// One stream the phone offers, from its catalog.
struct StreamInfo {
    std::string topic;         // allsense.<device>.<category>.<name>
    std::string name;          // the name chosen in the app, e.g. "accelerometer"
    std::string id;            // stable hardware ID; survives renames and reboots
    std::string category;      // "sensors", "gnss", "battery", "wifi", "cellular", "robot", "vision", "camera", "mic"
    std::string kind;          // "vector", "image" (camera) or "audio" (microphone)
    std::string type;          // human-readable, e.g. "Accelerometer"
    std::optional<int> sensor_type;  // Android Sensor.TYPE_*, e.g. 1 = accelerometer; empty for non-sensors
    std::string stream_type;   // machine-readable: "android.sensor.accelerometer", "allsense.gnss.position"…
    std::string hardware;
    std::string vendor;
    std::vector<std::string> fields;
    std::string unit;          // shared by all fields, or "" when they differ (then see units)
    std::vector<std::string> units;  // one per field
    std::string reporting;     // "continuous", "on change", "one shot" or "special trigger"
    std::optional<double> max_rate_hz;
    bool wake_up = false;
    bool basic = true;         // false: only shown in the app's Advanced mode
    bool shared = true;        // false: the user hasn't shared it over the network
    json raw;                  // the catalog entry as sent (url, facing, zoom_range…)

    bool continuous() const { return reporting == "continuous"; }
    static StreamInfo from_json(const json& j);
};

/// One reading. `s[0]` or `s["x"]` picks a value; missing values (JSON null) are NaN.
struct Sample {
    std::string topic;
    std::int64_t seq = 0;      // counts up per subscription; gaps are lost samples
    std::int64_t t = 0;        // the phone's hardware timestamp, ns on its boot clock (use for intervals)
    double time = 0;           // the same moment as Unix seconds on *this* computer's clock (clock-synced)
    std::vector<double> values;
    int accuracy = 0;          // Android's: 3 high, 2 medium, 1 low, 0 unreliable
    std::shared_ptr<const std::vector<std::string>> fields;  // labels for values

    double operator[](std::size_t i) const { return values.at(i); }
    double operator[](const std::string& field) const;  // throws Error for an unknown field
};

namespace detail {
struct PhoneImpl;
struct SubState;
}

/// Samples from one stream. A cheap handle: copies share the same queue. Keeps the newest 1000
/// samples; if your code is slower than the stream, the oldest are dropped (counted in dropped()).
/// The stream runs until close() or until the Phone is closed.
class Subscription {
public:
    Subscription() = default;

    Sample get();                                         // waits for the next sample; throws Error once closed
    std::optional<Sample> get(std::chrono::milliseconds timeout);  // empty after the timeout
    std::optional<Sample> latest() const;                 // the newest sample, never waits (control loops)
    std::vector<Sample> read(std::size_t max = SIZE_MAX); // everything queued since the last call
    void set_rate(double hz);                             // continuous streams only
    void close();                                         // stops the stream

    const StreamInfo& info() const;
    const std::string& topic() const { return info().topic; }
    std::uint64_t dropped() const;
    /// "" until the phone answers, then "streaming", "armed" (waiting for a first reading),
    /// "refused" (no data will come; see status()) or "not_shared" (starts once the user shares it).
    std::string state() const;
    std::string status() const;                           // the phone's reason, for refused/not_shared
    bool closed() const;

private:
    friend class Phone;
    explicit Subscription(std::shared_ptr<detail::SubState> s) : s_(std::move(s)) {}
    std::shared_ptr<detail::SubState> s_;
};

struct Options {
    std::string token;                                    // if the link has none
    std::string name = "cpp";                             // shown in the app's client list
    std::chrono::milliseconds timeout{5000};              // to connect and for commands
    bool reconnect = true;                                // reconnect and resubscribe if the link drops
    std::chrono::milliseconds stall_timeout{6000};        // nothing at all for this long = the link is dead
    std::chrono::milliseconds wait{0};                    // keep trying this long while the phone isn't reachable yet
};

struct CameraOptions {
    std::string codec = "mjpeg";                          // or "h264"
    std::string size = "640x480";
    int fps = 15;
    std::optional<int> quality;                           // JPEG 10–95
    std::optional<double> zoom;                           // see info.raw["zoom_range"]
    std::optional<int> bitrate;                           // H.264, bits/s
    bool raw = false;                                     // H.264 as a plain Annex B stream (ffmpeg, OpenCV, VLC)
};

/// A connection to one phone. Closes when destroyed.
class Phone {
public:
    explicit Phone(const std::string& link, const Options& options = {});
    ~Phone();
    Phone(Phone&&) noexcept;
    Phone& operator=(Phone&&) noexcept;
    Phone(const Phone&) = delete;
    Phone& operator=(const Phone&) = delete;

    std::string device() const;                           // the phone's device name
    std::vector<StreamInfo> catalog() const;              // everything, including Advanced-only streams
    std::vector<StreamInfo> streams(bool all = false) const;  // the app's Basic set, or all
    /// By name ("accelerometer"), category and name ("robot.accelerometer"), topic or stable ID.
    /// Throws Error with a "Did you mean …?" hint.
    StreamInfo find(const std::string& name) const;

    /// `rate` in Hz for continuous streams (default 50, max 1000, limited by the sensor; 0 = default).
    /// `callback` runs on the reader thread for every sample: keep it quick.
    Subscription subscribe(const std::string& name, double rate = 0,
                           std::function<void(const Sample&)> callback = nullptr);
    std::vector<Subscription> subscribe(const std::vector<std::string>& names, double rate = 0);

    /// Runs a command (https://docs.allsense.otterbridge.org/PROTOCOL/#commands): command("vibrate", {{"ms", 300}}).
    /// Returns the phone's answer; throws Error with its reason (e.g. commands not allowed in the app).
    json command(const std::string& name, const json& args = json::object());

    std::vector<StreamInfo> cameras() const;
    std::vector<StreamInfo> microphones() const;
    std::string snapshot(const std::string& camera, const std::string& size = "640x480",
                         std::optional<int> quality = {}, std::optional<double> zoom = {});  // one JPEG's bytes
    /// The camera's video URL with the token, e.g. for cv::VideoCapture or ffmpeg.
    std::string camera_url(const std::string& camera, const CameraOptions& options = {}) const;
    /// Live WAV (mono 16-bit) URL with the token.
    std::string microphone_url(const std::string& mic = "mic", int rate = 16000) const;

    std::vector<StreamInfo> refresh_catalog();            // asks again, e.g. after renaming in the app
    std::optional<double> round_trip_ms() const;          // best recent network round trip (clock sync quality)
    std::string last_error() const;
    bool closed() const;
    void close();

private:
    std::shared_ptr<detail::PhoneImpl> p_;
};

/// Connects to a phone: `link` is the one from the app (Home → Connect), with the token.
inline Phone connect(const std::string& link, const Options& options = {}) { return Phone(link, options); }

}  // namespace allsense
