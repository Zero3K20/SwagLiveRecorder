// SwagLiveAPI.cpp
//
// Implementation of the swag.live REST discovery flow, fully reverse-engineered
// from a TLS-decrypted network capture (Swag.live.pcap + sslkey.log).
//
// Discovery flow:
//   1. FindLiveUser  – GET /feeds/user_livestream-v2 → search by username → userId
//   2. GetStreamSession – GET /pusher/retained-events?channels=private-enc-stream@{userId}
//                          → stream.online event → sessionId
//   3. GetAgoraToken – GET /streams/{sessionId}/token → agora_token, agora_token_session_id

#include "SwagLiveAPI.h"
#include "tlsclient/tlsclient.h"
#include "json_minimal.h"

#include <algorithm>
#include <cctype>
#include <sstream>

// ─── Base URL ─────────────────────────────────────────────────────────────────
static const std::string SWAG_API = "https://api.swag.live";

// ─── Constructor ──────────────────────────────────────────────────────────────
SwagLiveAPI::SwagLiveAPI() = default;

// ─── Internal HTTP helper ─────────────────────────────────────────────────────
bool SwagLiveAPI::ApiGet(const std::string& path, std::string& body) {
    TLSClient client;

    // Headers observed in PCAP for every api.swag.live request:
    std::string hdrs;
    hdrs += "Accept: application/json\r\n";
    hdrs += "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/127.0.0.0 Safari/537.36\r\n";
    hdrs += "x-version: 3.281.0\r\n";
    hdrs += "Origin: https://swag.live\r\n";
    hdrs += "Referer: https://swag.live/\r\n";

    if (!m_clientId.empty())
        hdrs += "x-client-id: " + m_clientId + "\r\n";
    if (!m_sessionId.empty())
        hdrs += "x-session-id: " + m_sessionId + "\r\n";
    if (!m_authToken.empty())
        hdrs += "Authorization: Bearer " + m_authToken + "\r\n";

    std::string raw;
    if (!client.HttpGet(SWAG_API + path, raw, hdrs)) {
        m_lastError = "HTTP GET failed: " + client.GetLastError();
        return false;
    }

    body = get_http_body(raw);
    if (body.empty()) body = raw;   // WinHTTP sometimes returns body directly
    return true;
}

// ─── FindLiveUser ─────────────────────────────────────────────────────────────
//
// GET /feeds/user_livestream-v2?limit=100&page=1&sorting=desc:s_score
//
// Response: JSON array of objects:
//   [{ "username": "lysr7777",
//      "id":       "697f4adbf87c532036bc8062",
//      "displayName": "小茴",
//      "badges": ["country:cn"],
//      "metadata": {…} }, …]
//
// We walk through pages until we find the username or exhaust results.
bool SwagLiveAPI::FindLiveUser(const std::string& username, StreamInfo& outInfo) {
    if (username.empty()) {
        m_lastError = "Username must not be empty";
        return false;
    }

    // Case-insensitive comparison helper
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    };
    const std::string targetLc = toLower(username);

    for (int page = 1; page <= 20; ++page) {
        std::ostringstream path;
        path << "/feeds/user_livestream-v2?limit=100&page=" << page
             << "&sorting=desc:s_score";

        std::string body;
        if (!ApiGet(path.str(), body)) return false;

        JsonValue root = parse_json(body);
        if (root.is_null() || root.type != JsonValue::Type::Array) {
            m_lastError = "Unexpected /feeds/user_livestream-v2 response";
            return false;
        }

        bool foundAny = false;
        for (size_t i = 0; i < root.size(); ++i) {
            const JsonValue& item = root[i];
            if (item.is_null()) continue;
            foundAny = true;

            std::string uname = item["username"].as_str();
            if (toLower(uname) == targetLc) {
                outInfo.username    = uname;
                outInfo.userId      = item["id"].as_str();
                outInfo.displayName = item["displayName"].as_str();
                outInfo.isLive      = true;
                return true;
            }
        }

        // Fewer than 100 results → we've reached the last page
        if (!foundAny || root.size() < 100) break;
    }

    m_lastError = "User '" + username + "' not found in live feed";
    return false;
}

// ─── GetStreamSession ─────────────────────────────────────────────────────────
//
// GET /pusher/retained-events?channels=private-enc-stream%40{userId}
//
// Response shape:
//   { "private-enc-stream@{userId}": [
//       { "event": "stream.online",
//         "data": {
//           "session":   "69d808eb65fd2d2df88eec4b",
//           "title":     "…",
//           "preset":    "preview",
//           "exclusive": false,
//           "price":     0,
//           "categories": […]
//         } } ] }
bool SwagLiveAPI::GetStreamSession(StreamInfo& outInfo) {
    if (outInfo.userId.empty()) {
        m_lastError = "userId required before calling GetStreamSession";
        return false;
    }

    const std::string path =
        "/pusher/retained-events?channels=private-enc-stream%40" + outInfo.userId;

    std::string body;
    if (!ApiGet(path, body)) return false;

    JsonValue root = parse_json(body);
    if (root.is_null() || root.type != JsonValue::Type::Object) {
        m_lastError = "Unexpected /pusher/retained-events response";
        return false;
    }

    // The key is "private-enc-stream@{userId}"
    const std::string channelKey = "private-enc-stream@" + outInfo.userId;
    const JsonValue& events = root[channelKey];
    if (events.is_null() || events.type != JsonValue::Type::Array || events.size() == 0) {
        m_lastError = "No retained events for user " + outInfo.userId + " – user may not be live";
        return false;
    }

    // Find the "stream.online" event
    for (size_t i = 0; i < events.size(); ++i) {
        const JsonValue& ev = events[i];
        if (ev["event"].as_str() != "stream.online") continue;

        const JsonValue& data = ev["data"];
        if (data.is_null()) continue;

        outInfo.sessionId  = data["session"].as_str();
        outInfo.title      = data["title"].as_str();
        outInfo.preset     = data["preset"].as_str();
        outInfo.exclusive  = data["exclusive"].as_bool();
        outInfo.price      = static_cast<int>(data["price"].as_num());
        outInfo.isLive     = true;
        return true;
    }

    m_lastError = "stream.online event not found for user " + outInfo.userId;
    return false;
}

// ─── GetAgoraToken ────────────────────────────────────────────────────────────
//
// GET /streams/{sessionId}/token
//
// Response:
//   { "agora_token":            "00619c9ed8…",   // subscribe (uid=0)
//     "agora_token_session_id": "00619c9ed8…",   // publish-capable (uid!=0)
//     "agora_exp":              1775766844 }
//
// No auth header required – the server issues a guest viewer token.
bool SwagLiveAPI::GetAgoraToken(StreamInfo& outInfo) {
    if (outInfo.sessionId.empty()) {
        m_lastError = "sessionId required before calling GetAgoraToken";
        return false;
    }

    const std::string path = "/streams/" + outInfo.sessionId + "/token";

    std::string body;
    if (!ApiGet(path, body)) return false;

    JsonValue root = parse_json(body);
    if (root.is_null() || root.type != JsonValue::Type::Object) {
        m_lastError = "Unexpected /streams/{id}/token response";
        return false;
    }

    outInfo.agoraToken          = root["agora_token"].as_str();
    outInfo.agoraTokenSessionId = root["agora_token_session_id"].as_str();
    outInfo.agoraExp            = static_cast<int64_t>(root["agora_exp"].as_num());
    outInfo.agoraChannel        = outInfo.sessionId;  // channel == session ID

    if (outInfo.agoraToken.empty()) {
        m_lastError = "agora_token missing in /streams/{id}/token response";
        return false;
    }
    return true;
}

// ─── ResolveStream ────────────────────────────────────────────────────────────
bool SwagLiveAPI::ResolveStream(const std::string& username, StreamInfo& outInfo) {
    outInfo = StreamInfo{};
    return FindLiveUser(username, outInfo)
        && GetStreamSession(outInfo)
        && GetAgoraToken(outInfo);
}
