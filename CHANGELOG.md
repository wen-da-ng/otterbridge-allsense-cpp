# Changelog

## 0.1.0 (unreleased)

- First release: `allsense::connect()`, streams and subscriptions (`get`, `latest`, `read`, callbacks), clock sync, reconnect with a stall watchdog, commands, snapshots, camera and microphone URLs. C++17, CMake, no dependencies beyond nlohmann/json.
- Speaks AllSense protocol version 0.
- Security: JSON nested deeper than 64 is skipped like any bad message (it could overflow the stack); nlohmann/json 3.11 or newer.
- Security: a WebSocket message (all fragments together) is limited to 4 MiB; a bigger one ends the link.
- Security: HTTP answers (snapshots) are limited to 32 MiB.
- Security: `https://` and `wss://` links are refused instead of silently connecting without TLS.
- Security: catalog media urls must be a plain path (start with `/`, no `@`, `\`, CR or LF), else the camera or mic is unavailable.
- Security: the connect wait uses `poll()` on POSIX, so a descriptor past `FD_SETSIZE` can't overflow an `fd_set`.
- Security: a bad `%` escape in the token stays as it is instead of throwing an uncaught exception.
- Security: the handshake checks the server's `Sec-WebSocket-Accept`.
