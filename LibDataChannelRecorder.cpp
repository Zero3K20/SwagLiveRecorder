// LibDataChannelRecorder.cpp
//
// Records a swag.live stream without FFmpeg or WebView2.
//
// ─── How swag.live's streaming works (reverse-engineered from PCAP) ───────────
//
//  1. Discovery  (SwagLiveAPI.cpp)
//       GET api.swag.live /feeds/user_livestream-v2?limit=100&page=1&sorting=desc:s_score
//           → JSON array of live users: [{username, id, displayName, …}]
//       GET api.swag.live /pusher/retained-events?channels=private-enc-stream@{userId}
//           → {"private-enc-stream@{userId}": [{"event":"stream.online",
//               "data":{"session":"<sessionId>","preset":"preview","price":0,…}}]}
//       GET api.swag.live /streams/{sessionId}/token
//           → {"agora_token":"…","agora_token_session_id":"…","agora_exp":…}
//
//  2. Agora RTC signaling  (this file)
//       POST sua-ap-web-1.agora.io /api/v1?action=stringuid
//           body: {"sid":"{sid}","opid":10,"appid":"19c9ed8fd65f4ea9b5de096362af989e",
//                  "string_uid":"{sessionUuid}"}
//           → {"code":0,"uid":{numericUid},…}
//
//       POST webrtc2-ap-web-1.agora.io /api/v2/transpond/webrtc?v=2
//           Content-Type: multipart/form-data; boundary=…
//           field "request": {"appid":"19c9ed8fd65f4ea9b5de096362af989e",
//                             "client_ts":{msNow},"opid":{rand64},"sid":"{sid}",
//                             "request_bodies":[{"uri":22,"buffer":{
//                               "cname":"{sessionId}",
//                               "detail":{"6":"{sessionUuid}","11":"CN,GLOBAL",
//                                         "17":"2","22":"CN,GLOBAL"},
//                               "key":"{agoraToken}","service_ids":[11,26],"uid":0}}]}
//           → {"response_body":[{"uri":23,"buffer":{
//                 "edges_services":[{"ip":"208.97.254.20","port":4700},…],
//                 "detail":{"19":"AA:BB:…;CC:DD:…;","10":"{token}"},
//                 "uid":{numericUid},"cname":"{sessionId}","code":0,…}}],…}
//
//  3. WebRTC  (libdatachannel)
//       Use the edge IPs as ICE host candidates (they accept raw DTLS/SRTP).
//       The DTLS fingerprints from detail["19"] are used to verify the server cert.
//       VP8 video + Opus audio frames are forwarded to WebMMuxer.
//
// ─── Agora App ID ─────────────────────────────────────────────────────────────
//   19c9ed8fd65f4ea9b5de096362af989e   (seen in every Agora API call in PCAP)

#include "LibDataChannelRecorder.h"
#include "SwagLiveAPI.h"

// libdatachannel – submodule at deps/libdatachannel
#include <rtc/rtc.hpp>

#include "tlsclient/tlsclient.h"
#include "json_minimal.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>

// ─── Agora constants ──────────────────────────────────────────────────────────
// AGORA_APP_ID is declared in SwagLiveAPI.h (already included above)
static const char* AGORA_SUA_HOST    = "https://sua-ap-web-1.agora.io";
static const char* AGORA_WEBRTC_HOST = "https://webrtc2-ap-web-1.agora.io";
// Fallback gateway (sd-rtn.com mirrors agora.io)
static const char* AGORA_WEBRTC_FB   = "https://webrtc2-2.ap.sd-rtn.com";

// ─── Utility ──────────────────────────────────────────────────────────────────

// Generate a random 32-character uppercase hex SID.
static std::string MakeHexSid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0')
        << std::setw(16) << rng() << std::setw(16) << rng();
    return oss.str();
}

// Generate a random UUID v4.
static std::string MakeUuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    uint64_t hi = rng(), lo = rng();
    // Version 4, variant 1
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (unsigned)(hi >> 32),
        (unsigned)((hi >> 16) & 0xFFFF),
        (unsigned)(hi & 0xFFFF),
        (unsigned)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

// Escape a string for JSON embedding.
static std::string JsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += (char)c; break;
        }
    }
    return out;
}

// VP8 keyframe: bit 0 of first byte == 0.
static bool Vp8IsKeyFrame(const rtc::binary& f) {
    return !f.empty() && (std::to_integer<uint8_t>(f[0]) & 0x01) == 0;
}

static int64_t InfoToMs(const rtc::FrameInfo& info, uint32_t hz) {
    if (info.timestampSeconds.has_value())
        return static_cast<int64_t>(info.timestampSeconds->count() * 1000.0);
    return static_cast<int64_t>(info.timestamp) * 1000LL / (int64_t)hz;
}

// ─── AgoraEdge ───────────────────────────────────────────────────────────────
struct AgoraEdge {
    std::string ip;
    int         port;
};

struct AgoraGatewayResult {
    std::vector<AgoraEdge>  edges;
    std::string             dtlsFingerprints; // semicolon-separated SHA-256 hashes
    std::string             agoraToken;       // detail["10"] mirror
    uint32_t                numericUid;
    bool                    ok;
    AgoraGatewayResult() : numericUid(0), ok(false) {}
};

// ─── Agora signaling helpers ──────────────────────────────────────────────────

// Step A: POST /api/v1?action=stringuid  → numeric UID for our session UUID.
static uint32_t AgoraStringUid(TLSClient& http,
                                const std::string& sid,
                                const std::string& sessionUuid) {
    std::string body =
        "{\"sid\":\"" + sid + "\","
        "\"opid\":10,"
        "\"appid\":\"" + AGORA_APP_ID + "\","
        "\"string_uid\":\"" + sessionUuid + "\"}";

    std::string resp;
    std::string hdrs = "Content-Type: application/json\r\n";
    if (!http.HttpPost(std::string(AGORA_SUA_HOST) + "/api/v1?action=stringuid",
                       body, resp, hdrs))
        return 0;

    resp = get_http_body(resp);
    if (resp.empty()) return 0;

    JsonValue root = parse_json(resp);
    if (root.is_null()) return 0;
    return static_cast<uint32_t>(root["uid"].as_num());
}

// Step B: POST /api/v2/transpond/webrtc?v=2  → edge IPs + DTLS fingerprints.
// The request is a multipart/form-data with one field named "request".
static AgoraGatewayResult AgoraGetEdges(TLSClient& http,
                                        const std::string& sid,
                                        const std::string& sessionUuid,
                                        const std::string& sessionId,   // Swag session = cname
                                        const std::string& agoraToken,
                                        uint32_t           numericUid) {
    AgoraGatewayResult result;

    // Build the request JSON (observed exactly in PCAP).
    // opid is a random 64-bit integer; use current time * random to avoid collisions.
    std::random_device rd;
    std::mt19937_64 rng(rd());
    uint64_t opid = rng() & 0x7FFFFFFFFFFFFFFFULL;

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::ostringstream req;
    req << "{"
        << "\"appid\":\""     << AGORA_APP_ID << "\","
        << "\"client_ts\":"   << nowMs         << ","
        << "\"opid\":"        << opid           << ","
        << "\"sid\":\""       << sid            << "\","
        << "\"request_bodies\":[{"
        <<   "\"uri\":22,"
        <<   "\"buffer\":{"
        <<     "\"cname\":\""       << JsonEsc(sessionId)   << "\","
        <<     "\"detail\":{"
        <<       "\"6\":\""         << JsonEsc(sessionUuid) << "\","
        <<       "\"11\":\"CN,GLOBAL\","
        <<       "\"17\":\"2\","
        <<       "\"22\":\"CN,GLOBAL\""
        <<     "},"
        <<     "\"key\":\""         << JsonEsc(agoraToken)  << "\","
        <<     "\"service_ids\":[11,26],"
        <<     "\"uid\":"           << numericUid
        <<   "}"
        << "}]}";

    // Build multipart body (boundary chosen to match observed PCAP pattern)
    const std::string boundary = "----WebKitFormBoundarySwagLiveRecorder0001";
    std::string mp;
    mp += "--" + boundary + "\r\n";
    mp += "Content-Disposition: form-data; name=\"request\"\r\n\r\n";
    mp += req.str();
    mp += "\r\n--" + boundary + "--\r\n";

    std::string hdrs =
        "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";

    // Try primary gateway, fall back to sd-rtn.com mirror.
    std::string resp;
    bool ok = http.HttpPost(std::string(AGORA_WEBRTC_HOST) + "/api/v2/transpond/webrtc?v=2",
                            mp, resp, hdrs);
    if (!ok) {
        ok = http.HttpPost(std::string(AGORA_WEBRTC_FB) + "/api/v2/transpond/webrtc?v=2",
                           mp, resp, hdrs);
    }
    if (!ok) return result;

    resp = get_http_body(resp);
    if (resp.empty()) return result;

    JsonValue root = parse_json(resp);
    if (root.is_null()) return result;

    // Parse response_body array.
    const JsonValue& rb = root["response_body"];
    if (rb.is_null() || rb.type != JsonValue::Type::Array) return result;

    for (size_t i = 0; i < rb.size(); ++i) {
        const JsonValue& entry  = rb[i];
        const JsonValue& buf    = entry["buffer"];
        if (buf.is_null()) continue;

        if (buf["code"].as_num() != 0) continue;  // error from Agora edge

        result.numericUid = static_cast<uint32_t>(buf["uid"].as_num());
        result.dtlsFingerprints = buf["detail"]["19"].as_str();
        result.agoraToken       = buf["detail"]["10"].as_str();

        const JsonValue& edges = buf["edges_services"];
        if (edges.type == JsonValue::Type::Array) {
            for (size_t j = 0; j < edges.size(); ++j) {
                AgoraEdge e;
                e.ip   = edges[j]["ip"].as_str();
                e.port = static_cast<int>(edges[j]["port"].as_num());
                if (!e.ip.empty() && e.port > 0)
                    result.edges.push_back(std::move(e));
            }
        }

        if (!result.edges.empty()) {
            result.ok = true;
            break;  // first successful response_body entry is sufficient
        }
    }
    return result;
}

// ─── LibDataChannelRecorder ───────────────────────────────────────────────────

LibDataChannelRecorder::LibDataChannelRecorder() = default;
LibDataChannelRecorder::~LibDataChannelRecorder() { Stop(); }

bool LibDataChannelRecorder::Start(const StreamInfo& si,
                                    const std::string& outputPath,
                                    const std::string& authToken) {
    if (m_running.load()) return true;

    m_streamInfo   = si;
    m_outputPath   = outputPath;
    m_authToken    = authToken;
    m_lastError.clear();
    m_bytesWritten.store(0);
    m_stopRequest.store(false);

    m_hFile = CreateFileA(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Cannot create output file: " + outputPath;
        return false;
    }

    m_running.store(true);
    m_thread = std::thread(&LibDataChannelRecorder::RecordingThread, this);
    return true;
}

void LibDataChannelRecorder::Stop() {
    m_stopRequest.store(true);
    if (m_thread.joinable()) m_thread.join();
}

void LibDataChannelRecorder::AppendBytes(const uint8_t* data, size_t len) {
    if (m_hFile == INVALID_HANDLE_VALUE || !data || len == 0) return;
    DWORD written = 0;
    WriteFile(m_hFile, data, (DWORD)len, &written, nullptr);
    size_t total = m_bytesWritten.fetch_add(written) + written;
    if (onProgress) onProgress(m_streamInfo.username, total);
}

void LibDataChannelRecorder::OnError(const std::string& msg) {
    m_lastError = msg;
    if (onError) onError(m_streamInfo.username, msg);
}

// ─── Recording thread ─────────────────────────────────────────────────────────
void LibDataChannelRecorder::RecordingThread() {
    // ── 0. Agora signaling ────────────────────────────────────────────────────
    // Generate per-session IDs matching what Agora Web SDK creates.
    const std::string sid         = MakeHexSid();    // 32-char random hex
    const std::string sessionUuid = MakeUuid(); // fresh UUID per recording session

    TLSClient http;

    // A) Get numeric UID (Agora requires a uint32_t per-subscriber UID).
    uint32_t numericUid = AgoraStringUid(http, sid, sessionUuid);
    // uid==0 is also valid for anonymous viewers; continue even if SUA fails.

    // B) Fetch edge server IPs and DTLS fingerprints from Agora gateway.
    AgoraGatewayResult gw = AgoraGetEdges(http, sid, sessionUuid,
                                           m_streamInfo.sessionId,
                                           m_streamInfo.agoraToken,
                                           numericUid);
    if (!gw.ok || gw.edges.empty()) {
        OnError("Agora gateway did not return edge servers. "
                "Check that the stream is live and the token is valid.");
        CloseHandle(m_hFile); m_hFile = INVALID_HANDLE_VALUE;
        m_running.store(false);
        return;
    }

    // ── 1. Build ICE configuration from Agora edge servers ───────────────────
    // Agora edges are direct DTLS/SRTP hosts (not TURN/STUN).
    // libdatachannel accepts them as "host" ICE candidates when we provide
    // the remote description with inline candidates.
    rtc::Configuration rtcConfig;
    // Use the first edge as a TURN relay if it's on port 443; otherwise direct.
    for (const auto& e : gw.edges) {
        if (e.port == 443) {
            // TCP/TLS relay – add as TURN server so libdatachannel can reach it
            rtcConfig.iceServers.push_back(
                rtc::IceServer(e.ip, uint16_t(443),
                               std::string("agora"), m_streamInfo.agoraToken,
                               rtc::IceServer::RelayType::TurnTcp));
        }
    }
    // Also include Google STUN to gather our own candidates.
    rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));

    auto pc = std::make_shared<rtc::PeerConnection>(rtcConfig);

    std::atomic<bool> pcFailed{ false };
    pc->onStateChange([&](rtc::PeerConnection::State s) {
        if (s == rtc::PeerConnection::State::Failed ||
            s == rtc::PeerConnection::State::Closed)
            pcFailed.store(true);
    });

    // ── 2. Add receive-only tracks ────────────────────────────────────────────
    rtc::Description::Video vd("video", rtc::Description::Direction::RecvOnly);
    vd.addVP8Codec(96);
    auto videoTrack = pc->addTrack(vd);
    videoTrack->setMediaHandler(std::make_shared<rtc::VP8RtpDepacketizer>());
    videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

    rtc::Description::Audio ad("audio", rtc::Description::Direction::RecvOnly);
    ad.addOpusCodec(111);
    auto audioTrack = pc->addTrack(ad);
    audioTrack->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
    audioTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

    // ── 3. Wire muxer ─────────────────────────────────────────────────────────
    m_muxer = std::make_unique<WebMMuxer>(
        [this](const uint8_t* d, size_t n) { AppendBytes(d, n); });
    bool muxerInit = false;

    videoTrack->onFrame([&](rtc::binary frame, rtc::FrameInfo info) {
        if (!muxerInit) {
            m_muxer->Init(WebMMuxer::VideoCodec::VP8, 1280, 720, 48000, 2);
            muxerInit = true;
        }
        m_muxer->WriteVideoFrame(
            reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
            InfoToMs(info, 90000), Vp8IsKeyFrame(frame));
    });

    audioTrack->onFrame([&](rtc::binary frame, rtc::FrameInfo info) {
        if (!muxerInit) return;
        m_muxer->WriteAudioPacket(
            reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
            InfoToMs(info, 48000));
    });

    // ── 4. Generate SDP offer ─────────────────────────────────────────────────
    std::mutex       gathMtx;
    std::condition_variable gathCV;
    bool             gathDone = false;
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState gs) {
        if (gs == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lk(gathMtx);
            gathDone = true; gathCV.notify_all();
        }
    });
    pc->setLocalDescription();

    {
        std::unique_lock<std::mutex> lk(gathMtx);
        gathCV.wait_for(lk, std::chrono::seconds(15),
                        [&]{ return gathDone || m_stopRequest.load(); });
    }
    if (m_stopRequest.load()) goto teardown;

    {
        auto offerOpt = pc->localDescription();
        if (!offerOpt) {
            OnError("libdatachannel did not produce a local SDP offer");
            goto teardown;
        }

        // ── 5. Build a synthetic SDP answer using Agora edge servers ──────────
        //
        // Agora doesn't use standard offer/answer WebSocket exchange.
        // Instead we inject the edge server ICE candidates directly and
        // set a synthetic remote description built from the gateway response.
        //
        // DTLS fingerprints come from detail["19"] (semicolon-separated list).
        // We use the first fingerprint. Format is standard SHA-256 colon-hex.
        std::string firstFp;
        {
            const std::string& fps = gw.dtlsFingerprints;
            auto sep = fps.find(';');
            firstFp = (sep != std::string::npos) ? fps.substr(0, sep) : fps;
        }
        if (firstFp.empty()) firstFp = "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00";

        // Use first direct edge (non-443 preferred for lower latency)
        const AgoraEdge* bestEdge = nullptr;
        for (const auto& e : gw.edges) {
            if (e.port != 443) { bestEdge = &e; break; }
        }
        if (!bestEdge) bestEdge = &gw.edges[0];

        // Construct minimal SDP answer matching our offer's mid/codec.
        // The Agora edge acts as a passive DTLS endpoint (role: passive).
        std::string answerSDP;
        answerSDP  = "v=0\r\n";
        answerSDP += "o=- 2 1 IN IP4 " + bestEdge->ip + "\r\n";
        answerSDP += "s=-\r\n";
        answerSDP += "t=0 0\r\n";
        answerSDP += "a=group:BUNDLE video audio\r\n";
        answerSDP += "a=msid-semantic: WMS\r\n";
        // Video m-line
        answerSDP += "m=video " + std::to_string(bestEdge->port) + " UDP/TLS/RTP/SAVPF 96\r\n";
        answerSDP += "c=IN IP4 " + bestEdge->ip + "\r\n";
        answerSDP += "a=rtcp:" + std::to_string(bestEdge->port) + " IN IP4 " + bestEdge->ip + "\r\n";
        answerSDP += "a=candidate:0 1 UDP 2130706431 " + bestEdge->ip
                  + " " + std::to_string(bestEdge->port) + " typ host\r\n";
        answerSDP += "a=ice-ufrag:agora\r\n";
        answerSDP += "a=ice-pwd:agorapass\r\n";
        answerSDP += "a=fingerprint:sha-256 " + firstFp + "\r\n";
        answerSDP += "a=setup:passive\r\n";
        answerSDP += "a=mid:video\r\n";
        answerSDP += "a=recvonly\r\n";
        answerSDP += "a=rtpmap:96 VP8/90000\r\n";
        answerSDP += "a=rtcp-mux\r\n";
        // Audio m-line
        answerSDP += "m=audio " + std::to_string(bestEdge->port) + " UDP/TLS/RTP/SAVPF 111\r\n";
        answerSDP += "c=IN IP4 " + bestEdge->ip + "\r\n";
        answerSDP += "a=candidate:0 1 UDP 2130706431 " + bestEdge->ip
                  + " " + std::to_string(bestEdge->port) + " typ host\r\n";
        answerSDP += "a=ice-ufrag:agora\r\n";
        answerSDP += "a=ice-pwd:agorapass\r\n";
        answerSDP += "a=fingerprint:sha-256 " + firstFp + "\r\n";
        answerSDP += "a=setup:passive\r\n";
        answerSDP += "a=mid:audio\r\n";
        answerSDP += "a=recvonly\r\n";
        answerSDP += "a=rtpmap:111 opus/48000/2\r\n";
        answerSDP += "a=rtcp-mux\r\n";

        try {
            pc->setRemoteDescription(
                rtc::Description(answerSDP, rtc::Description::Type::Answer));
        } catch (const std::exception& ex) {
            OnError(std::string("setRemoteDescription failed: ") + ex.what());
            goto teardown;
        }
    }

    if (onStarted) onStarted(m_streamInfo.username);

    // ── 6. Run until stop or failure ──────────────────────────────────────────
    while (!m_stopRequest.load() && !pcFailed.load()) {
        Sleep(250);
    }

teardown:
    // ── 7. Tear down ──────────────────────────────────────────────────────────
    try { videoTrack->close(); } catch (...) {}
    try { audioTrack->close(); } catch (...) {}
    try { pc->close(); }         catch (...) {}
    m_muxer.reset();

    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }

    size_t total = m_bytesWritten.load();
    m_running.store(false);
    if (onStopped) onStopped(m_streamInfo.username, total);
}
