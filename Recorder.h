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
#include "SwagLiveAPI.h"

// Configuration for the recorder
struct RecorderConfig {
    std::string outputDirectory;    // Where to save recordings
    std::string ffmpegPath;         // Path to ffmpeg.exe (leave empty to auto-detect)
    int segmentDurationSec;         // Duration of each recording segment in seconds (0 = no segments)
    bool appendTimestamp;           // Whether to append timestamp to output filenames

    RecorderConfig()
        : outputDirectory("recordings"),
          segmentDurationSec(0),
          appendTimestamp(true) {}
};

// Manages an active recording session for a single stream
class RecordingSession {
public:
    RecordingSession(const StreamInfo& streamInfo, const RecorderConfig& config);
    ~RecordingSession();

    // Start the recording (launches ffmpeg or begins HLS download)
    bool Start();

    // Stop the recording gracefully
    void Stop();

    // Check if the recording is currently active
    bool IsRunning() const;

    // Get the output file path
    std::string GetOutputPath() const { return m_outputPath; }

    // Get the model username
    std::string GetUsername() const { return m_streamInfo.username; }

    // Get last error
    std::string GetLastError() const { return m_lastError; }

private:
    StreamInfo m_streamInfo;
    RecorderConfig m_config;
    std::string m_outputPath;
    std::string m_lastError;

    HANDLE m_hProcess;      // Handle to ffmpeg process
    HANDLE m_hStdin;        // Write end of stdin pipe sent to ffmpeg (for 'q' shutdown)

    volatile bool m_running;

    // Build the output file path from stream info and config
    std::string BuildOutputPath() const;

    // Ensure output directory exists
    bool EnsureOutputDirectory() const;

    // Find ffmpeg executable
    static std::string FindFfmpeg(const std::string& configPath);

    // Build ffmpeg command for capturing a WebRTC/HLS stream
    std::string BuildFfmpegCommand(const std::string& ffmpegPath) const;

    // Try to find an HLS or RTMP URL for the stream
    bool FindStreamUrl(std::string& outUrl) const;
};

// Top-level recorder that manages multiple recording sessions
class Recorder {
public:
    explicit Recorder(const RecorderConfig& config);
    ~Recorder();

    // Start recording a stream
    bool StartRecording(const StreamInfo& streamInfo);

    // Stop recording for a specific model
    bool StopRecording(const std::string& username);

    // Stop all recordings
    void StopAll();

    // Check if a model is currently being recorded
    bool IsRecording(const std::string& username) const;

    // Get list of currently recording model usernames
    std::vector<std::string> GetActiveRecordings() const;

    // Remove completed/dead sessions from the active list
    void CleanupFinished();

private:
    RecorderConfig m_config;
    std::vector<RecordingSession*> m_sessions;
};
