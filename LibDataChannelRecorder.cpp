// LibDataChannelRecorder.cpp
//
// FFmpeg-free, WebView2-free stream recorder.
//
// Signaling strategy (best-effort reverse engineering of swag.live)
// ─────────────────────────────────────────────────────────────────
// swag.live delivers WebRTC through what appears to be a mediasoup-based SFU
// (inferred from the page JS bundle structure and network traffic patterns
// typical of Asian live-streaming platforms).
//
// The implementation below uses rtc::WebSocket (built into libdatachannel –
// no extra libraries needed) to connect to the signaling server.  It follows
// the standard offer/answer model:
//
//   client → (join + SDP offer)  → server
//   client ← (SDP answer)        ← server
//   client ↔ (trickle ICE cands) ↔ server   [optional]
//
// If swag.live uses a proprietary mediasoup REST protocol instead of raw
// WebSocket SDP exchange, update the two marked TODO sections below after
// capturing a real WebSocket session with browser DevTools → Network → WS.
//
// Everything else (PeerConnection, depacketizers, muxer) is self-contained
// and requires no changes regardless of the signaling protocol.

#include "LibDataChannelRecorder.h"

// libdatachannel – submodule at deps/libdatachannel
#include <rtc/rtc.hpp>

#include "json_minimal.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>

// ─── Signaling constants ──────────────────────────────────────────────────────
// TODO: verify / update by capturing the WS session in browser DevTools.

// WebSocket URL for swag.live's signaling.
// Common patterns for mediasoup platforms:
//   wss://signal.swag.live/ws
//   wss://rtc.swag.live/
//   wss://api.swag.live/rtc/ws
static const std::string SIG_WS_URL = "wss://signal.swag.live/ws"; // TODO: verify

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Build a one-line JSON string suitable for WebSocket signaling.
static std::string JsonObj(std::initializer_list<std::pair<std::string,std::string>> kvs,
                            bool valueIsJson = false) {
    std::string s = "{";
    bool first = true;
    for (auto& [k, v] : kvs) {
        if (!first) s += ",";
        first = false;
        s += "\"" + k + "\":";
        if (valueIsJson) s += v; else s += "\"" + v + "\"";
    }
    s += "}";
    return s;
}

// VP8 keyframe detection from the raw VP8 bitstream (after RTP depayloading).
// RFC 6386 §19.1: frame_tag bit 0 == 0 means key frame.
static bool Vp8IsKeyFrame(const rtc::binary& frame) {
    return !frame.empty() && (std::to_integer<uint8_t>(frame[0]) & 0x01) == 0;
}

// Convert FrameInfo timestamp to milliseconds.
static int64_t InfoToMs(const rtc::FrameInfo& info, uint32_t clockHz) {
    if (info.timestampSeconds.has_value())
        return static_cast<int64_t>(info.timestampSeconds->count() * 1000.0);
    // Fallback: raw RTP timestamp / clock rate
    return static_cast<int64_t>(info.timestamp) * 1000LL / static_cast<int64_t>(clockHz);
}

// ─── LibDataChannelRecorder ───────────────────────────────────────────────────

LibDataChannelRecorder::LibDataChannelRecorder() = default;
LibDataChannelRecorder::~LibDataChannelRecorder() { Stop(); }

// ─── Start / Stop ─────────────────────────────────────────────────────────────

bool LibDataChannelRecorder::Start(const StreamInfo& streamInfo,
                                    const std::string& outputPath,
                                    const std::string& authToken) {
    if (m_running.load()) return true;

    m_streamInfo   = streamInfo;
    m_outputPath   = outputPath;
    m_authToken    = authToken;
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
    m_hFile = CreateFileA(m_outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

// ─── Recording thread ─────────────────────────────────────────────────────────

void LibDataChannelRecorder::RecordingThread() {
    // ── 1. PeerConnection ─────────────────────────────────────────────────────
    rtc::Configuration rtcConfig;
    rtcConfig.iceServers = {
        rtc::IceServer("stun:stun.l.google.com:19302"),
        rtc::IceServer("stun:stun1.l.google.com:19302"),
    };
    auto pc = std::make_shared<rtc::PeerConnection>(rtcConfig);

    std::atomic<bool> pcFailed { false };
    pc->onStateChange([&](rtc::PeerConnection::State s) {
        if (s == rtc::PeerConnection::State::Failed ||
            s == rtc::PeerConnection::State::Closed)
            pcFailed.store(true);
    });

    // ── 2. Add receive-only tracks and wire depacketizers ─────────────────────
    // Video: offer both VP8 and VP9; the server will pick one in its answer.
    rtc::Description::Video vd("video", rtc::Description::Direction::RecvOnly);
    vd.addVP8Codec(96);
    vd.addVP9Codec(98);
    auto videoTrack = pc->addTrack(vd);
    videoTrack->setMediaHandler(std::make_shared<rtc::VP8RtpDepacketizer>());
    videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

    // Audio: Opus
    rtc::Description::Audio ad("audio", rtc::Description::Direction::RecvOnly);
    ad.addOpusCodec(111);
    auto audioTrack = pc->addTrack(ad);
    audioTrack->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
    audioTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

    // ── 3. Init the muxer ─────────────────────────────────────────────────────
    m_muxer = std::make_unique<WebMMuxer>(
        [this](const uint8_t* d, size_t n) { AppendBytes(d, n); });
    // Codec choice deferred until first frame; default dimensions are 1280×720.
    // The muxer header is written in Init() below once the codec is known.
    // We initialise with VP8 now; if the server chose VP9 we'll have written
    // the wrong codec ID – acceptable trade-off without SDP parsing.
    bool muxerInitialized = false;

    videoTrack->onFrame([&](rtc::binary frame, rtc::FrameInfo info) {
        if (!muxerInitialized) {
            // Initialise on first video frame so we know a keyframe is coming.
            // VP8 keyframe heuristic: bit 0 of first byte == 0.
            bool isVP9 = (std::to_integer<uint8_t>(frame[0]) & 0xC0) == 0x80; // rough VP9 heuristic
            m_muxer->Init(isVP9 ? WebMMuxer::VideoCodec::VP9 : WebMMuxer::VideoCodec::VP8,
                          1280, 720, 48000, 2);
            muxerInitialized = true;
        }
        bool kf     = Vp8IsKeyFrame(frame);
        int64_t pts = InfoToMs(info, 90000);
        m_muxer->WriteVideoFrame(
            reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), pts, kf);
    });

    audioTrack->onFrame([&](rtc::binary frame, rtc::FrameInfo info) {
        if (!muxerInitialized) return;  // don't write audio before video init
        int64_t pts = InfoToMs(info, 48000);
        m_muxer->WriteAudioPacket(
            reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), pts);
    });

    // ── 4. Generate SDP offer ─────────────────────────────────────────────────
    std::mutex       gathMtx;
    std::condition_variable gathCV;
    bool             gathDone = false;

    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState gs) {
        if (gs == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lk(gathMtx);
            gathDone = true;
            gathCV.notify_all();
        }
    });

    pc->setLocalDescription();   // kicks off ICE gathering, fires onLocalDescription

    {
        std::unique_lock<std::mutex> lk(gathMtx);
        gathCV.wait_for(lk, std::chrono::seconds(15),
                        [&]{ return gathDone || m_stopRequest.load(); });
    }
    if (m_stopRequest.load()) { CloseOutputFile(); m_running.store(false); return; }

    auto offerDesc = pc->localDescription();
    if (!offerDesc) {
        OnError("libdatachannel did not produce a local description");
        CloseOutputFile(); m_running.store(false); return;
    }
    std::string offerSDP = std::string(*offerDesc);

    // ── 5. Signaling via rtc::WebSocket ───────────────────────────────────────
    // TODO: update SIG_WS_URL and the join/offer/answer message shapes to
    // match what browser DevTools shows for wss://... on swag.live/livestream/<user>.

    std::mutex wsMtx;
    std::condition_variable wsCV;
    std::string answerSDP;
    bool answerReady = false;

    rtc::WebSocket::Configuration wsCfg;
    // No extra TLS config needed; libdatachannel uses its built-in TLS backend.
    auto ws = std::make_shared<rtc::WebSocket>(wsCfg);

    ws->onOpen([&]() {
        // TODO: adjust field names / nesting to match the actual protocol.
        // Common mediasoup / protoo pattern:
        //   { "type": "join",
        //     "data": { "streamId": "<id>", "token": "<jwt>", "role": "viewer" } }
        std::string streamId = m_streamInfo.streamId.empty()
                             ? m_streamInfo.username
                             : m_streamInfo.streamId;
        std::ostringstream msg;
        msg << "{"
            << "\"type\":\"join\","
            << "\"data\":{"
            <<   "\"streamId\":\"" << streamId << "\","
            <<   "\"token\":\""    << m_authToken << "\","
            <<   "\"role\":\"viewer\""
            << "}}";
        ws->send(msg.str());

        // Send our SDP offer.
        // TODO: some platforms expect the offer inside the join message;
        //       others send it as a separate message after joining.
        std::ostringstream offerMsg;
        offerMsg << "{"
                 << "\"type\":\"offer\","
                 << "\"data\":{"
                 <<   "\"sdp\":\"" << offerSDP << "\""  // NOTE: SDP may need JSON escaping
                 << "}}";
        ws->send(offerMsg.str());
    });

    ws->onMessage([&](std::variant<rtc::binary, rtc::string> message) {
        if (!std::holds_alternative<rtc::string>(message)) return;
        const auto& text = std::get<rtc::string>(message);
        JsonValue root = parse_json(text);
        if (root.is_null()) return;

        std::string msgType = root["type"].as_str();

        if (msgType == "answer") {
            // Shape A: { "type":"answer", "data": { "sdp":"..." } }
            std::string sdp = root["data"]["sdp"].as_str();
            // Shape B: { "type":"answer", "sdp":"..." }
            if (sdp.empty()) sdp = root["sdp"].as_str();

            if (!sdp.empty()) {
                std::lock_guard<std::mutex> lk(wsMtx);
                answerSDP   = sdp;
                answerReady = true;
                wsCV.notify_all();
            }
        } else if (msgType == "candidate") {
            // Trickle ICE from server
            // Shape: { "type":"candidate", "data": { "candidate":"...", "sdpMid":"..." } }
            std::string cand = root["data"]["candidate"].as_str();
            std::string mid  = root["data"]["sdpMid"].as_str();
            if (cand.empty()) { cand = root["candidate"].as_str(); mid = root["sdpMid"].as_str(); }
            if (!cand.empty()) {
                try { pc->addRemoteCandidate(rtc::Candidate(cand, mid)); }
                catch (...) {}
            }
        } else if (msgType == "error") {
            std::string errMsg = root["message"].as_str();
            if (errMsg.empty()) errMsg = root["data"]["message"].as_str();
            OnError("Signaling error: " + errMsg);
            std::lock_guard<std::mutex> lk(wsMtx);
            answerReady = true;  // unblock wait
            wsCV.notify_all();
        }
    });

    ws->onClosed([&]() {
        // Unblock the answer wait if the server closed the connection early.
        std::lock_guard<std::mutex> lk(wsMtx);
        if (!answerReady) wsCV.notify_all();
    });

    // Connect (non-blocking; open() returns before the handshake completes).
    ws->open(SIG_WS_URL);

    {
        std::unique_lock<std::mutex> lk(wsMtx);
        wsCV.wait_for(lk, std::chrono::seconds(30),
                      [&]{ return answerReady || m_stopRequest.load(); });
    }

    if (answerSDP.empty()) {
        OnError("Timed out waiting for SDP answer from signaling server. "
                "Update SIG_WS_URL and join message in LibDataChannelRecorder.cpp.");
        ws->close();
        CloseOutputFile(); m_running.store(false); return;
    }

    // ── 6. Set remote description (server's answer) ───────────────────────────
    try {
        pc->setRemoteDescription(rtc::Description(answerSDP, rtc::Description::Type::Answer));
    } catch (const std::exception& ex) {
        OnError(std::string("setRemoteDescription failed: ") + ex.what());
        ws->close();
        CloseOutputFile(); m_running.store(false); return;
    }

    if (onStarted) onStarted(m_streamInfo.username);

    // ── 7. Run until stop requested or connection dies ────────────────────────
    while (!m_stopRequest.load() && !pcFailed.load()) {
        Sleep(250);
    }

    // ── 8. Tear down ──────────────────────────────────────────────────────────
    ws->close();
    videoTrack->close();
    audioTrack->close();
    pc->close();
    m_muxer.reset();

    size_t total = m_bytesWritten.load();
    CloseOutputFile();
    m_running.store(false);
    if (onStopped) onStopped(m_streamInfo.username, total);
}
