# CLAUDE.md

The C++17 client for AllSense by OtterBridge: `include/allsense/allsense.hpp` (public API), `src/allsense.cpp` (client, clock sync, reconnect, stall watchdog), `src/net.cpp` (own WebSocket over sockets + HTTP GET; Winsock or POSIX). Only dependency: nlohmann/json (found or fetched by CMake). Examples in `examples/` (the docs site includes them); `examples/project/` is the docs' sample CMake project. Public, MIT.

## Working here

- **The user makes every commit and push.** Edit files and run the tests, then hand over: what changed, the test results, and a one-line commit message suggestion. Never run `git commit`, `git push`, tags or releases; add no Co-Authored-By line unless asked.
- **Public repository:** never write the user's serials, IPs, Wi-Fi details, home location, tokens or file paths into it. Machine facts go in `CLAUDE.local.md` (git-ignored).

## Build and test

With GCC, CMake and Ninja on PATH (where they live on this machine: `CLAUDE.local.md`):

```bash
cmake -S . -B build -G Ninja && cmake --build build
allsense-conformance --example build/quickstart.exe --example build/print_values.exe -- build/conformance.exe
```

`allsense-conformance` comes from the test kit (`pip install -e ../otterbridge-allsense-protocol` in any venv). Also builds with MSVC (`-G "Visual Studio 16 2019" -A x64`, /W4 clean) and on Linux; CI covers GCC, Clang and MSVC.

## How it fits

- Speaks the protocol in `otterbridge-allsense-protocol`; `tests/conformance.cpp` implements its scenarios (args `<link> <scenario>`, `WAIT <action>` / `GO`, `PASS`). A protocol change lands there first, then here.
- Same API shape as the Python and Rust clients (connect, streams, subscribe → get/latest/read, commands, snapshot, camera/mic URLs).
- After a dropped link, shut the socket down (the server waits for our FIN); macOS needs `SO_NOSIGPIPE`.
