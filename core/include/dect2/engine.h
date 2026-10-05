// Engine: owns the source, the sample ring and the analysis thread. UI talks only to this.
#pragma once
#include "source.h"
#include "spectrum.h"
#include "t2rx.h"
#include "dvbt_rx.h"
#include "atsc_rx.h"
#include "dab.h"
#include "atsc3_rx.h"
#include "bbunpack.h"
#include "ts.h"
#include "tsout.h"
#include "player.h"
#include "teletext.h"
#include "bandwidth.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cstdint>
#include <map>
#include <utility>

namespace dect2 {

// Wait/hold times of the analysis thread around the transport-stream lock (diagnostics, shown by the GUI status log).
std::string engineWaitProfile();

struct FileOptions {
    std::string path;
    FileFormat format = FileFormat::CS8;
    double sampleRate = 18285714.2857;
    bool loop = true;
};

class Engine {
public:
    Engine();
    ~Engine();

    bool start(const DeviceInfo& dev, const TuneSettings& tune, const FileOptions& file);
    void stop();
    bool retune(const TuneSettings& tune);
    // Retune and flush every stage (receiver, spectrum, transport stream) - used by the channel scanner.
    bool retuneReset(const TuneSettings& tune);
    uint64_t resetCount() const { return resets_.load(); }
    bool running() const { return running_; }
    // A live radio that has stopped delivering samples (unplugged, or its USB connection broke). A HackRF is reopened when it comes back.
    bool radioLost() const { return radioLost_; }
    void setComputeMode(int m) { rx_.setComputeMode(m); }
    void selectPlp(int id) { rx_.selectPlp(id); }
    // Which standard to decode: 0 = automatic (alternates between DVB-T2 and DVB-T until one locks), 1 = DVB-T2, 2 = DVB-T, 3 = ATSC, 4 = DAB, 5 = ATSC 3.0
    void setStandard(int m) { stdMode_ = m; stdReq_ = true; }
    int standardMode() const { return stdMode_.load(); }
    int activeStandard() const { return activeStd_.load(); }  // 0 = DVB-T2, 1 = DVB-T, 2 = ATSC, 3 = DAB, 4 = ATSC 3.0
    double sampleRate() const { return rate_; }
    // Channel bandwidth: with automatic detection on, the engine measures the width of the signal in the spectrum while nothing is
    // locked and reconfigures the receivers by itself. setBandwidth() forces a value (used by the scanner).
    void setBandwidthAuto(bool on) { if (bwAuto_.exchange(on) != on) bwReset_ = true; }
    void setBandwidth(double mhz) { bwReqVal_ = mhz; bwReq_ = true; }
    double activeBandwidth() const { return bwActive_.load(); }
    double detectedBandwidth() const { return bwDetected_.load(); }   // last bandwidth the detector settled on (0 = none yet)
    uint64_t droppedSamples() const { return ring_.dropped(); }
    std::string loadProfile() const;   // seconds the analysis thread spent on the spectrum and on the receiver, and the signal it has processed (diagnostics)

    // Latest spectrum snapshot; returns true if newer than `lastSeq`.
    bool latestSpectrum(SpectrumFrame& out, uint64_t lastSeq);
    // no spectrum fft when headless, autobw needs it
    void setSpectrumEnabled(bool on) { analyzer_.setTransform(on); }

    bool latestRx(RxTelemetry& out, uint64_t lastSeq);
    void setPlpDump(std::function<void(const PlpResult&)> cb) { plpDump_ = std::move(cb); }

    // Transport stream (phase 4)
    TsSnapshot tsSnapshot();
    BbStats bbStats();
    std::map<int, std::vector<EpgEvent>> epg();
    // Every burst of transport stream packets is also handed to this function (the network tuner); it runs on the receiver thread, so it must be quick.
    void setPacketTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*)> f);
    void setOutputs(const OutputConfig& c) { outputs_.configure(c); }
    OutputConfig outputConfig() const { return outputs_.config(); }
    OutputStats outputStats() const { return outputs_.stats(); }
    Player& player() { return player_; }
    // DAB / DAB+: the ensemble found on the channel, and the station to play (a sub-channel id, -1 = none)
    DabEnsemble dabEnsemble() const { return rxD_.ensemble(); }
    void dabSelect(int subId) { rxD_.select(subId); }
    int dabSelected() const { return rxD_.selected(); }
    DabAudio& dabAudio() { return rxD_.audio(); }
    TeletextDecoder& teletext() { return ttx_; }
    // ATSC 3.0: the service list and statistics, and which service to receive (-1 = the first video service)
    bool atsc3Telemetry(Atsc3Telemetry& t) const { std::lock_guard<std::mutex> lk(atsc3Mu_); t = atsc3Tel_; return atsc3Tel_.seq != 0; }
    void atsc3Select(int serviceId) { rxA3_.selectService(serviceId); }

    // Log
    void log(const std::string& line);
    std::vector<std::string> logSnapshot(size_t& total);
    void clearLog();

private:
    void analysisLoop();
    void logRxEvents(const RxTelemetry& t);
    std::function<void(const PlpResult&)> plpDump_;
    void onPlp(const PlpResult& r);
    std::mutex tsMu_;
    BbUnpacker unpack_;
    TsDemux demux_;
    TsSnapshot tsSnap_;
    OutputManager outputs_;
    std::function<void(const uint8_t*, size_t, const TsSnapshot*)> tap_;
    Player player_;
    TeletextDecoder ttx_;
    int lastT2Frame_ = -1;
    std::atomic<bool> resetReq_{false};
    std::atomic<uint64_t> resets_{0};
    void applyReset();
    std::vector<uint8_t> frameBuf_;
    double tSpec_ = 0, tRx_ = 0;
    double nSamp_ = 0;
    uint64_t logP1Count_ = 0;
    int logS1_ = -1, logS2_ = -1, logGi_ = -2, logState_ = -1, logFrameSyms_ = 0;

    IqRing ring_;
    std::unique_ptr<IqSource> src_;
    SpectrumAnalyzer analyzer_{4096};
    T2Receiver rx_;
    DvbtReceiver rxT_;
    AtscReceiver rxA_;
    DabReceiver rxD_;
    Atsc3Rx rxA3_;
    mutable std::mutex atsc3Mu_;
    Atsc3Telemetry atsc3Tel_;
    uint64_t atsc3Seq_ = 0;
    int logA3State_ = -1;
    bool logA3Svc_ = false;
    uint64_t dabSeq_ = 0;
    int logDState_ = -1;
    bool logDEns_ = false;
    void logDabEvents(const RxTelemetry& t);
    uint64_t atscSeq_ = 0;
    int logAState_ = -1;
    void logAtscEvents(const RxTelemetry& t);
    void logAtsc3Events(const Atsc3Telemetry& a);
    std::atomic<int> stdMode_{0}, activeStd_{0};
    std::atomic<bool> stdReq_{false};
    double autoMark_ = 0, lastLockSec_ = 0;
    double bwMhz_ = 8;
    std::atomic<double> bwActive_{8}, bwDetected_{0}, bwReqVal_{0};
    std::atomic<bool> bwAuto_{false}, bwReq_{false}, bwReset_{false};
    BandwidthDetector bwDet_;
    double bwMark_ = 0, bwLastEval_ = 0;
    double bwVote_ = 0;
    void bandwidthStep(const SpectrumFrame& f, const RxTelemetry& t, bool tLocked);
    void changeBandwidth(double mhz);
    void feedRx(const cf32* x, size_t n);
    void autoSelect(const RxTelemetry& t, bool tLocked);
    void onTsPackets(const uint8_t* pk, size_t n, double secs);
    void logDvbtEvents(const RxTelemetry& t);
    int logTMode_ = -1, logTGi_ = -1, logTTps_ = -1; bool logTFec_ = false;
    std::mutex rxMu_;
    RxTelemetry rxTel_;
    std::thread th_;
    std::atomic<bool> running_{false}, stopReq_{false};
    void watchRadio();
    std::atomic<bool> radioLost_{false};
    std::mutex tuneMu_;                 // lastDev_ / lastTune_: what to reopen the radio with
    DeviceInfo lastDev_;
    TuneSettings lastTune_;
    std::chrono::steady_clock::time_point lastSamples_{}, nextReconnect_{};
    std::string reconnectErr_;
    std::atomic<double> rate_{0};

    std::mutex specMu_;
    SpectrumFrame spec_;

    std::mutex logMu_;
    std::vector<std::string> log_;
    std::chrono::steady_clock::time_point t0_;
};

} // namespace dect2
