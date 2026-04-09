#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <string>
#include <vector>

// Describes the status and properties of a swag.live live stream
struct StreamInfo {
    std::string streamId;       // Unique stream identifier
    std::string username;       // Model's username/slug
    std::string displayName;    // Model's display name
    std::string status;         // Stream status (e.g. "live", "offline")
    std::string chatMode;       // Chat mode (e.g. "free", "private", "group")
    std::string thumbnailUrl;   // Thumbnail image URL
    std::string streamToken;    // Stream access token (if available)
    int viewerCount;            // Number of current viewers
    bool isLive;                // Whether the stream is currently live
    bool isFreeChat;            // Whether the stream is in free chat mode

    StreamInfo() : viewerCount(0), isLive(false), isFreeChat(false) {}
};

// Client for communicating with the swag.live API
class SwagLiveAPI {
public:
    SwagLiveAPI();

    // Set authentication token (if required by the API)
    void SetAuthToken(const std::string& token) { m_authToken = token; }

    // Fetch list of all currently live streams
    // Returns true on success; fills outStreams with available streams
    bool GetLiveStreams(std::vector<StreamInfo>& outStreams);

    // Fetch status for a specific model by username
    // Returns true if the model was found; fills outInfo with stream details
    bool GetModelStatus(const std::string& username, StreamInfo& outInfo);

    // Get the last error message
    std::string GetLastError() const { return m_lastError; }

private:
    std::string m_authToken;
    std::string m_lastError;

    // Perform authenticated GET request against the API
    bool ApiGet(const std::string& path, std::string& responseBody);

    // Parse a stream info JSON object
    static StreamInfo ParseStreamInfo(const std::string& json);
};
