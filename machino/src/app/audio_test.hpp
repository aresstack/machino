// `machino --audio-test`: bring the codec up from a shell and prove, with a
// multimeter or a pair of ears, what the board's audio connector carries -
// without the daemon, without writing to flash.
//
//   bias   [seconds]          microphone path on (MICBIAS up), prints the level once a second
//   record <file.wav> [s]     microphone -> WAV (s16 mono)
//   tone   [seconds] [hz]     sine on the output - measure AC between OUT and GND
//   play   <file.wav>         WAV (s16 mono, 8/16 kHz) -> output
//
// Options (before the mode): --rate 8000|16000, --volume 0..100, --gain 0..31.
// The daemon must not hold the codec at the same time (streamerctl stop).
//
// Vendor-neutral: the caller hands in the factories. The helpers below are
// pure and host-tested.
#pragma once
#include "ports/iaudio.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace machino { namespace app {

using AudioInFactory  = std::function<std::unique_ptr<IAudioIn>(const AudioParams&)>;
using AudioOutFactory = std::function<std::unique_ptr<IAudioOut>(const AudioParams&)>;

int run_audio_test(int argc, char** argv, int first, const AudioInFactory& in, const AudioOutFactory& out);

// 44-byte canonical RIFF/WAVE header for s16le mono.
std::vector<uint8_t> wav_header(int rate, uint32_t samples);
// Parses a canonical s16 mono WAV; false with `err` for anything else.
bool wav_parse(const std::vector<uint8_t>& file, int& rate, std::vector<int16_t>& pcm, std::string& err);
// RMS and peak of a frame in dBFS (-120 for digital silence).
void level_dbfs(const std::vector<int16_t>& pcm, double& rms_db, double& peak_db);
// `n` samples of a sine at `hz`, amplitude `amp` (0..1), continuing at `phase` (updated).
void sine(std::vector<int16_t>& out, size_t n, int rate, int hz, double amp, double& phase);

}} // namespace machino::app
