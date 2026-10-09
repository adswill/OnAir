// Engine: owns the source, the sample ring and the analysis thread. UI talks only to this.
#pragma once
#include "dect2/iq_correct.h"
#include "dect2/offset_tune.h"
#include "dect2/exact_resampler.h"
#include "dect2/delivery_check.h"
#include "source.h"
#include "spectrum.h"
#include "t2rx.h"
#include "dvbt_rx.h"
#include "atsc_rx.h"
#include "dab.h"
#include "atsc3_rx.h"
#include "isdbt_rx.h"
#include "fm_rx.h"
#include "dvbs_rx.h"
#include "dtmb_rx.h"
#include "atv_rx.h"
#include "dmr_rx.h"
#include "drm_rx.h"
#include "adsb_rx.h"
#include "gnss_rx.h"
#include "sonde_rx.h"
#include "ais_rx.h"
#include "marine_rx.h"
#include "acars_rx.h"
#include "inmc_rx.h"
#include "aero_rx.h"
#include "iridium_rx.h"
#include "mesh_rx.h"
#include "hdr_rx.h"
#include "cdr_rx.h"
#include "pager_rx.h"
#include "packet_rx.h"
#include "hfdig_rx.h"
#include "bbunpack.h"
#include "ts.h"
#include "tsout.h"
#include "player.h"
#include "teletext.h"
#include "bandwidth.h"
#include <atomic>
#include <condition_variable>
#include <deque>
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

// Where samples were lost since the radio was started. The radio side: samples that never came out of the radio (its USB link or driver
// could not keep up, or the computer slept), found by comparing what arrived with the radio's sample rate. The OnAir side: samples the
// receiver had to skip because it could not keep up with the rate (a slow or busy processor). The two need opposite remedies.
struct SampleLoss {
    bool live = false;                 // a live radio (files and the test signal never lose samples: they wait for the receiver)
    double radioSec = 0, cpuSec = 0;   // seconds of signal lost on each side
    uint64_t radioEvents = 0, cpuEvents = 0;
    double sinceRadioSec = -1, sinceCpuSec = -1;   // seconds since the last loss of each kind, -1 = none yet
    float loadPct = 0;                 // the receiver's share of real time on the sample thread over the last seconds (100 = just keeping up)
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
    // What the radio's driver said without failing: at the last retune (a frequency outside what the radio covers, ...) or else at the start
    // (a lower sample rate than asked for, ...); empty when it had nothing to say. The app shows it with the radio messages.
    std::string sourceNote() const { std::lock_guard<std::mutex> lk(noteMu_); return retuneNote_.empty() ? note_ : retuneNote_; }
    // A live radio that has stopped delivering samples (unplugged, or its USB connection broke). A HackRF is reopened when it comes back.
    bool radioLost() const { return radioLost_; }
    void setComputeMode(int m) { rx_.setComputeMode(m); }
    void selectPlp(int id) { rx_.selectPlp(id); }
    // Which standard to decode: 0 = automatic (alternates between DVB-T2 and DVB-T until one locks), 1 = DVB-T2, 2 = DVB-T, 3 = ATSC, 4 = DAB, 5 = ATSC 3.0, 6 = ISDB-T, 7 = FM,
    // 8 = DVB-S/S2, 9 = DTMB, 10 = analog TV, 11 = DMR, 12 = DRM, 13 = ADS-B, 14 = GNSS,
    // 15 = radiosonde, 16 = AIS, 17 = marine, 18 = ACARS, 19 = Inmarsat-C, 20 = Inmarsat Aero, 21 = Iridium, 22 = mesh,
    // 23 = HD Radio, 24 = CDR, 25 = pagers, 26 = APRS / packet, 27 = HF digital
    void setStandard(int m) { stdMode_ = m; stdReq_ = true; }
    int standardMode() const { return stdMode_.load(); }
    int activeStandard() const { return activeStd_.load(); }  // 0 = DVB-T2, 1 = DVB-T, 2 = ATSC, 3 = DAB, 4 = ATSC 3.0, 5 = ISDB-T, 6 = FM, 7 = DVB-S/S2, 8 = DTMB, 9 = analog TV, 10 = DMR, 11 = DRM, 12 = ADS-B, 13 = GNSS, 14 to 26 = radiosonde, AIS, marine, ACARS, Inmarsat-C, Inmarsat Aero, Iridium, mesh, HD Radio, CDR, pagers, APRS / packet, HF digital (always the standard code minus one)
    double sampleRate() const { return rate_; }
    // Channel bandwidth: with automatic detection on, the engine measures the width of the signal in the spectrum while nothing is
    // locked and reconfigures the receivers by itself. setBandwidth() forces a value (used by the scanner).
    void setBandwidthAuto(bool on) { if (bwAuto_.exchange(on) != on) bwReset_ = true; }
    void setBandwidth(double mhz) { bwReqVal_ = mhz; bwReq_ = true; }
    double activeBandwidth() const { return bwActive_.load(); }
    double detectedBandwidth() const { return bwDetected_.load(); }   // last bandwidth the detector settled on (0 = none yet)
    uint64_t droppedSamples() const { return ring_.dropped(); }   // the OnAir side only, in samples (see sampleLoss())
    SampleLoss sampleLoss() const;
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
    const Player& player() const { return player_; }
    // DC spike removal and IQ imbalance correction of the radio's samples (iq_correct.h), off by default
    IqCorrector& iqFix() { return iqFix_; }
    const IqCorrector& iqFix() const { return iqFix_; }
    // Offset tuning, the alternative to removing the DC spike: the radio is tuned beside the channel so that its spike falls outside it.
    // Radios only, for the modes with one channel on the centre; a radio too slow for it gets the DC removal instead. Applies at start().
    void setAutoOffset(bool on) { autoOffset_ = on; }
    bool autoOffset() const { return autoOffset_; }
    double offsetHz() const { return offHz_; }   // the offset in use, 0 = none
    // DAB / DAB+: the ensemble found on the channel, and the station to play (a sub-channel id, -1 = none)
    DabEnsemble dabEnsemble() const { return rxD_.ensemble(); }
    void dabSelect(int subId) { rxD_.select(subId); }
    int dabSelected() const { return rxD_.selected(); }
    DabAudio& dabAudio() { return rxD_.audio(); }
    FmReceiver& fm() { return rxFm_; }   // volume, mute and de-emphasis of the FM receiver
    DvbsReceiver& dvbs() { return rxDvbs_; }   // DVB-S/S2: the receiver's own controls and results (the interface reads telemetry through latestRx)
    DtmbReceiver& dtmb() { return rxDtmb_; }   // DTMB: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AtvReceiver& atv() { return rxAtv_; }   // Analog TV: the receiver's own controls and results (the interface reads telemetry through latestRx)
    DmrReceiver& dmr() { return rxDmr_; }   // DMR: the receiver's own controls and results (the interface reads telemetry through latestRx)
    DrmReceiver& drm() { return rxDrm_; }   // DRM: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AdsbReceiver& adsb() { return rxAdsb_; }   // ADS-B: the receiver's own controls and results (the interface reads telemetry through latestRx)
    GnssReceiver& gnss() { return rxGnss_; }   // GNSS: the receiver's own controls and results (the interface reads telemetry through latestRx)
    SondeReceiver& sonde() { return rxSonde_; }   // Radiosonde: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AisReceiver& ais() { return rxAis_; }   // AIS: the receiver's own controls and results (the interface reads telemetry through latestRx)
    MarineReceiver& marine() { return rxMarine_; }   // Marine: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AcarsReceiver& acars() { return rxAcars_; }   // ACARS: the receiver's own controls and results (the interface reads telemetry through latestRx)
    InmcReceiver& inmc() { return rxInmc_; }   // Inmarsat-C: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AeroReceiver& aero() { return rxAero_; }   // Inmarsat Aero: the receiver's own controls and results (the interface reads telemetry through latestRx)
    IridiumReceiver& iridium() { return rxIridium_; }   // Iridium: the receiver's own controls and results (the interface reads telemetry through latestRx)
    MeshReceiver& mesh() { return rxMesh_; }   // Mesh (LoRa): the receiver's own controls and results (the interface reads telemetry through latestRx)
    HdrReceiver& hdr() { return rxHdr_; }   // HD Radio: the receiver's own controls and results (the interface reads telemetry through latestRx)
    CdrReceiver& cdr() { return rxCdr_; }   // CDR: the receiver's own controls and results (the interface reads telemetry through latestRx)
    PagerReceiver& pager() { return rxPager_; }   // Pagers: the receiver's own controls and results (the interface reads telemetry through latestRx)
    PacketReceiver& packet() { return rxPacket_; }   // APRS / packet: the receiver's own controls and results (the interface reads telemetry through latestRx)
    HfdigReceiver& hfdig() { return rxHfdig_; }   // HF digital: the receiver's own controls and results (the interface reads telemetry through latestRx)
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
    // the spectrum is computed on its own thread, so that it never holds up the receiver (it takes a seventh of a slow core)
    void spectrumLoop();
    void feedSpectrum(const cf32* x, size_t n);
    std::thread specTh_;
    std::mutex specQMu_;
    std::condition_variable specQCv_;
    std::deque<std::vector<cf32>> specQ_;
    bool specStop_ = false;
    void catchUp();
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
    IqCorrector iqFix_;
    // offset tuning (offset_tune.h): the radio is tuned offHz_ below the channel at offRate_, and offMix_ brings the channel back to the centre;
    // offHz_ is 0 when it is not in use. offsetDc_: offset tuning was asked for but is not possible, the DC spike is removed instead
    std::atomic<bool> autoOffset_{false}, offsetDc_{false};
    std::atomic<double> offHz_{0}, offRate_{0};
    OffsetMixer offMix_;
    // with offset tuning the radio runs faster than the mode needs; right after the shift the samples go back to the mode's own rate, so the
    // receivers, the spectrum and the waterfall see exactly what they see without the offset (only the radio's USB stream is larger)
    ExactResampler offRs_;
    bool offResample_ = false;
    std::vector<cf32> offBuf_;
    double srcRate_ = 0;   // the radio's rate (rate_ is what the receivers get: the same, or the mode's rate with offset tuning)
    bool offsetAllowed() const;
    // the radio's samples into the receivers: clean-up, offset shift and resampling, spectrum, receiver, accounting
    void ingest(cf32* x, size_t n);
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
    IsdbtReceiver rxI_;
    FmReceiver rxFm_;
    DvbsReceiver rxDvbs_;
    DtmbReceiver rxDtmb_;
    AtvReceiver rxAtv_;
    DmrReceiver rxDmr_;
    DrmReceiver rxDrm_;
    AdsbReceiver rxAdsb_;
    GnssReceiver rxGnss_;
    SondeReceiver rxSonde_;
    AisReceiver rxAis_;
    MarineReceiver rxMarine_;
    AcarsReceiver rxAcars_;
    InmcReceiver rxInmc_;
    AeroReceiver rxAero_;
    IridiumReceiver rxIridium_;
    MeshReceiver rxMesh_;
    HdrReceiver rxHdr_;
    CdrReceiver rxCdr_;
    PagerReceiver rxPager_;
    PacketReceiver rxPacket_;
    HfdigReceiver rxHfdig_;
    uint64_t modeSeq_[20] = {};      // the last report taken from each of the modes added after FM (same order as above)
    mutable std::mutex atsc3Mu_;
    Atsc3Telemetry atsc3Tel_;
    uint64_t atsc3Seq_ = 0;
    int logA3State_ = -1;
    bool logA3Svc_ = false;
    uint64_t dabSeq_ = 0;
    int dmbSub_ = -1;                // the DMB video sub-channel the player is showing (-1 = none)
    uint64_t fmSeq_ = 0;
    int logDState_ = -1;
    bool logDEns_ = false;
    void logDabEvents(const RxTelemetry& t);
    uint64_t atscSeq_ = 0;
    int logAState_ = -1;
    void logAtscEvents(const RxTelemetry& t);
    void logAtsc3Events(const Atsc3Telemetry& a);
    std::atomic<int> stdMode_{0}, activeStd_{0};
    FileOptions lastFile_;   // what start() got, for a restart when a retune changes the radio's sample rate
    bool restartIfRateChanged(const TuneSettings& tune);
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
    void logIsdbtEvents(const RxTelemetry& t);
    int logIMode_ = -1, logIGi_ = -1, logITmcc_ = -1; bool logISync_ = false;
    int logTMode_ = -1, logTGi_ = -1, logTTps_ = -1; bool logTFec_ = false;
    std::mutex rxMu_;
    RxTelemetry rxTel_;
    // Every receiver counts its reports from 1, so after a change of mode (or a restart) its numbers would lie below the last one the interface
    // took and latestRx() would hold them back. The published report therefore carries a number of the engine that only grows. Under rxMu_.
    uint64_t rxPub_ = 0;
    void publishRx(RxTelemetry&& t) { t.seq = ++rxPub_; rxTel_ = std::move(t); }
    std::thread th_;
    std::atomic<bool> running_{false}, stopReq_{false};
    void watchRadio();
    std::atomic<bool> radioLost_{false};
    TuneSettings radioTune(const TuneSettings& t) const;   // what the radio is tuned to: the user's frequency plus the mode's tuneOffsetHz
    std::mutex tuneMu_;                 // lastDev_ / lastTune_: what to reopen the radio with
    DeviceInfo lastDev_;
    TuneSettings lastTune_;
    std::chrono::steady_clock::time_point lastSamples_{}, nextReconnect_{}, lastSkipLog_{};
    uint64_t skippedSamples_ = 0, skipEvents_ = 0;   // analysis thread only
    // sampleLoss(): the radio's deliveries checked against its rate (checkDelivery(), analysis thread), the OnAir side from the ring
    void checkDelivery();
    void holdDelivery(int ms);   // start the check again after a start, retune or reset (the radio may pause around them)
    std::atomic<int64_t> delivHoldNs_{0};
    std::atomic<bool> delivReanchor_{true};
    DeliveryCheck deliv_;   // analysis thread
    std::atomic<double> radioMissing_{0};
    std::atomic<uint64_t> radioMissEvents_{0}, cpuDropEvents_{0}, ringDroppedSeen_{0};
    std::atomic<int64_t> lastRadioMissNs_{0}, lastCpuDropNs_{0}, lastRadioLogNs_{0};
    std::atomic<float> loadPct_{0};
    std::atomic<bool> live_{false};   // the source is a live radio (sampleLoss() is read by the interface without touching src_)
    double loadT_ = 0, loadRx_ = 0, loadSamp_ = 0;   // analysis thread: where the last load measurement started
    std::string reconnectErr_;
    mutable std::mutex noteMu_;
    std::string note_, retuneNote_;     // sourceNote(): the driver's note at the start, at the last retune
    void setNote(const std::string& n, bool retune);
    std::atomic<double> rate_{0};

    std::mutex specMu_;
    SpectrumFrame spec_;

    std::mutex logMu_;
    std::vector<std::string> log_;
    std::chrono::steady_clock::time_point t0_;
};

} // namespace dect2
