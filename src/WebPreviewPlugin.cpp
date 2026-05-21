#include "WebPreviewPlugin.hpp"
#include "WebPreviewOutput.hpp"

// Suppress httplib deprecation warnings on MSVC
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996 4244 4267)
#endif
#define CPPHTTPLIB_USE_POLL
#include "httplib.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <obs-module.h>
#include <util/platform.h>

#include <chrono>
#include <fstream>
#include <future>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------

WebPreviewPlugin::WebPreviewPlugin()
{
    for (int i = 0; i < kMaxStreams; ++i)
        streams_[i].output = std::make_unique<WebPreviewOutput>();

    for (int i = 0; i < kMaxStreams; ++i)
        configs_[i].name = "Stream " + std::to_string(i + 1);

    LoadHtml();
    localIps_ = GetLocalIps();
    LoadSettings();
}

WebPreviewPlugin::~WebPreviewPlugin()
{
    for (int i = 0; i < numStreams_; ++i)
        if (streams_[i].streaming)
            Stop(i);
}

void WebPreviewPlugin::LoadHtml()
{
    auto loadFile = [](const char* relPath) -> std::string {
        char* p = obs_module_file(relPath);
        if (!p) return {};
        std::ifstream f(p);
        bfree(p);
        if (!f.is_open()) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    landingContent_ = loadFile("web/index.html");
    viewerContent_  = loadFile("web/viewer.html");
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

bool WebPreviewPlugin::Start(int idx)
{
    if (idx < 0 || idx >= numStreams_)
        return false;
    auto& s = streams_[idx];
    if (s.streaming)
        return false;

    bool started = s.output->Start(configs_[idx].sourceName, configs_[idx].bitrateKbps,
        [this, idx](encoder_packet* pkt) { FeedVideoPacket(idx, pkt); });
    if (!started)
        return false;

    EnsureServerRunning();
    s.streaming = true;
    return true;
}

void WebPreviewPlugin::Stop(int idx)
{
    if (idx < 0 || idx >= numStreams_)
        return;
    auto& s = streams_[idx];
    if (!s.streaming)
        return;

    s.streaming = false;
    s.output->Stop();

    {
        std::lock_guard<std::mutex> lock(s.peersMutex);
        for (auto& peer : s.activePeers)
            peer->pc->close();
        s.activePeers.clear();
    }

    TryStopServer();
}

bool WebPreviewPlugin::IsStreaming(int idx) const
{
    if (idx < 0 || idx >= numStreams_)
        return false;
    return streams_[idx].streaming.load();
}

int WebPreviewPlugin::GetViewerCount(int idx)
{
    if (idx < 0 || idx >= numStreams_)
        return 0;
    auto& s = streams_[idx];
    std::lock_guard<std::mutex> lock(s.peersMutex);
    CleanDeadPeers(s);
    int n = 0;
    for (auto& p : s.activePeers)
        if (p->ready) ++n;
    return n;
}

int WebPreviewPlugin::GetNumStreams() const
{
    return numStreams_;
}

void WebPreviewPlugin::SetNumStreams(int n)
{
    if (n < 1) n = 1;
    if (n > kMaxStreams) n = kMaxStreams;

    // Stop any streams that are now out of range
    for (int i = n; i < numStreams_; ++i)
        if (streams_[i].streaming)
            Stop(i);

    // Initialize names for newly added streams if empty
    for (int i = numStreams_; i < n; ++i)
        if (configs_[i].name.empty())
            configs_[i].name = "Stream " + std::to_string(i + 1);

    numStreams_ = n;
}

int WebPreviewPlugin::GetPort() const
{
    return port_;
}

void WebPreviewPlugin::SetPort(int p)
{
    port_ = p;
}

const StreamConfig& WebPreviewPlugin::GetConfig(int idx) const
{
    static StreamConfig dummy;
    if (idx < 0 || idx >= kMaxStreams)
        return dummy;
    return configs_[idx];
}

void WebPreviewPlugin::SetConfig(int idx, const StreamConfig& cfg)
{
    if (idx < 0 || idx >= kMaxStreams)
        return;
    configs_[idx] = cfg;
}

std::vector<std::string> WebPreviewPlugin::GetLandingUrls() const
{
    std::vector<std::string> urls;
    for (const auto& ip : localIps_)
        urls.push_back("http://" + ip + ":" + std::to_string(port_) + "/");
    return urls;
}

void WebPreviewPlugin::FeedVideoPacket(int idx, encoder_packet* pkt)
{
    if (idx < 0 || idx >= numStreams_)
        return;
    auto& s = streams_[idx];
    if (!s.streaming)
        return;
    FeedPacketToPool(pkt, s);
}

// -------------------------------------------------------------------------
// Settings
// -------------------------------------------------------------------------

void WebPreviewPlugin::LoadSettings()
{
    char* path = obs_module_get_config_path(obs_current_module(), "settings.json");
    if (!path)
        return;
    obs_data_t* data = obs_data_create_from_json_file(path);
    bfree(path);
    if (!data)
        return;

    int port = static_cast<int>(obs_data_get_int(data, "port"));
    if (port >= 1024 && port <= 65535)
        port_ = port;

    int numStreams = static_cast<int>(obs_data_get_int(data, "num_streams"));
    if (numStreams >= 1 && numStreams <= kMaxStreams)
        numStreams_ = numStreams;

    for (int i = 0; i < kMaxStreams; ++i) {
        std::string prefix = "stream_" + std::to_string(i) + "_";

        const char* name = obs_data_get_string(data, (prefix + "name").c_str());
        if (name && name[0])
            configs_[i].name = name;
        else if (configs_[i].name.empty())
            configs_[i].name = "Stream " + std::to_string(i + 1);

        const char* source = obs_data_get_string(data, (prefix + "source").c_str());
        if (source)
            configs_[i].sourceName = source;

        int bitrate = static_cast<int>(obs_data_get_int(data, (prefix + "bitrate").c_str()));
        if (bitrate >= 500 && bitrate <= 20000)
            configs_[i].bitrateKbps = bitrate;
    }

    obs_data_release(data);
}

void WebPreviewPlugin::SaveSettings()
{
    char* dir = obs_module_get_config_path(obs_current_module(), "");
    if (dir) {
        os_mkdirs(dir);
        bfree(dir);
    }

    char* path = obs_module_get_config_path(obs_current_module(), "settings.json");
    if (!path)
        return;

    obs_data_t* data = obs_data_create();
    obs_data_set_int(data, "port", port_);
    obs_data_set_int(data, "num_streams", numStreams_);

    for (int i = 0; i < kMaxStreams; ++i) {
        std::string prefix = "stream_" + std::to_string(i) + "_";
        obs_data_set_string(data, (prefix + "name").c_str(),   configs_[i].name.c_str());
        obs_data_set_string(data, (prefix + "source").c_str(), configs_[i].sourceName.c_str());
        obs_data_set_int   (data, (prefix + "bitrate").c_str(), configs_[i].bitrateKbps);
    }

    obs_data_save_json_safe(data, path, "tmp", "bak");
    obs_data_release(data);
    bfree(path);
}

// -------------------------------------------------------------------------
// HTTP server lifecycle
// -------------------------------------------------------------------------

bool WebPreviewPlugin::AnyStreaming() const
{
    for (int i = 0; i < numStreams_; ++i)
        if (streams_[i].streaming.load())
            return true;
    return false;
}

void WebPreviewPlugin::EnsureServerRunning()
{
    if (server_)
        return; // already running

    server_ = std::make_unique<httplib::Server>();
    RegisterRoutes();

    serverThread_ = std::thread([this]() {
        server_->listen("0.0.0.0", port_);
    });
}

void WebPreviewPlugin::TryStopServer()
{
    if (AnyStreaming())
        return; // another stream still active

    if (server_)
        server_->stop();
    if (serverThread_.joinable())
        serverThread_.join();
    server_.reset();
}

void WebPreviewPlugin::RegisterRoutes()
{
    // Landing page
    server_->Get("/", [this](const httplib::Request&, httplib::Response& res) {
        if (landingContent_.empty()) {
            res.set_content(
                "<html><body><p>index.html not found in plugin data directory.</p></body></html>",
                "text/html");
        } else {
            res.set_content(landingContent_, "text/html");
        }
    });

    // Active streams list for the landing page JS
    server_->Get("/streams", [this](const httplib::Request&, httplib::Response& res) {
        std::string json = "[";
        bool first = true;
        for (int i = 0; i < numStreams_; ++i) {
            if (!streams_[i].streaming)
                continue;
            if (!first) json += ",";
            first = false;
            std::string escaped;
            for (char c : configs_[i].name) {
                if (c == '"')       escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else                escaped += c;
            }
            json += "{\"name\":\"" + escaped + "\",\"path\":\"/" + std::to_string(i + 1) + "\"}";
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    // Per-stream routes registered dynamically
    for (int i = 0; i < numStreams_; ++i) {
        // Viewer HTML: /1, /2, /3, ...
        std::string viewerPath = "/" + std::to_string(i + 1);
        server_->Get(viewerPath, [this](const httplib::Request&, httplib::Response& res) {
            if (viewerContent_.empty()) {
                res.set_content(
                    "<html><body><p>viewer.html not found in plugin data directory.</p></body></html>",
                    "text/html");
            } else {
                res.set_content(viewerContent_, "text/html");
            }
        });

        // Offer endpoint: /offer/0, /offer/1, ...
        std::string offerPath = "/offer/" + std::to_string(i);
        server_->Post(offerPath, [this, i](const httplib::Request& req, httplib::Response& res) {
            if (!streams_[i].streaming) {
                res.status = 503;
                res.set_content("{\"error\":\"not streaming\"}", "application/json");
                return;
            }
            HandleOfferRequest(req, res, streams_[i]);
        });

        // Viewer count: /viewers/0, /viewers/1, ...
        std::string viewersPath = "/viewers/" + std::to_string(i);
        server_->Get(viewersPath, [this, i](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"count\":" + std::to_string(GetViewerCount(i)) + "}",
                            "application/json");
        });
    }
}

// -------------------------------------------------------------------------
// WebRTC offer handler (shared between streams)
// -------------------------------------------------------------------------

void WebPreviewPlugin::HandleOfferRequest(const httplib::Request& req, httplib::Response& res,
                                          StreamState& stream)
{
    obs_data_t* jdata = obs_data_create_from_json(req.body.c_str());
    if (!jdata) { res.status = 400; return; }
    const char* sdpStr  = obs_data_get_string(jdata, "sdp");
    std::string offerSdp = sdpStr ? sdpStr : "";
    obs_data_release(jdata);

    if (offerSdp.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"missing sdp\"}", "application/json");
        return;
    }

    std::string bindIp;
    if (req.has_header("Host")) {
        std::string host = req.get_header_value("Host");
        auto colon = host.rfind(':');
        bindIp = (colon != std::string::npos) ? host.substr(0, colon) : host;
    }
    blog(LOG_INFO, "[obs-web-preview] /offer POST: bindIp=%s", bindIp.c_str());

    rtc::Configuration rtcConfig;
    if (!bindIp.empty() && bindIp != "localhost" && bindIp != "127.0.0.1")
        rtcConfig.bindAddress = bindIp;
    rtcConfig.portRangeBegin         = 50000;
    rtcConfig.portRangeEnd           = 50010;
    rtcConfig.disableAutoNegotiation = true;

    auto peer = std::make_shared<PeerInfo>();
    peer->pc  = std::make_shared<rtc::PeerConnection>(rtcConfig);

    peer->pc->onStateChange([wp = std::weak_ptr<PeerInfo>(peer)](rtc::PeerConnection::State st) {
        using State = rtc::PeerConnection::State;
        const char* name = "unknown";
        switch (st) {
            case State::New:          name = "New"; break;
            case State::Connecting:   name = "Connecting"; break;
            case State::Connected:    name = "Connected"; break;
            case State::Disconnected: name = "Disconnected"; break;
            case State::Failed:       name = "Failed"; break;
            case State::Closed:       name = "Closed"; break;
        }
        blog(LOG_INFO, "[obs-web-preview] peer state → %s", name);
        if (auto p = wp.lock()) {
            if (st == State::Connected) {
                p->ready = true;
            } else if (st == State::Disconnected || st == State::Failed || st == State::Closed) {
                p->ready = false;
                p->dead  = true;
            }
        }
    });

    peer->pc->onIceStateChange([](rtc::PeerConnection::IceState st) {
        const char* name = "unknown";
        switch (st) {
            case rtc::PeerConnection::IceState::New:          name = "New"; break;
            case rtc::PeerConnection::IceState::Checking:     name = "Checking"; break;
            case rtc::PeerConnection::IceState::Connected:    name = "Connected"; break;
            case rtc::PeerConnection::IceState::Completed:    name = "Completed"; break;
            case rtc::PeerConnection::IceState::Failed:       name = "Failed"; break;
            case rtc::PeerConnection::IceState::Disconnected: name = "Disconnected"; break;
            case rtc::PeerConnection::IceState::Closed:       name = "Closed"; break;
        }
        blog(LOG_INFO, "[obs-web-preview] ICE state → %s", name);
    });

    // Register gathering callback BEFORE setRemoteDescription to avoid missing
    // the Complete event (auto-negotiation is disabled so gathering fires after
    // our explicit setLocalDescription call below).
    std::promise<std::string> answerPromise;
    std::atomic<bool> answerSet{false};

    peer->pc->onGatheringStateChange(
        [&peer, &answerPromise, &answerSet](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete && !answerSet.exchange(true)) {
                if (auto desc = peer->pc->localDescription())
                    answerPromise.set_value(std::string(*desc));
            }
        });

    // Accept browser's offer (won't auto-answer because disableAutoNegotiation=true)
    try {
        peer->pc->setRemoteDescription(rtc::Description(offerSdp, "offer"));
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "[obs-web-preview] setRemoteDescription(offer) failed: %s", e.what());
        res.status = 400;
        res.set_content("{\"error\":\"bad offer SDP\"}", "application/json");
        return;
    }

    // Find the H264 payload type from the browser's offer — the PT we use in
    // rtpConfig must match what the browser negotiated.
    int h264Pt = 96; // fallback
    {
        std::istringstream iss(offerSdp);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.rfind("a=rtpmap:", 0) == 0 &&
                (line.find("H264/") != std::string::npos ||
                 line.find("h264/") != std::string::npos)) {
                int pt = 0;
                if (sscanf(line.c_str(), "a=rtpmap:%d", &pt) == 1)
                    h264Pt = pt;
                break;
            }
        }
    }
    blog(LOG_INFO, "[obs-web-preview] using H264 PT=%d", h264Pt);

    // Add H264 SendOnly track using the browser's negotiated PT
    rtc::Description::Video videoDesc("video", rtc::Description::Direction::SendOnly);
    videoDesc.addH264Codec(h264Pt);
    peer->track = peer->pc->addTrack(videoDesc);

    const uint32_t ssrc = 42;
    peer->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, "obs-web-preview", h264Pt, rtc::H264RtpPacketizer::defaultClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, peer->rtpConfig);
    peer->track->setMediaHandler(packetizer);

    peer->track->onOpen([wp = std::weak_ptr<PeerInfo>(peer)]() {
        blog(LOG_INFO, "[obs-web-preview] track opened — peer is ready");
        if (auto p = wp.lock())
            p->ready = true;
    });

    peer->track->onClosed([]() {
        blog(LOG_INFO, "[obs-web-preview] track closed");
    });

    peer->pc->setLocalDescription(); // creates answer + starts ICE gathering

    auto answerFuture = answerPromise.get_future();
    if (answerFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        blog(LOG_WARNING, "[obs-web-preview] ICE gathering timed out for bindIp=%s", bindIp.c_str());
        res.status = 504;
        res.set_content("{\"error\":\"ice gathering timeout\"}", "application/json");
        return;
    }

    std::string answerSdp = answerFuture.get();

    {
        std::istringstream iss(answerSdp);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.rfind("a=candidate",   0) == 0 ||
                line.rfind("a=fingerprint", 0) == 0 ||
                line.rfind("a=setup",       0) == 0)
                blog(LOG_INFO, "[obs-web-preview] answer: %s", line.c_str());
        }
    }

    {
        std::lock_guard<std::mutex> lock(stream.peersMutex);
        stream.activePeers.push_back(peer);
    }

    std::string escaped;
    for (char c : answerSdp) {
        if (c == '"')       escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else                escaped += c;
    }
    res.set_content("{\"type\":\"answer\",\"sdp\":\"" + escaped + "\"}", "application/json");
}

// -------------------------------------------------------------------------
// Video packet feeding — called from OBS encoder thread
// -------------------------------------------------------------------------

void WebPreviewPlugin::FeedPacketToPool(encoder_packet* pkt, StreamState& stream)
{
    double ptsSeconds = static_cast<double>(pkt->pts)
                      * pkt->timebase_num / pkt->timebase_den;
    uint32_t rtpTs = static_cast<uint32_t>(ptsSeconds * 90000.0);

    std::lock_guard<std::mutex> lock(stream.peersMutex);
    CleanDeadPeers(stream);

    for (auto& peer : stream.activePeers) {
        if (!peer->ready)
            continue;
        try {
            peer->rtpConfig->timestamp = rtpTs;
            auto* bytes = reinterpret_cast<const std::byte*>(pkt->data);
            peer->track->send(bytes, pkt->size);
        } catch (...) {
            peer->dead = true;
        }
    }
}

void WebPreviewPlugin::CleanDeadPeers(StreamState& stream)
{
    // Must be called with stream.peersMutex held
    stream.activePeers.erase(
        std::remove_if(stream.activePeers.begin(), stream.activePeers.end(),
            [](const std::shared_ptr<PeerInfo>& p) { return p->dead.load(); }),
        stream.activePeers.end());
}

// -------------------------------------------------------------------------
// GetLocalIps
// -------------------------------------------------------------------------

std::vector<std::string> WebPreviewPlugin::GetLocalIps()
{
    std::vector<std::string> ips;
#ifdef _WIN32
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    PIP_ADAPTER_ADDRESSES addrs = nullptr;

    for (int attempt = 0; attempt < 3 && ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(bufLen);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        ret = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufLen);
    }

    if (ret == NO_ERROR && addrs) {
        for (auto* adapter = addrs; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp)
                continue;
            for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
                auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
                std::string ip(ipStr);
                if (ip != "127.0.0.1")
                    ips.push_back(ip);
            }
        }
    }
#endif
    if (ips.empty())
        ips.push_back("127.0.0.1");
    return ips;
}
