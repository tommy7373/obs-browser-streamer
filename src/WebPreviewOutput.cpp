#include "WebPreviewOutput.hpp"

#include <obs-module.h>

#include <cctype>
#include <cstring>

// -------------------------------------------------------------------------
// Null output type — hosts the encoder pipeline without writing anywhere.
// Encoder packets reach us via obs_output_add_packet_callback. Replaces
// ffmpeg_muxer, which required writing a real file and added disk I/O on
// the encoder thread (potential source of backpressure-driven stutter).
// -------------------------------------------------------------------------

static const char* null_output_get_name(void*)
{
    return "obs-web-preview Null Output";
}

static void* null_output_create(obs_data_t*, obs_output_t* output)
{
    return output;
}

static void null_output_destroy(void*) {}

static bool null_output_start(void* data)
{
    auto* output = static_cast<obs_output_t*>(data);
    if (!obs_output_can_begin_data_capture(output, 0))
        return false;
    if (!obs_output_initialize_encoders(output, 0))
        return false;
    return obs_output_begin_data_capture(output, 0);
}

static void null_output_stop(void* data, uint64_t)
{
    auto* output = static_cast<obs_output_t*>(data);
    obs_output_end_data_capture(output);
}

static void null_output_packet(void*, encoder_packet*) {}

static struct obs_output_info null_output_info = {
    /* id                    */ "obs_web_preview_null_output",
    /* flags                 */ OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,
    /* get_name              */ null_output_get_name,
    /* create                */ null_output_create,
    /* destroy               */ null_output_destroy,
    /* start                 */ null_output_start,
    /* stop                  */ null_output_stop,
    /* raw_video             */ nullptr,
    /* raw_audio             */ nullptr,
    /* encoded_packet        */ null_output_packet,
    /* update                */ nullptr,
    /* get_defaults          */ nullptr,
    /* get_properties        */ nullptr,
    /* unused1               */ nullptr,
    /* get_total_bytes       */ nullptr,
    /* get_dropped_frames    */ nullptr,
    /* type_data             */ nullptr,
    /* free_type_data        */ nullptr,
    /* get_congestion        */ nullptr,
    /* get_connect_time_ms   */ nullptr,
    /* encoded_video_codecs  */ "h264",
    /* encoded_audio_codecs  */ "opus",
    /* raw_audio2            */ nullptr,
    /* protocols             */ nullptr,
};

void WebPreviewOutput::RegisterOutputType()
{
    obs_register_output(&null_output_info);
}

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

    source_ = obs_get_source_by_name(sourceName.c_str());
    if (!source_)
        return false;

    obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi)) {
        obs_source_release(source_);
        source_ = nullptr;
        return false;
    }

    // --- obs_view_t-backed video pipeline ---
    // The view shares OBS's main canvas video clock and graphics thread, which
    // means encoders (including obs_nvenc_* and texture-path encoders) treat
    // it as canvas-equivalent. This replaces the previous video_output_open +
    // main_render_callback path, which had subtle timing-drift issues until
    // obs_reset_video() ran.
    //
    // Base dimensions track the source's natural size so the source fills the
    // view's base canvas; output dimensions follow the OBS scaled-resolution
    // setting (typical canvas downscale path).
    uint32_t srcW = obs_source_get_width(source_);
    uint32_t srcH = obs_source_get_height(source_);
    if (srcW == 0 || srcH == 0) {
        srcW = ovi.base_width;
        srcH = ovi.base_height;
    }
    ovi.base_width    = srcW;
    ovi.base_height   = srcH;
    // Cap output size at base size — there's no point upscaling on the
    // encoder, the source's native pixels are all we have.
    if (ovi.output_width  > srcW) ovi.output_width  = srcW;
    if (ovi.output_height > srcH) ovi.output_height = srcH;
    renderWidth_  = ovi.output_width;
    renderHeight_ = ovi.output_height;

    view_ = obs_view_create();
    if (!view_) {
        obs_source_release(source_);
        source_ = nullptr;
        return false;
    }
    obs_view_set_source(view_, 0, source_);
    viewVideo_ = obs_view_add2(view_, &ovi);
    if (!viewVideo_) {
        blog(LOG_WARNING, "[obs-web-preview] obs_view_add2 returned null");
        obs_view_destroy(view_);
        view_ = nullptr;
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
    obs_encoder_set_video(vidEncoder_, viewVideo_);

    // --- Audio encoder — Opus is the only audio codec WebRTC requires every
    // browser to support. ffmpeg_opus is bundled with OBS via obs-ffmpeg.
    // Enable in-band FEC: Opus can reconstruct a lost packet using redundancy
    // carried in the next packet, which is exactly the right tool for the
    // occasional WiFi drop that NACK can't recover in time (audio has a much
    // tighter latency budget than video).
    obs_data_t* aenc = obs_data_create();
    obs_data_set_int(aenc, "bitrate", 128);
    obs_data_set_int(aenc, "packet_loss", 10); // tells libopus to budget FEC for ~10% loss
    audEncoder_ = obs_audio_encoder_create("ffmpeg_opus", "web_preview_aenc", aenc, 0, nullptr);
    obs_data_release(aenc);
    if (!audEncoder_) {
        TeardownPipeline();
        return false;
    }
    obs_encoder_set_audio(audEncoder_, obs_get_audio());

    // --- Null output — hosts the encoder pipeline, writes nothing. We pull
    // packets via obs_output_add_packet_callback below.
    obsOutput_ = obs_output_create("obs_web_preview_null_output",
                                   "web_preview_output", nullptr, nullptr);
    if (!obsOutput_) {
        TeardownPipeline();
        return false;
    }

    obs_output_add_packet_callback(obsOutput_, PacketInterceptCb, this);
    obs_output_set_video_encoder(obsOutput_, vidEncoder_);
    obs_output_set_audio_encoder(obsOutput_, audEncoder_, 0);

    if (!obs_output_start(obsOutput_)) {
        const char* err = obs_output_get_last_error(obsOutput_);
        blog(LOG_WARNING, "[obs-web-preview] output start failed: %s", err ? err : "(none)");
        obs_output_remove_packet_callback(obsOutput_, PacketInterceptCb, this);
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

    active_ = false;

    if (obsOutput_) {
        obs_output_remove_packet_callback(obsOutput_, PacketInterceptCb, this);
        obs_output_stop(obsOutput_);
    }

    TeardownPipeline();
    blog(LOG_INFO, "[obs-web-preview] streaming stopped");
}

// -------------------------------------------------------------------------
// SPS/PPS helpers
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
    if (!active_ || !pkt || !packetCb_)
        return;

    // Audio packets pass through unmodified — the WebRTC layer wraps them in
    // RTP. Only video keyframes need the SPS/PPS prepending below.
    if (pkt->type != OBS_ENCODER_VIDEO) {
        packetCb_(pkt);
        return;
    }

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
// Pipeline teardown
// -------------------------------------------------------------------------

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
    if (view_) {
        // obs_view_remove unhooks the view's video_t from the main render
        // loop. After this returns, no more frames are produced.
        obs_view_remove(view_);
        viewVideo_ = nullptr;
        obs_view_destroy(view_);
        view_ = nullptr;
    }
    if (source_) {
        obs_source_release(source_);
        source_ = nullptr;
    }
}
