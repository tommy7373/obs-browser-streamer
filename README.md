# Browser Streamer — OBS Plugin

An OBS Studio plugin for Windows that streams any scene or source to browsers on your local network via WebRTC. No external software, no Node.js, no separate signaling server — everything runs inside OBS.

## Features

- Up to **8 independent streams** simultaneously, each with its own source, encoder, and bitrate
- **Hardware encoder support** — pick `obs_x264` (software), NVENC, AMF, or QSV per stream to offload encoding from the CPU
- **Landing page** at one URL — viewers open it and tap the stream they want
- Works on Chrome, Firefox, Edge, and Safari on any LAN device (phone, tablet, laptop, TV)
- **Loss-tolerant**: RTCP NACK retransmission, PLI keyframe recovery, and a small browser-side jitter buffer for smooth playback on flaky Wi-Fi
- **New-viewer fast start**: each stream caches its latest keyframe and delivers it immediately on connect, so first frame appears in milliseconds instead of waiting up to a second for the next IDR
- Adjustable bitrate per stream (up to 50 Mbps)
- Browser viewer has rotation and fullscreen controls

## Requirements

- **Windows 10/11** (64-bit) — Windows only for now
- **OBS Studio 31.0** or later
- Modern browser on the viewing device: Chrome 90+, Firefox 90+, Edge, Safari (iOS 15+)
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
2. Click **Settings** to configure:
   - **Port** — HTTP server port (default 8080)
   - **Streams** — number of streams to expose (1–8)
   - For each stream: **Source** (scene or capture), **Encoder**, and **Bitrate**
3. Click **Start** on whichever streams you want active
4. Open any of the URLs shown in the dock on a viewing device
5. The landing page lists active streams; tap one to open the full-screen viewer

The viewer page has:
- **⬅** — back to the landing page
- **⟳** — rotate the video 90° (useful for portrait sources on landscape screens)
- **⛶** — toggle fullscreen

### Choosing an encoder

The Encoder dropdown lists every non-deprecated, non-texture-only H.264 encoder OBS has registered:

| Encoder id | When to use |
|---|---|
| `obs_x264` | Software encoder; works everywhere, uses CPU |
| `ffmpeg_nvenc` | NVIDIA GPUs — offloads encoding to the GPU's NVENC silicon |
| `ffmpeg_amf` / `h264_amf` | AMD GPUs — uses AMF/VCN hardware encoder |
| `obs_qsv11_h264` | Intel CPUs with iGPU — uses QuickSync |

Texture-only encoders (e.g. `jim_nvenc`) are deliberately hidden — they pull GPU textures directly from OBS's main video pipeline and can't be wired into the plugin's per-stream isolated pipeline.

## Building from Source

### Prerequisites

- Visual Studio 2022 (or 2026) with the **Desktop development with C++** workload
- CMake 3.24+
- OBS plugin dev dependencies (see below)

### Getting the OBS plugin dev dependencies

Download the prebuilt dependency archives from the [obs-deps releases page](https://github.com/obsproject/obs-deps/releases) — you need both the main archive and the Qt6 archive for your date/version:

```
obs-deps-YYYY-MM-DD-x64.zip        → extract to C:\obs-plugin-work\.deps\obs-deps-YYYY-MM-DD-x64\
obs-deps-qt6-YYYY-MM-DD-x64.zip    → extract to C:\obs-plugin-work\.deps\obs-deps-qt6-YYYY-MM-DD-x64\
```

libdatachannel is included in the main obs-deps archive — no separate download needed.

### Configure, build, install

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --config RelWithDebInfo
```

A `CMakeUserPresets.json` setting `CMAKE_PREFIX_PATH` to the three deps directories is the easiest way to configure paths.

The install step copies the DLLs and data files directly into your OBS plugin directory.

### Third-party dependencies

| Dependency | How included |
|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | Vendored single header at `third-party/httplib.h` |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) | Prebuilt, sourced from OBS deps package |

## How It Works

```
OBS Source/Scene
  → obs_add_main_render_callback fires every frame (GPU thread)
  → gs_texrender renders the source to an off-screen texture
  → gs_stagesurface downloads it to CPU memory (BGRA)
  → video_output_lock_frame pushes the frame into a per-stream video_t
  → user-selected encoder (obs_x264, ffmpeg_nvenc, ffmpeg_amf, obs_qsv11_h264)
  → ffmpeg_muxer (routed to NUL — no disk use)
      + obs_output_add_packet_callback intercepts H.264 NAL units
  → SPS/PPS prepended if needed (Annex-B conversion for AVCC extra_data)
  → H264RtpPacketizer + RtcpSrReporter + RtcpNackResponder + PliHandler  (libdatachannel)
  → rtc::PeerConnection per browser viewer
  → Browser <video> element
```

Each stream has its own isolated video pipeline (`video_t` + GPU readback), which is why texture-only encoders that pull from OBS's main canvas don't work here.

### Signaling

A simple HTTP POST exchange: the browser creates a WebRTC offer and POSTs it to `/offer/N`; the plugin creates the answer, completes ICE gathering, and returns it. No trickle ICE — one round trip.

The plugin's answer SDP mirrors the browser's offer for the video m-section's `mid`, picks an H.264 PT with `packetization-mode=1` from the offered codecs, mirrors the `profile-level-id`, advertises `nack`/`nack pli`/`ccm fir`/`goog-remb` feedback, and includes `a=ssrc` + `a=msid` lines. All of this is required for Chrome (Unified Plan) to actually set up its inbound video stream.

### Loss recovery

- **NACK** — the browser asks for specific lost RTP packets by sequence number; `RtcpNackResponder` keeps a rolling buffer of the last 1024 packets and retransmits them. Recovers from short bursts of loss without any visible glitch.
- **PLI** — when too many packets are lost to recover with NACK, the browser sends a Picture Loss Indication and we hold P-frames until the next keyframe (`keyint_sec=1`).
- **Keyframe cache** — when a new viewer connects, the cached latest IDR is sent immediately on the WebRTC track opening, instead of waiting up to a second for the next encoder keyframe.
- **playoutDelayHint = 0.1** — the viewer asks the browser to maintain a small ~100 ms jitter buffer; Firefox needs this to avoid stuttering on decode timing variance, Chrome is happy with 0 either way.

### Embedded HTTP server

cpp-httplib serves the landing page (`/`), the viewer page (`/1` through `/N`), and handles the `/offer/N` POST endpoints on the configured port.

## Known Limitations

- **Windows only** — uses Windows-specific network APIs (`GetAdaptersInfo`)
- **LAN only** — no STUN/TURN configured; viewers must be on the same network
- **Video only** — audio is not streamed (OBS outputs AAC; WebRTC requires Opus — transcoding not implemented)
- **No texture encoders** — only fallback (CPU-path) hardware encoders can be selected; this is a fundamental constraint of running per-stream isolated video pipelines

## License

MIT — see [LICENSE](LICENSE)
