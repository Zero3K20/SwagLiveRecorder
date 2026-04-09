# SwagLiveRecorder

A Windows desktop application that automatically records **swag.live** live streams from a user-maintained watchlist.  When a watched model goes live in free chat, recording starts automatically.

---

## How recording works

swag.live delivers streams over **WebRTC** — not HLS or RTMP.  The program uses the same technique as the [LivestreamRecorder userscript](https://github.com/zero3k20/LivestreamRecorder):

1. A hidden **WebView2** (Edge-based) browser instance opens the model's stream page (`https://swag.live/livestream/<username>`).
2. JavaScript is injected at document-start that **hooks `HTMLMediaElement.prototype.srcObject`**.  When the page's player assigns a `MediaStream` to a `<video>` element, the hook fires.
3. The browser's built-in **`MediaRecorder` API** records the stream as a series of 500 ms WebM/VP8+Opus chunks.
4. Each chunk is base64-encoded and sent to the C++ host via `window.chrome.webview.postMessage`.
5. The C++ side decodes the chunk and appends it to the output **`.webm`** file on disk — no buffering in memory.

Output files can be played directly in VLC or remuxed to MP4 without re-encoding:

```
ffmpeg -i recording.webm -c copy output.mp4
```

---

## Features

- **Win32 GUI** — watchlist with columns (username, enabled, status, bytes recorded), log area, settings panel
- **Automatic monitoring** — background thread polls the swag.live API at a configurable interval (default: 60 s)
- **Free-chat gating** — only starts recording when a model is live *and* in free chat; stops when they go private or offline
- **Multiple simultaneous recordings** — each model gets its own independent WebView2 / MediaRecorder session
- **Persistent watchlist** — stored in `models.txt` next to the executable; survives restarts
- **Auth token support** — paste your swag.live Bearer token so the API returns complete stream status data

---

## Requirements

### Build
| Component | Version |
|-----------|---------|
| Visual Studio | 2019 (toolset v142) |
| Windows SDK | 10.0 |
| C++ standard | C++17 |
| NuGet package | `Microsoft.Web.WebView2` 1.0.2739.15 |
| NuGet package | `Microsoft.Windows.ImplementationLibrary` 1.0.240122.1 (WIL — shipped with the WebView2 package) |

### Runtime
| Component | Notes |
|-----------|-------|
| Windows 10 1803+ or Windows 11 | WebView2 runtime ships with Win11 and is auto-updated on Win10 |
| WebView2 Runtime | Installed automatically via Windows Update; can also be installed manually from [aka.ms/webview2](https://developer.microsoft.com/microsoft-edge/webview2/) |

---

## Building

1. **Clone or download** this repository.
2. **Restore NuGet packages** — open a Developer Command Prompt and run:
   ```
   nuget restore SwagLiveRecorder.sln
   ```
   Or open the solution in Visual Studio 2019 and let the automatic package restore run.
3. **Open** `SwagLiveRecorder.sln` in Visual Studio 2019.
4. Select the **Release | x64** (or Win32) configuration and press **Build → Build Solution** (`Ctrl+Shift+B`).
5. The executable is written to `x64\Release\SwagLiveRecorder.exe`.

> **Note on WIL:** the Windows Implementation Library (`wil/com.h`) is included as a NuGet dependency of WebView2.  No separate download is needed after NuGet restore.

---

## Usage

1. **Run** `SwagLiveRecorder.exe`.
2. Click **Add Model** and type a swag.live username (e.g. `alice`).
3. *(Optional)* Click **Browse…** next to *Output directory* to choose where `.webm` files are saved (default: `recordings\` beside the executable).
4. *(Optional)* Paste your swag.live Bearer token into the *Auth token* field and adjust the *Poll interval*.
5. Click **Start Monitor**.  The log area shows polling activity.
6. When a watched model goes live in free chat the log shows `[Recorder] Starting recording...` and the *Recorded* column fills with live byte counts.
7. Click **Stop Monitor** (or close the window) to stop all recordings cleanly.

### Model list

| Button | Action |
|--------|--------|
| Add Model | Prompt for a username and add to the watchlist |
| Remove Model | Remove selected model (stops any active recording) |
| Enable | Re-enable a disabled model |
| Disable | Skip a model without removing it |

---

## File layout

```
SwagLiveRecorder/
├── SwagLiveRecorder.cpp       # WinMain entry point, monitor thread, app wiring
├── MainWindow.h/.cpp          # Win32 GUI — watchlist listview, log, settings
├── WebRTCRecorder.h/.cpp      # Per-model WebView2 + MediaRecorder recording session
├── Recorder.h/.cpp            # Multi-session manager delegating to WebRTCRecorder
├── SwagLiveAPI.h/.cpp         # HTTPS API client for swag.live (uses TLSClient)
├── ModelList.h/.cpp           # Persistent watchlist (models.txt)
├── json_minimal.h             # Minimal single-header JSON parser
├── chunked_decode.h           # HTTP chunked-transfer decoder
├── tlsclient/                 # TLS/WinHTTP client (from Tardsplaya project)
│   ├── tlsclient.h
│   ├── tlsclient.cpp
│   └── lock.h
├── packages.config            # NuGet package references
├── SwagLiveRecorder.vcxproj   # VS2019 project (Windows app, v142 toolset)
└── SwagLiveRecorder.sln
```

---

## TLS client

The TLS client used to query the swag.live API is based on the implementation in the [Tardsplaya project](https://github.com/Zero3K/Tardsplaya/tree/main/tlsclient).  It uses Windows' built-in **WinHTTP** stack — no OpenSSL or other third-party TLS library is required.

---

## Output format

Recordings are saved as **`.webm`** files (WebM container, VP8 or VP9 video, Opus audio) — the native output of the browser's `MediaRecorder` API.  This is the same format produced by the LivestreamRecorder userscript for WebRTC streams.
