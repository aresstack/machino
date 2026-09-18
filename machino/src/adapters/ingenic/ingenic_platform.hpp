// Ingenic adapter: IMP SDK 1.3.1 (T40/T40NN) platform. Owns the sensor/ISP
// system state; frame-source and encoder channels are separate RAII objects.
#pragma once
#include "adapters/ingenic/board_wiring.hpp"
#include "ports/iplatform.hpp"

#include <imp/imp_isp.h>

namespace machino { namespace ingenic {

class IngenicPlatform final : public IPlatform {
public:
    IngenicPlatform(const SensorConfig& sensor, const BoardWiring& wiring);
    ~IngenicPlatform() override;

    const char* name() const override { return "ingenic-imp-1.3.1"; }
    Result bring_up() override;
    void   tear_down() override;

    std::unique_ptr<IFrameSource> create_framesource(int chn, const StreamConfig& sc) override;
    std::unique_ptr<IEncoder>     create_encoder(int chn, const StreamConfig& sc) override;
    Result bind(IFrameSource& fs, IEncoder& enc) override;
    Result unbind(IFrameSource& fs, IEncoder& enc) override;
    int64_t timestamp_us() override;

private:
    SensorConfig  sensor_;
    BoardWiring   wiring_;
    IMPSensorInfo info_{};
    enum class Stage { Down, IspOpen, SensorAdded, SensorEnabled, SystemInit, TuningOn } stage_ = Stage::Down;
};

}} // namespace machino::ingenic
