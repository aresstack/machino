// Application policy for image controls and the transparent latency preset.
// It depends only on ports and core services; no vendor type crosses here.
#pragma once
#include "core/config.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/power/apply.hpp"
#include "core/stream_hub.hpp"
#include "ports/iimage_control.hpp"
#include "ports/iplatform.hpp"
#include <array>
#include <mutex>

namespace machino { namespace media {

struct ResolvedLatency {
    LatencyProfile profile = LatencyProfile::Normal;
    int gop = 40;
    int framesource_buffers = 2;
    int encoder_buffers = 0;       // 0 = vendor default
    int consumer_queue_depth = 4;
};

struct TuningState {
    ResolvedLatency latency;
    LatencySettings requested_latency;
    std::array<int, (int)ImageControl::COUNT> image_requested{};
    std::array<int, (int)ImageControl::COUNT> image_effective{};
};

class TuningService {
public:
    TuningService(lifecycle::PipelineManager& pipeline, IPlatform& platform, StreamHub& hub,
                  const EffectiveStream& base, const ImageSettings& image, const LatencySettings& latency);
    ~TuningService();
    TuningService(const TuningService&) = delete;
    TuningService& operator=(const TuningService&) = delete;

    ImageCaps image_caps() const;
    TuningState state() const;
    LatencyStats latency_stats() const { return hub_.latency(); }
    Result exposure(ExposureReadback& out);

    power::ApplyResult set_image(ImageControl c, int value);
    power::ApplyResult set_latency_profile(LatencyProfile p);
    power::ApplyResult set_gop(int frames);
    power::ApplyResult set_framesource_buffers(int n);
    power::ApplyResult set_encoder_buffers(int n);
    power::ApplyResult set_queue_depth(int n);

    // Re-resolve a relative preset (low: GOP == current stream fps) after a
    // performance/stream-FPS change. Explicit values still win.
    power::ApplyResult refresh_after_stream_change();

private:
    ResolvedLatency resolve_locked(const EffectiveStream& stream) const;
    power::ApplyResult apply_latency(const ResolvedLatency& target, int requested, const char* what);
    void apply_images_after_start();
    void load_image_settings(const ImageSettings& image);

    lifecycle::PipelineManager& pipeline_;
    IPlatform& platform_;
    StreamHub& hub_;
    IImageControl* image_ = nullptr;
    ImageCaps image_caps_{};
    EffectiveStream base_;

    mutable std::mutex m_;
    LatencySettings latency_;
    ResolvedLatency resolved_;
    std::array<int, (int)ImageControl::COUNT> requested_{};
    std::array<int, (int)ImageControl::COUNT> effective_{};
};

}} // namespace machino::media
