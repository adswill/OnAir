// CDR test signal: a GY/T 268 transmitter as complex baseband. Nothing is transmitted: the samples only exist in memory or in a file.
//
// The multiplex carries two audio services and, optionally, a text service:
//   service 0x1001  audio, 48 kHz stereo, about 55 % of the service data channel, 30 audio units per logical frame
//   service 0x1002  audio, 32 kHz mono, about 35 %, 20 audio units per logical frame
//   service 0x1003  data: one data broadcast unit (Table 12 type 160) with a short UTF-8 text in every logical frame
// The audio units are filler bytes, not DRA+ (the codec is not openly specified): a receiver can list and measure them but not play them.
// GY/T 268.2 leaves the codes of the audio algorithm field to later documents; the generator writes 0 there. The service description
// channel carries the service multiplex configuration table and the network information table ("OnAir CDR", country CHN) in every frame.
//
// Options of the synthetic source (SynthConfig::modeOpt, all 0 = the default shown):
//   modeOpt[0]  transmission mode: 0 mode 1 (default), 1 .. 3 = mode 1 .. 3
//   modeOpt[1]  spectrum mode: 0 index 1 (default: 100 kHz all digital), 1 index 1, 2 index 2 (200 kHz), 3 index 9, 4 index 10, 5 index 22,
//               6 index 23 (9, 10, 22, 23: two digital blocks beside an analogue FM programme)
//   modeOpt[2]  service data constellation: 0 QPSK, 1 16QAM, 2 64QAM
//   modeOpt[3]  LDPC code rate: 0 3/4 (default), 1 1/4, 2 1/3, 3 1/2, 4 3/4
//   modeOpt[4]  service description constellation: 0 QPSK, 1 16QAM, 2 64QAM
//   modeOpt[5]  sub-frame allocation: 0 mode 1 (default), 1 .. 3 = mode 1 .. 3 (time interleaving over 1, 2 or 4 logical frames)
//   modeOpt[6]  analogue FM programme in the middle of the hybrid spectrum modes: 0 on, 10 dB above the digital signal; 1 off
//   modeOpt[7]  text service: 0 on, 1 off
// SynthConfig::snrDb is the carrier to noise ratio in the digital signal's own bandwidth (its active carriers), cfoHz a carrier offset.
#pragma once
#include "dect2/cdr_defs.h"
#include "dect2/cdr_mux.h"
#include "mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct CdrTxConfig {
    int tm = 1;                    // transmission mode 1..3
    int sm = 1;                    // spectrum mode index 1, 2, 9, 10, 22, 23
    int msdMod = cdr::kQpsk, sdiMod = cdr::kQpsk;
    int rate = 3;                  // LDPC rate index: 0 1/4, 1 1/3, 2 1/2, 3 3/4
    int alloc = 1;                 // sub-frame allocation mode 1..3
    bool textService = true;
    std::string network = "OnAir CDR";
    uint32_t freq10Hz = 10610000;  // 106.1 MHz, in the network information table
    uint64_t networkId = 0x100;
    uint16_t serviceA = 0x1001, serviceB = 0x1002, serviceText = 0x1003;
    std::string text = "OnAir CDR test signal. CDR sound uses the DRA+ codec.";
    uint32_t seed = 1;
};
CdrTxConfig cdrTxConfigFrom(const SynthConfig& sc);

// What the multiplex of a configuration carries (for tests and the tools)
struct CdrTxPlan {
    int capacityBytes = 0;         // service multiplex frame per logical frame
    int sdiBits = 0;               // service description payload per logical frame
    int rateA = 0, rateB = 0;      // audio bit rates (bit/s)
    cdr::Smct smct;
    cdr::Nit nit;
    std::vector<cdr::AudioStreamDesc> streamA, streamB;
};

class CdrTransmitter {
public:
    explicit CdrTransmitter(const CdrTxConfig& cfg);
    ~CdrTransmitter();
    bool ok() const;
    const CdrTxConfig& config() const;
    const CdrTxPlan& plan() const;
    // The next physical sub-frame at 816 ksps: kSubframeLen samples, mean power about 1 (the windowed guard interval runs on into the next)
    void nextSubframe(std::vector<cf32>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeCdrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
