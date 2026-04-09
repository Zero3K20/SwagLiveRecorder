#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <functional>

#include "ModelList.h"
#include "SwagLiveAPI.h"

// Message IDs posted from the monitor thread to the main window
#define WM_MONITOR_STATUS  (WM_APP + 1)  // wParam = 0 (model status update)
#define WM_MONITOR_LOG     (WM_APP + 2)  // lParam = new LPWSTR log line (caller frees)
#define WM_RECORDING_EVENT (WM_APP + 3)  // wParam = event type (see below)

// wParam values for WM_RECORDING_EVENT
#define REC_EVENT_STARTED   1
#define REC_EVENT_STOPPED   2
#define REC_EVENT_ERROR     3
#define REC_EVENT_PROGRESS  4

// Current settings read/written by the GUI
struct AppSettings {
    std::string outputDirectory;
    std::string authToken;
    std::string modelsFile;
    int         pollIntervalSec;

    AppSettings()
        : outputDirectory("recordings"),
          modelsFile("models.txt"),
          pollIntervalSec(60) {}
};

// Encapsulates the main application window.
// All UI control creation, message routing, and layout live here.
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    // Create the window and child controls. Returns false on failure.
    bool Create(HINSTANCE hInstance, int nCmdShow);

    // Standard Win32 message loop. Runs until WM_QUIT.
    int MessageLoop();

    // Called by the monitor thread to post a log line (thread-safe).
    // The string is heap-allocated; MainWindow frees it.
    void PostLogLine(const std::wstring& line);

    // Called by the monitor thread to trigger a listview refresh (thread-safe).
    void PostStatusRefresh();

    // Read current settings from the GUI controls.
    AppSettings GetSettings() const;

    // Update model list display.
    void RefreshModelList(const ModelList& models);

    // Update the recording status for a single model in the list.
    void UpdateRecordingStatus(const std::string& username, const std::string& status, size_t bytesWritten = 0);

    // Append a line to the log area.
    void AppendLog(const std::wstring& line);

    // Provide access to the HWND (needed by some callers for parenting dialogs).
    HWND GetHwnd() const { return m_hwnd; }

    // Callbacks set by main code
    std::function<void()>                         onStartMonitor;
    std::function<void()>                         onStopMonitor;
    std::function<void(const std::string& name)>  onAddModel;
    std::function<void(const std::string& name)>  onRemoveModel;
    std::function<void(const std::string& name, bool enabled)> onSetEnabled;
    std::function<void(const std::string& dir)>   onSetOutputDir;

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    // Child controls
    HWND m_hwndList     = nullptr;  // ListView: model list
    HWND m_hwndLog      = nullptr;  // Edit (read-only): log output
    HWND m_hwndAdd      = nullptr;  // Button: Add model
    HWND m_hwndRemove   = nullptr;  // Button: Remove model
    HWND m_hwndEnable   = nullptr;  // Button: Enable
    HWND m_hwndDisable  = nullptr;  // Button: Disable
    HWND m_hwndStart    = nullptr;  // Button: Start Monitor
    HWND m_hwndStop     = nullptr;  // Button: Stop Monitor
    HWND m_hwndOutLabel = nullptr;  // Static: "Output:"
    HWND m_hwndOutEdit  = nullptr;  // Edit: output directory
    HWND m_hwndBrowse   = nullptr;  // Button: Browse…
    HWND m_hwndIntLabel = nullptr;  // Static: "Interval (s):"
    HWND m_hwndInterval = nullptr;  // Edit: poll interval
    HWND m_hwndTokLabel = nullptr;  // Static: "Auth token:"
    HWND m_hwndToken    = nullptr;  // Edit: auth token
    HWND m_hwndStatus   = nullptr;  // Static: status bar

    bool m_monitoring = false;

    // Window class name
    static const wchar_t* ClassName() { return L"SwagLiveRecorderMainWnd"; }

    // Register the window class (called once).
    static bool RegisterWindowClass(HINSTANCE hInstance);

    // Create all child controls.
    void CreateControls();

    // Layout all child controls to fit the current window size.
    void LayoutControls();

    // WM_COMMAND handler.
    void OnCommand(WORD id, WORD code);

    // Show an input dialog and return the entered string, or "" on cancel.
    std::wstring PromptString(const std::wstring& title, const std::wstring& prompt);

    // Browse for a directory.
    std::wstring BrowseForDirectory();

    // Get the currently selected model username from the list view.
    std::string GetSelectedUsername() const;

    // Static WndProc, delegates to the instance method.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // Control IDs
    enum CtrlId {
        ID_LIST    = 101,
        ID_LOG     = 102,
        ID_ADD     = 103,
        ID_REMOVE  = 104,
        ID_ENABLE  = 105,
        ID_DISABLE = 106,
        ID_START   = 107,
        ID_STOP    = 108,
        ID_BROWSE  = 109,
        ID_OUT_EDT = 110,
        ID_INT_EDT = 111,
        ID_TOK_EDT = 112,
    };
};
