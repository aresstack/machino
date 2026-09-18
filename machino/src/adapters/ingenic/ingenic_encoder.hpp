// Ingenic adapter: IEncoder over RAII EncoderGroup + EncoderChannel and a
// StreamReceiver that exists only between start() and stop().
// M5: live bitrate (Get/SetChnAttrRcMode) and encoder frame rate (SetChnFrmRate).
#pragma once
#include "adapters/ingenic/imp_sessions.hpp"
#include "core/config.hpp"
#include "ports/iencoder.hpp"
#include <memory>

namespace machino { namespace ingenic {

class IngenicEncoder final : public IEncoder {
public:
    static std::unique_ptr<IngenicEncoder> create(int chn, const EffectiveStream& sc);
    ~IngenicEncoder() override = default;

    Result start() override;
    Result stop() override;
    Result fetch(AccessUnit& out, int timeout_ms) override;
    void   request_idr() override { if (rx_) rx_->request_idr(); }
    int    channel() const override { return chan_->chn(); }

    Result set_bitrate(int kbps, int& effective) override;
    Result set_fps(int fps, int& effective) override;

private:
    IngenicEncoder(std::unique_ptr<imp::EncoderGroup> g, std::unique_ptr<imp::EncoderChannel> c, RcMode rc)
        : group_(std::move(g)), chan_(std::move(c)), rc_(rc) {}
    std::unique_ptr<imp::EncoderGroup>   group_;
    std::unique_ptr<imp::EncoderChannel> chan_;
    std::unique_ptr<imp::StreamReceiver> rx_;
    RcMode rc_;
};

}} // namespace machino::ingenic
