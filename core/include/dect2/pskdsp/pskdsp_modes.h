// PSK demodulator and analyser: the mode numbers (docs/MODE-NUMBERS.md). Not the PSK31 mode (core/src/psk).
#pragma once
namespace dect2::pskd {
inline constexpr int kFamily = 202;
inline constexpr int kStandard = kFamily + 2;          // 204, Engine::setStandard
inline constexpr int kActiveStandard = kStandard - 1;  // 203, RxTelemetry::standard, feed / reset / configure
inline constexpr int kPollSlot = 163;                  // seqs[] in Engine::pollMode, below Engine::kPollSeqSlots
inline bool matchesMode(int family, int activeStandard) { return family == kFamily && activeStandard == kActiveStandard; }
} // namespace dect2::pskd
