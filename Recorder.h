#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "SwagLiveAPI.h"
#include "LibDataChannelRecorder.h"

// Configuration for the recorder
struct RecorderConfig {
    std::string outputDirectory;  // Directory to save .webm recordings
    bool        appendTimestamp;  // Append _YYYYMMDD_HHMMSS to filename

    RecorderConfig()
        : outputDirectory("recordings"),
          appendTimestamp(true) {}
};

using RecorderStartedCb  = std::function<void(const std::string& username)>;
using RecorderStoppedCb  = std::function<void(const std::string& username, size_t bytes)>;
using RecorderErrorCb    = std::function<void(const std::string& username, const std::string& err)>;
using RecorderProgressCb = std::function<void(const std::string& username, size_t bytes)>;

// Top-level recorder: manages per-model LibDataChannelRecorder sessions.
// Thread-safe for Start/Stop calls; can be called from any thread.
class Recorder {
public:
    explicit Recorder(const RecorderConfig& config, HWND /*unused_hwnd*/ = nullptr);
    ~Recorder();

    RecorderStartedCb  onStarted;
    RecorderStoppedCb  onStopped;
    RecorderErrorCb    onError;
    RecorderProgressCb onProgress;

    // Start recording a stream (non-blocking)
    bool StartRecording(const StreamInfo& streamInfo,
                        const std::string& authToken = "");

    // Stop recording for a specific model
    bool StopRecording(const std::string& username);

    // Stop all active recordings
    void StopAll();

    // True if there is an active (or starting) session for the given model
    bool IsRecording(const std::string& username) const;

    // List usernames currently being recorded
    std::vector<std::string> GetActiveRecordings() const;

    // Remove sessions that have finished on their own
    void CleanupFinished();

private:
    RecorderConfig                       m_config;
    std::vector<LibDataChannelRecorder*> m_sessions;

    LibDataChannelRecorder* FindSession(const std::string& username) const;

    // Build the output .webm path for a given username.
    std::string BuildOutputPath(const std::string& username) const;
};
