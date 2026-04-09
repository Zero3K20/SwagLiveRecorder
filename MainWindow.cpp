#include "MainWindow.h"

#include <shlobj.h>      // SHBrowseForFolder
#include <commctrl.h>
#include <sstream>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// ─── Column indices in the listview ──────────────────────────────────────────
enum LvCol { COL_USER = 0, COL_ENABLED, COL_STATUS, COL_BYTES, COL_COUNT };

// ─── Helper: wstring↔string ──────────────────────────────────────────────────

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring r(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &r[0], n);
    return r;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string r(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &r[0], n, nullptr, nullptr);
    return r;
}

// ─── MainWindow ──────────────────────────────────────────────────────────────

MainWindow::MainWindow() {}
MainWindow::~MainWindow() {}

bool MainWindow::RegisterWindowClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = ClassName();
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    return RegisterClassExW(&wc) != 0;
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (!RegisterWindowClass(hInstance)) return false;

    m_hwnd = CreateWindowExW(
        0, ClassName(), L"SwagLive Recorder",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 620,
        nullptr, nullptr, hInstance, this);

    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

int MainWindow::MessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

// ─── Control creation ────────────────────────────────────────────────────────

void MainWindow::CreateControls() {
    HINSTANCE hi = m_hInstance;
    HWND hw = m_hwnd;
    DWORD exS = WS_EX_CLIENTEDGE;
    DWORD btn = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    DWORD edt = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
    DWORD stc = WS_CHILD | WS_VISIBLE | SS_LEFT;

    // Toolbar buttons
    m_hwndAdd     = CreateWindowW(L"BUTTON", L"Add Model",      btn, 0, 0, 1, 1, hw, (HMENU)ID_ADD,    hi, nullptr);
    m_hwndRemove  = CreateWindowW(L"BUTTON", L"Remove Model",   btn, 0, 0, 1, 1, hw, (HMENU)ID_REMOVE, hi, nullptr);
    m_hwndEnable  = CreateWindowW(L"BUTTON", L"Enable",         btn, 0, 0, 1, 1, hw, (HMENU)ID_ENABLE,  hi, nullptr);
    m_hwndDisable = CreateWindowW(L"BUTTON", L"Disable",        btn, 0, 0, 1, 1, hw, (HMENU)ID_DISABLE, hi, nullptr);
    m_hwndStart   = CreateWindowW(L"BUTTON", L"Start Monitor",  btn, 0, 0, 1, 1, hw, (HMENU)ID_START,   hi, nullptr);
    m_hwndStop    = CreateWindowW(L"BUTTON", L"Stop Monitor",   btn, 0, 0, 1, 1, hw, (HMENU)ID_STOP,    hi, nullptr);

    // Model listview
    m_hwndList = CreateWindowExW(exS, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 1, 1, hw, (HMENU)ID_LIST, hi, nullptr);

    ListView_SetExtendedListViewStyle(m_hwndList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    const wchar_t* colNames[] = { L"Username", L"Enabled", L"Status", L"Recorded" };
    int colWidths[] = { 180, 70, 120, 110 };
    for (int i = 0; i < COL_COUNT; ++i) {
        LVCOLUMNW lvc = {};
        lvc.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx      = colWidths[i];
        lvc.pszText = const_cast<wchar_t*>(colNames[i]);
        lvc.iSubItem = i;
        ListView_InsertColumn(m_hwndList, i, &lvc);
    }

    // Settings row
    m_hwndOutLabel = CreateWindowW(L"STATIC",  L"Output directory:", stc, 0, 0, 1, 1, hw, nullptr, hi, nullptr);
    m_hwndOutEdit  = CreateWindowW(L"EDIT",    L"recordings",        edt | ES_READONLY, 0, 0, 1, 1, hw, (HMENU)ID_OUT_EDT, hi, nullptr);
    m_hwndBrowse   = CreateWindowW(L"BUTTON",  L"Browse\u2026",      btn, 0, 0, 1, 1, hw, (HMENU)ID_BROWSE, hi, nullptr);
    m_hwndIntLabel = CreateWindowW(L"STATIC",  L"Poll interval (s):", stc, 0, 0, 1, 1, hw, nullptr, hi, nullptr);
    m_hwndInterval = CreateWindowW(L"EDIT",    L"60",                edt, 0, 0, 1, 1, hw, (HMENU)ID_INT_EDT, hi, nullptr);
    m_hwndTokLabel = CreateWindowW(L"STATIC",  L"Auth token:",       stc, 0, 0, 1, 1, hw, nullptr, hi, nullptr);
    m_hwndToken    = CreateWindowW(L"EDIT",    L"",                  edt | ES_PASSWORD, 0, 0, 1, 1, hw, (HMENU)ID_TOK_EDT, hi, nullptr);

    // Log area
    m_hwndLog = CreateWindowExW(exS, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 1, 1, hw, (HMENU)ID_LOG, hi, nullptr);

    // Status bar
    m_hwndStatus = CreateWindowW(L"STATIC", L"Ready.", stc, 0, 0, 1, 1, hw, nullptr, hi, nullptr);

    EnableWindow(m_hwndStop, FALSE);
}

// ─── Layout ──────────────────────────────────────────────────────────────────

void MainWindow::LayoutControls() {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = 6;   // margin
    int bh = 26, bw = 110, sh = 20, eh = 22;

    // Row 1: toolbar buttons
    int x = m, y = m;
    SetWindowPos(m_hwndAdd,     nullptr, x, y, bw,     bh, SWP_NOZORDER); x += bw + m;
    SetWindowPos(m_hwndRemove,  nullptr, x, y, bw,     bh, SWP_NOZORDER); x += bw + m;
    SetWindowPos(m_hwndEnable,  nullptr, x, y, bw - 20,bh, SWP_NOZORDER); x += bw - 20 + m;
    SetWindowPos(m_hwndDisable, nullptr, x, y, bw - 20,bh, SWP_NOZORDER); x += bw - 20 + m;
    // Right-align monitor buttons
    SetWindowPos(m_hwndStop,  nullptr, W - m - bw,         y, bw, bh, SWP_NOZORDER);
    SetWindowPos(m_hwndStart, nullptr, W - m - bw * 2 - m, y, bw, bh, SWP_NOZORDER);

    // Row 2: settings
    y += bh + m;
    int lblW = 130, edtW = W - lblW * 3 - bw - m * 7 - 70, intW = 50, tokW = edtW - 50;
    x = m;
    SetWindowPos(m_hwndOutLabel, nullptr, x, y + 2, lblW,  sh, SWP_NOZORDER); x += lblW + m;
    SetWindowPos(m_hwndOutEdit,  nullptr, x, y,     edtW,  eh, SWP_NOZORDER); x += edtW + m;
    SetWindowPos(m_hwndBrowse,   nullptr, x, y,     60,    bh, SWP_NOZORDER); x += 60 + m * 2;
    SetWindowPos(m_hwndIntLabel, nullptr, x, y + 2, lblW,  sh, SWP_NOZORDER); x += lblW + m;
    SetWindowPos(m_hwndInterval, nullptr, x, y,     intW,  eh, SWP_NOZORDER); x += intW + m * 2;
    SetWindowPos(m_hwndTokLabel, nullptr, x, y + 2, lblW - 20, sh, SWP_NOZORDER); x += lblW - 20 + m;
    SetWindowPos(m_hwndToken,    nullptr, x, y,     W - x - m, eh, SWP_NOZORDER);

    // Row 3 onward: listview
    y += bh + m;
    int listH = (H - y - m - 120 - m - sh - m);
    if (listH < 60) listH = 60;
    SetWindowPos(m_hwndList, nullptr, m, y, W - m * 2, listH, SWP_NOZORDER);

    // Log box
    y += listH + m;
    int logH = H - y - m - sh - m;
    if (logH < 40) logH = 40;
    SetWindowPos(m_hwndLog, nullptr, m, y, W - m * 2, logH, SWP_NOZORDER);

    // Status bar
    y += logH + m;
    SetWindowPos(m_hwndStatus, nullptr, m, y, W - m * 2, sh, SWP_NOZORDER);
}

// ─── WndProc ─────────────────────────────────────────────────────────────────

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        pThis = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hwnd = hwnd;
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (pThis) return pThis->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateControls();
        return 0;

    case WM_SIZE:
        LayoutControls();
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;

    case WM_MONITOR_LOG: {
        // lParam is a heap-allocated wchar_t* line; we own it
        auto* line = reinterpret_cast<std::wstring*>(lp);
        if (line) {
            AppendLog(*line);
            delete line;
        }
        return 0;
    }

    case WM_MONITOR_STATUS:
        // Trigger a listview refresh by the main thread (model statuses updated externally)
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

// ─── Commands ────────────────────────────────────────────────────────────────

void MainWindow::OnCommand(WORD id, WORD /*code*/) {
    switch (id) {
    case ID_ADD: {
        std::wstring ws = PromptString(L"Add Model", L"Enter model username:");
        if (!ws.empty() && onAddModel)
            onAddModel(WideToUtf8(ws));
        break;
    }
    case ID_REMOVE: {
        std::string sel = GetSelectedUsername();
        if (!sel.empty() && onRemoveModel) onRemoveModel(sel);
        break;
    }
    case ID_ENABLE: {
        std::string sel = GetSelectedUsername();
        if (!sel.empty() && onSetEnabled) onSetEnabled(sel, true);
        break;
    }
    case ID_DISABLE: {
        std::string sel = GetSelectedUsername();
        if (!sel.empty() && onSetEnabled) onSetEnabled(sel, false);
        break;
    }
    case ID_START:
        if (!m_monitoring) {
            m_monitoring = true;
            EnableWindow(m_hwndStart, FALSE);
            EnableWindow(m_hwndStop,  TRUE);
            SetWindowTextW(m_hwndStatus, L"Monitor running...");
            if (onStartMonitor) onStartMonitor();
        }
        break;
    case ID_STOP:
        if (m_monitoring) {
            m_monitoring = false;
            EnableWindow(m_hwndStart, TRUE);
            EnableWindow(m_hwndStop,  FALSE);
            SetWindowTextW(m_hwndStatus, L"Monitor stopped.");
            if (onStopMonitor) onStopMonitor();
        }
        break;
    case ID_BROWSE: {
        std::wstring dir = BrowseForDirectory();
        if (!dir.empty()) {
            SetWindowTextW(m_hwndOutEdit, dir.c_str());
            if (onSetOutputDir) onSetOutputDir(WideToUtf8(dir));
        }
        break;
    }
    }
}

// ─── Public interface ─────────────────────────────────────────────────────────

void MainWindow::PostLogLine(const std::wstring& line) {
    auto* heap = new std::wstring(line);
    PostMessageW(m_hwnd, WM_MONITOR_LOG, 0, reinterpret_cast<LPARAM>(heap));
}

void MainWindow::PostStatusRefresh() {
    PostMessageW(m_hwnd, WM_MONITOR_STATUS, 0, 0);
}

AppSettings MainWindow::GetSettings() const {
    AppSettings s;
    wchar_t buf[1024] = {};

    GetWindowTextW(m_hwndOutEdit, buf, 1024);
    s.outputDirectory = WideToUtf8(buf);

    GetWindowTextW(m_hwndInterval, buf, 32);
    s.pollIntervalSec = _wtoi(buf);
    if (s.pollIntervalSec < 5) s.pollIntervalSec = 5;

    GetWindowTextW(m_hwndToken, buf, 1024);
    s.authToken = WideToUtf8(buf);

    return s;
}

void MainWindow::RefreshModelList(const ModelList& models) {
    ListView_DeleteAllItems(m_hwndList);

    const auto& list = models.GetModels();
    for (int i = 0; i < (int)list.size(); ++i) {
        const auto& m = list[i];
        std::wstring wUser = Utf8ToWide(m.username);

        LVITEMW lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        lvi.pszText = const_cast<wchar_t*>(wUser.c_str());
        ListView_InsertItem(m_hwndList, &lvi);

        ListView_SetItemText(m_hwndList, i, COL_ENABLED,
            const_cast<wchar_t*>(m.enabled ? L"Yes" : L"No"));
        ListView_SetItemText(m_hwndList, i, COL_STATUS, const_cast<wchar_t*>(L"—"));
        ListView_SetItemText(m_hwndList, i, COL_BYTES,  const_cast<wchar_t*>(L"—"));
    }
}

void MainWindow::UpdateRecordingStatus(const std::string& username,
                                       const std::string& status,
                                       size_t bytesWritten) {
    std::wstring wUser = Utf8ToWide(username);
    int count = ListView_GetItemCount(m_hwndList);
    for (int i = 0; i < count; ++i) {
        wchar_t buf[256] = {};
        ListView_GetItemText(m_hwndList, i, COL_USER, buf, 256);
        if (wUser == buf) {
            std::wstring wStatus = Utf8ToWide(status);
            ListView_SetItemText(m_hwndList, i, COL_STATUS,
                const_cast<wchar_t*>(wStatus.c_str()));

            if (bytesWritten > 0) {
                // Format bytes
                wchar_t sz[64];
                if (bytesWritten < 1024)
                    swprintf_s(sz, L"%zu B", bytesWritten);
                else if (bytesWritten < 1048576)
                    swprintf_s(sz, L"%.1f KB", bytesWritten / 1024.0);
                else
                    swprintf_s(sz, L"%.2f MB", bytesWritten / 1048576.0);
                ListView_SetItemText(m_hwndList, i, COL_BYTES, sz);
            }
            break;
        }
    }
}

void MainWindow::AppendLog(const std::wstring& line) {
    // Append to the edit box and scroll to bottom
    int len = GetWindowTextLengthW(m_hwndLog);
    SendMessageW(m_hwndLog, EM_SETSEL, len, len);
    std::wstring text = line + L"\r\n";
    SendMessageW(m_hwndLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(m_hwndLog, EM_SCROLLCARET, 0, 0);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string MainWindow::GetSelectedUsername() const {
    int idx = ListView_GetNextItem(m_hwndList, -1, LVNI_SELECTED);
    if (idx < 0) return "";
    wchar_t buf[256] = {};
    ListView_GetItemText(m_hwndList, idx, COL_USER, buf, 256);
    return WideToUtf8(buf);
}

std::wstring MainWindow::PromptString(const std::wstring& title, const std::wstring& prompt) {
    // Simple modal input dialog using a message box with an edit control is not
    // straightforward in pure Win32 without a dialog resource.  We use a small
    // custom dialog created programmatically.
    struct DlgData { std::wstring prompt; std::wstring result; };
    DlgData data;
    data.prompt = prompt;

    // Dialog template in memory
    struct DlgTmpl {
        DLGTEMPLATE hdr;
        WORD menu, cls, title_w;
        wchar_t titleBuf[64];
    };

    // Use a simple InputBox via a common dialog workaround: DialogBoxIndirectParam
    // is complex to set up without resources.  Fall back to a simple message-box-
    // style dialog using a static resource defined at compile time.  Since we have
    // no .rc file, prompt the user via a separate popup window.

    // Create a simple popup window with an edit and two buttons
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        0, 0, 360, 110,
        m_hwnd, nullptr, m_hInstance, nullptr);

    if (!dlg) return L"";

    HWND hLabel = CreateWindowW(L"STATIC", prompt.c_str(),
        WS_CHILD | WS_VISIBLE, 10, 10, 330, 20, dlg, nullptr, m_hInstance, nullptr);
    HWND hEdit  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 34, 330, 24, dlg, (HMENU)1, m_hInstance, nullptr);
    HWND hOK    = CreateWindowW(L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 170, 68, 80, 24, dlg, (HMENU)IDOK, m_hInstance, nullptr);
    HWND hCancel= CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 260, 68, 80, 24, dlg, (HMENU)IDCANCEL, m_hInstance, nullptr);
    (void)hLabel; (void)hOK; (void)hCancel;

    // Centre on parent
    RECT pr, dr;
    GetWindowRect(m_hwnd, &pr);
    GetWindowRect(dlg, &dr);
    int dx = pr.left + (pr.right - pr.left - (dr.right - dr.left)) / 2;
    int dy = pr.top  + (pr.bottom - pr.top - (dr.bottom - dr.top)) / 2;
    SetWindowPos(dlg, nullptr, dx, dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    SetFocus(hEdit);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // Run a nested message loop that continues to dispatch all messages so that
    // WebView2 and the parent window remain responsive while the dialog is open.
    // IsDialogMessage handles Tab/Enter/Escape navigation for the dialog controls.
    std::wstring result;
    bool done = false;
    MSG msg;
    while (!done && GetMessageW(&msg, nullptr, 0, 0)) {
        // Translate Enter/Escape at the top level before IsDialogMessage consumes them
        if (msg.hwnd == hEdit || msg.hwnd == dlg) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_RETURN) {
                    wchar_t buf[512] = {};
                    GetWindowTextW(hEdit, buf, 512);
                    result = buf;
                    done = true;
                    continue;
                } else if (msg.wParam == VK_ESCAPE) {
                    done = true;
                    continue;
                }
            }
        }
        // Handle OK / Cancel button clicks
        if (msg.message == WM_COMMAND && msg.hwnd == dlg) {
            WORD cid = LOWORD(msg.wParam);
            if (cid == IDOK) {
                wchar_t buf[512] = {};
                GetWindowTextW(hEdit, buf, 512);
                result = buf;
                done = true;
                continue;
            } else if (cid == IDCANCEL) {
                done = true;
                continue;
            }
        }
        // IsDialogMessage handles Tab/focus traversal inside the dialog
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyWindow(dlg);
    return result;
}

std::wstring MainWindow::BrowseForDirectory() {
    // Use SHBrowseForFolder (old-style, works on all Windows versions)
    wchar_t buf[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = m_hwnd;
    bi.pszDisplayName = buf;
    bi.lpszTitle = L"Select output directory for recordings:";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";

    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return path;
}
