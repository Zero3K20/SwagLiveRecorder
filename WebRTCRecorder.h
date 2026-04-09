#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>

#include <string>
#include <functional>
#include <fstream>

#include "SwagLiveAPI.h"

// Callback types for recording events
using RecordingStartedCallback  = std::function<void(const std::string& username)>;
using RecordingStoppedCallback  = std::function<void(const std::string& username, size_t totalBytes)>;
using RecordingErrorCallback    = std::function<void(const std::string& username, const std::string& error)>;
using RecordingProgressCallback = std::function<void(const std::string& username, size_t bytesWritten)>;

// Configuration for WebRTC recording
struct WebRTCConfig {
    std::string outputDirectory;  // Directory to save .webm files
    bool appendTimestamp;         // Append _YYYYMMDD_HHMMSS to filename

    WebRTCConfig()
        : outputDirectory("recordings"),
          appendTimestamp(true) {}
};

// Records a single swag.live WebRTC stream using an embedded WebView2 browser.
//
// How it works:
//  1. A hidden Win32 window is created to host the WebView2 controller.
//  2. WebView2 navigates to https://swag.live/livestream/<username>.
//  3. JavaScript is injected (document-start) that hooks HTMLMediaElement.srcObject.
//     When the page's React code assigns a MediaStream to a <video> element the hook
//     creates a MediaRecorder and starts collecting WebM chunks (500 ms intervals).
//  4. Each chunk is base64-encoded and sent to C++ via window.chrome.webview.postMessage.
//  5. C++ decodes the chunk and appends it to the output .webm file.
//
// All WebView2 operations must happen on the same thread as the Win32 message loop
// (the main UI thread).  Start() is called from the main thread; it is non-blocking.
// Recording continues asynchronously until Stop() is called or the stream ends.
class WebRTCRecorder {
public:
    WebRTCRecorder();
    ~WebRTCRecorder();

    // Callbacks (set before calling Start)
    RecordingStartedCallback  onStarted;
    RecordingStoppedCallback  onStopped;
    RecordingErrorCallback    onError;
    RecordingProgressCallback onProgress;

    // Start recording the stream for the given model.
    // parentHwnd: any visible HWND in the main window hierarchy (used as WebView2 parent).
    // Returns true if initialisation was kicked off; actual recording begins asynchronously
    // once the WebView2 environment is ready and the page has loaded.
    bool Start(const StreamInfo& streamInfo,
               const WebRTCConfig& config,
               HWND parentHwnd);

    // Stop recording gracefully. Sends a stop message to the browser, waits for the
    // final flush, then closes the WebView2 controller and the output file.
    void Stop();

    // True between Start() and the final Stop()/error callback.
    bool IsRunning() const { return m_running; }

    std::string GetUsername()   const { return m_streamInfo.username; }
    std::string GetOutputPath() const { return m_outputPath; }
    size_t      GetBytesWritten() const { return m_bytesWritten; }
    std::string GetLastError()  const { return m_lastError; }

private:
    StreamInfo  m_streamInfo;
    WebRTCConfig m_config;
    std::string m_outputPath;
    std::string m_lastError;
    size_t      m_bytesWritten = 0;
    bool        m_running      = false;
    bool        m_stopping     = false;

    // WebView2 objects
    HWND m_hwndHost = nullptr;                               // Hidden host window
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>     m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>               m_webview;
    EventRegistrationToken m_msgToken  = {};
    EventRegistrationToken m_navToken  = {};

    // Output file
    HANDLE m_hFile = INVALID_HANDLE_VALUE;

    // Initialise the WebView2 environment and controller (async).
    void InitWebView2(HWND parentHwnd);

    // Called once the WebView2 controller is ready.
    void OnControllerReady(ICoreWebView2Controller* controller);

    // Called when navigation completes – injects the MediaRecorder hook script.
    void OnNavigationCompleted();

    // Called when a WebMessage arrives from the page JavaScript.
    void OnWebMessage(const std::string& message);

    // Build the output file path.
    std::string BuildOutputPath() const;

    // Ensure the output directory exists.
    bool EnsureOutputDirectory() const;

    // Decode a base64 string to raw bytes.
    static std::vector<uint8_t> Base64Decode(const std::string& encoded);

    // JavaScript injected at document-start to hook WebRTC and MediaRecorder.
    static const char* HookScript();
};
