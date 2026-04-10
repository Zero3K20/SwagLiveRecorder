//=============================================================================
// SwagLiveRecorder - Automatic WebRTC live-stream recorder for swag.live
//
// Architecture:
//   - Win32 GUI (MainWindow) for the watchlist, recording status, and settings
//   - Background monitor thread polls the swag.live API (SwagLiveAPI + TLSClient)
//   - LibDataChannelRecorder performs WebRTC signaling + media receipt using the
//     libdatachannel library (at deps/libdatachannel) and writes
//     directly to .webm files via the hand-rolled WebMMuxer.
//
// Build requirements:
//   - Visual Studio 2019 (toolset v142), Windows SDK 10.0
//   - libdatachannel: run build_deps.cmd once before building
//=============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

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
                bool found = api->ResolveStream(model.username, info);

                std::wstringstream ms;
                ms << L"  " << Utf8ToWide(model.username) << L" - ";

                if (!found) {
                    ms << L"offline/not found";
                    wnd->PostLogLine(ms.str());

                    // Stop any running recording for this model
                    if (recorder->IsRecording(model.username)) {
                        log(L"    [Recorder] Model offline - stopping recording.");
                        recorder->StopRecording(model.username);
                    }
                    // Update status in list
                    PostMessage(wnd->GetHwnd(), WM_APP + 11,
                        (WPARAM)new std::string(model.username),
                        (LPARAM)new std::string("offline"));
                    continue;
                }

                // preset "preview" = free chat (price == 0)
                // preset "sd"      = paid chat  (price > 0)
                bool isFreePreview = (info.preset == "preview" && info.price == 0);

                ms << L"LIVE";
                if (!info.title.empty())
                    ms << L" [" << Utf8ToWide(info.title) << L"]";
                ms << L", preset=" << Utf8ToWide(info.preset)
                   << (info.exclusive ? L" (exclusive)" : L"");
                wnd->PostLogLine(ms.str());

                std::string displayStatus = isFreePreview ? "Live (Free Preview)" : "Live (Paid)";

                // Update status column on main thread
                PostMessage(wnd->GetHwnd(), WM_APP + 11,
                    (WPARAM)new std::string(model.username),
                    (LPARAM)new std::string(displayStatus));

                if (isFreePreview && !recorder->IsRecording(model.username)) {
                    log(L"    [Recorder] Starting recording (free preview)...");
                    auto settings = wnd->GetSettings();
                    recorder->StartRecording(info, settings.authToken);
                } else if (recorder->IsRecording(model.username) && !info.isLive) {
                    log(L"    [Recorder] No longer live - stopping.");
                    recorder->StopRecording(model.username);
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

    // ── Create recorder ───────────────────────────────────────────────────────
    RecorderConfig recCfg;
    recCfg.outputDirectory = app.outputDir;
    recCfg.appendTimestamp = true;

    Recorder recorder(recCfg);
    app.recorder = &recorder;

    // Recorder callbacks → update GUI (callbacks may fire from the recorder's
    // background thread, so use PostMessage rather than direct UI calls).
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

    // ── Message loop ─────────────────────────────────────────────────────────
    int ret = mainWnd.MessageLoop();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    app.stopMonitor.store(true);
    if (app.monThread.joinable()) app.monThread.join();

    recorder.StopAll();

    CoUninitialize();
    WSACleanup();
    return ret;
}
