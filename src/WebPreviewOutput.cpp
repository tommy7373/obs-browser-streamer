#include "WebPreviewOutput.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>

#include <cctype>
#include <cstdio>
#include <cstring>

// -------------------------------------------------------------------------
// WebPreviewOutput
// -------------------------------------------------------------------------

WebPreviewOutput::WebPreviewOutput() {}

WebPreviewOutput::~WebPreviewOutput()
{
    if (active_)
        Stop();
}

void WebPreviewOutput::PacketInterceptCb(obs_output_t*, encoder_packet* pkt,
                                         encoder_packet_time*, void* param)
{
    auto* self = static_cast<WebPreviewOutput*>(param);
    if (self && pkt)
        self->HandlePacket(pkt);
}

bool WebPreviewOutput::Start(const std::string& sourceName,
                             const std::string& encoderId,
                             int bitrateKbps,
                             PacketCallback cb)
{
    if (active_)
        return false;

    packetCb_ = std::move(cb);

    // Reject texture-only encoders up-front — they pull from OBS's main GPU
    // pipeline and produce no output when wired to a custom video_output_t,
    // which also corrupts OBS's encoder thread for subsequent runs.
    const char* idCheck = encoderId.empty() ? "obs_x264" : encoderId.c_str();
    uint32_t encCaps = obs_get_encoder_caps(idCheck);
    if (encCaps & OBS_ENCODER_CAP_PASS_TEXTURE) {
        blog(LOG_WARNING,
             "[obs-web-preview] encoder '%s' is texture-only and cannot be used "
             "with a custom video pipeline — pick a fallback encoder instead "
             "(e.g. ffmpeg_nvenc, ffmpeg_amf, obs_qsv11_h264, obs_x264)",
             idCheck);
        return false;
    }

    source_ = obs_get_source_by_name(sourceName.c_str());
    if (!source_)
        return false;

    obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi)) {
        obs_source_release(source_);
        source_ = nullptr;
        return false;
    }
    renderWidth_  = ovi.output_width;
    renderHeight_ = ovi.output_height;

    // --- Raw video output (BGRA frames we'll fill from the GPU readback) ---
    // Name must be unique per active output, so encode the pointer.
    char nameBuf[64];
    snprintf(nameBuf, sizeof(nameBuf), "web_preview_vout_%p", (void*)this);
    videoOutputName_ = nameBuf;

    video_output_info voi = {};
    voi.format     = VIDEO_FORMAT_BGRA;
    voi.width      = renderWidth_;
    voi.height     = renderHeight_;
    voi.fps_num    = ovi.fps_num;
    voi.fps_den    = ovi.fps_den;
    voi.colorspace = ovi.colorspace;
    voi.range      = ovi.range;
    voi.name       = videoOutputName_.c_str();
    voi.cache_size = 8;  // CRITICAL — without this the video_t starts with
                          // available_frames=0 and every lock_frame() fails.
                          // OBS clamps to MAX_CACHE_SIZE internally (typically 16).
    if (video_output_open(&videoOutput_, &voi) != VIDEO_OUTPUT_SUCCESS) {
        obs_source_release(source_);
        source_ = nullptr;
        return false;
    }

    // --- Video encoder ---
    // Encoder-specific low-latency settings dispatched by encoder id.  We can't
    // just set every possible key on one obs_data_t because some keys collide
    // across encoders (e.g. `tune` is "zerolatency" for x264 but "ll" for NVENC).
    obs_data_t* venc = obs_data_create();
    obs_data_set_int(venc, "bitrate",      bitrateKbps);
    obs_data_set_int(venc, "keyint_sec",   1);
    obs_data_set_string(venc, "rate_control", "CBR");
    obs_data_set_int(venc, "bf",           0); // no B-frames for low latency

    const char* encName = encoderId.empty() ? "obs_x264" : encoderId.c_str();
    std::string idLower(encName);
    for (auto& c : idLower) c = (char)tolower((unsigned char)c);

    if (idLower.find("nvenc") != std::string::npos) {
        obs_data_set_string(venc, "preset2",   "p1");
        obs_data_set_string(venc, "tune",      "ll");
        obs_data_set_string(venc, "multipass", "disabled");
        obs_data_set_bool  (venc, "lookahead", false);
        obs_data_set_bool  (venc, "psycho_aq", false);
        obs_data_set_string(venc, "profile",   "baseline");
    } else if (idLower.find("amf") != std::string::npos) {
        obs_data_set_string(venc, "usage",   "lowlatency");
        obs_data_set_string(venc, "quality", "speed");
        obs_data_set_string(venc, "profile", "baseline");
    } else if (idLower.find("qsv") != std::string::npos) {
        obs_data_set_string(venc, "target_usage", "speed");
        obs_data_set_string(venc, "profile",      "baseline");
    } else {
        // obs_x264 / ffmpeg_x264 / unknown — assume x264-style keys
        obs_data_set_string(venc, "preset",  "veryfast");
        obs_data_set_string(venc, "profile", "baseline");
        obs_data_set_string(venc, "tune",    "zerolatency");
    }

    vidEncoder_ = obs_video_encoder_create(encName, "web_preview_venc", venc, nullptr);
    obs_data_release(venc);
    if (!vidEncoder_) {
        blog(LOG_WARNING, "[obs-web-preview] failed to create video encoder '%s'", encName);
        TeardownPipeline();
        return false;
    }
    obs_encoder_set_video(vidEncoder_, videoOutput_);

    // --- Audio encoder (ffmpeg_muxer needs an AV pair to start cleanly) ---
    obs_data_t* aenc = obs_data_create();
    obs_data_set_int(aenc, "bitrate", 128);
    audEncoder_ = obs_audio_encoder_create("ffmpeg_aac", "web_preview_aenc", aenc, 0, nullptr);
    obs_data_release(aenc);
    if (!audEncoder_) {
        TeardownPipeline();
        return false;
    }
    obs_encoder_set_audio(audEncoder_, obs_get_audio());

    // --- ffmpeg_muxer to /dev/null — we intercept packets before they hit it ---
    obs_data_t* mux = obs_data_create();
    // MPEG-TS, not MP4: NUL doesn't support seek-back for moov finalization.
    obs_data_set_string(mux, "path", "NUL.ts");
    obsOutput_ = obs_output_create("ffmpeg_muxer", "web_preview_output", mux, nullptr);
    obs_data_release(mux);
    if (!obsOutput_) {
        TeardownPipeline();
        return false;
    }

    obs_output_add_packet_callback(obsOutput_, PacketInterceptCb, this);
    obs_output_set_video_encoder(obsOutput_, vidEncoder_);
    obs_output_set_audio_encoder(obsOutput_, audEncoder_, 0);

    // --- GPU resources + render callback ---
    SetupGraphicsResources();
    obs_add_main_render_callback(RenderCallback, this);

    if (!obs_output_start(obsOutput_)) {
        const char* err = obs_output_get_last_error(obsOutput_);
        blog(LOG_WARNING, "[obs-web-preview] output start failed: %s", err ? err : "(none)");
        obs_remove_main_render_callback(RenderCallback, this);
        obs_output_remove_packet_callback(obsOutput_, PacketInterceptCb, this);
        TeardownGraphicsResources();
        TeardownPipeline();
        return false;
    }

    active_ = true;
    blog(LOG_INFO, "[obs-web-preview] streaming started: source=%s encoder=%s %dx%d @ %u/%u",
         sourceName.c_str(), encName, renderWidth_, renderHeight_, ovi.fps_num, ovi.fps_den);
    return true;
}

void WebPreviewOutput::Stop()
{
    if (!active_)
        return;

    // Order matters here.  We set active_=false so the render callback bails
    // immediately, but DO NOT remove the callback yet — obs_output_stop may
    // block waiting for the encoder to drain its remaining frames, which
    // requires the video pipeline to keep ticking.  We remove the callback
    // only after the output has fully stopped.
    active_ = false;

    if (obsOutput_) {
        obs_output_remove_packet_callback(obsOutput_, PacketInterceptCb, this);
        obs_output_stop(obsOutput_);
    }

    obs_remove_main_render_callback(RenderCallback, this);

    TeardownGraphicsResources();
    TeardownPipeline();
    blog(LOG_INFO, "[obs-web-preview] streaming stopped");
}

// -------------------------------------------------------------------------
// Render callback — runs on the OBS graphics thread every frame
// -------------------------------------------------------------------------

void WebPreviewOutput::RenderCallback(void* param, uint32_t /*cx*/, uint32_t /*cy*/)
{
    static_cast<WebPreviewOutput*>(param)->DoRender();
}

void WebPreviewOutput::DoRender()
{
    if (!active_ || !source_ || !texrender_ || !stagesurface_ || !videoOutput_)
        return;

    uint32_t w = renderWidth_;
    uint32_t h = renderHeight_;

    // Source's natural size — for a scene this is OBS's base canvas dimensions
    // (e.g. 1440p), not the output dimensions.  We set gs_ortho to this size
    // so the source renders in its own coordinate space, while the texrender
    // framebuffer is at the encode resolution (e.g. 1080p) — the viewport
    // transform between the two performs the scale-down.
    uint32_t srcW = obs_source_get_width(source_);
    uint32_t srcH = obs_source_get_height(source_);
    if (srcW == 0 || srcH == 0) { srcW = w; srcH = h; }

    // Render source to off-screen texture
    gs_texrender_reset(texrender_);
    if (!gs_texrender_begin(texrender_, w, h))
        return;

    struct vec4 bg;
    vec4_zero(&bg);
    gs_clear(GS_CLEAR_COLOR, &bg, 1.0f, 0);
    gs_ortho(0.0f, (float)srcW, 0.0f, (float)srcH, -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

    obs_source_video_render(source_);

    gs_blend_state_pop();
    gs_texrender_end(texrender_);

    // Download GPU texture → CPU
    gs_texture_t* tex = gs_texrender_get_texture(texrender_);
    if (!tex)
        return;
    gs_stage_texture(stagesurface_, tex);

    uint8_t*  data     = nullptr;
    uint32_t  linesize = 0;
    if (!gs_stagesurface_map(stagesurface_, &data, &linesize))
        return;

    struct video_frame frame = {};
    uint64_t ts = os_gettime_ns();
    if (video_output_lock_frame(videoOutput_, &frame, 1, ts)) {
        if (frame.data[0]) {
            uint8_t* dst = frame.data[0];
            uint8_t* src = data;
            uint32_t dst_ls = frame.linesize[0];
            for (uint32_t row = 0; row < h; row++) {
                memcpy(dst, src, w * 4);
                dst += dst_ls;
                src += linesize;
            }
        }
        video_output_unlock_frame(videoOutput_);
    }

    gs_stagesurface_unmap(stagesurface_);
}

// -------------------------------------------------------------------------
// SPS/PPS helpers (unchanged from previous canvas-based implementation)
// -------------------------------------------------------------------------

static bool PacketHasParameterSets(const uint8_t* d, size_t size)
{
    uint8_t nalType = 0;
    if (size >= 5 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1)
        nalType = d[4] & 0x1F;
    else if (size >= 4 && d[0]==0 && d[1]==0 && d[2]==1)
        nalType = d[3] & 0x1F;
    return nalType == 7 || nalType == 8; // SPS or PPS
}

static std::vector<uint8_t> ExtraDataToAnnexB(const uint8_t* data, size_t size)
{
    if (size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 0 || data[2] == 1))
        return std::vector<uint8_t>(data, data + size);

    if (size < 7 || data[0] != 1)
        return {};

    static const uint8_t kSC[4] = {0, 0, 0, 1};
    std::vector<uint8_t> out;

    size_t off = 5;
    uint8_t numSPS = data[off++] & 0x1F;
    for (uint8_t i = 0; i < numSPS && off + 2 <= size; ++i) {
        uint16_t len = (uint16_t(data[off]) << 8) | data[off + 1];
        off += 2;
        if (off + len > size) break;
        out.insert(out.end(), kSC, kSC + 4);
        out.insert(out.end(), data + off, data + off + len);
        off += len;
    }

    if (off < size) {
        uint8_t numPPS = data[off++];
        for (uint8_t i = 0; i < numPPS && off + 2 <= size; ++i) {
            uint16_t len = (uint16_t(data[off]) << 8) | data[off + 1];
            off += 2;
            if (off + len > size) break;
            out.insert(out.end(), kSC, kSC + 4);
            out.insert(out.end(), data + off, data + off + len);
            off += len;
        }
    }

    return out;
}

void WebPreviewOutput::HandlePacket(encoder_packet* pkt)
{
    if (!active_ || !pkt || pkt->type != OBS_ENCODER_VIDEO || !packetCb_)
        return;

    if (pkt->keyframe) {
        if (extraData_.empty() && vidEncoder_) {
            uint8_t* ed = nullptr;
            size_t   edSize = 0;
            if (obs_encoder_get_extra_data(vidEncoder_, &ed, &edSize) && ed && edSize > 0)
                extraData_ = ExtraDataToAnnexB(ed, edSize);
        }

        const auto* d = reinterpret_cast<const uint8_t*>(pkt->data);
        if (!extraData_.empty() && !PacketHasParameterSets(d, pkt->size)) {
            kfBuffer_.clear();
            kfBuffer_.insert(kfBuffer_.end(), extraData_.begin(), extraData_.end());
            kfBuffer_.insert(kfBuffer_.end(), d, d + pkt->size);
            encoder_packet patched = *pkt;
            patched.data = kfBuffer_.data();
            patched.size = kfBuffer_.size();
            packetCb_(&patched);
            return;
        }
    }

    packetCb_(pkt);
}

// -------------------------------------------------------------------------
// Graphics + pipeline teardown
// -------------------------------------------------------------------------

void WebPreviewOutput::SetupGraphicsResources()
{
    obs_enter_graphics();
    texrender_    = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    stagesurface_ = gs_stagesurface_create(renderWidth_, renderHeight_, GS_BGRA);
    obs_leave_graphics();
}

void WebPreviewOutput::TeardownGraphicsResources()
{
    obs_enter_graphics();
    if (texrender_) {
        gs_texrender_destroy(texrender_);
        texrender_ = nullptr;
    }
    if (stagesurface_) {
        gs_stagesurface_destroy(stagesurface_);
        stagesurface_ = nullptr;
    }
    obs_leave_graphics();
}

void WebPreviewOutput::TeardownPipeline()
{
    extraData_.clear();
    kfBuffer_.clear();

    if (obsOutput_) {
        obs_output_release(obsOutput_);
        obsOutput_ = nullptr;
    }
    if (audEncoder_) {
        obs_encoder_release(audEncoder_);
        audEncoder_ = nullptr;
    }
    if (vidEncoder_) {
        obs_encoder_release(vidEncoder_);
        vidEncoder_ = nullptr;
    }
    if (videoOutput_) {
        video_output_close(videoOutput_);
        videoOutput_ = nullptr;
    }
    if (source_) {
        obs_source_release(source_);
        source_ = nullptr;
    }
}
