#include "AudioPacer.hpp"

#include <util/platform.h>

#include <chrono>

AudioPacer::AudioPacer() = default;

AudioPacer::~AudioPacer()
{
    Stop();
}

void AudioPacer::Start(SendCb cb)
{
    if (running_) return;
    sendCb_   = std::move(cb);
    anchored_ = false;
    running_  = true;
    worker_   = std::thread(&AudioPacer::WorkerLoop, this);
}

void AudioPacer::Stop()
{
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    sendCb_ = nullptr;
}

void AudioPacer::Enqueue(const uint8_t* data, size_t size, uint32_t rtpTs48k)
{
    if (!running_ || !data || size == 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({std::vector<uint8_t>(data, data + size), rtpTs48k});
    }
    cv_.notify_one();
}

uint64_t AudioPacer::ReleaseTimeNs(uint32_t rtpTs48k) const
{
    // Signed delta to handle wraparound naturally on subtraction.
    const int32_t delta = static_cast<int32_t>(rtpTs48k - anchorRtpTs_);
    const int64_t deltaNs = static_cast<int64_t>(delta) * 1'000'000'000LL / 48000LL;
    return anchorWallNs_ + static_cast<uint64_t>(deltaNs);
}

void AudioPacer::WorkerLoop()
{
    while (running_) {
        QueuedPacket pkt;
        bool havePkt = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !running_ || !queue_.empty(); });
            if (!running_) return;
            pkt = std::move(queue_.front());
            queue_.pop_front();
            havePkt = true;
        }

        if (!havePkt) continue;

        // Anchor on the first packet we see.
        if (!anchored_) {
            anchorWallNs_ = os_gettime_ns();
            anchorRtpTs_  = pkt.rtpTs48k;
            anchored_     = true;
        }

        // Sleep until this packet's wallclock release time. If we're
        // already past it (encoder fell behind, or a long pause stalled
        // the queue), release immediately — better late than never.
        const uint64_t releaseNs = ReleaseTimeNs(pkt.rtpTs48k);
        const uint64_t nowNs     = os_gettime_ns();
        if (releaseNs > nowNs) {
            const uint64_t waitNs = releaseNs - nowNs;
            // Don't sleep absurd amounts — if the queue's anchor went
            // catastrophically wrong (e.g. an encoder restart with reset
            // pts), cap at 1s. Long real waits indicate misconfiguration,
            // not normal operation.
            const uint64_t cappedNs = std::min<uint64_t>(waitNs, 1'000'000'000ULL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(cappedNs));
        }

        if (running_ && sendCb_)
            sendCb_(pkt.data.data(), pkt.data.size(), pkt.rtpTs48k);
    }
}
