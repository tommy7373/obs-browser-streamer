#pragma once

#include <obs.h>
#include <media-io/video-io.h>
#include <graphics/graphics.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class WebPreviewOutput {
public:
    using PacketCallback = std::function<void(encoder_packet*)>;

    WebPreviewOutput();
    ~WebPreviewOutput();

    // encoderId may be "obs_x264" or any fallback (non-texture) video encoder
    // such as "ffmpeg_nvenc", "ffmpeg_amf", "obs_qsv11_h264".  Texture
    // encoders (OBS_ENCODER_CAP_PASS_TEXTURE) cannot be used because they
    // pull GPU textures directly from OBS's main video pipeline and don't
    // accept frames from a custom video_output_t.
    bool Start(const std::string& sourceName,
               const std::string& encoderId,
               int bitrateKbps,
               PacketCallback cb);
    void Stop();
    bool IsActive() const { return active_; }

    void HandlePacket(encoder_packet* pkt);

private:
    // OBS pipeline
    obs_source_t*   source_      = nullptr;
    video_t* videoOutput_ = nullptr;
    obs_encoder_t*  vidEncoder_  = nullptr;
    obs_encoder_t*  audEncoder_  = nullptr;
    obs_output_t*   obsOutput_   = nullptr;

    // GPU-side resources (graphics thread only).  We render the source to an
    // off-screen texture each frame, then download it to a stage surface for
    // CPU access, then push the BGRA pixels into videoOutput_.  This is the
    // same readback pattern used by obs-multirecording and works with both
    // software and fallback hardware encoders.
    gs_texrender_t* texrender_    = nullptr;
    gs_stagesurf_t* stagesurface_ = nullptr;
    uint32_t        renderWidth_  = 0;
    uint32_t        renderHeight_ = 0;
    std::string     videoOutputName_;
    std::string     discardPath_;

    std::atomic<bool>    active_{false};
    PacketCallback       packetCb_;
    std::vector<uint8_t> extraData_; // SPS/PPS from encoder, prepended to bare IDR keyframes
    std::vector<uint8_t> kfBuffer_;  // scratch buffer for patched keyframe data

    static void RenderCallback(void* param, uint32_t cx, uint32_t cy);
    void        DoRender();

    static void PacketInterceptCb(obs_output_t*, encoder_packet* pkt,
                                  encoder_packet_time*, void* param);
    void SetupGraphicsResources();
    void TeardownGraphicsResources();
    void TeardownPipeline();
};
