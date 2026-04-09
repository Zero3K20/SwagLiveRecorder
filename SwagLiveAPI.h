#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <string>
#include <vector>

// Describes the status and properties of a swag.live live stream.
// Field names match the actual swag.live REST API (confirmed via PCAP + JS analysis).
struct StreamInfo {
    // Primary identifiers
    std::string streamId;       // Livestream session ID (ObjectId hex, e.g. "57a42a779f22bb6bcc434520")
    std::string userId;         // Streamer's user ID  (ObjectId hex, e.g. "57ac5ef86912ff264ff1e010")
    std::string username;       // Streamer's @username slug (e.g. "akira")
    std::string displayName;    // Streamer's display name

    // Stream metadata
    std::string status;         // "live", "offline", etc.
    std::string chatMode;       // "free", "paid", "exclusive", "preview", "sd"
    std::string provider;       // RTC provider: "agora", "byteplus", "tencent", "swag"
    std::string thumbnailUrl;

    // Agora credentials (populated when provider == "agora")
    std::string agoraAppId;     // Agora App ID (32-char hex)
    std::string agoraChannel;   // Agora channel name
    std::string agoraToken;     // Agora RTC token
    unsigned int agoraUid;      // Local viewer UID

    // Pusher real-time channel (format: "presence-enc-stream-exclusive@{streamId}")
    std::string pusherChannel;

    std::string streamToken;    // Generic access token (for paid/exclusive streams)
    int viewerCount;
    bool isLive;
    bool isFreeChat;

    StreamInfo() : agoraUid(0), viewerCount(0), isLive(false), isFreeChat(false) {}
};

// Client for communicating with the swag.live REST API.
//
// Confirmed API endpoints (from PCAP analysis + JS reverse-engineering):
//   GET  /u/{username}                   → user profile: id, username, displayName, …
//   GET  /users/{userId}                 → same as above by internal ID
//   GET  /feeds/user_livestream-v2       → paginated live stream list (requires auth)
//   GET  /configurations/bootstrap       → app config (Pusher app ID, etc.)
//   GET  /configurations/{group}.json    → per-group config values
//   POST /pusher/auth                    → authenticate a Pusher channel subscription
//
// The live-stream player page URL pattern is:
//   https://swag.live/livestream/{streamId}   (direct, by stream session ID)
//   https://swag.live/user/{userId}/livestream (per-user, redirects to above)
class SwagLiveAPI {
public:
    SwagLiveAPI();

    // Optionally supply a Bearer auth token for protected endpoints
    void SetAuthToken(const std::string& token) { m_authToken = token; }
    // Supply the random client UUID (X-Client-ID header) generated at startup
    void SetClientId(const std::string& clientId) { m_clientId = clientId; }

    // Resolve a username → StreamInfo (userId, username, displayName).
    // Uses GET /u/{username}. Returns false if the user does not exist.
    bool GetUserByUsername(const std::string& username, StreamInfo& outInfo);

    // Fetch the list of all currently live streams.
    // Uses GET /feeds/user_livestream-v2 (requires auth).
    bool GetLiveStreams(std::vector<StreamInfo>& outStreams);

    // Fetch live-stream status for a model by username.
    // Resolves username → userId via /u/{username}, then looks up the stream.
    bool GetModelStatus(const std::string& username, StreamInfo& outInfo);

    std::string GetLastError() const { return m_lastError; }

private:
    std::string m_authToken;
    std::string m_clientId;
    std::string m_lastError;

    bool ApiGet(const std::string& path, std::string& responseBody);

    // Parse an item from the /feeds/user_livestream-v2 array
    static StreamInfo ParseFeedItem(const JsonValue& item);
};
