#pragma once

// LibDataChannelRecorder
//
// Records a swag.live stream without FFmpeg or WebView2:
//  1. Connects to swag.live's WebRTC signaling server using rtc::WebSocket
//     (built into libdatachannel – no WinHTTP WebSocket needed).
//  2. Creates an rtc::PeerConnection with RecvOnly video + audio tracks.
//  3. Uses libdatachannel's built-in VP8RtpDepacketizer / OpusRtpDepacketizer
//     as media handlers; receives reassembled frames via track->onFrame().
//  4. Muxes frames into a .webm file via the hand-rolled WebMMuxer.
//
// ─── Build dependency ─────────────────────────────────────────────────────────
//  libdatachannel (git submodule at deps/libdatachannel).
//  Build it once with build_deps.cmd, then MSVC links datachannel.lib.
//  No vcpkg, no NuGet, no FFmpeg.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "SwagLiveAPI.h"
#include "WebMMuxer.h"

// ─── Callback types ───────────────────────────────────────────────────────────

using LDCStartedCallback  = std::function<void(const std::string& username)>;
using LDCStoppedCallback  = std::function<void(const std::string& username, size_t totalBytes)>;
using LDCErrorCallback    = std::function<void(const std::string& username, const std::string& err)>;
using LDCProgressCallback = std::function<void(const std::string& username, size_t bytesWritten)>;

// ─── LibDataChannelRecorder ───────────────────────────────────────────────────

class LibDataChannelRecorder {
public:
    LibDataChannelRecorder();
    ~LibDataChannelRecorder();

    LDCStartedCallback  onStarted;
    LDCStoppedCallback  onStopped;
    LDCErrorCallback    onError;
    LDCProgressCallback onProgress;

    // Start recording to outputPath (.webm).  Returns immediately; all
    // network/muxer work runs on a background thread.
    bool Start(const StreamInfo& streamInfo,
               const std::string& outputPath,
               const std::string& authToken = "");

    // Signal a graceful stop; blocks until the background thread exits.
    void Stop();

    bool        IsRunning()      const { return m_running.load(); }
    std::string GetUsername()    const { return m_streamInfo.username; }
    std::string GetOutputPath()  const { return m_outputPath; }
    size_t      GetBytesWritten()const { return m_bytesWritten.load(); }
    std::string GetLastError()   const { return m_lastError; }

private:
    StreamInfo       m_streamInfo;
    std::string      m_outputPath;
    std::string      m_authToken;
    std::string      m_lastError;

    std::atomic<bool>   m_running     { false };
    std::atomic<bool>   m_stopRequest { false };
    std::atomic<size_t> m_bytesWritten{ 0 };

    std::unique_ptr<WebMMuxer> m_muxer;
    HANDLE                     m_hFile  = INVALID_HANDLE_VALUE;
    std::thread                m_thread;

    void RecordingThread();
    void AppendBytes(const uint8_t* data, size_t len);
    bool OpenOutputFile();
    void CloseOutputFile();
    void OnError(const std::string& msg);
};
