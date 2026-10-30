// nRF24L01 Enhanced ShockBurst test signal (GFSK, Gaussian filter BT 0.5, deviation 160 kHz at 250 kbit/s and 1 Mbit/s, 320 kHz at 2 Mbit/s).
// Nothing is transmitted: the samples only exist in memory or in a file. Frames separated by quiet gaps, in white noise, with a carrier offset
// (the crystals of the real chips are 40 ppm off, 100 kHz at 2.4 GHz).
// SynthConfig (makeNrf24Synth): snrDb is Eb/N0 in dB, cfoHz the carrier offset, modeOpt[0] the data rate 0 = 1 Mbit/s, 1 = 2 Mbit/s, 2 = 250 kbit/s,
// 3 = all three in turn; modeOpt[1] 0 CRC-16 frames, 1 CRC-8 frames, 2 both; modeOpt[2] 1 = bursts of random bits (no frame, for the negative tests).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/nrf24/nrf24_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct Nrf24GenConfig {
    double rate = 8e6;
    int rateKbps = 1000;             // 250, 1000, 2000; overridden per frame by rates when that is not empty
    double dialOffsetHz = 0;         // where the channel sits in the samples
    double cfoHz = 0;
    double ebn0Db = 99;              // >= 99: no noise
    double amp = 0.4;
    double gapSec = 0.002;           // quiet time between frames
    double clockPpm = 0;
    std::vector<nrf24::Frame> frames;   // sent one after the other, then the list starts again
    std::vector<int> rates;          // the data rate of each frame (kbit/s); empty: rateKbps for all
    bool randomBits = false;
    uint32_t seed = 1;
};

class Nrf24Generator {
public:
    explicit Nrf24Generator(const Nrf24GenConfig& c);
    ~Nrf24Generator();
    void generate(cf32* out, size_t n);
    // a burst of the frame alone (no noise, no gap), at the config's rate, for tests that feed single frames
    static std::vector<cf32> burst(const nrf24::Frame& f, int rateKbps, double rate, double dialOffsetHz, double amp = 0.4);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// the frames of the test signal: a few devices, CRC-16 and CRC-8, addresses of 3, 4 and 5 bytes, text and binary payloads
std::vector<nrf24::Frame> nrf24TestFrames(int crcMode);
std::unique_ptr<ModeSynth> makeNrf24Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
