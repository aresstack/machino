// Machino core: parse a board profile from flat "key = value" text.
//
//   board_id   = t40nn-imx307-board-a
//   platform   = ingenic-t40nn
//   sensor     = imx307
//   i2c_bus    = 1
//   i2c_addr   = 0x1a
//   mclk       = 1
//   reset_gpio = 91          # or "none"
//   pwdn_gpio  = 0
//   mode       = 1920x1080@20
//   hardware_verified = yes
//   notes      = ...
//
// Missing wiring keys stay unset (fail-closed). Unknown keys are reported.
#pragma once
#include "core/hw/descriptors.hpp"
#include <string>

namespace machino { namespace hw {

// Returns false with `err` on a syntax/value error. `warnings` collects
// unknown keys (one line each) without failing.
bool parse_board_profile(const std::string& text, BoardProfile& out, std::string& err, std::string* warnings = nullptr);
bool load_board_profile_file(const char* path, BoardProfile& out, std::string& err, std::string* warnings = nullptr);

// "1920x1080@20" -> SensorMode
bool parse_mode(const std::string& s, SensorMode& out);

}} // namespace machino::hw
