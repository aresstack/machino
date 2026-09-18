// Ingenic adapter: IEncoder over RAII EncoderGroup + EncoderChannel and a
// StreamReceiver that exists only between start() and stop().
#pragma once
#include "adapters/ingenic/imp_sessions.hpp"
#include "core/config.hpp"
#include "ports/iencoder.hpp"
#include <memory>

namespace machino { namespace ingenic {

class IngenicEncoder final : public IEncoder {
public:
    static std::unique_ptr<IngenicEncoder> create(int chn, const StreamConfig& sc);
    ~IngenicEncoder() override = default;   // receiver, channel, group - in that order

    Result start() override;
    Result stop() override;
    Result fetch(AccessUnit& out, int timeout_ms) override;
    void   request_idr() override { if (rx_) rx_->request_idr(); }
    int    channel() const override { return chan_->chn(); }

private:
    IngenicEncoder(std::unique_ptr<imp::EncoderGroup> g, std::unique_ptr<imp::EncoderChannel> c)
        : group_(std::move(g)), chan_(std::move(c)) {}
    std::unique_ptr<imp::EncoderGroup>   group_;
    std::unique_ptr<imp::EncoderChannel> chan_;
    std::unique_ptr<imp::StreamReceiver> rx_;
};

}} // namespace machino::ingenic
