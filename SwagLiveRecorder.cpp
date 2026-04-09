//=============================================================================
// SwagLiveRecorder - Automatic live-stream recorder for swag.live
//
// This console-mode application monitors a user-maintained watchlist of model
// usernames.  Whenever a watched model starts a free-chat live stream the
// program automatically begins recording the stream to disk via ffmpeg.
//
// Build requirements:
//   - Visual Studio 2019 (toolset v142)
//   - Windows SDK 10.0 or later
//   - ffmpeg.exe in PATH (or configured via --ffmpeg option)
//
// Usage:
//   SwagLiveRecorder.exe [options]
//
// Options:
//   --list                    Print current watchlist
//   --add <username>          Add model to watchlist
//   --remove <username>       Remove model from watchlist
//   --enable <username>       Enable auto-record for model
//   --disable <username>      Disable auto-record for model
//   --output <directory>      Set recording output directory (default: recordings)
//   --ffmpeg <path>           Path to ffmpeg.exe
//   --models <file>           Path to models watchlist file (default: models.txt)
//   --interval <seconds>      Polling interval in seconds (default: 60)
//   --token <auth_token>      swag.live authentication token (if required)
//   --run                     Start monitoring loop (default if no other action)
//
//=============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdlib>

#include "ModelList.h"
#include "SwagLiveAPI.h"
#include "Recorder.h"
#include "tlsclient/tlsclient.h"

// -----------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------

static volatile bool g_shutdown = false;

// -----------------------------------------------------------------------
// Signal handler
// -----------------------------------------------------------------------

static void HandleSignal(int sig) {
    (void)sig;
    g_shutdown = true;
}

// -----------------------------------------------------------------------
// Helper: case-insensitive string comparison
// -----------------------------------------------------------------------

static std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

// -----------------------------------------------------------------------
// Monitoring loop
// -----------------------------------------------------------------------

static void RunMonitorLoop(ModelList& modelList,
                           SwagLiveAPI& api,
                           Recorder& recorder,
                           int pollIntervalSec) {
    std::cout << "\n[Monitor] Starting monitor loop (poll every "
              << pollIntervalSec << "s). Press Ctrl+C to stop.\n\n";

    while (!g_shutdown) {
        const auto& models = modelList.GetModels();
        if (models.empty()) {
            std::cout << "[Monitor] Watchlist is empty. "
                         "Add models with --add <username>.\n";
        } else {
            std::cout << "[Monitor] Checking " << models.size()
                      << " model(s)...\n";

            for (const auto& model : models) {
                if (!model.enabled) continue;
                if (g_shutdown) break;

                StreamInfo info;
                bool found = api.GetModelStatus(model.username, info);

                if (!found) {
                    std::cout << "  " << model.username
                              << " - not found / offline\n";

                    // If we were recording and the model went offline, stop
                    if (recorder.IsRecording(model.username)) {
                        std::cout << "  [Recorder] Model went offline - "
                                     "stopping recording.\n";
                        recorder.StopRecording(model.username);
                    }
                    continue;
                }

                std::cout << "  " << model.username
                          << " - " << (info.isLive ? "LIVE" : "offline");

                if (info.isLive) {
                    std::cout << ", chat: " << (info.chatMode.empty()
                                                ? "unknown" : info.chatMode)
                              << ", viewers: " << info.viewerCount;
                }
                std::cout << "\n";

                if (info.isLive && info.isFreeChat) {
                    if (!recorder.IsRecording(model.username)) {
                        std::cout << "  [Recorder] Starting recording for "
                                  << model.username << "...\n";
                        recorder.StartRecording(info);
                    }
                } else {
                    // Model is not in free chat (private, group, etc.) or offline
                    if (recorder.IsRecording(model.username)) {
                        std::cout << "  [Recorder] Stream is no longer in "
                                     "free chat - stopping recording.\n";
                        recorder.StopRecording(model.username);
                    }
                }
            }
        }

        // Clean up sessions for recordings that have finished on their own
        recorder.CleanupFinished();

        // Display active recordings
        auto active = recorder.GetActiveRecordings();
        if (!active.empty()) {
            std::cout << "[Monitor] Currently recording:";
            for (const auto& name : active) {
                std::cout << " " << name;
            }
            std::cout << "\n";
        }

        // Sleep for polling interval, waking every second to check shutdown
        for (int s = 0; s < pollIntervalSec && !g_shutdown; ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "\n[Monitor] Shutdown requested - stopping all recordings.\n";
    recorder.StopAll();
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Install signal handler for graceful shutdown
    signal(SIGINT,  HandleSignal);
    signal(SIGTERM, HandleSignal);

    // Initialise TLS/networking subsystem
    TLSClient::InitializeGlobal();

    // ----------------------------------------------------------------
    // Parse command-line arguments
    // ----------------------------------------------------------------

    std::string modelsFile      = "models.txt";
    std::string outputDir       = "recordings";
    std::string ffmpegPath;
    std::string authToken;
    int pollIntervalSec         = 60;
    bool runLoop                = false;

    // Actions requested by the user
    bool doList                 = false;
    bool doAdd                  = false;
    bool doRemove               = false;
    bool doEnable               = false;
    bool doDisable              = false;
    std::string actionUsername;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--list") {
            doList = true;
        } else if (arg == "--add" && i + 1 < argc) {
            doAdd = true;
            actionUsername = argv[++i];
        } else if (arg == "--remove" && i + 1 < argc) {
            doRemove = true;
            actionUsername = argv[++i];
        } else if (arg == "--enable" && i + 1 < argc) {
            doEnable = true;
            actionUsername = argv[++i];
        } else if (arg == "--disable" && i + 1 < argc) {
            doDisable = true;
            actionUsername = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputDir = argv[++i];
        } else if (arg == "--ffmpeg" && i + 1 < argc) {
            ffmpegPath = argv[++i];
        } else if (arg == "--models" && i + 1 < argc) {
            modelsFile = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            pollIntervalSec = std::atoi(argv[++i]);
            if (pollIntervalSec < 5) pollIntervalSec = 5;
        } else if (arg == "--token" && i + 1 < argc) {
            authToken = argv[++i];
        } else if (arg == "--run") {
            runLoop = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "SwagLiveRecorder - Automatic recorder for swag.live live streams\n\n"
                "Usage: SwagLiveRecorder.exe [options]\n\n"
                "Options:\n"
                "  --list               Print current watchlist\n"
                "  --add <username>     Add model to watchlist\n"
                "  --remove <username>  Remove model from watchlist\n"
                "  --enable <username>  Enable auto-record for model\n"
                "  --disable <username> Disable auto-record for model\n"
                "  --output <dir>       Recording output directory (default: recordings)\n"
                "  --ffmpeg <path>      Path to ffmpeg.exe\n"
                "  --models <file>      Watchlist file path (default: models.txt)\n"
                "  --interval <sec>     Poll interval in seconds (default: 60)\n"
                "  --token <token>      swag.live auth token (if required)\n"
                "  --run                Start the monitoring loop\n"
                "  --help               Show this help message\n"
                "\nExamples:\n"
                "  SwagLiveRecorder.exe --add modelname\n"
                "  SwagLiveRecorder.exe --list\n"
                "  SwagLiveRecorder.exe --run --output D:\\recordings --interval 30\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg
                      << "\nRun with --help for usage information.\n";
            return 1;
        }
    }

    // Default: start the monitoring loop if no management action specified
    if (!doList && !doAdd && !doRemove && !doEnable && !doDisable) {
        runLoop = true;
    }

    // ----------------------------------------------------------------
    // Load watchlist
    // ----------------------------------------------------------------

    ModelList modelList(modelsFile);

    // ----------------------------------------------------------------
    // Execute management actions
    // ----------------------------------------------------------------

    if (doList) {
        std::cout << "Watchlist (" << modelsFile << "):\n";
        modelList.Print();
        return 0;
    }

    if (doAdd) {
        if (modelList.AddModel(actionUsername)) {
            modelList.Save();
            std::cout << "Added '" << actionUsername << "' to watchlist.\n";
        } else {
            std::cout << "'" << actionUsername << "' is already in the watchlist.\n";
        }
        return 0;
    }

    if (doRemove) {
        if (modelList.RemoveModel(actionUsername)) {
            modelList.Save();
            std::cout << "Removed '" << actionUsername << "' from watchlist.\n";
        } else {
            std::cout << "'" << actionUsername << "' was not in the watchlist.\n";
        }
        return 0;
    }

    if (doEnable) {
        if (modelList.SetEnabled(actionUsername, true)) {
            modelList.Save();
            std::cout << "Enabled recording for '" << actionUsername << "'.\n";
        } else {
            std::cout << "'" << actionUsername << "' not found in watchlist.\n";
        }
        return 0;
    }

    if (doDisable) {
        if (modelList.SetEnabled(actionUsername, false)) {
            modelList.Save();
            std::cout << "Disabled recording for '" << actionUsername << "'.\n";
        } else {
            std::cout << "'" << actionUsername << "' not found in watchlist.\n";
        }
        return 0;
    }

    // ----------------------------------------------------------------
    // Start monitoring loop
    // ----------------------------------------------------------------

    if (runLoop) {
        std::cout << "SwagLiveRecorder - Automatic recorder for swag.live\n";
        std::cout << "Watchlist file : " << modelsFile << "\n";
        std::cout << "Output dir     : " << outputDir << "\n";
        std::cout << "Poll interval  : " << pollIntervalSec << "s\n";

        if (!authToken.empty()) {
            std::cout << "Auth token     : [set]\n";
        }

        SwagLiveAPI api;
        if (!authToken.empty()) {
            api.SetAuthToken(authToken);
        }

        RecorderConfig recConfig;
        recConfig.outputDirectory  = outputDir;
        recConfig.ffmpegPath       = ffmpegPath;
        recConfig.appendTimestamp  = true;

        Recorder recorder(recConfig);

        RunMonitorLoop(modelList, api, recorder, pollIntervalSec);
    }

    WSACleanup();
    return 0;
}
