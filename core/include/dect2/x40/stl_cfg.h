// Settings of the STL link mode (x40 variant Stl, docs/STL-4.1.md): analogue studio-to-transmitter links. One object for the process, read by the decoder at every block
// (the interface thread writes, the analysis thread reads); the mode needs none of them for its first run: the defaults are a +-60 kHz composite link in a 250 kHz channel.
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>

namespace dect2 {

struct StlConfig {
    std::atomic<double> deviationHz{60000};      // the deviation that means 100 % modulation of the link (sets the sound level; the readings are in kHz as they arrive)
    std::atomic<double> channelWidthHz{280000};  // the whole pass band of the channel filter (250 / 500 kHz channels; 0 = the receiver's default)
    std::atomic<int> deemphasisUs{75};           // 75 (the US), 50, or 0 for none (flat programme audio)
    std::atomic<int> mode{0};                    // 0 automatic (stereo when the pilot is locked), 1 composite stereo whenever a pilot is there, 2 mono
    std::atomic<int> scaListen{-1};              // -1 programme, 0 SCA 67 kHz, 1 SCA 92 kHz
    std::atomic<int> rdsRegion{2};               // 0 automatic, 1 RDS (Europe), 2 RBDS (US): the meanings of the programme types and call letters
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false};
    std::atomic<bool> silent{false};             // do not open the sound device (tests); read when the decoder is built
    std::atomic<float> peakDbfs{-120.f}, rmsDbfs{-120.f};   // out: the sound level of the last half second (the meters)
    std::function<void(const float* left, const float* right, size_t n)> audioTap;   // tests: the demodulated audio at 48 kHz; set before the decoder is built
};
StlConfig& stlConfig();

} // namespace dect2
