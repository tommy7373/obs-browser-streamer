#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Holds audio packets in a queue and releases them on a dedicated worker
// thread at the wallclock time implied by their RTP timestamps. The first
// packet anchors the RTP-to-wallclock mapping; every subsequent packet is
// released at anchor_wall + (rtpTs - anchor_rtp) / 48000 seconds.
//
// This decouples on-wire timing from the OBS encoder thread's bursty
// delivery, which keeps the receiver's RTP-to-NTP clock model (built from
// SR + arrival times) stable enough that packets don't arrive past their
// playout deadline.
class AudioPacer {
public:
    // The callback runs on the pacer's worker thread. Don't block in it.
    using SendCb = std::function<void(const uint8_t* data, size_t size, uint32_t rtpTs)>;

    AudioPacer();
    ~AudioPacer();

    void Start(SendCb cb);
    void Stop();

    // Enqueue a packet for paced release. Safe to call from any thread.
    void Enqueue(const uint8_t* data, size_t size, uint32_t rtpTs);

private:
    struct QueuedPacket {
        std::vector<uint8_t> data;
        uint32_t rtpTs48k;
    };

    void WorkerLoop();
    uint64_t ReleaseTimeNs(uint32_t rtpTs48k) const;

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::deque<QueuedPacket> queue_;
    std::thread             worker_;
    std::atomic<bool>       running_{false};
    SendCb                  sendCb_;

    // Anchor — set on first enqueue, never updated. If wallclock drifts
    // from the RTP clock over very long sessions (>1h), we'd see latency
    // creep. Acceptable for monitoring.
    uint64_t anchorWallNs_  = 0;
    uint32_t anchorRtpTs_   = 0;
    bool     anchored_      = false;
};
