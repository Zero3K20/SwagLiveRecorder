#include "Recorder.h"
#include "tlsclient/tlsclient.h"

#include <iostream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <direct.h>   // _mkdir
#include <sys/stat.h>

// -----------------------------------------------------------------------
// RecordingSession
// -----------------------------------------------------------------------

RecordingSession::RecordingSession(const StreamInfo& streamInfo, const RecorderConfig& config)
    : m_streamInfo(streamInfo),
      m_config(config),
      m_hProcess(INVALID_HANDLE_VALUE),
      m_hStdin(INVALID_HANDLE_VALUE),
      m_running(false) {
    m_outputPath = BuildOutputPath();
}

RecordingSession::~RecordingSession() {
    Stop();
}

std::string RecordingSession::BuildOutputPath() const {
    std::string dir = m_config.outputDirectory;
    if (dir.empty()) dir = "recordings";

    // Sanitise username for use in filename
    std::string safeName = m_streamInfo.username;
    std::replace_if(safeName.begin(), safeName.end(),
        [](char c) { return c == '\\' || c == '/' || c == ':' ||
                            c == '*'  || c == '?' || c == '"' ||
                            c == '<'  || c == '>' || c == '|'; },
        '_');

    std::string filename = safeName;

    if (m_config.appendTimestamp) {
        time_t now = time(nullptr);
        struct tm tmInfo;
        localtime_s(&tmInfo, &now);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "_%Y%m%d_%H%M%S", &tmInfo);
        filename += timeBuf;
    }

    filename += ".ts";

    return dir + "\\" + filename;
}

bool RecordingSession::EnsureOutputDirectory() const {
    const std::string& dir = m_config.outputDirectory;
    if (dir.empty()) return true;

    struct _stat st;
    if (_stat(dir.c_str(), &st) == 0) {
        return (st.st_mode & _S_IFDIR) != 0;
    }

    return _mkdir(dir.c_str()) == 0;
}

std::string RecordingSession::FindFfmpeg(const std::string& configPath) {
    // 1. Use user-specified path
    if (!configPath.empty()) {
        struct _stat st;
        if (_stat(configPath.c_str(), &st) == 0) return configPath;
    }

    // 2. Look in common locations
    const char* candidates[] = {
        "ffmpeg.exe",
        "ffmpeg\\ffmpeg.exe",
        "C:\\ffmpeg\\bin\\ffmpeg.exe",
        "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
        nullptr
    };

    for (int i = 0; candidates[i] != nullptr; ++i) {
        struct _stat st;
        if (_stat(candidates[i], &st) == 0) return candidates[i];
    }

    // 3. Try PATH via CreateProcess with just "ffmpeg"
    return "ffmpeg";
}

bool RecordingSession::FindStreamUrl(std::string& outUrl) const {
    // swag.live streams are delivered via WebRTC.
    // Many platforms also publish HLS/DASH versions of the stream for
    // compatibility.  We attempt the most common CDN URL patterns first.
    //
    // If none are reachable the caller falls back to launching ffmpeg with
    // the WebRTC capture approach (which requires additional configuration).

    const std::string& id   = m_streamInfo.streamId;
    const std::string& user = m_streamInfo.username;
    const std::string& tok  = m_streamInfo.streamToken;

    // Build candidate HLS URLs (adjust patterns to match swag.live's CDN)
    std::vector<std::string> candidates;

    if (!id.empty()) {
        candidates.push_back("https://live.swag.live/hls/" + id + "/index.m3u8");
        candidates.push_back("https://cdn.swag.live/hls/" + id + "/index.m3u8");
        candidates.push_back("https://stream.swag.live/" + id + "/index.m3u8");
    }
    if (!user.empty()) {
        candidates.push_back("https://live.swag.live/hls/" + user + "/index.m3u8");
        candidates.push_back("https://cdn.swag.live/hls/" + user + "/index.m3u8");
    }
    if (!tok.empty()) {
        candidates.push_back("https://live.swag.live/hls/" + tok + "/index.m3u8");
    }

    TLSClient client;
    for (const auto& url : candidates) {
        std::string resp;
        if (client.HttpGet(url, resp) && !resp.empty()) {
            // A valid M3U8 playlist starts with #EXTM3U
            if (resp.find("#EXTM3U") != std::string::npos) {
                outUrl = url;
                return true;
            }
        }
    }

    return false;
}

std::string RecordingSession::BuildFfmpegCommand(const std::string& ffmpegPath) const {
    // Build an ffmpeg invocation that writes transport stream output.
    // The -i parameter is set to the HLS playlist URL that FindStreamUrl()
    // discovered (or a placeholder when the caller knows the URL already).

    std::string streamUrl;
    FindStreamUrl(streamUrl);

    if (streamUrl.empty()) {
        // No HLS URL found; record from the swag.live page via the WebRTC
        // capture plugin built into a headless browser approach is beyond
        // the scope of this binary.  Log a warning so the user can supply
        // an explicit URL via a future config extension.
        std::cout << "[Recorder] Warning: could not locate an HLS URL for '"
                  << m_streamInfo.username << "'.\n"
                  << "           Please check swag.live in a browser and supply\n"
                  << "           the HLS playlist URL manually if one is available.\n";
        return "";
    }

    // -hide_banner    suppress ffmpeg startup banner
    // -loglevel error only show errors
    // -i <url>        input stream URL
    // -c copy         copy streams without re-encoding
    // -f mpegts       output as MPEG-TS
    // <output>        destination file

    std::string cmd = "\"" + ffmpegPath + "\"";
    cmd += " -hide_banner -loglevel error";
    cmd += " -i \"" + streamUrl + "\"";
    cmd += " -c copy -f mpegts";
    cmd += " \"" + m_outputPath + "\"";

    return cmd;
}

bool RecordingSession::Start() {
    if (m_running) return true;

    if (!EnsureOutputDirectory()) {
        m_lastError = "Failed to create output directory: " + m_config.outputDirectory;
        return false;
    }

    std::string ffmpegPath = FindFfmpeg(m_config.ffmpegPath);
    std::string cmd = BuildFfmpegCommand(ffmpegPath);

    if (cmd.empty()) {
        m_lastError = "Could not build ffmpeg command (no stream URL found)";
        return false;
    }

    std::cout << "[Recorder] Starting recording for: " << m_streamInfo.username << "\n";
    std::cout << "[Recorder] Output: " << m_outputPath << "\n";
    std::cout << "[Recorder] Command: " << cmd << "\n";

    // Create a pipe so we can send 'q' to ffmpeg's stdin for graceful shutdown
    HANDLE hStdinRead  = INVALID_HANDLE_VALUE;
    HANDLE hStdinWrite = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle       = TRUE;

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        m_lastError = "Failed to create stdin pipe (error " +
                      std::to_string(GetLastError()) + ")";
        return false;
    }
    // The write end must NOT be inherited by the child
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = hStdinRead;
    si.hStdOutput  = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError   = GetStdHandle(STD_ERROR_HANDLE);
    ZeroMemory(&pi, sizeof(pi));

    // Create ffmpeg process without a separate console window
    if (!CreateProcessA(
            nullptr,
            const_cast<char*>(cmd.c_str()),
            nullptr,
            nullptr,
            TRUE,            // inherit handles (so child gets our pipe)
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        DWORD err = GetLastError();
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        m_lastError = "Failed to launch ffmpeg (error " + std::to_string(err) + ")";
        return false;
    }

    // Child now owns the read end; close our copy so the pipe works correctly
    CloseHandle(hStdinRead);

    m_hProcess = pi.hProcess;
    m_hStdin   = hStdinWrite;
    CloseHandle(pi.hThread);
    m_running = true;

    return true;
}

void RecordingSession::Stop() {
    if (!m_running) return;

    if (m_hProcess != INVALID_HANDLE_VALUE) {
        // Try to stop ffmpeg gracefully by sending 'q\n' to its stdin.
        // ffmpeg treats 'q' on stdin as a quit signal and finalises the output.
        if (m_hStdin != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(m_hStdin, "q\n", 2, &written, nullptr);
            CloseHandle(m_hStdin);
            m_hStdin = INVALID_HANDLE_VALUE;
        }

        // Wait up to 10 seconds for a clean exit, then force-terminate
        if (WaitForSingleObject(m_hProcess, 10000) != WAIT_OBJECT_0) {
            TerminateProcess(m_hProcess, 0);
            WaitForSingleObject(m_hProcess, 5000);
        }

        CloseHandle(m_hProcess);
        m_hProcess = INVALID_HANDLE_VALUE;
    } else if (m_hStdin != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hStdin);
        m_hStdin = INVALID_HANDLE_VALUE;
    }

    m_running = false;
    std::cout << "[Recorder] Stopped recording for: " << m_streamInfo.username << "\n";
}

bool RecordingSession::IsRunning() const {
    if (!m_running) return false;

    if (m_hProcess != INVALID_HANDLE_VALUE) {
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(m_hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) {
            // Process has exited
            const_cast<RecordingSession*>(this)->m_running = false;
            return false;
        }
    }

    return m_running;
}

// -----------------------------------------------------------------------
// Recorder
// -----------------------------------------------------------------------

Recorder::Recorder(const RecorderConfig& config)
    : m_config(config) {
}

Recorder::~Recorder() {
    StopAll();
    for (auto* session : m_sessions) {
        delete session;
    }
    m_sessions.clear();
}

bool Recorder::StartRecording(const StreamInfo& streamInfo) {
    // Don't start a duplicate session for the same model
    if (IsRecording(streamInfo.username)) {
        return true; // Already recording
    }

    RecordingSession* session = new RecordingSession(streamInfo, m_config);
    if (!session->Start()) {
        std::cerr << "[Recorder] Failed to start recording for "
                  << streamInfo.username << ": "
                  << session->GetLastError() << "\n";
        delete session;
        return false;
    }

    m_sessions.push_back(session);
    return true;
}

bool Recorder::StopRecording(const std::string& username) {
    std::string usernameLower = username;
    std::transform(usernameLower.begin(), usernameLower.end(), usernameLower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        std::string sessionName = (*it)->GetUsername();
        std::transform(sessionName.begin(), sessionName.end(), sessionName.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        if (sessionName == usernameLower) {
            (*it)->Stop();
            delete *it;
            m_sessions.erase(it);
            return true;
        }
    }
    return false;
}

void Recorder::StopAll() {
    for (auto* session : m_sessions) {
        session->Stop();
    }
}

bool Recorder::IsRecording(const std::string& username) const {
    std::string usernameLower = username;
    std::transform(usernameLower.begin(), usernameLower.end(), usernameLower.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    for (const auto* session : m_sessions) {
        std::string sessionName = session->GetUsername();
        std::transform(sessionName.begin(), sessionName.end(), sessionName.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        if (sessionName == usernameLower && session->IsRunning()) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> Recorder::GetActiveRecordings() const {
    std::vector<std::string> result;
    for (const auto* session : m_sessions) {
        if (session->IsRunning()) {
            result.push_back(session->GetUsername());
        }
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
