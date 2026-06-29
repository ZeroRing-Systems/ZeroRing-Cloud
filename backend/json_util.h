// ============================================================================
// json_util.h — Minimal JSON parser/builder for WebSocket command protocol
// ============================================================================
// Intentionally dependency-free: no nlohmann, no rapidjson.
// Handles the flat { "key": "value" } objects used by the ZeroRing protocol.
// ============================================================================
#pragma once
#include <string>
#include <map>

namespace json {

// ---------------------------------------------------------------------------
// Parse a flat JSON object: {"key1":"val1","key2":"val2",...}
// Returns key-value map. Supports only string values (sufficient for our
// command protocol). Ignores whitespace. Does NOT handle nested objects,
// arrays, or escapes beyond \" — by design.
// ---------------------------------------------------------------------------
inline std::map<std::string, std::string> parse(const std::string& input) {
    std::map<std::string, std::string> result;
    size_t i = 0;
    auto skip_ws = [&]() { while (i < input.size() && (input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\r')) i++; };
    auto read_string = [&]() -> std::string {
        std::string s;
        if (i >= input.size() || input[i] != '"') return s;
        i++; // skip opening quote
        while (i < input.size() && input[i] != '"') {
            if (input[i] == '\\' && i + 1 < input.size()) {
                i++;
                if (input[i] == '"') s += '"';
                else if (input[i] == '\\') s += '\\';
                else if (input[i] == 'n') s += '\n';
                else if (input[i] == 't') s += '\t';
                else { s += '\\'; s += input[i]; }
            } else {
                s += input[i];
            }
            i++;
        }
        if (i < input.size()) i++; // skip closing quote
        return s;
    };

    skip_ws();
    if (i >= input.size() || input[i] != '{') return result;
    i++; // skip '{'

    while (i < input.size()) {
        skip_ws();
        if (input[i] == '}') break;
        if (input[i] == ',') { i++; continue; }

        std::string key = read_string();
        skip_ws();
        if (i < input.size() && input[i] == ':') i++;
        skip_ws();
        std::string val = read_string();
        if (!key.empty()) result[key] = val;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Build a JSON response string
// ---------------------------------------------------------------------------
inline std::string ok(const std::string& data) {
    return "{\"status\":\"ok\",\"data\":\"" + data + "\"}";
}

inline std::string ok_raw(const std::string& data) {
    // data is already valid JSON (array, object, etc.)
    return "{\"status\":\"ok\",\"data\":" + data + "}";
}

inline std::string error(const std::string& msg) {
    return "{\"status\":\"error\",\"msg\":\"" + msg + "\"}";
}

// Build a JSON array of strings: ["a","b","c"]
inline std::string array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) out += ",";
        out += "\"" + items[i] + "\"";
    }
    out += "]";
    return out;
}

} // namespace json
