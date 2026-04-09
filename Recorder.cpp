#include "Recorder.h"
#include "WebRTCRecorder.h"

#include <algorithm>
#include <cctype>

// ─── Case-insensitive helper ──────────────────────────────────────────────────

static std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

// ─── Recorder ────────────────────────────────────────────────────────────────

Recorder::Recorder(const RecorderConfig& config, HWND parentHwnd)
    : m_config(config), m_parentHwnd(parentHwnd) {}

Recorder::~Recorder() {
    StopAll();
    for (auto* s : m_sessions) delete s;
    m_sessions.clear();
}

WebRTCRecorder* Recorder::FindSession(const std::string& username) const {
    std::string lower = ToLower(username);
    for (auto* s : m_sessions) {
        if (ToLower(s->GetUsername()) == lower) return s;
    }
    return nullptr;
}

bool Recorder::StartRecording(const StreamInfo& streamInfo) {
    if (IsRecording(streamInfo.username)) return true; // already running

    WebRTCConfig cfg;
    cfg.outputDirectory = m_config.outputDirectory;
    cfg.appendTimestamp = m_config.appendTimestamp;

    auto* session = new WebRTCRecorder();

    // Wire callbacks
    session->onStarted  = [this](const std::string& u) {
        if (onStarted) onStarted(u);
    };
    session->onStopped  = [this](const std::string& u, size_t b) {
        if (onStopped) onStopped(u, b);
    };
    session->onError    = [this](const std::string& u, const std::string& e) {
        if (onError) onError(u, e);
    };
    session->onProgress = [this](const std::string& u, size_t b) {
        if (onProgress) onProgress(u, b);
    };

    if (!session->Start(streamInfo, cfg, m_parentHwnd)) {
        if (onError) onError(streamInfo.username, session->GetLastError());
        delete session;
        return false;
    }

    m_sessions.push_back(session);
    return true;
}

bool Recorder::StopRecording(const std::string& username) {
    std::string lower = ToLower(username);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (ToLower((*it)->GetUsername()) == lower) {
            (*it)->Stop();
            delete *it;
            m_sessions.erase(it);
            return true;
        }
    }
    return false;
}

void Recorder::StopAll() {
    for (auto* s : m_sessions) s->Stop();
}

bool Recorder::IsRecording(const std::string& username) const {
    auto* s = FindSession(username);
    return s && s->IsRunning();
}

std::vector<std::string> Recorder::GetActiveRecordings() const {
    std::vector<std::string> result;
    for (const auto* s : m_sessions) {
        if (s->IsRunning()) result.push_back(s->GetUsername());
    }
    return result;
}

void Recorder::CleanupFinished() {
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (!(*it)->IsRunning()) {
            delete *it;
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}
