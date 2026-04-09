//=============================================================================
// SwagLiveRecorder - Automatic WebRTC live-stream recorder for swag.live
//
// Architecture:
//   - Win32 GUI (MainWindow) for the watchlist, recording status, and settings
//   - Background monitor thread polls the swag.live API (SwagLiveAPI + TLSClient)
//   - WebView2-embedded browser (WebRTCRecorder) records each stream using the
//     browser's MediaRecorder API, writing chunks as .webm files to disk
//     (same technique as the LivestreamRecorder userscript)
//
// Build requirements:
//   - Visual Studio 2019 (toolset v142), Windows SDK 10.0
//   - Microsoft.Web.WebView2 NuGet package (run: nuget restore)
//   - WebView2 Runtime installed on the target machine (ships with Windows 11;
//     auto-updated via Windows Update on Win10 1803+)
//=============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "MainWindow.h"
#include "ModelList.h"
#include "SwagLiveAPI.h"
#include "Recorder.h"
#include "tlsclient/tlsclient.h"

// ─── Forward declarations ─────────────────────────────────────────────────────
static void MonitorThreadProc(MainWindow* wnd, std::atomic<bool>* stopFlag,
                              ModelList* modelList, SwagLiveAPI* api,
                              Recorder* recorder, int* pollInterval);

// ─── Application state ───────────────────────────────────────────────────────

struct AppState {
    ModelList    modelList;
    SwagLiveAPI  api;
    Recorder*    recorder  = nullptr;
    MainWindow*  mainWnd   = nullptr;
    std::thread  monThread;
    std::atomic<bool> stopMonitor { false };
    int          pollInterval = 60;
    std::string  modelsFile   = "models.txt";
    std::string  outputDir    = "recordings";

    AppState() : modelList("models.txt") {}
};

// ─── Helper ──────────────────────────────────────────────────────────────────

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring r(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &r[0], n);
    return r;
}

// ─── Monitor thread ──────────────────────────────────────────────────────────

static void MonitorThreadProc(MainWindow* wnd,
                               std::atomic<bool>* stopFlag,
                               ModelList* modelList,
                               SwagLiveAPI* api,
                               Recorder* recorder,
                               int* pollInterval) {
    auto log = [&](const std::wstring& line) {
        wnd->PostLogLine(line);
    };

    log(L"[Monitor] Started.");

    while (!stopFlag->load()) {
        const auto& models = modelList->GetModels();
        if (models.empty()) {
            log(L"[Monitor] Watchlist is empty - add models via 'Add Model'.");
        } else {
            std::wstringstream ss;
            ss << L"[Monitor] Checking " << models.size() << L" model(s)...";
            log(ss.str());

            for (const auto& model : models) {
                if (stopFlag->load()) break;
                if (!model.enabled) continue;

                StreamInfo info;
                bool found = api->GetModelStatus(model.username, info);

                std::wstringstream ms;
                ms << L"  " << Utf8ToWide(model.username) << L" - ";

                if (!found) {
                    ms << L"offline/not found";
                    wnd->PostLogLine(ms.str());

                    // Stop any running recording for this model
                    if (recorder->IsRecording(model.username)) {
                        log(L"    [Recorder] Model offline - stopping recording.");
                        // Must post to main thread to stop (WebView2 is main-thread only)
                        SendMessage(wnd->GetHwnd(), WM_APP + 10,
                            (WPARAM)new std::string(model.username), 0);
                    }
                    // Update status in list
                    SendMessage(wnd->GetHwnd(), WM_APP + 11,
                        (WPARAM)new std::string(model.username),
                        (LPARAM)new std::string("offline"));
                    continue;
                }

                ms << (info.isLive ? L"LIVE" : L"offline");
                if (info.isLive) {
                    ms << L", chat=" << Utf8ToWide(info.chatMode.empty() ? "?" : info.chatMode)
                       << L", viewers=" << info.viewerCount;
                }
                wnd->PostLogLine(ms.str());

                std::string displayStatus = info.isLive
                    ? (info.isFreeChat ? "Live (Free)" : "Live (Private)")
                    : "offline";

                // Update status column on main thread
                SendMessage(wnd->GetHwnd(), WM_APP + 11,
                    (WPARAM)new std::string(model.username),
                    (LPARAM)new std::string(displayStatus));

                if (info.isLive && info.isFreeChat && !recorder->IsRecording(model.username)) {
                    log(L"    [Recorder] Starting recording...");
                    // Start must be called on main thread (WebView2 requirement)
                    // We copy the StreamInfo onto the heap and post it
                    SendMessage(wnd->GetHwnd(), WM_APP + 12,
                        (WPARAM)new StreamInfo(info), 0);
                } else if (recorder->IsRecording(model.username) &&
                           (!info.isLive || !info.isFreeChat)) {
                    log(L"    [Recorder] No longer in free chat - stopping.");
                    SendMessage(wnd->GetHwnd(), WM_APP + 10,
                        (WPARAM)new std::string(model.username), 0);
                }
            }
        }

        // Sleep for pollInterval seconds, checking stopFlag every second
        for (int s = 0; s < *pollInterval && !stopFlag->load(); ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    log(L"[Monitor] Stopped.");
}

// ─── WinMain ─────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int nCmdShow) {
    // Initialise Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Initialise COM (required by WebView2 and SHBrowseForFolder)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    AppState app;

    // ── Create GUI ────────────────────────────────────────────────────────────
    MainWindow mainWnd;
    app.mainWnd = &mainWnd;

    if (!mainWnd.Create(hInstance, nCmdShow)) {
        MessageBoxW(nullptr, L"Failed to create main window.", L"SwagLiveRecorder", MB_ICONERROR);
        return 1;
    }

    // ── Create recorder (main thread, shares message loop) ───────────────────
    RecorderConfig recCfg;
    recCfg.outputDirectory = app.outputDir;
    recCfg.appendTimestamp = true;

    Recorder recorder(recCfg, mainWnd.GetHwnd());
    app.recorder = &recorder;

    // Recorder callbacks → update GUI
    recorder.onStarted = [&](const std::string& u) {
        mainWnd.UpdateRecordingStatus(u, "● Recording");
        mainWnd.PostLogLine(L"[Recorder] Started: " + Utf8ToWide(u));
    };
    recorder.onStopped = [&](const std::string& u, size_t bytes) {
        mainWnd.UpdateRecordingStatus(u, "Stopped", bytes);
        std::wstringstream ss;
        ss << L"[Recorder] Stopped: " << Utf8ToWide(u)
           << L" (" << bytes / 1024 << L" KB)";
        mainWnd.PostLogLine(ss.str());
    };
    recorder.onError = [&](const std::string& u, const std::string& err) {
        mainWnd.UpdateRecordingStatus(u, "Error");
        mainWnd.PostLogLine(L"[Recorder] Error for " + Utf8ToWide(u) + L": " + Utf8ToWide(err));
    };
    recorder.onProgress = [&](const std::string& u, size_t bytes) {
        mainWnd.UpdateRecordingStatus(u, "● Recording", bytes);
    };

    // ── GUI callbacks ─────────────────────────────────────────────────────────
    mainWnd.onAddModel = [&](const std::string& name) {
        if (app.modelList.AddModel(name)) {
            app.modelList.Save();
            mainWnd.RefreshModelList(app.modelList);
            mainWnd.PostLogLine(L"Added: " + Utf8ToWide(name));
        } else {
            mainWnd.PostLogLine(L"'" + Utf8ToWide(name) + L"' already in list.");
        }
    };
    mainWnd.onRemoveModel = [&](const std::string& name) {
        if (recorder.IsRecording(name)) recorder.StopRecording(name);
        if (app.modelList.RemoveModel(name)) {
            app.modelList.Save();
            mainWnd.RefreshModelList(app.modelList);
            mainWnd.PostLogLine(L"Removed: " + Utf8ToWide(name));
        }
    };
    mainWnd.onSetEnabled = [&](const std::string& name, bool en) {
        app.modelList.SetEnabled(name, en);
        app.modelList.Save();
        mainWnd.RefreshModelList(app.modelList);
    };
    mainWnd.onSetOutputDir = [&](const std::string& dir) {
        app.outputDir = dir;
        recCfg.outputDirectory = dir;
    };
    mainWnd.onStartMonitor = [&]() {
        auto settings = mainWnd.GetSettings();
        app.pollInterval = settings.pollIntervalSec;
        if (!settings.authToken.empty())
            app.api.SetAuthToken(settings.authToken);
        app.stopMonitor.store(false);
        app.monThread = std::thread(MonitorThreadProc,
            &mainWnd, &app.stopMonitor,
            &app.modelList, &app.api,
            &recorder, &app.pollInterval);
        mainWnd.PostLogLine(L"[App] Monitor started (interval: " +
            std::to_wstring(app.pollInterval) + L"s).");
    };
    mainWnd.onStopMonitor = [&]() {
        app.stopMonitor.store(true);
        // Join on a helper thread so the main message loop keeps running
        // (WebView2 requires the main thread to keep pumping messages)
        if (app.monThread.joinable()) {
            std::thread joiner([&]() {
                if (app.monThread.joinable()) app.monThread.join();
            });
            joiner.detach();
        }
        mainWnd.PostLogLine(L"[App] Monitor stop requested.");
    };

    // Populate model list from file
    mainWnd.RefreshModelList(app.modelList);

    // ── Sub-class the main window HWND to handle custom WM_APP messages
    //    (main-thread operations posted from the monitor thread) ──────────────
    static WNDPROC origProc = nullptr;
    origProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(mainWnd.GetHwnd(), GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(
                [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                    static AppState* pApp = nullptr;
                    static Recorder* pRec = nullptr;
                    static MainWindow* pWnd = nullptr;

                    // First call: store pointers via WM_CREATE can't work here since
                    // we subclass after creation.  Use thread-local-like statics set
                    // from the enclosing lambda capture – we use a trick via WM_USER.
                    if (msg == WM_USER + 99) {
                        pApp = reinterpret_cast<AppState*>(wp);
                        pRec = reinterpret_cast<Recorder*>(lp);
                        pWnd = pApp->mainWnd;
                        return 0;
                    }
                    if (!pRec) {
                        return CallWindowProcW(origProc, hwnd, msg, wp, lp);
                    }

                    // WM_APP+10: stop recording for username (heap string, we free it)
                    if (msg == WM_APP + 10) {
                        auto* name = reinterpret_cast<std::string*>(wp);
                        if (name) { pRec->StopRecording(*name); delete name; }
                        return 0;
                    }
                    // WM_APP+11: update status label (heap strings, we free them)
                    if (msg == WM_APP + 11) {
                        auto* name   = reinterpret_cast<std::string*>(wp);
                        auto* status = reinterpret_cast<std::string*>(lp);
                        if (name && status && pWnd)
                            pWnd->UpdateRecordingStatus(*name, *status);
                        delete name; delete status;
                        return 0;
                    }
                    // WM_APP+12: start recording (heap StreamInfo, we free it)
                    if (msg == WM_APP + 12) {
                        auto* info = reinterpret_cast<StreamInfo*>(wp);
                        if (info) { pRec->StartRecording(*info); delete info; }
                        return 0;
                    }
                    return CallWindowProcW(origProc, hwnd, msg, wp, lp);
                }
            )
        )
    );

    // Initialise the subclass statics
    SendMessageW(mainWnd.GetHwnd(), WM_USER + 99,
        reinterpret_cast<WPARAM>(&app),
        reinterpret_cast<LPARAM>(&recorder));

    // ── Message loop ─────────────────────────────────────────────────────────
    int ret = mainWnd.MessageLoop();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    app.stopMonitor.store(true);
    // Wait for the monitor thread to finish (it sleeps in 1 s increments and
    // re-checks stopFlag, so it will exit within one poll interval at most)
    if (app.monThread.joinable()) app.monThread.join();

    recorder.StopAll();

    CoUninitialize();
    WSACleanup();
    return ret;
}
