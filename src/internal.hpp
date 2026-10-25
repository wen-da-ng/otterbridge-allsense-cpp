// Checks on what the phone sends, shared by the client and tests/unit.cpp.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace allsense::detail {

// Deeper JSON than this is refused: copying or printing it recurses and would overflow the stack.
const int kMaxJsonDepth = 64;

// json::parse with a nesting limit; throws (like any bad message) when it's exceeded.
nlohmann::json parse_message(const std::string& msg);

// A catalog media url the client may append to http://host:port: a path, nothing that changes the host or the request.
bool safe_media_path(const std::string& url);

}  // namespace allsense::detail
