// PSK demodulator and analyser: the mode numbers (docs/MODE-NUMBERS.md). Not the PSK31 mode (core/src/psk).
#pragma once
namespace dect2::pskd {
inline constexpr int kPskdFamily = 202;
inline constexpr int kPskdStandard = kPskdFamily + 2;          // 204, Engine::setStandard
inline constexpr int kPskdActiveStandard = kPskdStandard - 1;  // 203, RxTelemetry::standard, feed / reset / configure
inline constexpr int kPskdPollSlot = 163;                  // seqs[] in Engine::pollMode, below Engine::kPollSeqSlots
inline bool matchesMode(int family, int activeStandard) { return family == kPskdFamily && activeStandard == kPskdActiveStandard; }
} // namespace dect2::pskd
