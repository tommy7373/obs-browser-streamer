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

static constexpr int kMaxStreams = 8;

struct PeerInfo {
    std::shared_ptr<rtc::PeerConnection>         pc;
    std::shared_ptr<rtc::Track>                  track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::atomic<bool> ready{false};
    std::atomic<bool> dead{false};
};

struct StreamState {
    std::unique_ptr<WebPreviewOutput>      output;
    std::mutex                             peersMutex;
    std::vector<std::shared_ptr<PeerInfo>> activePeers;
    std::atomic<bool>                      streaming{false};
};

struct StreamConfig {
    std::string name;            // display name, default "Stream N"
    std::string sourceName;      // OBS source/scene name
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
    void FeedVideoPacket(int idx, encoder_packet* pkt);

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
    void FeedPacketToPool(encoder_packet* pkt, StreamState& stream);
    void CleanDeadPeers(StreamState& stream);
    std::vector<std::string> GetLocalIps();
};
