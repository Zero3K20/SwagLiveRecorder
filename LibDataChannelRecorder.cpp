// LibDataChannelRecorder.cpp
//
// FFmpeg-free recording via libdatachannel + a hand-rolled WebM muxer.
//
// ─── Signaling notes ──────────────────────────────────────────────────────────
// swag.live uses WebRTC delivered by a mediasoup-based SFU (inferred from the
// page structure and typical platform architecture for Asian live-streaming
// sites).  The exact WebSocket URL, room-join sequence, and SDP exchange format
// below are a best-effort reverse-engineering attempt based on:
//   • the REST API base "https://api.swag.live" already in the project,
//   • common mediasoup / protoo-WebSocket signalling patterns, and
//   • what a browser DevTools network capture of swag.live/livestream/<user>
//     would typically reveal.
//
// If the signaling details have changed, capture the WebSocket traffic in
// browser DevTools (Filter: WS) and update the constants / message shapes in
// the two functions FetchSDPOffer() and SubmitSDPAnswer() below.
// ─────────────────────────────────────────────────────────────────────────────

#include "LibDataChannelRecorder.h"

// libdatachannel full C++ header (install via vcpkg: libdatachannel:x64-windows)
#include <rtc/rtc.hpp>

#include "tlsclient/tlsclient.h"
#include "json_minimal.h"

#include <winhttp.h>

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

// ─── Signaling constants ──────────────────────────────────────────────────────
// TODO: verify / update these by inspecting the WebSocket traffic in browser
// DevTools when visiting https://swag.live/livestream/<username>.

// swag.live appears to use a mediasoup / protoo WebSocket signaling server.
// The signaling host is separate from the REST API host.
static const wchar_t* SIG_HOST = L"signal.swag.live";      // TODO: verify hostname
static const INTERNET_PORT SIG_PORT = 443;                  // WSS
static const wchar_t* SIG_PATH = L"/ws";                   // TODO: verify path

// After connecting, the client sends a "join" request and the server replies
// with an SDP offer wrapped in a JSON envelope.  Message field names and the
// join payload shape depend on the exact server-side framework.
static const char* MSG_TYPE_OFFER  = "offer";              // TODO: verify
static const char* MSG_TYPE_ANSWER = "answer";             // TODO: verify
static const char* MSG_TYPE_ICE    = "candidate";          // TODO: verify

// ─── Minimal WinHTTP WebSocket helper ────────────────────────────────────────

class WsClient {
public:
    ~WsClient() { Close(); }

    bool Connect(const wchar_t* host, INTERNET_PORT port, const wchar_t* path,
                 const std::wstring& extraHeaders) {
        m_hSession = WinHttpOpen(L"SwagLiveRecorder/1.0",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_hSession) return false;

        m_hConnect = WinHttpConnect(m_hSession, host, port, 0);
        if (!m_hConnect) return false;

        DWORD flags = WINHTTP_FLAG_SECURE;
        m_hRequest = WinHttpOpenRequest(m_hConnect, L"GET", path,
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!m_hRequest) return false;

        // Signal WinHTTP that we want a WebSocket upgrade
        if (!WinHttpSetOption(m_hRequest,
                              WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                              nullptr, 0)) return false;

        // Extra headers (e.g. Authorization)
        if (!extraHeaders.empty()) {
            WinHttpAddRequestHeaders(m_hRequest, extraHeaders.c_str(),
                                     static_cast<DWORD>(-1L),
                                     WINHTTP_ADDREQ_FLAG_ADD);
        }

        if (!WinHttpSendRequest(m_hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;

        if (!WinHttpReceiveResponse(m_hRequest, nullptr)) return false;

        m_hWs = WinHttpWebSocketCompleteUpgrade(m_hRequest, 0);
        return m_hWs != nullptr;
    }

    // Send a UTF-8 text frame.
    bool SendText(const std::string& msg) {
        if (!m_hWs) return false;
        return WinHttpWebSocketSend(m_hWs,
                                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    const_cast<void*>(static_cast<const void*>(msg.data())),
                                    static_cast<DWORD>(msg.size())) == ERROR_SUCCESS;
    }

    // Receive one complete message (text or binary).  Blocks until data arrives
    // or an error occurs.  Returns false on error/close.
    bool Recv(std::string& out) {
        out.clear();
        if (!m_hWs) return false;

        std::vector<uint8_t> buf(4096);
        for (;;) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
            DWORD rc = WinHttpWebSocketReceive(m_hWs,
                                               buf.data(),
                                               static_cast<DWORD>(buf.size()),
                                               &bytesRead, &bufType);
            if (rc != ERROR_SUCCESS) return false;
            if (bytesRead > 0)
                out.append(reinterpret_cast<const char*>(buf.data()), bytesRead);

            // Fragment types end with _MESSAGE_BUFFER_TYPE (not _FRAGMENT_BUFFER_TYPE)
            if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                return true;
            }
            if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                return false;
            }
        }
    }

    void Close() {
        if (m_hWs) {
            WinHttpWebSocketClose(m_hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr, 0);
            WinHttpCloseHandle(m_hWs); m_hWs = nullptr;
        }
        if (m_hRequest) { WinHttpCloseHandle(m_hRequest); m_hRequest = nullptr; }
        if (m_hConnect) { WinHttpCloseHandle(m_hConnect); m_hConnect = nullptr; }
        if (m_hSession) { WinHttpCloseHandle(m_hSession); m_hSession = nullptr; }
    }

private:
    HINTERNET m_hSession  = nullptr;
    HINTERNET m_hConnect  = nullptr;
    HINTERNET m_hRequest  = nullptr;
    HINTERNET m_hWs       = nullptr;
};

// ─── LibDataChannelRecorder – lifecycle ───────────────────────────────────────

LibDataChannelRecorder::LibDataChannelRecorder() = default;

LibDataChannelRecorder::~LibDataChannelRecorder() {
    Stop();
}

bool LibDataChannelRecorder::Start(const StreamInfo& streamInfo,
                                    const std::string& outputPath,
                                    const std::string& authToken) {
    if (m_running.load()) return true;

    m_streamInfo  = streamInfo;
    m_outputPath  = outputPath;
    m_authToken   = authToken;
    m_lastError.clear();
    m_bytesWritten.store(0);
    m_stopRequest.store(false);

    if (!OpenOutputFile()) return false;

    m_running.store(true);
    m_thread = std::thread(&LibDataChannelRecorder::RecordingThread, this);
    return true;
}

void LibDataChannelRecorder::Stop() {
    m_stopRequest.store(true);
    if (m_thread.joinable()) m_thread.join();
}

// ─── File I/O ─────────────────────────────────────────────────────────────────

bool LibDataChannelRecorder::OpenOutputFile() {
    m_hFile = CreateFileA(m_outputPath.c_str(),
                          GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Cannot create output file: " + m_outputPath;
        return false;
    }
    return true;
}

void LibDataChannelRecorder::CloseOutputFile() {
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
}

void LibDataChannelRecorder::AppendBytes(const uint8_t* data, size_t len) {
    if (m_hFile == INVALID_HANDLE_VALUE || len == 0) return;
    DWORD written = 0;
    WriteFile(m_hFile, data, static_cast<DWORD>(len), &written, nullptr);
    size_t total = m_bytesWritten.fetch_add(written) + written;
    if (onProgress) onProgress(m_streamInfo.username, total);
}

void LibDataChannelRecorder::OnError(const std::string& msg) {
    m_lastError = msg;
    if (onError) onError(m_streamInfo.username, msg);
}

// ─── Signaling – FetchSDPOffer ────────────────────────────────────────────────
//
// Connects to swag.live's signaling WebSocket, joins the stream room, and
// retrieves the server's SDP offer.
//
// PROTOCOL NOTES (best-effort – update from DevTools capture if needed):
//
//  Connection:  wss://signal.swag.live/ws
//
//  After the WebSocket upgrade, the client sends:
//    { "type":"join",
//      "data":{ "streamId":"<streamId>", "token":"<authToken>" } }
//
//  The server replies with one or more messages.  We look for one whose
//  "type" field equals "offer" and extract "data.sdp".
//
//  Ice candidates follow as:
//    { "type":"candidate", "data":{ "candidate":"...", "sdpMid":"...", "sdpMLineIndex":0 } }

bool LibDataChannelRecorder::FetchSDPOffer(std::string& sdpOffer,
                                            std::string& sessionId) {
    sdpOffer.clear();
    sessionId.clear();

    // Build the Authorization header if we have a token.
    std::wstring headers;
    if (!m_authToken.empty()) {
        headers  = L"Authorization: Bearer ";
        headers += std::wstring(m_authToken.begin(), m_authToken.end());
        headers += L"\r\n";
    }
    // Include Origin so the server does not reject the WebSocket handshake.
    headers += L"Origin: https://swag.live\r\n";

    WsClient ws;
    if (!ws.Connect(SIG_HOST, SIG_PORT, SIG_PATH, headers)) {
        m_lastError = "WebSocket connect to signaling server failed";
        return false;
    }

    // ── Send join request ─────────────────────────────────────────────────────
    // TODO: adjust the message shape to match what the server actually expects.
    //  - "streamId" vs "slug" vs "username" – inspect the JS bundle or network
    //    traffic to find the correct field name.
    //  - Some platforms expect a REST call first to get a "room token", which is
    //    then passed in the WebSocket join message.
    std::string streamId = m_streamInfo.streamId.empty()
                         ? m_streamInfo.username
                         : m_streamInfo.streamId;

    std::ostringstream joinMsg;
    joinMsg << "{"
            << "\"type\":\"join\","
            << "\"data\":{"
            <<   "\"streamId\":\"" << streamId << "\","
            <<   "\"role\":\"viewer\","
            <<   "\"token\":\"" << m_authToken << "\""
            << "}}";
    if (!ws.SendText(joinMsg.str())) {
        m_lastError = "Failed to send join request to signaling server";
        return false;
    }

    // ── Wait for the SDP offer ────────────────────────────────────────────────
    // We loop, discarding messages we don't recognise, until we see one with
    // "type":"offer" (or until a timeout / stop is requested).
    const int MAX_MESSAGES = 20;
    for (int i = 0; i < MAX_MESSAGES && !m_stopRequest.load(); ++i) {
        std::string raw;
        if (!ws.Recv(raw)) break;

        JsonValue root = parse_json(raw);
        if (root.is_null()) continue;

        std::string msgType = root["type"].as_str();

        if (msgType == MSG_TYPE_OFFER) {
            // Shape A: { "type":"offer", "data":{ "sdp":"...", "sessionId":"..." } }
            if (!root["data"]["sdp"].is_null()) {
                sdpOffer  = root["data"]["sdp"].as_str();
                sessionId = root["data"]["sessionId"].as_str();
                return !sdpOffer.empty();
            }
            // Shape B: { "type":"offer", "sdp":"..." }
            if (!root["sdp"].is_null()) {
                sdpOffer  = root["sdp"].as_str();
                sessionId = root["sessionId"].as_str();
                return !sdpOffer.empty();
            }
        } else if (msgType == "error") {
            m_lastError = "Signaling error: " + root["message"].as_str();
            return false;
        }
        // Any other message type (e.g. "joined", "routerRtpCapabilities") is
        // silently skipped here.  You may need to handle them for some platforms.
    }

    m_lastError = "Timed out waiting for SDP offer from signaling server";
    return false;
}

// ─── Signaling – SubmitSDPAnswer ─────────────────────────────────────────────

bool LibDataChannelRecorder::SubmitSDPAnswer(const std::string& sessionId,
                                              const std::string& sdpAnswer) {
    // For many platforms the answer is sent back on the same WebSocket.
    // Here we use a fresh HTTPS POST to the REST API as an alternative –
    // adjust the approach to match what the server expects.
    //
    // TODO: determine whether swag.live expects the answer on the WebSocket
    // (re-use the same WsClient) or via a separate HTTP POST.

    std::ostringstream body;
    body << "{"
         << "\"sessionId\":\"" << sessionId << "\","
         << "\"type\":\"answer\","
         << "\"sdp\":" << sdpAnswer       // sdpAnswer is already a JSON string value
         << "}";

    std::string rawBody = body.str();

    // Build headers
    std::string headers;
    headers  = "Content-Type: application/json\r\n";
    headers += "Accept: application/json\r\n";
    headers += "Origin: https://swag.live\r\n";
    if (!m_authToken.empty())
        headers += "Authorization: Bearer " + m_authToken + "\r\n";

    // TODO: replace with the correct REST endpoint for submitting the answer.
    std::string url = "https://api.swag.live/webrtc/answer";

    TLSClient client;
    std::string response;
    if (!client.HttpPost(url, rawBody, response, headers)) {
        // Not a hard failure – some servers accept the answer only via WebSocket
        // (in which case FetchSDPOffer / SubmitSDPAnswer would share a WsClient).
        // Log but continue; the ICE exchange may still succeed.
        m_lastError = "Answer POST failed (may be harmless): " + client.GetLastError();
    }
    return true;
}

// ─── PeerConnection setup ─────────────────────────────────────────────────────

void LibDataChannelRecorder::SetupPeerConnection(const std::string& sdpOffer) {
    // Configure STUN server(s).
    rtc::Configuration config;
    config.iceServers = {
        rtc::IceServer("stun:stun.l.google.com:19302"),
        rtc::IceServer("stun:stun1.l.google.com:19302"),
    };
    config.disableAutoNegotiation = false;

    m_pc = std::make_shared<rtc::PeerConnection>(config);

    // ── Callbacks ─────────────────────────────────────────────────────────────

    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            m_stopRequest.store(true);
        }
    });

    m_pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
        const std::string& mid = track->mid();

        // Determine codec from the track's SDP media description.
        std::string desc = std::string(track->description());
        bool isVideo = (desc.find("m=video") != std::string::npos);
        bool isVP9   = (desc.find("VP9")  != std::string::npos);

        if (isVideo) {
            m_videoTrack = track;
            track->onMessage([this, isVP9](rtc::binary msg) {
                const auto* data = reinterpret_cast<const uint8_t*>(msg.data());
                size_t      len  = msg.size();
                if (isVP9)
                    m_vp9Depay.Push(data, len);
                else
                    m_vp8Depay.Push(data, len);
            }, nullptr /* string handler not used for RTP */);
        } else {
            m_audioTrack = track;
            track->onMessage([this](rtc::binary msg) {
                const auto* data = reinterpret_cast<const uint8_t*>(msg.data());
                m_opusDepay.Push(data, msg.size());
            }, nullptr);
        }
    });

    // ── Depayloader callbacks → muxer ────────────────────────────────────────

    m_vp8Depay.onFrame = [this](const uint8_t* d, size_t n,
                                 int64_t pts_ms, bool kf) {
        if (m_muxer) m_muxer->WriteVideoFrame(d, n, pts_ms, kf);
    };
    m_vp9Depay.onFrame = [this](const uint8_t* d, size_t n,
                                 int64_t pts_ms, bool kf) {
        if (m_muxer) m_muxer->WriteVideoFrame(d, n, pts_ms, kf);
    };
    m_opusDepay.onPacket = [this](const uint8_t* d, size_t n, int64_t pts_ms) {
        if (m_muxer) m_muxer->WriteAudioPacket(d, n, pts_ms);
    };

    // ── Set remote description (server's offer) ───────────────────────────────
    m_pc->setRemoteDescription(rtc::Description(sdpOffer, rtc::Description::Type::Offer));
    // onLocalDescription fires synchronously: the PeerConnection creates our answer.
}

// ─── Recording thread ─────────────────────────────────────────────────────────

void LibDataChannelRecorder::RecordingThread() {
    // Step 1: signaling – get the server's SDP offer.
    std::string sdpOffer, sessionId;
    if (!FetchSDPOffer(sdpOffer, sessionId)) {
        CloseOutputFile();
        m_running.store(false);
        if (onError) onError(m_streamInfo.username, m_lastError);
        return;
    }

    // Step 2: set up the muxer (we infer codec from the SDP offer).
    // Default to VP8; override to VP9 if the offer says so.
    WebMMuxer::VideoCodec codec = (sdpOffer.find("VP9") != std::string::npos)
                                ? WebMMuxer::VideoCodec::VP9
                                : WebMMuxer::VideoCodec::VP8;

    // Attempt to parse video dimensions from the SDP (a=imageattr or guessing).
    // Fallback: 1280×720.  The header will be overwritten once the first frame
    // arrives if needed (muxer doesn't support rewriting; use sane defaults).
    uint16_t width = 1280, height = 720;

    m_muxer = std::make_unique<WebMMuxer>(
        [this](const uint8_t* d, size_t n) { AppendBytes(d, n); });
    m_muxer->Init(codec, width, height, 48000, 2);

    // Step 3: create PeerConnection and set the remote offer.
    // The library will produce an SDP answer via the onLocalDescription callback.
    std::string sdpAnswer;
    std::mutex  answerMutex;
    std::condition_variable answerCV;
    bool answerReady = false;

    // Temporarily wire onLocalDescription before calling SetupPeerConnection so
    // we can capture the answer string.
    //
    // We do the setup in a small scope to avoid a race.
    SetupPeerConnection(sdpOffer);

    // Capture the local description (our SDP answer).
    m_pc->onLocalDescription([&](rtc::Description desc) {
        std::lock_guard<std::mutex> lk(answerMutex);
        sdpAnswer    = std::string(desc);
        answerReady  = true;
        answerCV.notify_all();
    });

    // Force local description generation (answer to the offer we just set).
    m_pc->setLocalDescription(rtc::Description::Type::Answer);

    {
        std::unique_lock<std::mutex> lk(answerMutex);
        answerCV.wait_for(lk, std::chrono::seconds(10),
                          [&]{ return answerReady || m_stopRequest.load(); });
    }

    if (sdpAnswer.empty()) {
        OnError("Timed out waiting for local SDP answer from libdatachannel");
        CloseOutputFile();
        m_running.store(false);
        return;
    }

    // Step 4: submit our answer to the server.
    SubmitSDPAnswer(sessionId, sdpAnswer);

    if (onStarted) onStarted(m_streamInfo.username);

    // Step 5: spin until the caller requests a stop or the PeerConnection dies.
    while (!m_stopRequest.load()) {
        Sleep(250);

        // Detect unexpected disconnection.
        if (m_pc) {
            auto state = m_pc->state();
            if (state == rtc::PeerConnection::State::Failed  ||
                state == rtc::PeerConnection::State::Closed  ||
                state == rtc::PeerConnection::State::Disconnected) {
                break;
            }
        }
    }

    // Step 6: tear down.
    if (m_videoTrack) { m_videoTrack->close(); m_videoTrack.reset(); }
    if (m_audioTrack) { m_audioTrack->close(); m_audioTrack.reset(); }
    if (m_pc)         { m_pc->close();         m_pc.reset(); }
    m_muxer.reset();

    size_t total = m_bytesWritten.load();
    CloseOutputFile();
    m_running.store(false);
    if (onStopped) onStopped(m_streamInfo.username, total);
}
