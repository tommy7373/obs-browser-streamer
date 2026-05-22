#pragma once

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Captures raw float PCM from OBS's audio mix via audio_output_connect,
// buffers it, and on a dedicated worker thread pulls exactly 20ms chunks
// at wallclock-strict intervals to encode as Opus packets via libavcodec.
//
// Replaces the OBS ffmpeg_opus encoder + AudioPacer combination. That stack
// produced bursty deliveries (60-165ms wallclock gaps between 20ms-duration
// packets) and even our wallclock-anchored pacer couldn't keep the
// receiver's jitter buffer stable. By driving the encoder ourselves we
// own end-to-end timing from source samples to the wire.
//
// The encoded callback runs on the worker thread — keep it cheap.
class OpusAudioCapture {
public:
    // (encoded_data, size, rtp_ts_48k) — rtp_ts ticks at 48 kHz, advancing
    // by 960 per call (20 ms of audio).
    using EncodedCb = std::function<void(const uint8_t*, size_t, uint32_t)>;

    OpusAudioCapture();
    ~OpusAudioCapture();

    bool Start(int bitrateKbps, EncodedCb cb);
    void Stop();

private:
    static void AudioCb(void* data, size_t mix_idx, struct audio_data* audio);
    void OnAudio(struct audio_data* audio);
    void WorkerLoop();
    bool InitEncoder(int bitrateKbps);
    void TeardownEncoder();

    audio_t*           audio_       = nullptr;
    AVCodecContext*    codecCtx_    = nullptr;
    AVFrame*           frame_       = nullptr;
    AVPacket*          pkt_         = nullptr;
    EncodedCb          cb_;
    std::thread        worker_;
    std::atomic<bool>  running_{false};

    // Ring buffer of interleaved float samples. Producer is the OBS audio
    // callback thread; consumer is the worker thread.
    std::mutex                 ringMutex_;
    std::condition_variable    ringCv_;
    std::vector<float>         ring_;
    size_t                     ringHead_ = 0;  // write index
    size_t                     ringTail_ = 0;  // read index
    size_t                     ringCount_ = 0; // available samples (per channel)

    int      sampleRate_  = 48000;
    int      channels_    = 2;
    uint32_t rtpTs_       = 0;
    uint64_t anchorWallNs_ = 0;
    uint64_t samplesEmitted_ = 0; // for wallclock-strict release scheduling

    static constexpr int kFrameSamples = 960; // 20ms at 48k
};
