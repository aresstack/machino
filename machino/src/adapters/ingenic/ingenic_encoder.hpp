// Ingenic adapter: IMP H.264 encoder channel + group (group id == channel id).
// RAII: UnRegister/Destroy in the dtor, in the proven reverse order.
#pragma once
#include "core/config.hpp"
#include "ports/iencoder.hpp"
#include <memory>

namespace machino { namespace ingenic {

class IngenicEncoder final : public IEncoder {
public:
    static std::unique_ptr<IngenicEncoder> create(int chn, const StreamConfig& sc);
    ~IngenicEncoder() override;

    Result start() override;
    Result stop() override;
    Result fetch(AccessUnit& out, int timeout_ms) override;
    void   request_idr() override;
    int    channel() const override { return chn_; }

private:
    explicit IngenicEncoder(int chn) : chn_(chn) {}
    int  chn_;
    bool group_     = false;
    bool channel_   = false;
    bool registered_= false;
    bool receiving_ = false;
    bool idr_pending_ = false;
};

}} // namespace machino::ingenic
