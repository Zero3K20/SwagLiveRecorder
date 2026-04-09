#pragma once

// LibDataChannelRecorder
//
// Records a swag.live stream without FFmpeg or WebView2.
//
// Signaling (reverse-engineered from TLS-decrypted PCAP):
//  1. SwagLiveAPI fetches the Agora token:
//       GET api.swag.live /feeds/user_livestream-v2 → userId
//       GET api.swag.live /pusher/retained-events?channels=private-enc-stream@{userId} → sessionId
//       GET api.swag.live /streams/{sessionId}/token → agora_token
//  2. LibDataChannelRecorder contacts the Agora gateway:
//       POST sua-ap-web-1.agora.io/api/v1?action=stringuid → numeric UID
//       POST webrtc2-ap-web-1.agora.io/api/v2/transpond/webrtc?v=2 (multipart)
//           → edge server IPs + DTLS fingerprints
//  3. A synthetic SDP answer is built from the gateway response and set as
//     the remote description; libdatachannel then performs DTLS/SRTP to the
//     Agora edge directly.
//  4. VP8 + Opus frames are muxed into .webm by WebMMuxer.

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
    void OnError(const std::string& msg);
};
