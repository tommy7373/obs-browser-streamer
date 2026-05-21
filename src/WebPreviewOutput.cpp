#include "WebPreviewOutput.hpp"
#include <obs-module.h>
#include <media-io/video-io.h>

#include <cstdio>

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

bool WebPreviewOutput::Start(const std::string& sourceName, int bitrateKbps, PacketCallback cb)
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

    // Canvas renders the selected source into its own video pipeline
    canvas_ = obs_canvas_create_private("web_preview_canvas", &ovi, ACTIVATE | EPHEMERAL);
    if (!canvas_) {
        obs_source_release(source_);
        source_ = nullptr;
        return false;
    }
    obs_canvas_set_channel(canvas_, 0, source_);

    // Video encoder
    obs_data_t* venc = obs_data_create();
    obs_data_set_int(venc, "bitrate", bitrateKbps);
    obs_data_set_string(venc, "preset", "veryfast");
    obs_data_set_string(venc, "profile", "baseline");
    obs_data_set_string(venc, "tune", "zerolatency");
    obs_data_set_int(venc, "keyint_sec", 2);
    vidEncoder_ = obs_video_encoder_create("obs_x264", "web_preview_venc", venc, nullptr);
    obs_data_release(venc);
    if (!vidEncoder_) {
        TeardownPipeline();
        return false;
    }
    obs_encoder_set_video(vidEncoder_, obs_canvas_get_video(canvas_));

    // Audio encoder — ffmpeg_muxer requires AV output to properly start encoders
    obs_data_t* aenc = obs_data_create();
    obs_data_set_int(aenc, "bitrate", 128);
    audEncoder_ = obs_audio_encoder_create("ffmpeg_aac", "web_preview_aenc", aenc, 0, nullptr);
    obs_data_release(aenc);
    if (!audEncoder_) {
        TeardownPipeline();
        return false;
    }
    obs_encoder_set_audio(audEncoder_, obs_get_audio());

    // ffmpeg_muxer routes to the Windows null device — no disk usage.
    // The H.264 packets are intercepted before they reach the muxer.
    obs_data_t* mux = obs_data_create();
    obs_data_set_string(mux, "path", "NUL.mp4");
    obsOutput_ = obs_output_create("ffmpeg_muxer", "web_preview_output", mux, nullptr);
    obs_data_release(mux);
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
    blog(LOG_INFO, "[obs-web-preview] streaming started: source=%s", sourceName.c_str());
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

void WebPreviewOutput::HandlePacket(encoder_packet* pkt)
{
    if (!active_ || !pkt || pkt->type != OBS_ENCODER_VIDEO || !packetCb_)
        return;
    packetCb_(pkt);
}

void WebPreviewOutput::TeardownPipeline()
{
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
    if (canvas_) {
        obs_canvas_remove(canvas_);
        obs_canvas_release(canvas_);
        canvas_ = nullptr;
    }
    if (source_) {
        obs_source_release(source_);
        source_ = nullptr;
    }
}
