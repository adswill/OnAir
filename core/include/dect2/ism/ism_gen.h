// ISM sensors test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Every test device builds its frame from the layout its rtl_433 decoder documents (the same file the decoder cites) and sends it with
// that file's timing: OOK on / off, or FSK with the upper tone as the mark. The devices of a scene transmit at their own periods and
// offsets from the dial frequency, over complex white noise.
//
// SynthConfig (makeIsmSynth):
//   snrDb       carrier to noise of a 0 dB device, the noise taken in 100 kHz
//   cfoHz       the radio's tuning error: every device moves by it
//   modeOpt[0]  scene: 0 the 433.92 MHz scene (ismTestScene(0)), 1 the 868 MHz scene, 2 Z-Wave on 868.4 MHz, 3 KNX RF, HomeMatic, EnOcean and DASH7 on 868.3 MHz,
//               4 the 3.9 devices (X10, Insteon, inFactory, WH31E, Nissan and GM TPMS, Efergy Optical, EMT7110, SCM+) side by side around the dial
//   modeOpt[1]  1: a strong continuous carrier 300 kHz above the dial (30 dB over a 0 dB device)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "ism_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The test devices' protocols (ismEncode's names)
enum class IsmTx {
    Nexus, Tx141th, AcuriteTower, Wh1080, F007th, OregonV3, OregonV21, Schrader, Ev1527, Toyota, Renault, Ford, Efergy, CurrentCost,
    Bresser6in1, Bresser5in1, Tx29, Wh24, MbusT, ZwaveR1, ZwaveR2, ZwaveR3, KnxRf, HomeMatic, EnOcean, Dash7,
    X10Rf, Insteon, Infactory, Wh31e, Nissan, GmTpms, EfergyOptical, Emt7110, ScmPlus,   // 3.9
    Count
};
const char* ismTxName(IsmTx p);

// One frame (with the repeats the device sends in one burst) as a pulse train. k: the transmission number; the temperature (or the
// pressure, the power) of some devices steps with it, so the history moves.
IsmTrain ismEncode(IsmTx p, int k);

// What the receiver must report for a device's transmission k: the model, the ID and one field. ismTruthMatches() checks a reading.
struct IsmTruth { const char* model; const char* id; const char* field; double value; double tol; };
IsmTruth ismTruth(IsmTx p, int k);
bool ismTruthMatches(const IsmReading& r, IsmTx* which = nullptr, int* k = nullptr);   // a reading of one of the test devices, with right values

// A Z-Wave frame (rate 1 = R1, 2 = R2, 3 = R3) with any payload; badCheck damages the checksum / CRC
IsmTrain ismEncodeZwave(int rate, uint32_t home, uint8_t src, uint8_t dst, const std::vector<uint8_t>& payload, int k = 0, bool badCheck = false);

struct IsmGenDevice {
    IsmTx proto = IsmTx::Nexus;
    double offsetHz = 0;         // from the dial frequency
    double levelDb = 0;          // relative to the 0 dB device
    double devHz = 0;            // FSK deviation (0: the protocol's usual)
    double period = 4, phase = 0;   // first sent at phase, then every period seconds
    double truncate = 1;         // fraction of each burst sent (below 1: the rest is cut off, as a packet that was not heard to its end)
};

struct IsmGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double snrDb = 30;           // carrier to noise of a 0 dB device in 100 kHz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.1;            // carrier amplitude of a 0 dB device
    double carrierHz = 0, carrierDb = 0;   // a continuous carrier at carrierHz from the dial, carrierDb over a 0 dB device (0: none)
    uint32_t seed = 1;
    std::vector<IsmGenDevice> devs;
};

class IsmGenerator {
public:
    explicit IsmGenerator(const IsmGenConfig& c);
    ~IsmGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    int sent(size_t dev) const;  // transmissions started so far by device dev
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Scene 4: the 3.9 devices around one dial frequency (their real bands differ: 310 / 433 / 868 / 915 MHz).
// Scene 0: 433.92 MHz, scene 1: 868 MHz (Bresser and LaCrosse on 868.3, wireless M-Bus on 868.95). The offsets are from the dial
// frequency of the scene (433.92 / 868.3 MHz) and include the drift of cheap sensors.
std::vector<IsmGenDevice> ismTestScene(int scene);

std::unique_ptr<ModeSynth> makeIsmSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
