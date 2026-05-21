# Browser Streamer — OBS Plugin

An OBS Studio plugin for Windows that streams any scene or source to browsers on your local network via WebRTC. No external software, no Node.js, no separate signaling server — everything runs inside OBS.

## Features

- Stream up to **two independent scenes/sources** simultaneously
- **Landing page** at one URL — viewers open it and tap the stream they want
- Works on any LAN device with a modern browser (phone, tablet, laptop, TV)
- H.264 video at adjustable bitrate
- Browser viewer has **rotation** and **fullscreen** controls
- Fully self-contained OBS plugin

## Requirements

- **Windows 10/11** (64-bit) — Windows only for now
- **OBS Studio 31.0** or later
- Modern browser on the viewing device: Chrome, Firefox, Edge, Safari (iOS 15+)
- Both OBS machine and viewer device on the same network

## Installation

1. Download the latest release zip from the [Releases](../../releases) page
2. Extract into `C:\ProgramData\obs-studio\plugins\` so the layout is:
   ```
   C:\ProgramData\obs-studio\plugins\obs-web-preview\
     bin\64bit\obs-web-preview.dll
     bin\64bit\datachannel.dll
     data\locale\en-US.ini
     data\web\index.html
     data\web\viewer.html
   ```
3. Restart OBS

## Usage

1. In OBS open **View → Docks → Browser Streamer**
2. For each stream you want to use:
   - Pick a **Source** (scene or capture)
   - Set **Bitrate** (default 2500 kbps; lower for slow Wi-Fi)
3. Set the **Port** (default 8080; only one port is needed for both streams)
4. Click **Start Stream** on whichever streams you want active
5. The URL(s) shown in the dock are the landing page addresses — open any of them on a viewing device
6. The landing page lists active streams; tap one to open the full-screen viewer

The viewer page has:
- **⬅** — back to the landing page
- **⟳** — rotate the video 90° (useful for portrait sources on landscape screens)
- **⛶** — toggle fullscreen

## Building from Source

### Prerequisites

- Visual Studio 2022 or later with the **Desktop development with C++** workload
- CMake 3.24+
- OBS plugin dev dependencies (see below)

### Getting the OBS plugin dev dependencies

Download the prebuilt dependency archives from the [obs-deps releases page](https://github.com/obsproject/obs-deps/releases) — you need both the main archive and the Qt6 archive for your date/version:

```
obs-deps-YYYY-MM-DD-x64.zip        → extract to C:\obs-plugin-work\.deps\obs-deps-YYYY-MM-DD-x64\
obs-deps-qt6-YYYY-MM-DD-x64.zip    → extract to C:\obs-plugin-work\.deps\obs-deps-qt6-YYYY-MM-DD-x64\
```

libdatachannel is included in the main obs-deps archive — no separate download needed.

### Configure

```powershell
cmake -B build_x64 `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/obs-plugin-work/.deps/obs-deps-YYYY-MM-DD-x64;C:/obs-plugin-work/.deps/obs-deps-qt6-YYYY-MM-DD-x64;C:/obs-plugin-work/.deps"
```

Adjust the paths to match where you extracted the deps.

### Build and install

```powershell
cmake --build build_x64 --config RelWithDebInfo
cmake --install build_x64 --config RelWithDebInfo
```

The install step copies the DLLs and data files directly into your OBS plugin directory.

### Third-party dependencies

| Dependency | How included |
|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | Vendored single header at `third-party/httplib.h` |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) | Prebuilt, sourced from OBS deps package |

## How It Works

```
OBS Source/Scene
  → obs_canvas_t  (ACTIVATE | EPHEMERAL)
  → obs_x264 encoder
  → ffmpeg_muxer output  (routed to NUL — no disk use)
      + obs_output_add_packet_callback  ← intercepts H.264 NAL units
  → H264RtpPacketizer  (libdatachannel)
  → rtc::PeerConnection per browser viewer
  → Browser <video> element
```

Signaling uses a simple HTTP POST exchange: the browser creates a WebRTC offer and POSTs it to `/offer` (or `/offer2` for stream 2); the plugin creates the answer, completes ICE gathering, and returns it. No trickle ICE — one round trip.

The plugin also serves the landing page (`/`) and viewer page (`/1`, `/2`) from an embedded cpp-httplib server on the configured port.

## Known Limitations

- **Windows only** — uses Windows-specific network APIs
- **LAN only** — no STUN/TURN; viewers must be on the same network
- **Video only** — audio is not streamed (OBS outputs AAC; WebRTC requires Opus — transcoding not yet implemented)
- Maximum 2 simultaneous streams

## License

MIT — see [LICENSE](LICENSE)
