// Ingenic adapter: IMP FrameSource channel (RAII: destroyed in the dtor).
#pragma once
#include "core/config.hpp"
#include "ports/iframesource.hpp"
#include <memory>

namespace machino { namespace ingenic {

class IngenicFrameSource final : public IFrameSource {
public:
    static std::unique_ptr<IngenicFrameSource> create(int chn, const StreamConfig& sc, const SensorConfig& sensor);
    ~IngenicFrameSource() override;

    Result enable() override;
    Result disable() override;
    int    channel() const override { return chn_; }

private:
    explicit IngenicFrameSource(int chn) : chn_(chn) {}
    int  chn_;
    bool enabled_ = false;
};

}} // namespace machino::ingenic
