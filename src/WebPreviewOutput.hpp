#pragma once

#include <obs.h>

#include <atomic>
#include <functional>
#include <string>

class WebPreviewOutput {
public:
    using PacketCallback = std::function<void(encoder_packet*)>;

    WebPreviewOutput();
    ~WebPreviewOutput();

    bool Start(const std::string& sourceName, int bitrateKbps, PacketCallback cb);
    void Stop();
    bool IsActive() const { return active_; }

    void HandlePacket(encoder_packet* pkt);

private:
    obs_source_t*  source_     = nullptr;
    obs_canvas_t*  canvas_     = nullptr;
    obs_encoder_t* vidEncoder_ = nullptr;
    obs_encoder_t* audEncoder_ = nullptr;
    obs_output_t*  obsOutput_  = nullptr;

    std::atomic<bool> active_{false};
    PacketCallback    packetCb_;

    static void PacketInterceptCb(obs_output_t*, encoder_packet* pkt,
                                  encoder_packet_time*, void* param);
    void TeardownPipeline();
};
