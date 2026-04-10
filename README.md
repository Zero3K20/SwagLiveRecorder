# SwagLiveRecorder

A Windows desktop application that automatically records **swag.live** live streams from a user-maintained watchlist.  When a watched model goes live in free chat, recording starts automatically.

---

## How recording works

swag.live delivers streams over **Agora RTC** (an Agora.io WebRTC gateway), not plain HLS/RTMP.  The recorder is fully native: no FFmpeg, no WebView2, no NuGet packages.

### Discovery — three REST calls against `api.swag.live`

| # | Method | Endpoint | What we get |
|---|--------|----------|-------------|
| 1 | `GET` | `/feeds/user_livestream-v2?limit=100&page=1&sorting=desc:s_score` | JSON array of live users: `[{username, id, displayName}]` |
| 2 | `GET` | `/pusher/retained-events?channels=private-enc-stream%40{userId}` | `stream.online` event → `session` ID, `preset`, `price` |
| 3 | `GET` | `/streams/{sessionId}/token` | `agora_token`, `agora_token_session_id`, `agora_exp` |

All three endpoints are unauthenticated for public/preview streams.

### Agora signaling — two more REST calls

| # | Method | Host | Path | Purpose |
|---|--------|------|------|---------|
| 4 | `POST` | `sua-ap-web-1.agora.io` | `/api/v1?action=stringuid` | Convert session UUID → numeric subscriber UID |
| 5 | `POST` | `webrtc2-ap-web-1.agora.io` | `/api/v2/transpond/webrtc?v=2` (multipart form) | Fetch edge server IPs + DTLS fingerprints |

The gateway response for step 5 contains `edges_services` (IP:port list) and `detail["19"]` (semicolon-separated SHA-256 DTLS fingerprints).

### WebRTC connection

A synthetic SDP answer is constructed from the gateway response and fed to a `libdatachannel` `PeerConnection`.  libdatachannel's built-in **`VP8RtpDepacketizer`** and **`OpusRtpDepacketizer`** handlers reassemble RTP packets into frames, which are then written to a `.webm` file by the hand-rolled **WebM/EBML muxer** (~300 lines, no external library).

#### Agora constants (from PCAP)

| Constant | Value |
|----------|-------|
| App ID | `19c9ed8fd65f4ea9b5de096362af989e` |
| Pusher App Key | `aad17fe8f682717df2c0` |
| Pusher App ID | `550591` |
| Pusher Host | `ws-ap1.pusher.com` |
| Pusher Auth | `api.swag.live/pusher/batch-authenticate` |

Output files can be played in VLC or remuxed to MP4 without re-encoding:

```
ffmpeg -i recording.webm -c copy output.mp4
```

---

## Features

- **Win32 GUI** — watchlist with columns (username, enabled, status, bytes recorded), log area, settings panel
- **Automatic monitoring** — background thread polls the swag.live API at a configurable interval (default: 60 s)
- **Free-preview gating** — only starts recording when a model is live *and* in free preview (`preset=preview`, `price=0`); stops when they switch to paid mode or go offline
- **Multiple simultaneous recordings** — each model gets its own independent PeerConnection / recording thread
- **Persistent watchlist** — stored in `models.txt` next to the executable; survives restarts
- **Auth token support** — paste your swag.live Bearer token so the API returns complete stream status data
- **No vcpkg / NuGet** — libdatachannel and all its dependencies are included as plain source in `deps/libdatachannel/`; build once with `build_deps.cmd`

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
git clone https://github.com/Zero3K20/SwagLiveRecorder.git
cd SwagLiveRecorder
build_deps.cmd
```

`build_deps.cmd` will:
1. Configure libdatachannel with CMake (uses Mbed TLS — no OpenSSL installation needed).
2. Build both **Release** and **Debug** configurations.

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
│   └── libdatachannel/            # libdatachannel source (paullouisageneau/libdatachannel @ 9ddf889)
├── build_deps.cmd                 # One-shot CMake build of libdatachannel
├── SwagLiveRecorder.vcxproj       # VS2019 project
└── SwagLiveRecorder.sln
```

---

## TLS client

The TLS client used to query the swag.live API is based on the implementation in the [Tardsplaya project](https://github.com/Zero3K/Tardsplaya/tree/main/tlsclient).  It uses Windows' built-in **WinHTTP** stack — no OpenSSL or other third-party TLS library is required for the API layer.

libdatachannel uses **Mbed TLS** (included in `deps/libdatachannel/deps/`) for its own DTLS/TLS stack.

---

## Output format

Recordings are saved as **`.webm`** files (WebM container, VP8 video, Opus audio).

