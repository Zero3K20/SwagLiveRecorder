#include "WebRTCRecorder.h"
#include "json_minimal.h"

#include <iostream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <direct.h>
#include <sys/stat.h>

// Suppress min/max macro conflicts
#define NOMINMAX

// ─── JavaScript hook injected at document-start ──────────────────────────────
// This script mirrors the approach used by the LivestreamRecorder userscript:
//  - Hook HTMLMediaElement.prototype.srcObject
//  - When a MediaStream with video tracks is assigned, start a MediaRecorder
//  - Send 500 ms WebM chunks back to the C++ host as base64-encoded JSON messages
// ─────────────────────────────────────────────────────────────────────────────
const char* WebRTCRecorder::HookScript() {
    return R"JS(
(function () {
    'use strict';
    if (window.__SLR_hooked) return;
    window.__SLR_hooked = true;

    let mediaRecorder = null;
    let recordingActive = false;

    function b64(buf) {
        let binary = '';
        const bytes = new Uint8Array(buf);
        for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
        return btoa(binary);
    }

    function postMsg(obj) {
        try { window.chrome.webview.postMessage(JSON.stringify(obj)); } catch(e) {}
    }

    function startRecording(stream) {
        if (recordingActive) return;
        recordingActive = true;

        const mimeTypes = [
            'video/webm;codecs=vp9,opus',
            'video/webm;codecs=vp8,opus',
            'video/webm'
        ];
        const mimeType = mimeTypes.find(m => MediaRecorder.isTypeSupported(m)) || 'video/webm';

        try {
            mediaRecorder = new MediaRecorder(stream, { mimeType: mimeType });
        } catch(e) {
            postMsg({ type: 'error', msg: 'MediaRecorder creation failed: ' + e.message });
            recordingActive = false;
            return;
        }

        mediaRecorder.ondataavailable = async function(e) {
            if (!e.data || e.data.size === 0) return;
            try {
                const buf = await e.data.arrayBuffer();
                postMsg({ type: 'data', data: b64(buf) });
            } catch(err) {
                postMsg({ type: 'error', msg: 'data read error: ' + err.message });
            }
        };

        mediaRecorder.onerror = function(e) {
            postMsg({ type: 'error', msg: String(e.error || 'MediaRecorder error') });
        };

        mediaRecorder.onstop = function() {
            postMsg({ type: 'stopped' });
            recordingActive = false;
        };

        mediaRecorder.onstart = function() {
            postMsg({ type: 'started', mimeType: mimeType });
        };

        mediaRecorder.start(500);
    }

    // Hook srcObject setter to capture WebRTC MediaStream
    const MediaElement = HTMLMediaElement;
    if (MediaElement && MediaElement.prototype) {
        const desc = Object.getOwnPropertyDescriptor(MediaElement.prototype, 'srcObject');
        if (desc && desc.set) {
            const origSet = desc.set;
            Object.defineProperty(MediaElement.prototype, 'srcObject', {
                configurable: true,
                get: desc.get,
                set: function(val) {
                    try {
                        if (val instanceof MediaStream && val.getVideoTracks().length > 0) {
                            startRecording(val);
                        }
                    } catch(e) { /* ignore hook errors */ }
                    return origSet.call(this, val);
                }
            });
        }
    }

    // Listen for stop command from C++ host
    window.chrome.webview.addEventListener('message', function(e) {
        try {
            const msg = JSON.parse(e.data);
            if (msg && msg.type === 'stop' && mediaRecorder && mediaRecorder.state !== 'inactive') {
                mediaRecorder.stop();
            }
        } catch(err) {}
    });

    postMsg({ type: 'hook_ready' });
})();
)JS";
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static const std::string BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

std::vector<uint8_t> WebRTCRecorder::Base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;
        size_t pos = BASE64_CHARS.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + (int)pos;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string WebRTCRecorder::BuildOutputPath() const {
    std::string dir = m_config.outputDirectory;
    if (dir.empty()) dir = "recordings";

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
    filename += ".webm";
    return dir + "\\" + filename;
}

bool WebRTCRecorder::EnsureOutputDirectory() const {
    const std::string& dir = m_config.outputDirectory;
    if (dir.empty()) return true;
    struct _stat st;
    if (_stat(dir.c_str(), &st) == 0) return (st.st_mode & _S_IFDIR) != 0;
    return _mkdir(dir.c_str()) == 0;
}

// ─── Host window ──────────────────────────────────────────────────────────────

static const wchar_t* HOST_WND_CLASS = L"SLR_WebView2Host";

static bool g_hostClassRegistered = false;
static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateHostWindow(HWND parent) {
    if (!g_hostClassRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = HostWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = HOST_WND_CLASS;
        RegisterClassW(&wc);
        g_hostClassRegistered = true;
    }
    // 1×1 pixel window off-screen – just needs a valid HWND for WebView2
    return CreateWindowExW(0, HOST_WND_CLASS, L"", WS_POPUP,
        -10, -10, 1, 1,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ─── WebRTCRecorder ──────────────────────────────────────────────────────────

WebRTCRecorder::WebRTCRecorder() {}

WebRTCRecorder::~WebRTCRecorder() {
    Stop();
}

bool WebRTCRecorder::Start(const StreamInfo& streamInfo,
                           const WebRTCConfig& config,
                           HWND parentHwnd) {
    if (m_running) return true;

    m_streamInfo  = streamInfo;
    m_config      = config;
    m_outputPath  = BuildOutputPath();
    m_bytesWritten = 0;
    m_stopping    = false;

    if (!EnsureOutputDirectory()) {
        m_lastError = "Cannot create output directory: " + config.outputDirectory;
        return false;
    }

    // Open output file
    m_hFile = CreateFileA(m_outputPath.c_str(),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE) {
        m_lastError = "Cannot open output file: " + m_outputPath +
                      " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    m_hwndHost = CreateHostWindow(parentHwnd);
    if (!m_hwndHost) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
        m_lastError = "Cannot create host window";
        return false;
    }

    m_running = true;
    InitWebView2(parentHwnd);
    return true;
}

void WebRTCRecorder::InitWebView2(HWND parentHwnd) {
    using namespace Microsoft::WRL;

    HWND hwndHost = m_hwndHost;

    // User data folder for this session (one per model to avoid profile conflicts)
    std::wstring userDataFolder = L"webview2_data\\";
    std::wstring wUser(m_streamInfo.username.begin(), m_streamInfo.username.end());
    userDataFolder += wUser;

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, hwndHost](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    m_lastError = "WebView2 environment creation failed (HRESULT " +
                                  std::to_string(hr) + "). "
                                  "Is the WebView2 Runtime installed?";
                    m_running = false;
                    if (onError) onError(m_streamInfo.username, m_lastError);
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    hwndHost,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(hr) || !ctrl) {
                                m_lastError = "WebView2 controller creation failed (HRESULT " +
                                              std::to_string(hr) + ")";
                                m_running = false;
                                if (onError) onError(m_streamInfo.username, m_lastError);
                                return S_OK;
                            }
                            OnControllerReady(ctrl);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void WebRTCRecorder::OnControllerReady(ICoreWebView2Controller* controller) {
    using namespace Microsoft::WRL;

    m_controller = controller;
    controller->get_CoreWebView2(m_webview.GetAddressOf());
    if (!m_webview) {
        m_lastError = "Failed to get ICoreWebView2 from controller";
        m_running = false;
        if (onError) onError(m_streamInfo.username, m_lastError);
        return;
    }

    // Inject the hook script before page scripts run
    std::wstring script(HookScript(), HookScript() + strlen(HookScript()));
    m_webview->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);

    // Listen for messages from the page
    m_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string msgW;
                if (SUCCEEDED(args->TryGetWebMessageAsString(msgW.addressof()))) {
                    std::wstring ws(msgW.get());
                    std::string msg(ws.begin(), ws.end());
                    OnWebMessage(msg);
                }
                return S_OK;
            }).Get(),
        &m_msgToken);

    // Navigate to the stream page
    std::wstring url = L"https://swag.live/livestream/";
    url += std::wstring(m_streamInfo.username.begin(), m_streamInfo.username.end());
    m_webview->Navigate(url.c_str());
}

void WebRTCRecorder::OnWebMessage(const std::string& message) {
    JsonValue root = parse_json(message);
    if (root.is_null()) return;

    std::string type = root["type"].as_str();

    if (type == "hook_ready") {
        // Hook is in place; page will set srcObject when the stream loads
    } else if (type == "started") {
        if (onStarted) onStarted(m_streamInfo.username);
    } else if (type == "data") {
        std::string b64 = root["data"].as_str();
        if (!b64.empty() && m_hFile != INVALID_HANDLE_VALUE) {
            auto bytes = Base64Decode(b64);
            if (!bytes.empty()) {
                DWORD written = 0;
                WriteFile(m_hFile, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
                m_bytesWritten += written;
                if (onProgress) onProgress(m_streamInfo.username, m_bytesWritten);
            }
        }
    } else if (type == "stopped") {
        // MediaRecorder has stopped; finalise the file
        if (m_hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hFile);
            m_hFile = INVALID_HANDLE_VALUE;
        }
        size_t total = m_bytesWritten;
        m_running = false;
        if (onStopped) onStopped(m_streamInfo.username, total);
    } else if (type == "error") {
        m_lastError = root["msg"].as_str();
        if (!m_lastError.empty()) {
            if (onError) onError(m_streamInfo.username, m_lastError);
        }
    }
}

void WebRTCRecorder::Stop() {
    if (!m_running && !m_stopping) return;
    m_stopping = true;

    // Tell the in-page MediaRecorder to stop (it will flush the last chunk)
    if (m_webview) {
        m_webview->PostWebMessageAsString(L"{\"type\":\"stop\"}");
        // Give the page up to 3 seconds to flush the last chunk
        DWORD deadline = GetTickCount() + 3000;
        MSG msg;
        while (m_running && GetTickCount() < deadline) {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                Sleep(10);
            }
        }
    }

    // Close output file if not already closed by the 'stopped' message
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }

    // Remove WebView2 event registrations and release COM objects
    if (m_webview) {
        m_webview->remove_WebMessageReceived(m_msgToken);
        m_webview.Reset();
    }
    if (m_controller) {
        m_controller->Close();
        m_controller.Reset();
    }

    if (m_hwndHost) {
        DestroyWindow(m_hwndHost);
        m_hwndHost = nullptr;
    }

    size_t total = m_bytesWritten;
    bool wasRunning = m_running;
    m_running  = false;
    m_stopping = false;

    if (wasRunning && onStopped) onStopped(m_streamInfo.username, total);
}
