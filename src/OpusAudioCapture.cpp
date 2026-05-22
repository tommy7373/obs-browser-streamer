#include "OpusAudioCapture.hpp"

#include <obs-module.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>

OpusAudioCapture::OpusAudioCapture() = default;

OpusAudioCapture::~OpusAudioCapture()
{
    Stop();
}

bool OpusAudioCapture::Start(int bitrateKbps, EncodedCb cb)
{
    if (running_) return false;
    cb_ = std::move(cb);

    audio_ = obs_get_audio();
    if (!audio_) {
        blog(LOG_WARNING, "[obs-web-preview] OpusAudioCapture: no audio_t");
        return false;
    }

    const audio_output_info* aoi = audio_output_get_info(audio_);
    if (!aoi) return false;
    sampleRate_ = static_cast<int>(aoi->samples_per_sec);
    channels_   = static_cast<int>(get_audio_channels(aoi->speakers));
    if (channels_ <= 0) channels_ = 2;

    // Ring buffer sized for ~200ms of audio per channel — way more than
    // OBS's ~21ms chunk size, plenty of headroom for scheduling jitter.
    ring_.assign(static_cast<size_t>(sampleRate_ / 5) * channels_, 0.0f);
    ringHead_ = ringTail_ = ringCount_ = 0;

    if (!InitEncoder(bitrateKbps))
        return false;

    rtpTs_           = 0;
    anchorWallNs_    = 0;
    samplesEmitted_  = 0;
    running_         = true;

    struct audio_convert_info conv = {};
    conv.format          = AUDIO_FORMAT_FLOAT;
    conv.samples_per_sec = static_cast<uint32_t>(sampleRate_);
    conv.speakers        = aoi->speakers;

    audio_output_connect(audio_, 0, &conv, &OpusAudioCapture::AudioCb, this);

    worker_ = std::thread(&OpusAudioCapture::WorkerLoop, this);
    blog(LOG_INFO,
         "[obs-web-preview] OpusAudioCapture started: %d ch @ %d Hz, %d kbps",
         channels_, sampleRate_, bitrateKbps);
    return true;
}

void OpusAudioCapture::Stop()
{
    if (!running_.exchange(false)) return;
    if (audio_)
        audio_output_disconnect(audio_, 0, &OpusAudioCapture::AudioCb, this);

    ringCv_.notify_all();
    if (worker_.joinable())
        worker_.join();

    TeardownEncoder();
    ring_.clear();
    cb_ = nullptr;
    audio_ = nullptr;
}

bool OpusAudioCapture::InitEncoder(int bitrateKbps)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        blog(LOG_WARNING, "[obs-web-preview] OpusAudioCapture: no Opus encoder");
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;

    codecCtx_->bit_rate    = static_cast<int64_t>(bitrateKbps) * 1000;
    codecCtx_->sample_rate = sampleRate_;
    codecCtx_->sample_fmt  = AV_SAMPLE_FMT_FLT;
    codecCtx_->time_base   = {1, sampleRate_};

    // Channel layout (stereo / mono / surround). Default to channels_'s
    // canonical layout; FFmpeg picks AV_CHANNEL_LAYOUT_STEREO for 2 etc.
    av_channel_layout_default(&codecCtx_->ch_layout, channels_);

    // 20ms frames — matches RTP packetization, what WebRTC expects.
    // Use the libopus-specific option rather than relying on context defaults.
    av_opt_set    (codecCtx_->priv_data, "application",    "audio",  0);
    av_opt_set_int(codecCtx_->priv_data, "frame_duration", 20,       0);
    av_opt_set_int(codecCtx_->priv_data, "vbr",            0,        0); // CBR
    av_opt_set_int(codecCtx_->priv_data, "dtx",            0,        0); // no DTX
    av_opt_set_int(codecCtx_->priv_data, "packet_loss",    0,        0);

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char err[128] = {0};
        av_strerror(ret, err, sizeof(err));
        blog(LOG_WARNING,
             "[obs-web-preview] avcodec_open2(libopus) failed: %s", err);
        TeardownEncoder();
        return false;
    }

    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (!frame_ || !pkt_) {
        TeardownEncoder();
        return false;
    }
    frame_->format         = AV_SAMPLE_FMT_FLT;
    frame_->nb_samples     = kFrameSamples;
    frame_->sample_rate    = sampleRate_;
    av_channel_layout_copy(&frame_->ch_layout, &codecCtx_->ch_layout);

    if (av_frame_get_buffer(frame_, 0) < 0) {
        TeardownEncoder();
        return false;
    }

    return true;
}

void OpusAudioCapture::TeardownEncoder()
{
    if (frame_) { av_frame_free(&frame_);   frame_ = nullptr; }
    if (pkt_)   { av_packet_free(&pkt_);    pkt_   = nullptr; }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
}

void OpusAudioCapture::AudioCb(void* data, size_t /*mix_idx*/, struct audio_data* audio)
{
    static_cast<OpusAudioCapture*>(data)->OnAudio(audio);
}

void OpusAudioCapture::OnAudio(struct audio_data* audio)
{
    if (!running_ || !audio || !audio->data[0]) return;

    // We requested AUDIO_FORMAT_FLOAT (interleaved) from the converter, so
    // audio->data[0] holds one contiguous LRLRLR... buffer of floats and
    // data[1..] are unused. Copy whole interleaved frames straight into the
    // ring buffer (which is also interleaved).
    const uint32_t frames = audio->frames;
    if (frames == 0) return;
    const float* in = reinterpret_cast<const float*>(audio->data[0]);

    std::lock_guard<std::mutex> lock(ringMutex_);
    const size_t ringFrames = ring_.size() / channels_;

    for (uint32_t i = 0; i < frames; ++i) {
        if (ringCount_ >= ringFrames) {
            blog(LOG_WARNING,
                 "[obs-web-preview] OpusAudioCapture: ring full, dropping samples");
            break;
        }
        const size_t writeBase = ringHead_ * channels_;
        const size_t readBase  = static_cast<size_t>(i) * channels_;
        for (int c = 0; c < channels_; ++c)
            ring_[writeBase + c] = in[readBase + c];
        ringHead_ = (ringHead_ + 1) % ringFrames;
        ringCount_++;
    }

    ringCv_.notify_one();
}

void OpusAudioCapture::WorkerLoop()
{
    std::vector<float> frameBuf(static_cast<size_t>(kFrameSamples) * channels_);

    while (running_) {
        // Block until we have a full 20ms frame, or shutdown.
        {
            std::unique_lock<std::mutex> lock(ringMutex_);
            ringCv_.wait(lock, [this]() {
                return !running_ || ringCount_ >= static_cast<size_t>(kFrameSamples);
            });
            if (!running_) return;

            const size_t ringFrames = ring_.size() / channels_;
            for (int i = 0; i < kFrameSamples; ++i) {
                const size_t readIdx = ringTail_ * channels_;
                for (int c = 0; c < channels_; ++c)
                    frameBuf[i * channels_ + c] = ring_[readIdx + c];
                ringTail_ = (ringTail_ + 1) % ringFrames;
            }
            ringCount_ -= kFrameSamples;
        }

        // Anchor wallclock on first frame so subsequent sends pace at
        // exactly 20ms intervals regardless of OBS audio chunk timing.
        const uint64_t nowNs = os_gettime_ns();
        if (anchorWallNs_ == 0) anchorWallNs_ = nowNs;

        // Re-fill the AVFrame with our interleaved samples. libopus wants
        // interleaved float at AV_SAMPLE_FMT_FLT (which is what we built).
        if (av_frame_make_writable(frame_) < 0) continue;
        std::memcpy(frame_->data[0], frameBuf.data(),
                    frameBuf.size() * sizeof(float));
        frame_->pts = static_cast<int64_t>(samplesEmitted_);

        int ret = avcodec_send_frame(codecCtx_, frame_);
        if (ret < 0) continue;

        while ((ret = avcodec_receive_packet(codecCtx_, pkt_)) == 0) {
            if (cb_)
                cb_(pkt_->data, static_cast<size_t>(pkt_->size), rtpTs_);
            rtpTs_         += static_cast<uint32_t>(kFrameSamples);
            samplesEmitted_ += kFrameSamples;
            av_packet_unref(pkt_);
        }

        // Sleep until the next 20ms boundary in wallclock — keeps the
        // encoder runaway-safe even if the ring momentarily over-fills
        // (e.g., right after start, when a large initial chunk arrives).
        const uint64_t nextNs = anchorWallNs_ +
            samplesEmitted_ * 1'000'000'000ULL / static_cast<uint64_t>(sampleRate_);
        const uint64_t now2Ns = os_gettime_ns();
        if (nextNs > now2Ns) {
            const uint64_t waitNs = std::min<uint64_t>(nextNs - now2Ns,
                                                       1'000'000'000ULL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(waitNs));
        }
    }
}
