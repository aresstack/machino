// Ingenic adapter: hardware JPEG over its own FrameSource + Encoder channel
// (JPEG profile) bound together, self-contained. Unlike the H.264 path the
// pipeline does not orchestrate framesource/encoder/bind for JPEG - create()
// builds the whole chain and StartRecvPic, so the returned object can capture
// immediately, and its destructor tears the chain down (no stale channel).
#pragma once
#include "adapters/ingenic/imp_sessions.hpp"
#include "core/config.hpp"
#include "ports/ijpeg.hpp"
#include <memory>
#include <mutex>

namespace machino { namespace ingenic {

class IngenicJpegEncoder final : public IJpegEncoder {
public:
    // native_w/native_h decide whether the FrameSource scaler is used, exactly
    // as for the video channels; the geometry itself is validated in the core.
    static std::unique_ptr<IngenicJpegEncoder> create(int chn, const JpegParams& p, int native_w, int native_h);
    ~IngenicJpegEncoder() override;

    Result capture(std::vector<uint8_t>& out, int timeout_ms) override;
    int    channel() const override { return chn_; }

private:
    IngenicJpegEncoder(int chn, std::unique_ptr<imp::FrameSourceChannel> fs,
                       std::unique_ptr<imp::EncoderGroup> g, std::unique_ptr<imp::EncoderChannel> c,
                       std::unique_ptr<imp::Binding> b)
        : chn_(chn), fs_(std::move(fs)), group_(std::move(g)), chan_(std::move(c)), bind_(std::move(b)) {}

    int  chn_;
    bool recv_ = false;
    std::unique_ptr<imp::FrameSourceChannel> fs_;
    std::unique_ptr<imp::EncoderGroup>       group_;
    std::unique_ptr<imp::EncoderChannel>     chan_;
    std::unique_ptr<imp::Binding>            bind_;
    std::mutex sdk_m_;
};

}} // namespace machino::ingenic
