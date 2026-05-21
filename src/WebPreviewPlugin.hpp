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

class WebPreviewPlugin {
public:
    WebPreviewPlugin();
    ~WebPreviewPlugin();

    bool Start(int streamIdx, const std::string& sourceName, int port, int bitrateKbps);
    void Stop(int streamIdx);

    bool IsStreaming(int streamIdx) const;
    int  GetViewerCount(int streamIdx);
    std::vector<std::string> GetStreamUrls(int streamIdx) const;
    void SetStreamName(int streamIdx, const std::string& name);
    std::string GetStreamName(int streamIdx) const;

    void FeedVideoPacket(int streamIdx, encoder_packet* pkt);

private:
    StreamState streams_[2];

    std::unique_ptr<httplib::Server> server_;
    std::thread                      serverThread_;
    int                              port_ = 8080;
    std::string                      landingContent_;
    std::string                      viewerContent_;
    std::vector<std::string>         localIps_;
    std::string                      streamNames_[2];

    bool AnyStreaming() const;
    void LoadHtml();
    void EnsureServerRunning(int port);
    void TryStopServer();
    void RegisterRoutes();
    void HandleOfferRequest(const httplib::Request& req, httplib::Response& res, StreamState& stream);
    void FeedPacketToPool(encoder_packet* pkt, StreamState& stream);
    void CleanDeadPeers(StreamState& stream);
    std::vector<std::string> GetLocalIps();
};
