#pragma once
#include <rtc/rtc.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <obs.h>

namespace httplib { class Server; class Request; class Response; }
class WebPreviewOutput;
class OpusAudioCapture;

static constexpr int kMaxStreams = 8;

struct PeerInfo {
    std::shared_ptr<rtc::PeerConnection>         pc;
    std::shared_ptr<rtc::Track>                  track;          // video
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;      // video
    std::shared_ptr<rtc::Track>                  audioTrack;
    std::shared_ptr<rtc::RtpPacketizationConfig> audioRtpConfig;
    std::atomic<bool> ready{false};                              // video track open
    std::atomic<bool> audioReady{false};
    std::atomic<bool> dead{false};
    // Set on connect and on PLI receipt; non-IDR packets are withheld until
    // the next keyframe so a desynchronised peer resumes cleanly.
    std::atomic<bool> needsKeyframe{true};
};

// Shared between StreamState and per-peer onOpen lambdas so new viewers get
// an immediate IDR instead of waiting up to keyint_sec.
struct KeyframeCache {
    std::mutex           mutex;
    std::vector<uint8_t> data;         // Annex-B IDR with SPS/PPS prepended
    uint32_t             rtpTimestamp = 0;
};

struct StreamState {
    std::unique_ptr<WebPreviewOutput>      output;
    std::unique_ptr<OpusAudioCapture>      audioCapture;
    std::mutex                             peersMutex;
    std::vector<std::shared_ptr<PeerInfo>> activePeers;
    std::atomic<bool>                      streaming{false};
    std::shared_ptr<KeyframeCache>         keyframeCache = std::make_shared<KeyframeCache>();
};

struct StreamConfig {
    std::string name;            // display name, default "Stream N"
    std::string sourceName;      // OBS source/scene name
    std::string encoderId = "obs_x264";  // video encoder id (e.g. obs_x264, ffmpeg_nvenc, ffmpeg_amf, obs_qsv11_h264)
    int         bitrateKbps = 2500;
};

class WebPreviewPlugin {
public:
    WebPreviewPlugin();
    ~WebPreviewPlugin();

    bool Start(int idx);
    void Stop(int idx);
    bool IsStreaming(int idx) const;
    int  GetViewerCount(int idx);

    int  GetNumStreams() const;
    void SetNumStreams(int n);
    int  GetPort() const;
    void SetPort(int p);

    const StreamConfig& GetConfig(int idx) const;
    void                SetConfig(int idx, const StreamConfig& cfg);

    std::vector<std::string> GetLandingUrls() const;

    void LoadSettings();
    void SaveSettings();
    void FeedPacket(int idx, encoder_packet* pkt);

private:
    StreamState  streams_[kMaxStreams];
    StreamConfig configs_[kMaxStreams];
    int          numStreams_ = 2;
    int          port_       = 8080;

    std::unique_ptr<httplib::Server> server_;
    std::thread                      serverThread_;
    std::string                      landingContent_;
    std::string                      viewerContent_;
    std::vector<std::string>         localIps_;

    bool AnyStreaming() const;
    void LoadHtml();
    void EnsureServerRunning();
    void TryStopServer();
    void RegisterRoutes();
    void HandleOfferRequest(const httplib::Request& req, httplib::Response& res, StreamState& stream);
    void FeedVideoToPeers(encoder_packet* pkt, StreamState& stream);
    void SendAudioToPeers(int idx, const uint8_t* data, size_t size, uint32_t rtpTs);
    void CleanDeadPeers(StreamState& stream);
    std::vector<std::string> GetLocalIps();
};
