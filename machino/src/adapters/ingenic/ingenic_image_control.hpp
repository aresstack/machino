// Ingenic adapter: IImageControl over IMP_ISP_Tuning_* (SDK 1.3.1, T40).
// Only functions that exist in the public header are mapped; everything
// else reports unsupported. Requires EnableSensor + EnableTuning (the
// platform tells us when tuning is active).
#pragma once
#include "ports/iimage_control.hpp"

namespace machino { namespace ingenic {

class IngenicImageControl final : public IImageControl {
public:
    void set_active(bool tuning_up) { active_ = tuning_up; }
    ImageCaps caps() const override;
    Result set(ImageControl c, int value, int& effective) override;
    Result get(ImageControl c, int& value) override;
    Result exposure(ExposureReadback& out) override;
private:
    bool active_ = false;
};

}} // namespace machino::ingenic
