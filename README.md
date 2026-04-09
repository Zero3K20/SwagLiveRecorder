# SwagLiveRecorder

A C++ console application that monitors [swag.live](https://swag.live/livestream)
live streams and automatically records watched models when they go live in free chat.

## Features

- **Watchlist management** – add, remove, enable or disable models from a
  persistent text-file watchlist.
- **Automatic monitoring** – polls the swag.live API at a configurable interval
  and detects when a watched model goes live in free chat.
- **Automatic recording** – launches `ffmpeg` to capture the stream to an MPEG-TS
  file as soon as a model starts a free-chat session.
- **Graceful stop** – recording is stopped automatically when the model ends her
  free-chat session or goes offline.
- **TLS networking** – all HTTPS communication uses the lightweight TLS client
  from the [Tardsplaya](https://github.com/Zero3K/Tardsplaya/tree/main/tlsclient)
  project (included in `tlsclient/`).

## Build requirements

| Requirement | Version |
|---|---|
| Visual Studio | 2019 (toolset v142) |
| Windows SDK | 10.0 or later |
| C++ standard | C++17 (`/std:c++17`) |
| ffmpeg | 4.x or later (runtime, not compile-time) |

## Building

1. Open `SwagLiveRecorder.sln` in **Visual Studio 2019**.
2. Select the desired configuration (`Debug|x64` or `Release|x64`).
3. Build the solution (**Build → Build Solution** or <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>B</kbd>).

The resulting `SwagLiveRecorder.exe` will be in `x64\Release\` (or the
corresponding configuration folder).

## Runtime dependency – ffmpeg

The recorder uses `ffmpeg` to capture and write the stream.  Download a static
build from <https://ffmpeg.org/download.html> and either:

* Place `ffmpeg.exe` in the same directory as `SwagLiveRecorder.exe`, **or**
* Add the folder containing `ffmpeg.exe` to your `PATH`, **or**
* Pass the full path via `--ffmpeg <path>`.

## Usage

```
SwagLiveRecorder.exe [options]

Options:
  --list               Print current watchlist
  --add <username>     Add model to watchlist
  --remove <username>  Remove model from watchlist
  --enable <username>  Enable auto-record for model
  --disable <username> Disable auto-record for model
  --output <dir>       Recording output directory (default: recordings)
  --ffmpeg <path>      Path to ffmpeg.exe
  --models <file>      Watchlist file path (default: models.txt)
  --interval <sec>     Poll interval in seconds (default: 60)
  --token <token>      swag.live auth token (if required by the API)
  --run                Start the monitoring loop
  --help               Show this help message
```

### Quick-start example

```cmd
:: Add two models to the watchlist
SwagLiveRecorder.exe --add modelname1
SwagLiveRecorder.exe --add modelname2

:: Start monitoring – recordings saved to D:\SwagRecordings
SwagLiveRecorder.exe --run --output D:\SwagRecordings --interval 30
```

Press **Ctrl+C** to stop the monitor and gracefully terminate all active recordings.

## Watchlist file format

The watchlist is a plain text file (default: `models.txt`).  Each line has the
format:

```
# Comments start with #
username,1        # enabled
otherusername,0   # disabled
```

You can edit the file by hand or use the `--add` / `--remove` / `--enable` /
`--disable` commands.

## How recording works

1. The program polls the swag.live API (via HTTPS/TLS) to check whether each
   watched model is live and in free-chat mode.
2. When a model is detected as live and in free chat, the program tries to
   locate an HLS playlist URL for the stream (common CDN patterns are tried
   automatically).
3. `ffmpeg` is launched with the HLS URL as input and writes an MPEG-TS file to
   the output directory.  Filenames include the model's username and a timestamp,
   e.g. `modelname_20240315_213045.ts`.
4. Recording stops when the model leaves free chat, goes offline, or Ctrl+C is
   pressed.

> **Note on WebRTC:** swag.live delivers its streams via WebRTC.  Many platforms
> also publish HLS/DASH variants through their CDN for compatibility, and the
> recorder attempts those URLs automatically.  If no HLS URL is found, a warning
> is printed.  In that case you can try supplying the HLS URL manually to ffmpeg,
> or use a browser extension to intercept the stream URL.

## Project structure

```
SwagLiveRecorder/
├── SwagLiveRecorder.cpp    Main entry point, argument parsing, monitor loop
├── SwagLiveAPI.h/.cpp      HTTPS client for the swag.live API
├── ModelList.h/.cpp        Persistent model watchlist
├── Recorder.h/.cpp         ffmpeg-based stream recorder
├── json_minimal.h          Minimal JSON parser (header-only)
├── chunked_decode.h        HTTP chunked-transfer decoder (header-only)
├── tlsclient/
│   ├── tlsclient.h         TLS/WinHTTP wrapper (public API)
│   └── tlsclient.cpp       TLS/WinHTTP wrapper (implementation)
├── SwagLiveRecorder.vcxproj  Visual Studio 2019 project
└── SwagLiveRecorder.sln      Visual Studio 2019 solution
```

## License

This project is provided as-is for educational purposes.  The TLS client code
in `tlsclient/` is from the
[Tardsplaya](https://github.com/Zero3K/Tardsplaya) project.
