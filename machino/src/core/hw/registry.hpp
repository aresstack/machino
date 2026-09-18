// Machino core: registry of known platforms, sensors and board profiles.
// The registry holds no data of its own; profiles are registered at startup
// (src/profiles) or loaded from files. Lookup is deterministic: exactly one
// profile is selected, no probing.
#pragma once
#include "core/hw/descriptors.hpp"
#include <vector>

namespace machino { namespace hw {

class Registry {
public:
    void add_platform(const PlatformDescriptor& p) { platforms_.push_back(p); }
    void add_sensor(const SensorDescriptor& s)     { sensors_.push_back(s); }
    void add_board(const BoardProfile& b)          { boards_.push_back(b); }

    const PlatformDescriptor* platform(const std::string& id) const {
        for (const auto& p : platforms_) if (p.id() == id) return &p;
        return nullptr;
    }
    const SensorDescriptor* sensor(const std::string& model) const {
        for (const auto& s : sensors_) if (s.model == model) return &s;
        return nullptr;
    }
    const BoardProfile* board(const std::string& board_id) const {
        for (const auto& b : boards_) if (b.board_id == board_id) return &b;
        return nullptr;
    }
    const std::vector<BoardProfile>& boards() const { return boards_; }
    size_t size() const { return platforms_.size() + sensors_.size() + boards_.size(); }

private:
    std::vector<PlatformDescriptor> platforms_;
    std::vector<SensorDescriptor>   sensors_;
    std::vector<BoardProfile>       boards_;
};

}} // namespace machino::hw
