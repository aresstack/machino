// Fake adapter for lifecycle tests: records the exact call order, can fail at
// a chosen stage, and produces one synthetic frame per fetch.
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
    FakeFrameSource(CallLog& l, int chn) : log_(l), chn_(chn) { log_.add("fs.create"); }
    ~FakeFrameSource() override { log_.add("fs.destroy"); }
    Result enable() override  { log_.add("fs.enable");  return Result::ok(); }
    Result disable() override { log_.add("fs.disable"); return Result::ok(); }
    int channel() const override { return chn_; }
private:
    CallLog& log_; int chn_;
};

class FakeEncoder final : public IEncoder {
public:
    FakeEncoder(CallLog& l, int chn, bool fail_start) : log_(l), chn_(chn), fail_start_(fail_start) { log_.add("enc.create"); }
    ~FakeEncoder() override { log_.add("enc.destroy"); }
    Result start() override { log_.add("enc.start"); return fail_start_ ? Result::error(-7) : Result::ok(); }
    Result stop() override  { log_.add("enc.stop");  return Result::ok(); }
    Result fetch(AccessUnit& out, int) override {
        out.data.assign({0, 0, 0, 1, 0x65, 0x11, 0x22});   // IDR
        out.key = true; out.pts_us += 50000;
        return Result::ok();
    }
    void request_idr() override {}
    int channel() const override { return chn_; }
private:
    CallLog& log_; int chn_; bool fail_start_;
};

class FakePlatform final : public IPlatform {
public:
    enum class FailAt { None, BringUp, FrameSource, Encoder, Bind, EncoderStart };
    explicit FakePlatform(CallLog& l) : log_(l) {}
    FailAt fail_at = FailAt::None;
    int    fail_times = 0;     // fail this many times, then succeed

    const char* name() const override { return "fake"; }
    CapabilitySet capabilities() const override { return CapabilitySet{}; }
    Result bring_up() override {
        log_.add("platform.bring_up");
        if (take(FailAt::BringUp)) return Result::error(-1);
        up_ = true; return Result::ok();
    }
    void tear_down() override { if (up_) { log_.add("platform.tear_down"); up_ = false; } }
    std::unique_ptr<IFrameSource> create_framesource(int chn, const EffectiveStream&) override {
        if (take(FailAt::FrameSource)) return nullptr;
        return std::make_unique<FakeFrameSource>(log_, chn);
    }
    std::unique_ptr<IEncoder> create_encoder(int chn, const EffectiveStream&) override {
        if (take(FailAt::Encoder)) return nullptr;
        return std::make_unique<FakeEncoder>(log_, chn, take(FailAt::EncoderStart));
    }
    Result bind(IFrameSource&, IEncoder&) override   { log_.add("bind");   return take(FailAt::Bind) ? Result::error(-3) : Result::ok(); }
    Result unbind(IFrameSource&, IEncoder&) override { log_.add("unbind"); return Result::ok(); }
    int64_t timestamp_us() override { return 0; }
    bool is_up() const { return up_; }
private:
    bool take(FailAt f) {
        if (fail_at != f) return false;
        if (fail_times > 0) { --fail_times; return true; }
        return false;
    }
    CallLog& log_; bool up_ = false;
};

class FakeTimer final : public lifecycle::IGraceTimer {
public:
    void arm(int ms) override { armed = true; last_ms = ms; ++arm_count; }
    void disarm() override { armed = false; ++disarm_count; }
    bool armed = false; int last_ms = -1; int arm_count = 0, disarm_count = 0;
};

}} // namespace machino::test
