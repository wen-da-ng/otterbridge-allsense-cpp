# allsense (C++)

Stream sensors from an Android phone running **OtterBridge AllSense** into C++: for robots, control loops and data logging on Linux, Windows or macOS (a Raspberry Pi or Jetson on the robot, or your laptop).

```cpp
#include <allsense/allsense.hpp>

auto phone = allsense::connect("http://192.168.1.20:8765/?token=abcd");  // link from the app
auto accel = phone.subscribe("accelerometer", 100);
for (;;) {
    allsense::Sample s = accel.get();
    std::printf("%f %f %f %f\n", s.time, s["x"], s["y"], s["z"]);
}
```

## Install

C++17 and CMake 3.14+. The only dependency is [nlohmann/json](https://github.com/nlohmann/json) (header-only): an installed copy is used if found, otherwise CMake downloads it. Networking is built in (no Boost, no OpenSSL).

In your `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(allsense GIT_REPOSITORY https://github.com/wen-da-ng/otterbridge-allsense-cpp.git GIT_TAG main)
FetchContent_MakeAvailable(allsense)
target_link_libraries(my_robot PRIVATE allsense::allsense)
```

Or from a copy of this repository: `add_subdirectory(path/to/otterbridge-allsense-cpp)`.

To build the examples here:

```
cmake -S . -B build
cmake --build build
build/quickstart "http://192.168.1.20:8765/?token=abcd"
```

## Getting started

1. In the AllSense app, tap **Start streaming** on Home.
2. Copy the link from **Connect → Programming** (`http://<phone-ip>:8765/?token=…`). It contains the pairing token.
3. Your computer must be on the same Wi-Fi, connected to the phone's hotspot, or connected by USB:
   - **USB tethering:** turn it on in Android's settings, then use the link the app shows.
   - **adb:** run `adb forward tcp:8766 tcp:8765`, then use `http://localhost:8766/?token=…`.

## Guide

**Names.** Streams use the names you give them in the app (Rename). `phone.subscribe("mag_left")` also accepts `category.name` (`robot.accelerometer`), the full topic (`allsense.rover.sensors.mag_left`) or the stable hardware `id`. A wrong name throws `allsense::Error` with a "Did you mean …?" hint.

```cpp
for (auto& info : phone.streams())    // phone.streams(true) adds vendor-specific sensors, wake-up copies and raw variants
    std::cout << info.name << " " << info.type << " " << info.unit << "\n";
```

**Reading samples.** Use whichever style fits your program:

```cpp
auto accel = phone.subscribe("accelerometer", 200);

allsense::Sample s = accel.get();                    // wait for the next sample
std::optional<allsense::Sample> n = accel.get(1s);   // or give up after a timeout
std::optional<allsense::Sample> l = accel.latest();  // newest sample, never waits (control loops)
std::vector<allsense::Sample> batch = accel.read();  // everything queued since the last call

accel.set_rate(50);
accel.close();
```

Several at once, or with a callback (runs on the reader thread; keep it quick):

```cpp
auto subs = phone.subscribe(std::vector<std::string>{"accelerometer", "gyroscope"}, 100);
auto light = phone.subscribe("light", 0, [](const allsense::Sample& s) { std::cout << "lux " << s[0] << "\n"; });
```

A `Subscription` is a cheap handle (copies share one queue). The stream runs until `close()` or until the `Phone` is destroyed.

**Samples.** `s.values` is a `std::vector<double>`; `s["x"]` or `s[0]` picks one value. Missing values (no GNSS bearing while standing still, for example) are NaN.
- `s.t`: the phone's **hardware** timestamp (ns on its boot clock), the moment the sensor measured.
- `s.time`: that same moment as Unix seconds on **your computer's clock**, using automatic clock sync. Use it to line up phone data with your robot's other data. `phone.round_trip_ms()` shows the sync quality.
- For intervals between samples, use `s.t`: it comes from one clock and never jumps when the sync improves.
- `s.seq` counts up; `sub.dropped()` counts samples lost on the network or not read in time. If your code is slower than the stream, the **oldest** queued samples are dropped (1000 are kept), so you always get fresh data.

**States.** `sub.state()` is `"streaming"`, `"armed"` (waiting for a first reading, e.g. a GNSS fix), `"refused"` (no data will come; `sub.status()` says why) or `"not_shared"` (the user doesn't share it over the network; it starts by itself once they do).

**Connection drops.** The client reconnects and resubscribes by itself, and treats 6 s of silence as a dead link (Wi-Fi that vanishes doesn't close sockets). A robot that boots before the phone can wait for it:

```cpp
allsense::Options opt;
opt.name = "my_robot";                 // shown in the app's client list
opt.wait = std::chrono::milliseconds::max();  // keep trying while the phone isn't reachable yet
auto phone = allsense::connect(link, opt);
```

A wrong token throws `allsense::TokenError` at once; other failures throw `allsense::Error`.

**Cameras and microphones.** Video and audio come over HTTP on the same link:

```cpp
std::string jpeg = phone.snapshot("back");                       // one JPEG's bytes
allsense::CameraOptions o;
o.codec = "h264"; o.raw = true;
cv::VideoCapture cap(phone.camera_url("back", o));                // OpenCV (with FFmpeg) decodes H.264
std::string wav = phone.microphone_url("mic", 16000);             // live WAV, mono 16-bit
```

Frames are as the sensor sees them: rotate by `info.raw["sensor_orientation"]` degrees clockwise to show them upright.

**Commands.** If allowed in the app (Robot → Robot can control the phone):

```cpp
phone.command("vibrate", {{"ms", 300}});
phone.command("say", {{"text", "Battery low"}});
phone.command("display", {{"face", "happy"}, {"text", "Charging"}});
auto level = phone.command("volume")["level"];
```

Arguments and answers are `nlohmann::json` (`allsense::json`). A refused command throws `allsense::Error` with the phone's reason.

## Reference

| Call | Does |
|---|---|
| `allsense::connect(link, options)` | Connects; `Options`: `name`, `token`, `timeout`, `wait`, `reconnect`, `stall_timeout` |
| `phone.streams(all)`, `phone.find(name)` | Shared streams (`StreamInfo`: name, topic, id, fields, units, …) |
| `phone.subscribe(name, rate, callback)` | A `Subscription`, or a vector for several names |
| `sub.get()`, `sub.get(timeout)`, `sub.latest()`, `sub.read()` | Samples |
| `sub.set_rate(hz)`, `sub.close()`, `sub.state()`, `sub.dropped()` | Control and status |
| `phone.command(name, args)` | Runs a command, if Commands is on in the app |
| `phone.snapshot(camera)`, `phone.camera_url(camera, options)`, `phone.microphone_url(mic, rate)` | Media over HTTP |
| `phone.round_trip_ms()`, `phone.refresh_catalog()`, `phone.close()` | Clock sync quality, names after a rename, disconnect |

## Examples

See [`examples/`](examples): the quickstart, printing values, and logging several sensors to CSV.

## Tests

`tests/conformance.cpp` runs the shared client scenarios against the fake phone, with `allsense-conformance` from the test kit ([otterbridge-allsense-protocol](https://github.com/wen-da-ng/otterbridge-allsense-protocol)):

```
pip install allsense-testkit   # or: pip install path/to/otterbridge-allsense-protocol
allsense-conformance --example build/print_values -- build/conformance
```

`tests/unit.cpp` tests the limits on what a phone may send against a loopback server: `ctest --test-dir build`.

## Protocol

The client speaks the AllSense protocol over WebSocket ([spec](https://docs.allsense.otterbridge.org/PROTOCOL/), [otterbridge-allsense-protocol](https://github.com/wen-da-ng/otterbridge-allsense-protocol)). Version 0 is experimental and may still change. Links are plain `http://` or `ws://` (the phone has no TLS; `https://`/`wss://` links are refused). The client refuses a message over 4 MiB or nested deeper than 64, and a snapshot over 32 MiB.

## License

MIT. This library and the protocol are MIT, so you can use them in any project. The AllSense app is a free download from [otterbridge-allsense](https://github.com/wen-da-ng/otterbridge-allsense).
