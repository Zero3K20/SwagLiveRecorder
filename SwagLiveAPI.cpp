#include "SwagLiveAPI.h"
#include "tlsclient/tlsclient.h"
#include "json_minimal.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

// Base URL for the swag.live API
static const std::string SWAG_API_BASE = "https://api.swag.live";

// Fallback to the main domain if the above doesn't work
static const std::string SWAG_MAIN_HOST = "https://swag.live";

SwagLiveAPI::SwagLiveAPI() {
}

bool SwagLiveAPI::ApiGet(const std::string& path, std::string& responseBody) {
    TLSClient client;

    std::string url = SWAG_API_BASE + path;
    std::string headers;

    headers += "Accept: application/json\r\n";
    headers += "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
               "AppleWebKit/537.36 (KHTML, like Gecko) "
               "Chrome/120.0.0.0 Safari/537.36\r\n";
    headers += "Origin: https://swag.live\r\n";
    headers += "Referer: https://swag.live/\r\n";

    if (!m_authToken.empty()) {
        headers += "Authorization: Bearer " + m_authToken + "\r\n";
    }

    std::string rawResponse;
    if (!client.HttpGet(url, rawResponse, headers)) {
        m_lastError = "HTTP GET failed for " + url + ": " + client.GetLastError();
        return false;
    }

    // Extract HTTP body (skip headers)
    responseBody = get_http_body(rawResponse);
    if (responseBody.empty()) {
        // Sometimes the raw response is just the body (WinHTTP)
        responseBody = rawResponse;
    }

    return true;
}

// Parse a JSON object representing a single stream into StreamInfo
StreamInfo SwagLiveAPI::ParseStreamInfo(const std::string& json) {
    StreamInfo info;
    JsonValue root = parse_json(json);

    if (root.is_null()) return info;

    // Try common field names used by streaming platforms
    // The exact field names depend on swag.live's actual API response structure

    // Stream ID
    if (!root["id"].is_null())
        info.streamId = root["id"].as_str();
    else if (!root["streamId"].is_null())
        info.streamId = root["streamId"].as_str();
    else if (!root["stream_id"].is_null())
        info.streamId = root["stream_id"].as_str();

    // Username / slug
    if (!root["username"].is_null())
        info.username = root["username"].as_str();
    else if (!root["slug"].is_null())
        info.username = root["slug"].as_str();
    else if (!root["name"].is_null())
        info.username = root["name"].as_str();

    // Try nested "user" or "model" object
    if (info.username.empty()) {
        const JsonValue& user = root["user"];
        if (!user.is_null()) {
            if (!user["username"].is_null())
                info.username = user["username"].as_str();
            else if (!user["slug"].is_null())
                info.username = user["slug"].as_str();
        }
        const JsonValue& model = root["model"];
        if (!model.is_null() && info.username.empty()) {
            if (!model["username"].is_null())
                info.username = model["username"].as_str();
        }
    }

    // Display name
    if (!root["displayName"].is_null())
        info.displayName = root["displayName"].as_str();
    else if (!root["display_name"].is_null())
        info.displayName = root["display_name"].as_str();
    else if (!root["nickname"].is_null())
        info.displayName = root["nickname"].as_str();

    // Stream status
    if (!root["status"].is_null())
        info.status = root["status"].as_str();
    else if (!root["liveStatus"].is_null())
        info.status = root["liveStatus"].as_str();

    // Chat/room mode
    if (!root["chatMode"].is_null())
        info.chatMode = root["chatMode"].as_str();
    else if (!root["roomMode"].is_null())
        info.chatMode = root["roomMode"].as_str();
    else if (!root["mode"].is_null())
        info.chatMode = root["mode"].as_str();

    // Thumbnail
    if (!root["thumbnailUrl"].is_null())
        info.thumbnailUrl = root["thumbnailUrl"].as_str();
    else if (!root["thumbnail"].is_null())
        info.thumbnailUrl = root["thumbnail"].as_str();
    else if (!root["previewUrl"].is_null())
        info.thumbnailUrl = root["previewUrl"].as_str();

    // Viewer count
    if (!root["viewerCount"].is_null())
        info.viewerCount = (int)root["viewerCount"].as_num();
    else if (!root["viewers"].is_null())
        info.viewerCount = (int)root["viewers"].as_num();
    else if (!root["viewCount"].is_null())
        info.viewerCount = (int)root["viewCount"].as_num();

    // Stream token (for accessing restricted streams)
    if (!root["token"].is_null())
        info.streamToken = root["token"].as_str();
    else if (!root["streamToken"].is_null())
        info.streamToken = root["streamToken"].as_str();

    // Determine live status
    std::string statusLower = info.status;
    std::transform(statusLower.begin(), statusLower.end(), statusLower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    info.isLive = (statusLower == "live" || statusLower == "online" ||
                   statusLower == "broadcasting" || statusLower == "public");

    // If no status field but we got stream data, assume it's live
    if (!root["isLive"].is_null())
        info.isLive = root["isLive"].as_bool();
    else if (!root["is_live"].is_null())
        info.isLive = root["is_live"].as_bool();
    else if (!root["online"].is_null())
        info.isLive = root["online"].as_bool();

    // Determine free chat status
    std::string modeLower = info.chatMode;
    std::transform(modeLower.begin(), modeLower.end(), modeLower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    // Determine free chat status - require explicit free/public mode indicator;
    // do not assume free chat when the mode is absent to avoid recording
    // private or group shows unintentionally.
    info.isFreeChat = (modeLower == "free" || modeLower == "public" ||
                       modeLower == "freechat" || modeLower == "free_chat");

    if (!root["isFreeChat"].is_null())
        info.isFreeChat = root["isFreeChat"].as_bool();

    return info;
}

bool SwagLiveAPI::GetLiveStreams(std::vector<StreamInfo>& outStreams) {
    outStreams.clear();

    // Try common API endpoints for listing live streams
    // These endpoints are typical for cam site APIs
    const char* endpoints[] = {
        "/livestream/onGoingList",
        "/livestreams",
        "/livestream/list",
        "/live/list",
        "/streams",
        nullptr
    };

    std::string responseBody;
    bool success = false;

    for (int i = 0; endpoints[i] != nullptr; ++i) {
        if (ApiGet(endpoints[i], responseBody) && !responseBody.empty()) {
            // Check if it looks like JSON
            if (!responseBody.empty() && (responseBody[0] == '{' || responseBody[0] == '[')) {
                success = true;
                break;
            }
        }
        responseBody.clear();
    }

    if (!success || responseBody.empty()) {
        m_lastError = "Failed to fetch live stream list from API";
        return false;
    }

    // Parse the response - handle both array and object wrapping
    JsonValue root = parse_json(responseBody);

    if (root.is_null()) {
        m_lastError = "Failed to parse API response as JSON";
        return false;
    }

    // The response might be a direct array, or wrapped in an object
    const JsonValue* arrayNode = nullptr;

    if (root.type == JsonValue::Type::Array) {
        arrayNode = &root;
    } else if (root.type == JsonValue::Type::Object) {
        // Try common wrapper keys
        const char* wrapKeys[] = { "data", "streams", "list", "items", "results", nullptr };
        for (int i = 0; wrapKeys[i] != nullptr; ++i) {
            if (!root[wrapKeys[i]].is_null() &&
                root[wrapKeys[i]].type == JsonValue::Type::Array) {
                arrayNode = &root[wrapKeys[i]];
                break;
            }
        }
    }

    if (!arrayNode) {
        m_lastError = "Unexpected JSON structure in API response";
        return false;
    }

    for (size_t i = 0; i < arrayNode->size(); ++i) {
        // Re-serialize each array element for ParseStreamInfo
        // Since we have a minimal JSON parser, extract from the already-parsed tree
        const JsonValue& item = (*arrayNode)[i];
        if (item.is_null()) continue;

        StreamInfo info;

        // Extract fields directly from parsed JSON
        if (!item["id"].is_null())
            info.streamId = item["id"].as_str();
        else if (!item["streamId"].is_null())
            info.streamId = item["streamId"].as_str();

        if (!item["username"].is_null())
            info.username = item["username"].as_str();
        else if (!item["slug"].is_null())
            info.username = item["slug"].as_str();

        const JsonValue& user = item["user"];
        if (!user.is_null() && info.username.empty()) {
            if (!user["username"].is_null())
                info.username = user["username"].as_str();
        }

        if (!item["displayName"].is_null())
            info.displayName = item["displayName"].as_str();
        else if (!item["display_name"].is_null())
            info.displayName = item["display_name"].as_str();
        else if (!item["nickname"].is_null())
            info.displayName = item["nickname"].as_str();

        if (!item["status"].is_null())
            info.status = item["status"].as_str();

        if (!item["chatMode"].is_null())
            info.chatMode = item["chatMode"].as_str();
        else if (!item["roomMode"].is_null())
            info.chatMode = item["roomMode"].as_str();

        if (!item["thumbnailUrl"].is_null())
            info.thumbnailUrl = item["thumbnailUrl"].as_str();
        else if (!item["thumbnail"].is_null())
            info.thumbnailUrl = item["thumbnail"].as_str();

        if (!item["viewerCount"].is_null())
            info.viewerCount = (int)item["viewerCount"].as_num();
        else if (!item["viewers"].is_null())
            info.viewerCount = (int)item["viewers"].as_num();

        if (!item["token"].is_null())
            info.streamToken = item["token"].as_str();

        // Determine live/free chat status
        std::string statusLower = info.status;
        std::transform(statusLower.begin(), statusLower.end(), statusLower.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        info.isLive = (statusLower == "live" || statusLower == "online" ||
                       statusLower == "broadcasting" || statusLower == "public");

        if (!item["isLive"].is_null())
            info.isLive = item["isLive"].as_bool();
        else if (!item["online"].is_null())
            info.isLive = item["online"].as_bool();

        std::string modeLower = info.chatMode;
        std::transform(modeLower.begin(), modeLower.end(), modeLower.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        info.isFreeChat = (modeLower == "free" || modeLower == "public" ||
                           modeLower == "freechat" || modeLower == "free_chat");

        if (!item["isFreeChat"].is_null())
            info.isFreeChat = item["isFreeChat"].as_bool();

        outStreams.push_back(info);
    }

    return true;
}

bool SwagLiveAPI::GetModelStatus(const std::string& username, StreamInfo& outInfo) {
    if (username.empty()) {
        m_lastError = "Username cannot be empty";
        return false;
    }

    // Try common per-model status endpoints
    // URL-encode the username (simple version - no special chars expected)
    std::string responseBody;
    bool success = false;

    const std::string paths[] = {
        "/livestream/" + username,
        "/user/" + username + "/livestream",
        "/model/" + username + "/stream",
        "/live/" + username,
        ""
    };

    for (size_t i = 0; !paths[i].empty(); ++i) {
        if (ApiGet(paths[i], responseBody) && !responseBody.empty()) {
            if (!responseBody.empty() && (responseBody[0] == '{' || responseBody[0] == '[')) {
                success = true;
                break;
            }
        }
        responseBody.clear();
    }

    if (!success || responseBody.empty()) {
        // Fallback: search the live stream list
        std::vector<StreamInfo> streams;
        if (GetLiveStreams(streams)) {
            std::string usernameLower = username;
            std::transform(usernameLower.begin(), usernameLower.end(), usernameLower.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });

            for (const auto& stream : streams) {
                std::string streamNameLower = stream.username;
                std::transform(streamNameLower.begin(), streamNameLower.end(),
                    streamNameLower.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });

                if (streamNameLower == usernameLower) {
                    outInfo = stream;
                    return true;
                }
            }
        }

        outInfo.username = username;
        outInfo.isLive = false;
        outInfo.isFreeChat = false;
        m_lastError = "Model not found or not currently live";
        return false;
    }

    outInfo = ParseStreamInfo(responseBody);
    if (outInfo.username.empty()) {
        outInfo.username = username;
    }

    return true;
}
