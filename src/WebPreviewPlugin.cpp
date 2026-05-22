#include "WebPreviewPlugin.hpp"
#include "WebPreviewOutput.hpp"
#include "OpusAudioCapture.hpp"
#include "Telestration.hpp"

#include <rtc/plihandler.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>

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
    for (int i = 0; i < kMaxStreams; ++i) {
        streams_[i].output       = std::make_unique<WebPreviewOutput>();
        streams_[i].audioCapture = std::make_unique<OpusAudioCapture>();
    }

    telestratorStream_.output       = std::make_unique<WebPreviewOutput>();
    telestratorStream_.audioCapture = std::make_unique<OpusAudioCapture>();
    telestratorConfig_.name         = "Telestrator";

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
    if (telestratorStream_.streaming)
        StopTelestrator();
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
    landingContent_     = loadFile("web/index.html");
    viewerContent_      = loadFile("web/viewer.html");
    telestratorContent_ = loadFile("web/telestrator.html");
}

// -------------------------------------------------------------------------
// Public API — regular streams
// -------------------------------------------------------------------------

bool WebPreviewPlugin::Start(int idx)
{
    if (idx < 0 || idx >= numStreams_)
        return false;
    return StartStream(streams_[idx], configs_[idx]);
}

void WebPreviewPlugin::Stop(int idx)
{
    if (idx < 0 || idx >= numStreams_)
        return;
    StopStream(streams_[idx]);
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

    for (int i = n; i < numStreams_; ++i)
        if (streams_[i].streaming)
            Stop(i);

    for (int i = numStreams_; i < n; ++i)
        if (configs_[i].name.empty())
            configs_[i].name = "Stream " + std::to_string(i + 1);

    numStreams_ = n;
}

int WebPreviewPlugin::GetPort() const     { return port_; }
void WebPreviewPlugin::SetPort(int p)     { port_ = p; }

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

std::vector<std::string> WebPreviewPlugin::GetTelestratorUrls() const
{
    std::vector<std::string> urls;
    for (const auto& ip : localIps_)
        urls.push_back("http://" + ip + ":" + std::to_string(port_) + "/telestrator");
    return urls;
}

void WebPreviewPlugin::FeedPacket(int /*idx*/, encoder_packet* /*pkt*/)
{
    // Retained as a no-op for API compatibility; the per-stream lambdas in
    // StartStream now feed packets directly to FeedVideoToPeers without
    // round-tripping through this method.
}

// -------------------------------------------------------------------------
// Telestrator slot
// -------------------------------------------------------------------------

bool WebPreviewPlugin::StartTelestrator()
{
    return StartStream(telestratorStream_, telestratorConfig_);
}

void WebPreviewPlugin::StopTelestrator()
{
    StopStream(telestratorStream_);
    // Wipe any in-flight strokes — a fresh session starts empty.
    telestratorStream_.telestration.Clear();
}

int WebPreviewPlugin::GetTelestratorViewerCount()
{
    auto& s = telestratorStream_;
    std::lock_guard<std::mutex> lock(s.peersMutex);
    CleanDeadPeers(s);
    int n = 0;
    for (auto& p : s.activePeers)
        if (p->ready) ++n;
    return n;
}

// -------------------------------------------------------------------------
// Shared start/stop
// -------------------------------------------------------------------------

bool WebPreviewPlugin::StartStream(StreamState& s, const StreamConfig& cfg)
{
    if (s.streaming)
        return false;

    StreamState* sp = &s;
    bool started = s.output->Start(cfg.sourceName, cfg.encoderId, cfg.bitrateKbps,
        [this, sp](encoder_packet* pkt) {
            if (!sp->streaming.load() || !pkt) return;
            if (pkt->type == OBS_ENCODER_VIDEO)
                FeedVideoToPeers(pkt, *sp);
        });
    if (!started)
        return false;

    s.audioCapture->Start(128 /* kbps */,
        [this, sp](const uint8_t* data, size_t size, uint32_t rtpTs) {
            SendAudioToStream(*sp, data, size, rtpTs);
        });

    EnsureServerRunning();
    s.streaming = true;
    return true;
}

void WebPreviewPlugin::StopStream(StreamState& s)
{
    if (!s.streaming)
        return;

    s.streaming = false;
    s.output->Stop();
    s.audioCapture->Stop();

    {
        std::lock_guard<std::mutex> lock(s.peersMutex);
        for (auto& peer : s.activePeers)
            peer->pc->close();
        s.activePeers.clear();
    }

    TryStopServer();
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
        if (bitrate >= 500 && bitrate <= 50000)
            configs_[i].bitrateKbps = bitrate;

        const char* enc = obs_data_get_string(data, (prefix + "encoder").c_str());
        if (enc && enc[0])
            configs_[i].encoderId = enc;
    }

    telestratorEnabled_ = obs_data_get_bool(data, "telestrator_enabled");
    const char* tsource = obs_data_get_string(data, "telestrator_source");
    if (tsource) telestratorConfig_.sourceName = tsource;
    const char* tenc = obs_data_get_string(data, "telestrator_encoder");
    if (tenc && tenc[0]) telestratorConfig_.encoderId = tenc;
    int tbitrate = static_cast<int>(obs_data_get_int(data, "telestrator_bitrate"));
    if (tbitrate >= 500 && tbitrate <= 50000)
        telestratorConfig_.bitrateKbps = tbitrate;

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
        obs_data_set_string(data, (prefix + "name").c_str(),    configs_[i].name.c_str());
        obs_data_set_string(data, (prefix + "source").c_str(),  configs_[i].sourceName.c_str());
        obs_data_set_string(data, (prefix + "encoder").c_str(), configs_[i].encoderId.c_str());
        obs_data_set_int   (data, (prefix + "bitrate").c_str(), configs_[i].bitrateKbps);
    }

    obs_data_set_bool  (data, "telestrator_enabled", telestratorEnabled_);
    obs_data_set_string(data, "telestrator_source",  telestratorConfig_.sourceName.c_str());
    obs_data_set_string(data, "telestrator_encoder", telestratorConfig_.encoderId.c_str());
    obs_data_set_int   (data, "telestrator_bitrate", telestratorConfig_.bitrateKbps);

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
    if (telestratorStream_.streaming.load())
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

    // Active streams list for the landing page JS — intentionally omits the
    // telestrator slot so the controller URL stays unlisted.
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

        std::string offerPath = "/offer/" + std::to_string(i);
        server_->Post(offerPath, [this, i](const httplib::Request& req, httplib::Response& res) {
            if (!streams_[i].streaming) {
                res.status = 503;
                res.set_content("{\"error\":\"not streaming\"}", "application/json");
                return;
            }
            HandleOfferRequest(req, res, streams_[i], /*isTelestrator=*/false);
        });

        std::string viewersPath = "/viewers/" + std::to_string(i);
        server_->Get(viewersPath, [this, i](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"count\":" + std::to_string(GetViewerCount(i)) + "}",
                            "application/json");
        });
    }

    // Telestrator routes — hidden from /streams but reachable directly.
    server_->Get("/telestrator", [this](const httplib::Request&, httplib::Response& res) {
        if (!telestratorStream_.streaming) {
            res.status = 503;
            res.set_content(
                "<html><body><p>Telestrator is not enabled.</p></body></html>",
                "text/html");
            return;
        }
        if (telestratorContent_.empty()) {
            res.set_content(
                "<html><body><p>telestrator.html not found in plugin data directory.</p></body></html>",
                "text/html");
        } else {
            res.set_content(telestratorContent_, "text/html");
        }
    });

    server_->Post("/offer/telestrator", [this](const httplib::Request& req, httplib::Response& res) {
        if (!telestratorStream_.streaming) {
            res.status = 503;
            res.set_content("{\"error\":\"telestrator not enabled\"}", "application/json");
            return;
        }
        HandleOfferRequest(req, res, telestratorStream_, /*isTelestrator=*/true);
    });

    server_->Get("/viewers/telestrator",
        [this](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"count\":" + std::to_string(GetTelestratorViewerCount()) + "}",
                            "application/json");
        });
}

// -------------------------------------------------------------------------
// WebRTC offer handler (shared between streams)
// -------------------------------------------------------------------------

void WebPreviewPlugin::HandleOfferRequest(const httplib::Request& req, httplib::Response& res,
                                          StreamState& stream, bool isTelestrator)
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
    blog(LOG_INFO, "[obs-web-preview] /offer POST: bindIp=%s%s",
         bindIp.c_str(), isTelestrator ? " (telestrator)" : "");

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

    // Telestrator-only: accept the browser-initiated "telestrate" DataChannel.
    // Must be installed BEFORE setRemoteDescription so the DC m-line in the
    // offer is acknowledged in our answer.
    if (isTelestrator) {
        peer->pc->onDataChannel([this, sp = &stream, wp = std::weak_ptr<PeerInfo>(peer)](
                                    std::shared_ptr<rtc::DataChannel> dc) {
            if (!dc) return;
            if (dc->label() != "telestrate") return;
            if (auto p = wp.lock())
                p->telestrateDc = dc;
            SetupTelestrateChannel(dc, *sp, wp);
        });
    }

    std::promise<std::string> answerPromise;
    std::atomic<bool> answerSet{false};

    peer->pc->onGatheringStateChange(
        [&peer, &answerPromise, &answerSet](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete && !answerSet.exchange(true)) {
                if (auto desc = peer->pc->localDescription())
                    answerPromise.set_value(std::string(*desc));
            }
        });

    try {
        peer->pc->setRemoteDescription(rtc::Description(offerSdp, "offer"));
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "[obs-web-preview] setRemoteDescription(offer) failed: %s", e.what());
        res.status = 400;
        res.set_content("{\"error\":\"bad offer SDP\"}", "application/json");
        return;
    }

    struct H264Offer { int pt = -1; int mode = 0; std::string profileLevelId; };
    std::vector<H264Offer> h264Offers;
    std::string videoMid = "video";
    std::string audioMid = "audio";
    int  opusPt        = -1;
    bool hasAudio      = false;
    {
        std::istringstream iss(offerSdp);
        std::string line;
        enum { None, Video, Audio } section = None;
        bool gotVideoMid = false;
        bool gotAudioMid = false;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("m=", 0) == 0) {
                if      (line.rfind("m=video", 0) == 0) section = Video;
                else if (line.rfind("m=audio", 0) == 0) { section = Audio; hasAudio = true; }
                else                                    section = None;
                continue;
            }
            if (section == Video && !gotVideoMid && line.rfind("a=mid:", 0) == 0) {
                videoMid = line.substr(6);
                gotVideoMid = true;
            }
            if (section == Audio && !gotAudioMid && line.rfind("a=mid:", 0) == 0) {
                audioMid = line.substr(6);
                gotAudioMid = true;
            }
            if (section == Video && line.rfind("a=rtpmap:", 0) == 0 &&
                (line.find("H264/") != std::string::npos ||
                 line.find("h264/") != std::string::npos)) {
                int pt = 0;
                if (sscanf(line.c_str(), "a=rtpmap:%d", &pt) == 1)
                    h264Offers.push_back({pt, 0, ""});
            } else if (section == Video && line.rfind("a=fmtp:", 0) == 0) {
                int pt = 0;
                if (sscanf(line.c_str(), "a=fmtp:%d", &pt) == 1) {
                    for (auto& e : h264Offers) {
                        if (e.pt != pt) continue;
                        if (line.find("packetization-mode=1") != std::string::npos)
                            e.mode = 1;
                        auto pos = line.find("profile-level-id=");
                        if (pos != std::string::npos && pos + 17 + 6 <= line.size())
                            e.profileLevelId = line.substr(pos + 17, 6);
                    }
                }
            } else if (section == Audio && opusPt < 0 &&
                       line.rfind("a=rtpmap:", 0) == 0 &&
                       (line.find("opus/") != std::string::npos ||
                        line.find("OPUS/") != std::string::npos)) {
                int pt = 0;
                if (sscanf(line.c_str(), "a=rtpmap:%d", &pt) == 1)
                    opusPt = pt;
            }
        }
    }

    int h264Pt = 96;
    std::string profileLevelId;
    for (auto& e : h264Offers) {
        if (e.mode == 1) { h264Pt = e.pt; profileLevelId = e.profileLevelId; break; }
    }
    if (h264Pt == 96 && !h264Offers.empty()) {
        h264Pt = h264Offers.front().pt;
        profileLevelId = h264Offers.front().profileLevelId;
    }
    if (profileLevelId.empty()) profileLevelId = "42c01f";

    blog(LOG_INFO, "[obs-web-preview] selected video mid='%s' H264 PT=%d profile=%s (from %zu H264 entries)",
         videoMid.c_str(), h264Pt, profileLevelId.c_str(), h264Offers.size());

    const uint32_t ssrc = 42;

    rtc::Description::Video videoDesc(videoMid, rtc::Description::Direction::SendOnly);
    std::string h264Profile = "profile-level-id=" + profileLevelId
                            + ";packetization-mode=1;level-asymmetry-allowed=1";
    videoDesc.addH264Codec(h264Pt, h264Profile);

    if (auto* rtpMap = videoDesc.rtpMap(h264Pt)) {
        rtpMap->addFeedback("nack");
        rtpMap->addFeedback("nack pli");
        rtpMap->addFeedback("ccm fir");
        rtpMap->addFeedback("goog-remb");
    }

    videoDesc.addSSRC(ssrc, "obs-web-preview", "obs-stream", "obs-video-track");
    peer->track = peer->pc->addTrack(videoDesc);

    peer->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, "obs-web-preview", h264Pt, rtc::H264RtpPacketizer::defaultClockRate);

    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, peer->rtpConfig);

    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(peer->rtpConfig);
    packetizer->addToChain(srReporter);

    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>(1024);
    packetizer->addToChain(nackResponder);

    auto pliHandler = std::make_shared<rtc::PliHandler>(
        [wp = std::weak_ptr<PeerInfo>(peer)]() {
            if (auto p = wp.lock()) {
                blog(LOG_INFO, "[obs-web-preview] PLI received — waiting for next keyframe");
                p->needsKeyframe.store(true);
            }
        });
    packetizer->addToChain(pliHandler);
    peer->track->setMediaHandler(packetizer);

    auto cachePtr = stream.keyframeCache;
    peer->track->onOpen([cachePtr, wp = std::weak_ptr<PeerInfo>(peer)]() {
        auto p = wp.lock();
        if (!p) return;
        p->ready = true;

        std::vector<uint8_t> kf;
        uint32_t ts = 0;
        {
            std::lock_guard<std::mutex> lock(cachePtr->mutex);
            kf = cachePtr->data;
            ts = cachePtr->rtpTimestamp;
        }
        if (!kf.empty()) {
            try {
                p->rtpConfig->timestamp = ts;
                p->track->send(reinterpret_cast<const std::byte*>(kf.data()), kf.size());
                p->needsKeyframe.store(false);
            } catch (...) {}
        }
    });

    if (hasAudio && opusPt >= 0) {
        const uint32_t audioSsrc = 43;
        rtc::Description::Audio audioDesc(audioMid, rtc::Description::Direction::SendOnly);
        audioDesc.addOpusCodec(opusPt);
        audioDesc.addSSRC(audioSsrc, "obs-web-preview", "obs-stream", "obs-audio-track");
        peer->audioTrack = peer->pc->addTrack(audioDesc);

        peer->audioRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            audioSsrc, "obs-web-preview", opusPt,
            rtc::OpusRtpPacketizer::DefaultClockRate);
        auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(peer->audioRtpConfig);

        auto audioSr = std::make_shared<rtc::RtcpSrReporter>(peer->audioRtpConfig);
        audioPacketizer->addToChain(audioSr);
        peer->audioTrack->setMediaHandler(audioPacketizer);

        peer->audioTrack->onOpen([wp = std::weak_ptr<PeerInfo>(peer)]() {
            if (auto p = wp.lock()) p->audioReady = true;
        });

        blog(LOG_INFO, "[obs-web-preview] audio: Opus PT=%d mid='%s'", opusPt, audioMid.c_str());
    } else {
        blog(LOG_INFO, "[obs-web-preview] no audio in offer (hasAudio=%d opusPt=%d)",
             hasAudio ? 1 : 0, opusPt);
    }

    peer->pc->setLocalDescription();

    auto answerFuture = answerPromise.get_future();
    if (answerFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        blog(LOG_WARNING, "[obs-web-preview] ICE gathering timed out for bindIp=%s", bindIp.c_str());
        res.status = 504;
        res.set_content("{\"error\":\"ice gathering timeout\"}", "application/json");
        return;
    }

    std::string answerSdp = answerFuture.get();

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
// Telestrate DataChannel handling
// -------------------------------------------------------------------------

void WebPreviewPlugin::SetupTelestrateChannel(std::shared_ptr<rtc::DataChannel> dc,
                                               StreamState& stream,
                                               std::weak_ptr<PeerInfo> wp)
{
    StreamState* sp = &stream;
    std::weak_ptr<rtc::DataChannel> wdc = dc;

    dc->onOpen([this, sp, wdc]() {
        auto d = wdc.lock();
        if (!d) return;
        try {
            d->send(SerializeSnapshotMessage(sp->telestration.Snapshot()));
        } catch (...) {}
    });

    dc->onMessage([this, sp, wp](rtc::message_variant msg) {
        std::string s;
        if (std::holds_alternative<std::string>(msg)) {
            s = std::get<std::string>(msg);
        } else {
            const auto& bin = std::get<rtc::binary>(msg);
            s.assign(reinterpret_cast<const char*>(bin.data()), bin.size());
        }

        TelestrateMessage m;
        if (!ParseTelestrateMessage(s, m)) return;

        bool changed = false;
        switch (m.op) {
            case TelestrateOp::Stroke_:
                sp->telestration.Add(std::move(m.stroke));
                changed = true;
                break;
            case TelestrateOp::Undo:
                changed = sp->telestration.UndoById(m.undoId);
                break;
            case TelestrateOp::Clear:
                sp->telestration.Clear();
                changed = true;
                break;
            default:
                break;
        }
        if (changed)
            BroadcastTelestrate(*sp, s, wp);
    });
}

void WebPreviewPlugin::BroadcastTelestrate(StreamState& stream, const std::string& json,
                                            const std::weak_ptr<PeerInfo>& sender)
{
    auto senderPtr = sender.lock();
    std::lock_guard<std::mutex> lock(stream.peersMutex);
    for (auto& peer : stream.activePeers) {
        if (!peer || peer->dead.load()) continue;
        if (senderPtr && peer.get() == senderPtr.get()) continue;
        auto dc = peer->telestrateDc;
        if (!dc) continue;
        try { dc->send(json); } catch (...) {}
    }
}

// -------------------------------------------------------------------------
// Video / audio packet feeding
// -------------------------------------------------------------------------

void WebPreviewPlugin::FeedVideoToPeers(encoder_packet* pkt, StreamState& stream)
{
    const double ptsSeconds = static_cast<double>(pkt->pts)
                            * pkt->timebase_num / pkt->timebase_den;
    const uint32_t rtpTs    = static_cast<uint32_t>(ptsSeconds * 90000.0);

    if (pkt->keyframe) {
        std::lock_guard<std::mutex> kfLock(stream.keyframeCache->mutex);
        stream.keyframeCache->data.assign(pkt->data, pkt->data + pkt->size);
        stream.keyframeCache->rtpTimestamp = rtpTs;
    }

    std::lock_guard<std::mutex> lock(stream.peersMutex);
    CleanDeadPeers(stream);
    if (stream.activePeers.empty()) return;

    auto* bytes = reinterpret_cast<const std::byte*>(pkt->data);
    for (auto& peer : stream.activePeers) {
        if (!peer->ready) continue;
        if (!pkt->keyframe && peer->needsKeyframe.load()) continue;
        try {
            peer->rtpConfig->timestamp = rtpTs;
            peer->track->send(bytes, pkt->size);
            if (pkt->keyframe) peer->needsKeyframe.store(false);
        } catch (...) {
            peer->dead = true;
        }
    }
}

void WebPreviewPlugin::SendAudioToStream(StreamState& stream, const uint8_t* data,
                                          size_t size, uint32_t rtpTs)
{
    if (!stream.streaming) return;

    std::lock_guard<std::mutex> lock(stream.peersMutex);
    CleanDeadPeers(stream);
    if (stream.activePeers.empty()) return;

    auto* bytes = reinterpret_cast<const std::byte*>(data);
    for (auto& peer : stream.activePeers) {
        if (!peer->audioReady || !peer->audioTrack || !peer->audioRtpConfig)
            continue;
        try {
            peer->audioRtpConfig->timestamp = rtpTs;
            peer->audioTrack->send(bytes, size);
        } catch (...) {
            peer->dead = true;
        }
    }
}

void WebPreviewPlugin::CleanDeadPeers(StreamState& stream)
{
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
