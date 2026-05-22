#pragma once

#include <obs.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class WebPreviewOutput {
public:
    using PacketCallback = std::function<void(encoder_packet*)>;

    WebPreviewOutput();
    ~WebPreviewOutput();

    // Registers the plugin's null obs_output_t type. Call once from
    // obs_module_load before any WebPreviewOutput instance is created.
    static void RegisterOutputType();

    // encoderId may be any H.264 encoder OBS exposes (obs_x264, obs_nvenc_*,
    // ffmpeg_amf_*, obs_qsv11_h264, etc.). The view-backed video pipeline
    // makes the encoder believe it is paired with the main canvas, so
    // hardware encoders (including the texture-path variants) work.
    bool Start(const std::string& sourceName,
               const std::string& encoderId,
               int bitrateKbps,
               PacketCallback cb);
    void Stop();
    bool IsActive() const { return active_; }

    void HandlePacket(encoder_packet* pkt);

private:
    obs_source_t*   source_     = nullptr;
    obs_view_t*     view_       = nullptr;
    video_t*        viewVideo_  = nullptr;
    obs_encoder_t*  vidEncoder_ = nullptr;
    obs_encoder_t*  audEncoder_ = nullptr;
    obs_output_t*   obsOutput_  = nullptr;

    uint32_t        renderWidth_  = 0;
    uint32_t        renderHeight_ = 0;

    std::atomic<bool>    active_{false};
    PacketCallback       packetCb_;
    std::vector<uint8_t> extraData_; // SPS/PPS from encoder, prepended to bare IDR keyframes
    std::vector<uint8_t> kfBuffer_;  // scratch buffer for patched keyframe data

    static void PacketInterceptCb(obs_output_t*, encoder_packet* pkt,
                                  encoder_packet_time*, void* param);
    void TeardownPipeline();
};
