# SwagLiveRecorder

A Windows desktop application that automatically records **swag.live** live streams from a user-maintained watchlist.  When a watched model goes live in free chat, recording starts automatically.

---

## How recording works

swag.live delivers streams over **WebRTC** — not HLS or RTMP.  The recorder is fully native: no FFmpeg, no WebView2, no NuGet packages.

1. A **`rtc::WebSocket`** (built into libdatachannel) connects to swag.live's WebRTC signaling server.
2. An **`rtc::PeerConnection`** creates a receive-only SDP offer for VP8/VP9 video and Opus audio.
3. After the SDP offer/answer exchange, libdatachannel's built-in **`VP8RtpDepacketizer`** and **`OpusRtpDepacketizer`** media handlers reassemble RTP packets into complete frames.
4. Assembled frames arrive via **`track->onFrame()`** callbacks.
5. A hand-rolled (~300-line) **WebM/EBML muxer** writes frames directly to a **`.webm`** file — no external muxer library.

Output files can be played directly in VLC or remuxed to MP4 without re-encoding:

```
ffmpeg -i recording.webm -c copy output.mp4
```

### Signaling notes

The signaling implementation is a best-effort reverse-engineering of swag.live's WebRTC signaling endpoint.  The WebSocket URL and JSON message shapes are documented with `// TODO:` comments inside `LibDataChannelRecorder.cpp`.  If recording fails to connect, capture the real WebSocket traffic via browser DevTools → Network → WS and update those constants.

---

## Features

- **Win32 GUI** — watchlist with columns (username, enabled, status, bytes recorded), log area, settings panel
- **Automatic monitoring** — background thread polls the swag.live API at a configurable interval (default: 60 s)
- **Free-chat gating** — only starts recording when a model is live *and* in free chat; stops when they go private or offline
- **Multiple simultaneous recordings** — each model gets its own independent PeerConnection / recording thread
- **Persistent watchlist** — stored in `models.txt` next to the executable; survives restarts
- **Auth token support** — paste your swag.live Bearer token so the API returns complete stream status data
- **No vcpkg / NuGet** — libdatachannel is a git submodule; build it once with `build_deps.cmd`

---

## Requirements

### Build
| Component | Version |
|-----------|---------|
| Visual Studio | 2019 (toolset v142) or later |
| Windows SDK | 10.0 |
| C++ standard | C++17 |
| CMake | 3.13+ (for building libdatachannel) |

### Runtime
| Component | Notes |
|-----------|-------|
| Windows 10 1803+ or Windows 11 | |
| `datachannel.dll` | Built by `build_deps.cmd`; copied automatically to the output directory by a post-build event |

---

## Building

### One-time setup (build libdatachannel)

From a **Developer Command Prompt for VS 2019** (so that `cmake` and `cl` are on the PATH):

```bat
git clone --recurse-submodules https://github.com/Zero3K20/SwagLiveRecorder.git
cd SwagLiveRecorder
build_deps.cmd
```

`build_deps.cmd` will:
1. Run `git submodule update --init --recursive --depth 1` to populate `deps/libdatachannel`.
2. Configure the library with CMake (uses Mbed TLS — no OpenSSL installation needed).
3. Build both **Release** and **Debug** configurations.

The resulting `datachannel.lib` / `datachannel.dll` land in `deps\libdatachannel\build\Release\` and `Debug\`.

### Build the application

1. **Open** `SwagLiveRecorder.sln` in Visual Studio 2019 (or later).
2. Select **Release | x64** and press `Ctrl+Shift+B`.
3. The post-build event automatically copies `datachannel.dll` next to the executable.
4. The executable is written to `x64\Release\SwagLiveRecorder.exe`.

> **No NuGet restore needed.**  All dependencies are in-tree.

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
├── SwagLiveRecorder.cpp           # WinMain, monitor thread, app wiring
├── MainWindow.h/.cpp              # Win32 GUI
├── LibDataChannelRecorder.h/.cpp  # WebRTC recorder (signaling + PeerConnection + muxer)
├── WebMMuxer.h/.cpp               # Hand-rolled EBML/WebM muxer (~300 lines, no external lib)
├── RTPDepayloader.h               # Standalone VP8/VP9/Opus RTP depayloaders (reference)
├── Recorder.h/.cpp                # Multi-session manager
├── SwagLiveAPI.h/.cpp             # HTTPS API client (WinHTTP/TLSClient)
├── ModelList.h/.cpp               # Persistent watchlist
├── json_minimal.h                 # Single-header JSON parser
├── chunked_decode.h               # HTTP chunked-transfer decoder
├── tlsclient/                     # WinHTTP TLS client
│   ├── tlsclient.h/.cpp
│   └── lock.h
├── deps/
│   └── libdatachannel/            # Git submodule (paullouisageneau/libdatachannel)
├── build_deps.cmd                 # One-shot CMake build of libdatachannel
├── SwagLiveRecorder.vcxproj       # VS2019 project
└── SwagLiveRecorder.sln
```

---

## TLS client

The TLS client used to query the swag.live API is based on the implementation in the [Tardsplaya project](https://github.com/Zero3K/Tardsplaya/tree/main/tlsclient).  It uses Windows' built-in **WinHTTP** stack — no OpenSSL or other third-party TLS library is required for the API layer.

libdatachannel uses **Mbed TLS** (compiled as a submodule) for its own DTLS/TLS stack.

---

## Output format

Recordings are saved as **`.webm`** files (WebM container, VP8 video, Opus audio).

