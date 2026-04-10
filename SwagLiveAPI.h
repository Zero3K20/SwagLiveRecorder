#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <string>
#include <vector>

// ─── Constants discovered by PCAP analysis ────────────────────────────────────
//
//  Pusher WebSocket
//    App key  : aad17fe8f682717df2c0
//    App ID   : 550591
//    Host     : ws-ap1.pusher.com
//    Auth     : GET api.swag.live/pusher/batch-authenticate
//                   ?app_id=550591&socket_id={socketId}&channels={channels}
//
//  Agora RTC (the only media transport swag.live uses)
//    App ID   : 19c9ed8fd65f4ea9b5de096362af989e
//    SUA      : POST https://sua-ap-web-1.agora.io/api/v1?action=stringuid
//    CDS      : POST https://cds-ap-web-1.agora.io/api/v1?action=config
//    Gateway  : POST https://webrtc2-ap-web-1.agora.io/api/v2/transpond/webrtc?v=2
//               (multipart/form-data; name="request")
//               Gateway response carries edge IPs + DTLS fingerprints.

static const char* AGORA_APP_ID     = "19c9ed8fd65f4ea9b5de096362af989e";
static const char* PUSHER_APP_KEY   = "aad17fe8f682717df2c0";
static const char* PUSHER_APP_ID    = "550591";
static const char* PUSHER_HOST      = "ws-ap1.pusher.com";

// ─── StreamInfo ───────────────────────────────────────────────────────────────
//
// Confirmed field layout from PCAP:
//
//   /feeds/user_livestream-v2 array item:
//     { "username": "lysr7777",
//       "id":       "697f4adbf87c532036bc8062",   ← user (not session) ID
//       "displayName": "…",
//       "badges": […],
//       "metadata": {…} }
//
//   /pusher/retained-events?channels=private-enc-stream@{userId}
//     { "private-enc-stream@{userId}": [
//         { "event": "stream.online",
//           "data": {
//             "session":   "69d808eb65fd2d2df88eec4b",   ← stream session ID
//             "preset":    "preview",                      ← "preview"|"sd"
//             "exclusive": false,
//             "price":     0,
//             "title":     "…",
//             "categories": […] } } ] }
//
//   /streams/{sessionId}/token
//     { "agora_token":            "00619c9ed8…",   ← subscribe token (uid=0)
//       "agora_token_session_id": "00619c9ed8…",   ← publish-capable token
//       "agora_exp":              1775766844 }
struct StreamInfo {
    // From /feeds/user_livestream-v2
    std::string userId;          // user ObjectId hex, e.g. "697f4adbf87c532036bc8062"
    std::string username;        // @username slug,  e.g. "lysr7777"
    std::string displayName;

    // From /pusher/retained-events?channels=private-enc-stream@{userId}
    std::string sessionId;       // stream session ObjectId hex
    std::string preset;          // "preview", "sd"
    std::string title;
    bool        exclusive;
    int         price;

    // From /streams/{sessionId}/token
    std::string agoraToken;          // viewer subscribe token (uid == 0)
    std::string agoraTokenSessionId; // publish-capable token  (uid != 0)
    int64_t     agoraExp;

    // Derived / computed
    std::string agoraChannel;    // == sessionId
    bool        isLive;

    StreamInfo() : exclusive(false), price(0), agoraExp(0), isLive(false) {}
};

// ─── SwagLiveAPI ──────────────────────────────────────────────────────────────
//
// Confirmed REST endpoints (api.swag.live, HTTPS/2):
//
//   GET /feeds/user_livestream-v2?limit=100&page=1&sorting=desc:s_score
//       → JSON array of user objects (username, id, displayName, badges)
//       No auth required for the feed list itself.
//
//   GET /pusher/retained-events?channels=private-enc-stream%40{userId}
//       → JSON object mapping channel → array of Pusher events.
//       Contains "stream.online" with session ID, title, preset, price.
//       No auth required.
//
//   GET /streams/{sessionId}/token
//       → JSON with agora_token, agora_token_session_id, agora_exp.
//       No auth required (public guest view token).
//
// Required request headers (from PCAP):
//   x-version: 3.281.0
//   x-client-id: {random UUID, stable per install}
//   x-session-id: {random UUID, new each session}
//   Origin: https://swag.live
//   Referer: https://swag.live/
class SwagLiveAPI {
public:
    SwagLiveAPI();

    // Optional Bearer auth token (some endpoints require a logged-in user).
    void SetAuthToken(const std::string& token)   { m_authToken  = token; }
    // Stable per-install client UUID (sent as X-Client-ID header).
    void SetClientId(const std::string& clientId) { m_clientId   = clientId; }
    // Per-session UUID (sent as X-Session-ID header).
    void SetSessionId(const std::string& sid)     { m_sessionId  = sid; }

    // Search /feeds/user_livestream-v2 for a model by username.
    // Populates userId, username, displayName in outInfo.
    // Returns false if not found or not currently live.
    bool FindLiveUser(const std::string& username, StreamInfo& outInfo);

    // Fetch the active session ID for a user who is live.
    // Calls GET /pusher/retained-events?channels=private-enc-stream@{userId}.
    // Populates sessionId, preset, title, exclusive, price in outInfo.
    bool GetStreamSession(StreamInfo& outInfo);

    // Fetch the Agora RTC token for a stream session.
    // Calls GET /streams/{sessionId}/token.
    // Populates agoraToken, agoraTokenSessionId, agoraExp, agoraChannel.
    bool GetAgoraToken(StreamInfo& outInfo);

    // Convenience: run FindLiveUser → GetStreamSession → GetAgoraToken in order.
    bool ResolveStream(const std::string& username, StreamInfo& outInfo);

    std::string GetLastError() const { return m_lastError; }

private:
    std::string m_authToken;
    std::string m_clientId;
    std::string m_sessionId;
    std::string m_lastError;

    // Perform a GET against api.swag.live with standard Swag headers.
    bool ApiGet(const std::string& path, std::string& responseBody);
};
