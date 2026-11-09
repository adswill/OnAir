// Bluetooth Classic test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Devices send GFSK at 1 Msym/s (Gaussian filter BT 0.5, modulation index 0.32, so +-160 kHz) on channels 2402 + k MHz: inquiry ID packets (the
// access code of the GIAC), inquiry responses (an FHS packet with the device's address and class of device) and the access code of a piconet
// followed by a header and payload bits. Each device sends at its own period and offset, over complex white noise.
//
// SynthConfig (makeBtcSynth): snrDb carrier to noise of a 0 dB device in 1 MHz, cfoHz the radio's tuning error.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/btc/btc_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct BtcGenDevice {
    enum Kind { Id, Response, Piconet } kind = Id;
    uint32_t lap = btc::kGiac;     // Id: the LAP sent (the GIAC for an inquiry); Piconet: the master's LAP
    btc::Fhs fhs;                  // Response
    int hdrClk = 0;                // Response: the whitening seed (CLK1-6)
    int channel = 0;               // k: 2402 + k MHz
    double offsetHz = 0;           // carrier offset of this device (cheap crystals drift up to about 75 kHz)
    double levelDb = 0;            // relative to the 0 dB device
    double period = 0.5, phase = 0;   // first sent at phase, then every period seconds
    double truncate = 1;           // fraction of the burst sent
};

struct BtcGenConfig {
    double rate = 20e6;
    double centerHz = 2441e6;
    double snrDb = 30;             // carrier to noise of a 0 dB device in 1 MHz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.1;
    uint32_t seed = 1;
    std::vector<BtcGenDevice> devs;
    double wifiAt = -1, wifiLen = 300e-6, wifiDb = 10;   // a 20 MHz wide burst every 1 s from this time (s), -1 none
};

class BtcGenerator {
public:
    explicit BtcGenerator(const BtcGenConfig& c);
    ~BtcGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    int sent(size_t dev) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The built-in scene: a phone and a headset answering inquiries, a scanning laptop (GIAC IDs) and two piconets, on channels around 2441 MHz
std::vector<BtcGenDevice> btcTestScene();

std::unique_ptr<ModeSynth> makeBtcSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
