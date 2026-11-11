// DVB-SH (family 746, engine standard 748, active receiver 747) in the engine: the SH-A receiver behind a resampler from the radio rate to
// the OFDM rate of the channel, the automatic channel width (5, 8, 7, 6 and 1.7 MHz tried in turn), the telemetry, the tuning and the test
// signal. The transport stream goes to the TV player like DVB-T. The TDM waveform (SH-B, dvbsh_tdm.h) is received by the same entry: on
// automatic each channel width is tried as OFDM and then as TDM (whose symbol rate is searched over table 5.13).
#pragma once
#include "dvbsh.h"
#include "dvbsh_tdm.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct ModeTuning;
struct SynthConfig;
class ModeSynth;

struct DvbshTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 no input, 1 searching, 2 OFDM symbols found, 3 TPS decoded, 4 transport stream
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    bool locked = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // turbo codewords (EFRAMEs) with all CRCs good / not
    uint64_t packetsOk = 0, packetsBad = 0, tpsFrames = 0;
    double bwMhz = 0;           // channel width in use (the automatic search moves it)
    dvbsh::Params params;       // valid from state 3
    int tiLmax = 0;             // time interleaver depth (cycles of 48 IU)
    double tiSeconds = 0;       // and its length in time
    bool tiTooLarge = false;
    double avgIterations = 0;
    double inputRate = 0;
    bool lowPriority = false;
    // TDM (SH-B)
    bool tdm = false;           // the receiver in use is the TDM one
    int tdmMod = -1;            // TdmMod
    double symbolRate = 0, clockPpm = 0;
    dvbsh::TdmSignalling sf;    // the last signalling field (valid from state 3 in TDM)
    uint64_t tdmFrames = 0;
};

class DvbshEngineReceiver {
public:
    DvbshEngineReceiver();
    ~DvbshEngineReceiver();
    void configure(double inputRateHz);
    bool ready() const;                         // false when the input rate is below the 1.7 MHz channel's OFDM rate
    void reset();
    void setBandwidth(double mhz);              // 0 automatic (default), or 8, 7, 6, 5, 1.7
    void setLowPriority(bool lp);               // hierarchical 16-QAM: the LP stream
    void setWaveform(int w);                    // 0 automatic (default): OFDM and TDM tried in turn, 1 OFDM (SH-A) only, 2 TDM (SH-B) only
    void setTdmGoldCode(int n);                 // TDM PL scrambling code number (default 0)
    void setSignalOffset(double hz);
    void feed(const cf32* x, size_t n);
    bool telemetry(DvbshTelemetry& out, uint64_t lastSeq);
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dvbshTuning();
std::string dvbshSummary(const DvbshTelemetry& t);
// The test signal (engine standard 748): SH-A OFDM with the test transport stream. SynthConfig: snrDb, cfoHz, sroPpm; modeOpt[0] channel
// width (0 5 MHz, 1 8 MHz, 2 7 MHz, 3 6 MHz, 4 1.7 MHz), modeOpt[1] FFT size (0 2K, 1 8K, 2 4K, 3 1K; 1K always at 1.7 MHz), modeOpt[2] 0 QPSK / 1 16-QAM,
// modeOpt[3] Punct_Pat_ID + 1 (0: 4, rate 1/3), modeOpt[4] guard + 1 (1 - 4 = 1/32 .. 1/4; 0: 1/8), modeOpt[5] 0 OFDM, 1 TDM QPSK, 2 TDM 8PSK,
// 3 TDM 16APSK (TDM: the symbol rate of table 5.13 for the width and guard, roll-off 0.25).
std::unique_ptr<ModeSynth> makeDvbshSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
