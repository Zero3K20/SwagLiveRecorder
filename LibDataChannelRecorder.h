#pragma once

// LibDataChannelRecorder – FFmpeg-free alternative to WebRTCRecorder.
//
// Records a swag.live stream by:
//  1. Connecting to swag.live's WebRTC signaling endpoint over WebSocket.
//  2. Performing SDP offer/answer exchange via libdatachannel (rtc::PeerConnection).
//  3. Receiving RTP packets on the two media tracks (video + audio).
//  4. Reassembling frames with the RTP depayloaders (RTPDepayloader.h).
//  5. Muxing directly to a .webm file with WebMMuxer (no FFmpeg).
//
// ─── Dependencies ─────────────────────────────────────────────────────────────
//  libdatachannel ≥ 0.18  (vcpkg: vcpkg install libdatachannel:x64-windows)
//  Link: datachannel.lib / datachannel-static.lib
//  Include: <rtc/rtc.hpp>
//
// The WebView2 NuGet package (already in the project) is NOT required by this
// recorder; it only needs ws2_32.lib and winhttp.lib (already linked).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "SwagLiveAPI.h"
#include "WebMMuxer.h"
#include "RTPDepayloader.h"

// Forward-declare libdatachannel types to avoid pulling the heavy header into
// every translation unit that includes this file.
namespace rtc {
    class PeerConnection;
    class Track;
}

// ─── Callbacks (same shape as WebRTCRecorder) ─────────────────────────────────

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

    // Start recording.  Returns immediately; recording runs on a background thread.
    // outputPath: full path of the .webm file to create.
    bool Start(const StreamInfo& streamInfo,
               const std::string& outputPath,
               const std::string& authToken = "");

    // Request a graceful stop.  Blocks until the background thread exits.
    void Stop();

    bool        IsRunning()      const { return m_running.load(); }
    std::string GetUsername()    const { return m_streamInfo.username; }
    std::string GetOutputPath()  const { return m_outputPath; }
    size_t      GetBytesWritten()const { return m_bytesWritten.load(); }
    std::string GetLastError()   const { return m_lastError; }

private:
    // ── State ─────────────────────────────────────────────────────────────────
    StreamInfo       m_streamInfo;
    std::string      m_outputPath;
    std::string      m_authToken;
    std::string      m_lastError;
    std::atomic<bool>   m_running    { false };
    std::atomic<bool>   m_stopRequest{ false };
    std::atomic<size_t> m_bytesWritten{ 0 };

    // ── libdatachannel objects ─────────────────────────────────────────────────
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::Track>          m_videoTrack;
    std::shared_ptr<rtc::Track>          m_audioTrack;

    // ── Muxer + depayloaders ───────────────────────────────────────────────────
    std::unique_ptr<WebMMuxer>       m_muxer;
    VP8Depayloader                   m_vp8Depay;
    VP9Depayloader                   m_vp9Depay;
    OpusDepayloader                  m_opusDepay;

    // ── Output file ───────────────────────────────────────────────────────────
    HANDLE m_hFile = INVALID_HANDLE_VALUE;

    // ── Background thread ─────────────────────────────────────────────────────
    std::thread m_thread;

    void RecordingThread();

    // ── Signaling ─────────────────────────────────────────────────────────────
    // Returns true and fills sdpOffer / sessionId on success.
    // The implementation connects to swag.live's signaling WebSocket,
    // authenticates, and retrieves the SDP offer for the requested stream.
    bool FetchSDPOffer(std::string& sdpOffer, std::string& sessionId);

    // Submits our SDP answer to the signaling server and processes any
    // trickle-ICE candidates that follow.
    bool SubmitSDPAnswer(const std::string& sessionId,
                         const std::string& sdpAnswer);

    // ── File I/O ──────────────────────────────────────────────────────────────
    bool OpenOutputFile();
    void CloseOutputFile();
    void AppendBytes(const uint8_t* data, size_t len);

    // ── Internal helpers ──────────────────────────────────────────────────────
    void SetupPeerConnection(const std::string& sdpOffer);
    void OnError(const std::string& msg);
};
