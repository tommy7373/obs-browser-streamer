# Browser Streamer — OBS Plugin

An OBS Studio plugin for Windows that streams any scene or source to browsers on your local network via WebRTC. No external software, no Node.js, no separate signaling server — everything runs inside OBS.

## Features

- Up to **8 independent streams** simultaneously, each with its own source, encoder, and bitrate
- **Hardware encoder support** — software x264, NVENC (CPU + texture paths), AMF (CPU + texture paths), Intel QSV — any H.264 encoder OBS exposes
- **Synchronized audio + video** — full OBS audio mix encoded as Opus and delivered alongside the video on the same WebRTC PeerConnection
- **Landing page** at one URL — viewers open it and tap the stream they want
- Works on Chrome, Firefox, Edge, and Safari on any LAN device (phone, tablet, laptop, TV)
- **Loss-tolerant**: RTCP NACK retransmission, PLI keyframe recovery, and a small browser-side jitter buffer
- **New-viewer fast start**: each stream caches its latest keyframe and delivers it immediately on connect, so first frame appears in milliseconds instead of waiting for the next IDR
- **Live stats overlay** in the viewer — press `s` or tap 📊 to see decode FPS, bitrate, loss, jitter, freeze count, audio concealment, and more
- **Auto-fading viewer chrome** — controls and status pill fade out after ~3s of inactivity so the video has the whole screen
- Mute/unmute toggle (browsers block autoplay with audio; audio starts muted, tap 🔇 to enable)

## Requirements

- **Windows 10/11** (64-bit) — Windows only for now
- **OBS Studio 31.0** or later (including 32.x)
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
- **🔇 / 🔊** — unmute / mute audio (starts muted because browsers require a user gesture for audio autoplay)
- **📊** — toggle the live stats overlay (or press `s`)

Controls auto-fade after ~3 seconds of inactivity. Any pointer movement, tap, or key brings them back.

### Choosing an encoder

The Encoder dropdown lists every non-deprecated H.264 encoder OBS has registered, including the texture-path variants:

| Encoder id | When to use |
|---|---|
| `obs_x264` | Software encoder; works everywhere, uses CPU. Fine at 720p; 1080p60 saturates many CPUs |
| `obs_nvenc_h264_tex` / `obs_nvenc_h264_soft` | NVIDIA GPUs — texture path is preferred; both work since the obs_view_t refactor |
| `h264_texture_amf` / `ffmpeg_amf` | AMD GPUs — uses AMF/VCN hardware encoder |
| `obs_qsv11_h264` | Intel CPUs with iGPU — uses QuickSync |

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

The main obs-deps archive bundles everything the plugin needs at runtime: libdatachannel, libavcodec/libavutil (for the Opus encoder), libopus, libsrtp, MbedTLS, etc. No separate downloads.

### Configure, build, install

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --config RelWithDebInfo
```

A `CMakeUserPresets.json` setting `CMAKE_PREFIX_PATH` to the three deps directories (plus the OBS install dir for libobs/obs-frontend-api) is the easiest way to configure paths.

The install step copies the DLLs and data files directly into your OBS plugin directory.

### Third-party dependencies

| Dependency | How included |
|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | Vendored single header at `third-party/httplib.h` |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) | Prebuilt, sourced from OBS deps package |
| [FFmpeg libavcodec/libavutil](https://ffmpeg.org/) | Prebuilt, sourced from OBS deps package (for inline Opus encoding) |

## How It Works

### Video pipeline

```
Selected source/scene
  → obs_view_create + obs_view_set_source
  → obs_view_add2 (returns a canvas-equivalent video_t)
  → user-selected encoder (any registered H.264 encoder, incl. texture-path)
  → custom null obs_output_t  (no disk/network IO; satisfies OBS's pipeline)
      + obs_output_add_packet_callback intercepts H.264 NAL units
  → SPS/PPS prepended if needed (Annex-B conversion from AVCC extra_data)
  → H264RtpPacketizer + RtcpSrReporter + RtcpNackResponder + PliHandler
  → rtc::PeerConnection per browser viewer
  → Browser <video> element
```

Using `obs_view_t` means the encoder gets a canvas-equivalent `video_t` driven by OBS's main render loop and graphics-thread clock — same timing infrastructure the main canvas uses. That's why texture-path encoders (like the rewritten obs-nvenc in OBS 31+) work here despite running per-source.

### Audio pipeline (independent of OBS's encoder infrastructure)

```
OBS audio mix
  → audio_output_connect (interleaved float, captured into a 200ms ring buffer)
  → dedicated worker thread, pulls EXACTLY 960 samples (20ms) per iteration
  → wallclock-strict sleep until the next 20ms boundary
  → libavcodec libopus encoder (CBR, 20ms frames, DTX off, FEC off)
  → OpusRtpPacketizer + RtcpSrReporter
  → same rtc::PeerConnection as video, second track
  → Browser <audio>-on-<video>-element (same MediaStream, lip-synced)
```

The audio path runs entirely outside OBS's encoder pipeline. OBS's bundled `ffmpeg_opus` encoder delivered packets in bursts of 60-165ms wallclock gaps, which caused the receiver's WebRTC jitter buffer to repeatedly destabilize. Driving libavcodec ourselves with a strict 20ms wallclock cadence eliminates the bursty delivery as a variable and the audio is rock-solid.

### Signaling

A simple HTTP POST exchange: the browser creates a WebRTC offer with `recvonly` video + audio transceivers and POSTs it to `/offer/N`; the plugin creates the answer, completes ICE gathering, and returns it. No trickle ICE — one round trip.

The plugin's answer SDP mirrors the browser's offer for both m-sections' `mid` values, picks an H.264 PT with `packetization-mode=1` from the offered codecs, mirrors the `profile-level-id`, picks the offered Opus PT, advertises feedback (`nack`/`nack pli`/`ccm fir`/`goog-remb` for video), and includes `a=ssrc` + `a=msid` lines so the browser groups both tracks into one MediaStream.

### Loss recovery

- **NACK** (video) — the browser asks for specific lost RTP packets by sequence number; `RtcpNackResponder` keeps a rolling 1024-packet buffer and retransmits them
- **PLI** — when too many packets are lost to recover with NACK, the browser sends a Picture Loss Indication and we hold P-frames until the next keyframe (`keyint_sec=1`)
- **Keyframe cache** — when a new viewer connects, the cached latest IDR is sent immediately on the WebRTC track opening, instead of waiting up to a second for the next encoder keyframe
- **playoutDelayHint = 0.1** — the viewer asks the browser to maintain a ~100 ms jitter buffer; Firefox needs this to avoid stuttering on decode timing variance

### Embedded HTTP server

cpp-httplib serves the landing page (`/`), the viewer page (`/1` through `/N`), and handles the `/offer/N` POST endpoints on the configured port.

## Known Limitations

- **Windows only** — uses Windows-specific network APIs (`GetAdaptersInfo`) and audio handling
- **LAN only** — no STUN/TURN configured; viewers must be on the same network
- **Audio is the full OBS mix** — there's no per-source audio routing; all sources audible in OBS are audible to viewers

## License

MIT — see [LICENSE](LICENSE)
