// Ingenic adapter: IFrameSource over an RAII FrameSourceChannel.
#pragma once
#include "adapters/ingenic/imp_sessions.hpp"
#include "core/config.hpp"
#include "ports/iframesource.hpp"
#include <memory>

namespace machino { namespace ingenic {

class IngenicFrameSource final : public IFrameSource {
public:
    static std::unique_ptr<IngenicFrameSource> create(int chn, const StreamConfig& sc, const SensorConfig& sensor);
    ~IngenicFrameSource() override = default;

    Result enable() override  { return chan_->enable(); }
    Result disable() override { return chan_->disable(); }
    int    channel() const override { return chan_->chn(); }

private:
    explicit IngenicFrameSource(std::unique_ptr<imp::FrameSourceChannel> c) : chan_(std::move(c)) {}
    std::unique_ptr<imp::FrameSourceChannel> chan_;
};

}} // namespace machino::ingenic
