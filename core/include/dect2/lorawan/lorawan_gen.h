// LoRaWAN test signal: LoRaWAN frames on several channels of a regional plan at once (different spreading factors, 125 / 250 / 500 kHz, gateway
// downlinks with inverted IQ), over complex white noise. The payloads are made-up bytes standing for encrypted data; the MICs are made up too.
// Nothing is transmitted: the samples only exist in memory or in a file. Uses the LoRa modulator of mesh_lora.h (TxFrame).
//
// SynthConfig (makeLorawanSynth): snrDb = SNR of a 125 kHz frame in its bandwidth; cfoHz added to every frame; sroPpm = the transmitters' clock error.
//   modeOpt[0]  the plan (lorawan::Plan: 0 EU868, 1 US915, 2 AU915, 3 AS923-1 ... 7 EU433)
// The plans with an FSK channel (EU868 868.8 MHz, AS923-1 924.8 MHz) also get LoRaWAN FSK frames (DR7, 50 kbit/s GFSK, lorawan_fsk.h) on it.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/mesh/mesh_lora.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct LorawanGenFrame {
    double mhz = 868.1;          // the channel's centre
    lora::Params p;
    std::vector<uint8_t> payload;   // the PHY payload (lorawan::build)
    bool inverted = false;       // a gateway's frame
    double gapSec = 0.2;         // silence after it
    double snrOffDb = 0;
    double cfoHz = 0;
    double startSec = -1;        // >= 0: fixed start time (frames overlap on different channels); else after the previous frame
};

struct LorawanFskFrame {         // a LoRaWAN FSK (DR7) frame
    double mhz = 868.8;
    std::vector<uint8_t> payload;   // the PHY payload (lorawan::build)
    double startSec = 0.1;       // from the start of the scene (again in every loop)
    double snrOffDb = 0;         // level against a LoRa frame of the scene
    double cfoHz = 0;
    double bt = 1.0;             // Gaussian BT; 0 plain FSK (an SX1276 as LoRaMac-node sets it up)
    uint8_t preamble = 0x55;     // 0x55 (SX126x) or 0xAA (SX1276)
};

struct LorawanGenConfig {
    double rate = 4e6;
    double dialMhz = 868.3;      // the frequency the samples are centred on, apart from dialOffsetHz
    double dialOffsetHz = 0;     // where the dial sits in the samples
    double snrDb = 20;           // SNR of a 125 kHz frame in its bandwidth; >= 99: no noise
    double cfoHz = 0;
    double sroPpm = 0;
    double amp = 0.3;
    double leadSec = 0.05;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<LorawanGenFrame> frames;
    std::vector<LorawanFskFrame> fsk;
    double fskPeriodSec = 0;     // loop period of the FSK frames; 0: the scene's length (lorawanSceneSeconds) and 0.3 s
};

class LorawanGenerator {
public:
    explicit LorawanGenerator(const LorawanGenConfig& c);
    ~LorawanGenerator();
    void generate(cf32* out, size_t n);
    double endSec() const;       // when the last scheduled frame ends
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The scene of a plan: about ten frames on the plan's channels around its dial frequency (uplinks, a join request, a gateway's join accept and
// downlinks with MAC commands). Frames follow each other, some overlap on different channels.
std::vector<LorawanGenFrame> lorawanTestSchedule(int plan);
double lorawanSceneSeconds(const std::vector<LorawanGenFrame>& v);
// The FSK frames of a plan's scene (empty when the plan has no FSK channel): an uplink while LoRa frames are on the air on other channels, a
// join request from an unshaped SX1276-style transmitter (0xAA preamble), and the gateway's acknowledgement with a LinkADRReq one second later
std::vector<LorawanFskFrame> lorawanFskTestSchedule(int plan);
double lorawanFskSceneSeconds(const std::vector<LorawanFskFrame>& v);

std::unique_ptr<ModeSynth> makeLorawanSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
