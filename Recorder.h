#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "SwagLiveAPI.h"
#include "WebRTCRecorder.h"

// Configuration for the recorder (mirrors WebRTCConfig for public API)
struct RecorderConfig {
    std::string outputDirectory;  // Directory to save .webm recordings
    bool appendTimestamp;         // Append _YYYYMMDD_HHMMSS to filenames

    RecorderConfig()
        : outputDirectory("recordings"),
          appendTimestamp(true) {}
};

// Callbacks for recording events (forwarded from WebRTCRecorder)
using RecorderStartedCb  = std::function<void(const std::string& username)>;
using RecorderStoppedCb  = std::function<void(const std::string& username, size_t bytes)>;
using RecorderErrorCb    = std::function<void(const std::string& username, const std::string& err)>;
using RecorderProgressCb = std::function<void(const std::string& username, size_t bytes)>;

// Top-level recorder: manages per-model WebRTCRecorder sessions.
// Must live on the same thread as the Win32 message loop (main thread).
class Recorder {
public:
    explicit Recorder(const RecorderConfig& config, HWND parentHwnd);
    ~Recorder();

    // Event callbacks – set these before calling StartRecording
    RecorderStartedCb  onStarted;
    RecorderStoppedCb  onStopped;
    RecorderErrorCb    onError;
    RecorderProgressCb onProgress;

    // Start recording a stream (non-blocking; recording begins asynchronously)
    bool StartRecording(const StreamInfo& streamInfo);

    // Stop recording for a specific model
    bool StopRecording(const std::string& username);

    // Stop all active recordings
    void StopAll();

    // True if there is an active (or starting) session for the given model
    bool IsRecording(const std::string& username) const;

    // List usernames of models currently being recorded
    std::vector<std::string> GetActiveRecordings() const;

    // Remove sessions that have finished on their own
    void CleanupFinished();

private:
    RecorderConfig m_config;
    HWND           m_parentHwnd;
    std::vector<WebRTCRecorder*> m_sessions;

    // Case-insensitive find helper
    WebRTCRecorder* FindSession(const std::string& username) const;
};
