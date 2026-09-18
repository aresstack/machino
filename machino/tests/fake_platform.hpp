// Fake adapter for lifecycle/performance tests: records the exact call
// order, can fail at a chosen stage, produces one synthetic frame per fetch,
// and models the M5 controls (sensor fps with read-back, live bitrate/fps).
#pragma once
#include "ports/iplatform.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace test {

struct CallLog {
    std::mutex m;
    std::vector<std::string> calls;
    void add(const std::string& s) { std::lock_guard<std::mutex> lk(m); calls.push_back(s); }
    int count(const std::string& s) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& c : calls) if (c == s) ++n; return n; }
    std::vector<std::string> snapshot() { std::lock_guard<std::mutex> lk(m); return calls; }
};

class FakeFrameSource final : public IFrameSource {
public:
    FakeFrameSource(CallLog& l, int chn, int fps) : log_(l), chn_(chn) { (void)fps; log_.add("fs.create"); }
    ~FakeFrameSource() override { log_.add("fs.destroy"); }
    Result enable() override  { log_.add("fs.enable");  return Result::ok(); }
    Result disable() override { log_.add("fs.disable"); return Result::ok(); }
    int channel() const override { return chn_; }
private:
    CallLog& log_; int chn_;
};

class FakeEncoder final : public IEncoder {
public:
    FakeEncoder(CallLog& l, int chn, bool fail_start, bool live_bitrate) : log_(l), chn_(chn), fail_start_(fail_start), live_bitrate_(live_bitrate) { log_.add("enc.create"); }
    ~FakeEncoder() override { log_.add("enc.destroy"); }
    Result start() override { log_.add("enc.start"); return fail_start_ ? Result::error(-7) : Result::ok(); }
    Result stop() override  { log_.add("enc.stop");  return Result::ok(); }
    Result fetch(AccessUnit& out, int) override {
        out.data.assign({0, 0, 0, 1, 0x65, 0x11, 0x22});
        out.key = true; out.pts_us += 50000;
        return Result::ok();
    }
    void request_idr() override {}
    int channel() const override { return chn_; }
    Result set_bitrate(int kbps, int& effective) override {
        log_.add("enc.set_bitrate@" + std::to_string(kbps));
        if (!live_bitrate_) { effective = -1; return Result::unsupported(); }
        effective = kbps; bitrate = kbps; return Result::ok();
    }
    Result set_fps(int fps, int& effective) override { log_.add("enc.set_fps@" + std::to_string(fps)); effective = fps; return Result::ok(); }
    int bitrate = 0;
private:
    CallLog& log_; int chn_; bool fail_start_, live_bitrate_;
};

class FakePlatform final : public IPlatform {
public:
    enum class FailAt { None, BringUp, FrameSource, Encoder, Bind, EncoderStart };
    explicit FakePlatform(CallLog& l, IPowerControl* p = nullptr) : log_(l) { power_ptr = p; }
    FailAt fail_at = FailAt::None;
    int    fail_times = 0;
    bool   sensor_fps_supported = true;     // set_sensor_fps works with read-back
    bool   live_bitrate = true;
    int    sensor_fps_effective = -1;        // what the "hardware" reports
    IPowerControl* power_ptr = nullptr;

    const char* name() const override { return "fake"; }
    CapabilitySet capabilities() const override {
        CapabilitySet c;
        c.video.h264 = Cap::Supported;
        c.video.fps = RangeCap{Cap::Supported, -1, -1, ApplyMode::PipelineRestart};
        c.video.bitrate = RangeCap{Cap::Supported, 100, 20000, live_bitrate ? ApplyMode::Live : ApplyMode::PipelineRestart};
        c.sensor.configurable_fps = sensor_fps_supported ? Cap::Supported : Cap::Unsupported;
        c.sensor.fps = RangeCap{sensor_fps_supported ? Cap::Supported : Cap::Unsupported, -1, -1, sensor_fps_supported ? ApplyMode::Live : ApplyMode::Unsupported};
        c.isp.available = Cap::Supported; c.encoder.hardware = Cap::Supported;
        return c;
    }
    Result bring_up() override {
        log_.add("platform.bring_up");
        if (take(FailAt::BringUp)) return Result::error(-1);
        up_ = true; return Result::ok();
    }
    void tear_down() override { if (up_) { log_.add("platform.tear_down"); up_ = false; sensor_fps_effective = -1; } }
    std::unique_ptr<IFrameSource> create_framesource(int chn, const EffectiveStream& s) override {
        if (take(FailAt::FrameSource)) return nullptr;
        last_stream = s;
        return std::make_unique<FakeFrameSource>(log_, chn, s.fps);
    }
    std::unique_ptr<IEncoder> create_encoder(int chn, const EffectiveStream&) override {
        if (take(FailAt::Encoder)) return nullptr;
        return std::make_unique<FakeEncoder>(log_, chn, take(FailAt::EncoderStart), live_bitrate);
    }
    Result bind(IFrameSource&, IEncoder&) override   { log_.add("bind");   return take(FailAt::Bind) ? Result::error(-3) : Result::ok(); }
    Result unbind(IFrameSource&, IEncoder&) override { log_.add("unbind"); return Result::ok(); }
    int64_t timestamp_us() override { return 0; }
    Result set_sensor_fps(int fps, int& effective) override {
        log_.add("platform.set_sensor_fps@" + std::to_string(fps));
        if (!up_) { effective = -1; return Result::busy(); }
        if (!sensor_fps_supported) { effective = -1; return Result::unsupported(); }
        sensor_fps_effective = fps; effective = fps; return Result::ok();
    }
    Result get_sensor_fps(int& fps) override { fps = sensor_fps_effective; return (up_ && fps > 0) ? Result::ok() : Result::busy(); }
    IPowerControl* power() override { return power_ptr; }
    bool is_up() const { return up_; }
    EffectiveStream last_stream{};
private:
    bool take(FailAt f) { if (fail_at != f) return false; if (fail_times > 0) { --fail_times; return true; } return false; }
    CallLog& log_; bool up_ = false;
};

class FakeTimer final : public lifecycle::IGraceTimer {
public:
    void arm(int ms) override { armed = true; last_ms = ms; ++arm_count; }
    void disarm() override { armed = false; ++disarm_count; }
    bool armed = false; int last_ms = -1; int arm_count = 0, disarm_count = 0;
};

}} // namespace machino::test
