// Engine: owns the source, the sample ring and the analysis thread. UI talks only to this.
#pragma once
#include "dect2/dsp/iq_correct.h"
#include "dect2/source/offset_tune.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/audio/audio_monitor.h"
#include "dect2/source/delivery_check.h"
#include "dect2/source/source.h"
#include "dect2/source/iq_record.h"
#include "dect2/source/time_machine.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/t2/t2rx.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/atsc/atsc_rx.h"
#include "dect2/dab/dab.h"
#include "dect2/atsc3/atsc3_rx.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/atv/atv_rx.h"
#include "dect2/dmr/dmr_rx.h"
#include "wide_modes.h"
#include "dect2/drm/drm_rx.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/gnss/gnss_rx.h"
#include "dect2/sonde/sonde_rx.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/inmc/inmc_rx.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/iridium/iridium_rx.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/cdr/cdr_rx.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "mode_tuning.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/vdl2/vdl2_rx.h"
#include "dect2/vdl4/vdl4_rx.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/vor/vor_rx.h"
#include "dect2/ndb/ndb_rx.h"
#include "dect2/dme/dme_live.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/mac/mac_rx.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include "dect2/uplink/uplink_rx.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/iono/iono_rx.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/specx/specx_rx.h"
#include "sig_ident.h"
#include "dect2/specan/specan_rx.h"
#include "dect2/astro/astro_rx.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/general/general_rx.h"
#include "dect2/time/time_rx.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/vdes/vdes_rx.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/hfbeacon/hfbeacon_rx.h"
#include "dect2/dji/dji_rx.h"
#include "dect2/nbiot/nbiot_rx.h"
#include "dect2/halow/halow_rx.h"
#include "dect2/codar/codar_rx.h"
#include "dect2/wb/wb_rx.h"
#include "dect2/datawell/datawell_rx.h"
#include "dect2/same/same_rx.h"
#include "dect2/ant/ant_rx.h"
#include "dect2/dect/dect_rx.h"
#include "dect2/multitone/multitone_rx.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/fpv/fpv_live.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/tones/tones_rx.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/fsq/fsq_rx.h"
#include "dect2/ifkp/ifkp_rx.h"
#include "dect2/kaal/kaal_live.h"
#include "dect2/kait/kait_live.h"
#include "dect2/kapp/kapp_live.h"
#include "dect2/kavb/kavb_live.h"
#include "dect2/kaws/kaws_live.h"
#include "dect2/kaft/kaft_live.h"
#include "dect2/cquam/cquam_rx.h"
#include "dect2/modeac/modeac_live.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/satbeacon/satbeacon_live.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/meteor/meteor_rx.h"
#include "dect2/nr/nr_rx.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/elektro/elektro_rx.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/railway/railway_rx.h"
#include "dect2/dgps/dgps_engine.h"
#include "dect2/eloran/eloran_engine.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/fusion/fusion_rx.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/milhf/milhf_engine.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/tv/bbunpack.h"
#include "multitv.h"
#include "dect2/tv/ts.h"
#include "dect2/tv/tsout.h"
#include "dect2/tv/player.h"
#include "dect2/tv/teletext.h"
#include "dect2/tv/dvbsub.h"
#include "dect2/dsp/bandwidth.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <complex>
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
    uint64_t offsetBytes = 0, endBytes = 0;   // SigMF / WAV: where the samples start and end in the file
    bool bigEndian = false;
};

// Where samples were lost since the radio was started. Callback gaps: the library delivered less than the radio's rate (the radio or its
// driver), found by comparing what arrived with that rate. Ring overflow: samples the library did hand over, which OnAir then discarded
// because it did not read the input ring in time. Receiver load (loadPct) is processing time against the block's duration, a third thing:
// it can be high while every sample is kept, and samples can be lost while it is low. deliveredPct is the rate that actually arrived.
struct SampleLoss {
    bool live = false;                 // a live radio (files and the test signal never lose samples: they wait for the receiver)
    double radioSec = 0, cpuSec = 0;   // seconds of signal lost on each side
    uint64_t radioEvents = 0, cpuEvents = 0;
    double sinceRadioSec = -1, sinceCpuSec = -1;   // seconds since the last loss of each kind, -1 = none yet
    float loadPct = 0;                 // the receiver's share of real time on the sample thread over the last seconds (100 = just keeping up)
    float deliveredPct = -1;           // samples the radio handed over, percent of the rate it is set to, over the last seconds. -1 = not measured yet
    bool rateLow = false;              // a live radio under about 97% of that rate (the radio or driver, not the receiver's CPU load)
};

class Engine {
    int receiverId_ = 1;
public:
    Engine();
    ~Engine();

    bool start(const DeviceInfo& dev, const TuneSettings& tune, const FileOptions& file);
    // The number of this receiver (RX 1, RX 2 ...): the radio it opens is claimed for it (device_claims.h), another receiver cannot open it too
    void setReceiverId(int id) { receiverId_ = id; }
    int receiverId() const { return receiverId_; }
    void stop();
    bool retune(const TuneSettings& tune);
    // Retune and flush every stage (receiver, spectrum, transport stream) - used by the channel scanner.
    bool retuneReset(const TuneSettings& tune);
    uint64_t resetCount() const { return resets_.load(); }
    bool running() const { return running_; }
    uint64_t startEpoch() const { return startEpoch_.load(); }   // grows at each successful start (map send: ignore leftover lists)
    bool syntheticSource() const;   // last start was the built-in test signal (never posted to the public map)
    // What the radio's driver said without failing: at the last retune (a frequency outside what the radio covers, ...) or else at the start
    // (a lower sample rate than asked for, ...); empty when it had nothing to say. The app shows it with the radio messages.
    std::string sourceNote() const { std::lock_guard<std::mutex> lk(noteMu_); return retuneNote_.empty() ? note_ : retuneNote_; }
    // A live radio that has stopped delivering samples (unplugged, or its USB connection broke). A HackRF is reopened when it comes back.
    bool radioLost() const { return radioLost_; }
    void setComputeMode(int m) { rx_.setComputeMode(m); rxDvbs_.setComputeMode(m); rxDtmb_.setComputeMode(m); }
    void selectPlp(int id) { rx_.selectPlp(id); }
    // Which standard to decode: 0 = automatic (alternates between DVB-T2 and DVB-T until one locks), 1 = DVB-T2, 2 = DVB-T, 3 = ATSC, 4 = DAB, 5 = ATSC 3.0, 6 = ISDB-T, 7 = FM,
    // 8 = DVB-S/S2, 9 = DTMB, 10 = analog TV, 11 = DMR, 12 = DRM, 13 = ADS-B, 14 = GNSS,
    // 15 = radiosonde, 16 = AIS, 17 = marine, 18 = ACARS, 19 = Inmarsat-C, 20 = Inmarsat Aero, 21 = Iridium, 22 = mesh,
    // 23 = HD Radio, 24 = CDR, 25 = pagers, 26 = APRS / packet, 27 = HF digital, 28 = airband, 29 = VDL Mode 2, 30 = ISM sensors, 32 = VOR / ILS, 33 = M17, 34 = cell towers, 35 = D-STAR, 36 = CW, 37 = HFDL, 38 = PSK31, 39 = JS8Call, 42 = DVB-C, 46 = Orbcomm, 40 = NXDN, 41 = GSM-R, 45 = Mode S uplink, 47 = dPMR, 51 = HF ALE, 48 = TETRA, 49 = MAC, 50 = radio astronomy, 51 = HF ALE, 52 = P25, 53 = Bluetooth LE, 54 = ionospheric sounders
    static constexpr int kPollSeqSlots = 150;   // pollMode() seqs[] indices; LBJ uses slot 76
    void setStandard(int m) { stdMode_ = m; stdReq_ = true; refreshIdle(); }
    int standardMode() const { return stdMode_.load(); }
    int activeStandard() const { return activeStd_.load(); }  // 0 = DVB-T2, 1 = DVB-T, 2 = ATSC, 3 = DAB, 4 = ATSC 3.0, 5 = ISDB-T, 6 = FM, 7 = DVB-S/S2, 8 = DTMB, 9 = analog TV, 10 = DMR, 11 = DRM, 12 = ADS-B, 13 = GNSS, 14 to 27 = radiosonde, AIS, marine, ACARS, Inmarsat-C, Inmarsat Aero, Iridium, mesh, HD Radio, CDR, pagers, APRS / packet, HF digital, airband, VDL Mode 2, ISM sensors, VOR / ILS, M17, 34 = D-STAR, 35 = CW, 36 = HFDL, 37 = PSK31, 38 = JS8Call, 41 = DVB-C, 44 = Mode S uplink, 45 = Orbcomm, 46 = dPMR, 39 = NXDN, 47 = TETRA, 48 = MAC, 49 = radio astronomy, 50 = HF ALE, 51 = P25, 52 = Bluetooth LE, 53 = ionospheric sounders , 54 = QO-100, 55 = Wi-Fi networks, 56 = general receiver (always the standard code minus one)
    double sampleRate() const { return rate_; }
    double radioRate() const { return srcRate_; }   // the rate the radio (or file) runs at, as it confirmed it at the start
    // Channel bandwidth: with automatic detection on, the engine measures the width of the signal in the spectrum while nothing is
    // locked and reconfigures the receivers by itself. setBandwidth() forces a value (used by the scanner).
    void setBandwidthAuto(bool on) { if (bwAuto_.exchange(on) != on) bwReset_ = true; }
    void setBandwidth(double mhz) { bwReqVal_ = mhz; bwReq_ = true; }
    double activeBandwidth() const { return bwActive_.load(); }
    double detectedBandwidth() const { return bwDetected_.load(); }   // last bandwidth the detector settled on (0 = none yet)
    // Record the radio's raw samples (as they leave the ring, before any clean-up) at the radio's own rate. Not for a file source.
    bool startRecording(const std::string& path, FileFormat fmt, std::string& err);
    void stopRecording() { rec_.stop(); }
    // A tap on the radio's raw samples (the same point the recorder takes them, at the radio's own rate): for the frequency self-calibration. Called on the
    // analysis thread; keep it quick. An empty function removes it.
    void setIqTap(std::function<void(const std::complex<float>*, size_t, double rateHz)> f) {
        std::lock_guard<std::mutex> lk(iqTapMu_);
        iqTap_ = std::move(f);
        iqTapOn_.store((bool)iqTap_);
    }
    // Time machine (time_machine.h): the last seconds of the raw samples are kept in memory; timeMachineSave() writes the window around now to a file.
    // setTimeMachine: seconds <= 0 is off; applied now when the radio runs, and again at every start. Returns the window the memory budget allows.
    double setTimeMachine(double seconds, int decimate, int memBudgetMB);
    // Saves the window around now (preSeconds back, postSeconds ahead) to path (name it with timeMachineName()). Returns at once.
    bool timeMachineSaveTo(const std::string& path, double preSeconds, double postSeconds, std::string& err);
    TimeMachineStats timeMachineStats() const { return tm_.stats(); }
    std::string timeMachineName() const;
    void setRecordSigmf(bool on) { recSigmf_ = on; }   // SigMF metadata beside the recordings (default on)
    RecordingStats recordingStats() const { return rec_.stats(); }
    // A file name for a recording of what the radio is tuned to right now (see recordingFileName())
    std::string recordingName(FileFormat fmt) const;
    uint64_t droppedSamples() const { return ring_.dropped(); }   // the OnAir side only, in samples (see sampleLoss())
    SampleLoss sampleLoss() const;
    std::string loadProfile() const;   // seconds the analysis thread spent on the spectrum and on the receiver, and the signal it has processed (diagnostics)

    // Latest spectrum snapshot; returns true if newer than `lastSeq`.
    bool latestSpectrum(SpectrumFrame& out, uint64_t lastSeq);
    // no spectrum fft when headless, autobw needs it
    void setSpectrumEnabled(bool on) { analyzer_.setTransform(on); }

    bool latestRx(RxTelemetry& out, uint64_t lastSeq);

    // Two modes at once: a second receiver gets a copy of the same samples on its own thread (it never holds up the first: with a live radio
    // a second receiver that falls behind loses blocks, counted in secondaryDropped()). It is the idle receiver of its mode, shifted so that
    // its frequency sits where the mode expects it, so its own controls (acars(), vdl2(), ...) work as for the first mode. Only the modes of
    // secondaryAllowed(), only a pair that fits in the capture (secondaryCheck()); a refused or suspended pair says why in secondaryNote().
    // offsetHz: the second mode's frequency less captureCentreHz(); stdMode 0 = off. Kept over restarts and retunes (as a frequency).
    bool setSecondary(int stdMode, double offsetHz);
    bool setSecondaryHz(int stdMode, double freqHz);   // the same with the second mode's own frequency
    void clearSecondary() { setSecondary(0, 0); }
    int secondaryStandard() const { return secStd_.load(); }   // the engine standard code asked for, 0 = none
    bool secondaryRunning() const { return secAct_.load() >= 0; }
    double secondaryHz() const { return secHz_.load(); }
    std::string secondaryNote() const { std::lock_guard<std::mutex> lk(secNoteMu_); return secNote_; }
    uint64_t secondaryDropped() const { return secDropped_.load(); }   // samples the second receiver lost (a live radio only)
    double secondaryRate() const { return secRate_.load(); }   // the rate it decodes at: its mode's own, the capture resampled down to it
    // Its sound: off by default (the first mode keeps the speakers); applies to the modes that play any (DMR, dPMR, airband, CW, ...)
    void setSecondarySound(bool on);
    // Monitor audio: the tuned channel of any mode, demodulated (USB / LSB / NFM / AM) to the speaker, a second device or a local TCP stream
    // (dect2/audio/audio_monitor.h). It uses a copy of the samples on its own thread, so it works beside every mode, the second mode included.
    // offsetHz: fine tuning from the tuned frequency (a SSB dial sits below the carrier). The monitor yields while the mode plays its own sound.
    AudioMonitor& monitor() { return monitor_; }
    void setMonitorOffsetHz(double hz) { monitorOffsetHz_ = hz; }
    bool latestRx2(RxTelemetry& out, uint64_t lastSeq);   // the second mode's report (as latestRx)
    // Display only: the spectrum is mixed by hz (the samples the decoders get are not touched). A sound card showing the audio range 0..Nyquist uses it.
    void setSpectrumShiftHz(double hz) { specShiftHz_ = hz; }
    double spectrumShiftHz() const { return specShiftHz_.load(); }
    double captureCentreHz() const;   // the frequency at the centre of the samples the receivers get
    // The modes that can run second: narrow channels that neither retune the radio, nor make a transport stream, picture or player sound
    static bool secondaryAllowed(int stdMode);
    // The same beside a given first mode: DVB-S/S2 (8) may run second only beside QO-100 (55), whose wideband view lists the DATV carriers; QO-100
    // makes no transport stream of its own, so the stream, the player and the picture are the second mode's then
    static bool secondaryAllowed(int stdMode, int primaryStd);
    // Empty when the second mode fits beside the first in a capture of rateHz around centreHz, else the reason (shown to the user).
    // chanHz: the width of the channel when the mode's own is not it (a DVB-S carrier: its symbol rate with the roll-off), 0 = the mode's
    static std::string secondaryCheck(int primaryStd, int secStd, double centreHz, double rateHz, double secHz, double chanHz = 0);
    // DVB-S/S2 as the second mode beside QO-100: the carrier at freqHz (what the radio sees, Qo100Datv::ifHz) with its symbol rate. The receiver gets the
    // carrier alone (mixed to the centre and resampled to a few times its symbol rate, so that the other carriers of the transponder stay out of its
    // spectrum search); its stream goes to the player, its report comes through latestRx2() as standard 7. Replaces any other second mode.
    bool setSecondaryDvbs(double freqHz, double symbolRate);

    void setPlpDump(std::function<void(const PlpResult&)> cb) { plpDump_ = std::move(cb); }

    // Multi-channel TV (DVB-T / DVB-T2, multitv.h): when the capture is wide enough for more than one channel of the chosen width, one receiver runs for
    // every occupied channel of the raster (up to maxChannels), each with its own telemetry and service list; one channel plays at a time while the others
    // keep decoding. grid: 0 finds out whether the capture is centred on a channel or between two, 1 on a channel, 2 between two. Off: the single receiver as before.
    void setMultiChannel(bool on, int maxChannels = 0, int grid = 0);
    bool multiChannel() const { return mtvOn_.load(); }                  // asked for
    bool multiChannelActive() const { return mtvActive_.load(); }        // running now (the mode, the span and the width allow it)
    static bool multiChannelSupported(int stdMode) { return stdMode == 1 || stdMode == 2; }   // DVB-T2, DVB-T
    static int defaultMaxChannels();                                      // 2 on a laptop, 3 on a desktop
    static int slotsInSpan(double spanRate, double widthMhz, int grid);   // channels of the raster that fit the capture
    void selectTvChannel(int slot);                                       // the channel that plays and shows in detail
    int selectedTvChannel() const { auto m = mtvGet(); return m ? m->selected() : 0; }
    std::vector<TvChannelRow> tvChannels();
    std::vector<MultiService> multiServices();                            // every service of every channel, with the channel it comes from
    std::string multiChannelNote() const { std::lock_guard<std::mutex> lk(mtvNoteMu_); return mtvNote_; }
    // Transport stream (phase 4)
    TsSnapshot tsSnapshot();
    BbStats bbStats();
    std::map<int, std::vector<EpgEvent>> epg();
    // Every burst of transport stream packets is also handed to this function (the network tuner); it runs on the receiver thread, so it must be quick.
    // `secs` is how long the burst represents in the stream, so a client can hand it to a player at that rate instead of in one lump.
    void setPacketTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*, double secs)> f);
    // The TV recorder's tap: every packet of the multiplex in stream order (EPG, recordings, timeshift). Separate from setPacketTap (the network tuner).
    void setRecorderTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*, double secs)> f);
    // true: the live multiplex no longer feeds the player (a recording or the timeshift buffer is playing through player().push() instead)
    void setPlayerExternal(bool on) { playerExternal_ = on; }
    bool playerExternal() const { return playerExternal_; }
    // The subtitle decoders follow the stream that plays through player().push() while setPlayerExternal(true)
    void feedPlaybackSubtitles(const uint8_t* pkts, size_t n) { for (size_t i = 0; i < n; i++) { ttx_.feedTs(pkts + i * 188); dvbsub_.feedTs(pkts + i * 188); } }
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
    // Sticky tuning: mix a signal at captureCentreHz() + shiftHz to DC without retuning the radio (spectrum click in decode-offset mode).
    void setDecodeShiftHz(double hz);
    double decodeShiftHz() const { return decodeShiftHz_.load(); }
    // DAB / DAB+: the ensemble found on the channel, and the station to play (a sub-channel id, -1 = none)
    DabEnsemble dabEnsemble() const { return rxD_.ensemble(); }
    DabFicAnalysis dabFic() const { return rxD_.ficAnalysis(); }
    void dabSelect(int subId) { rxD_.select(subId); }
    int dabSelected() const { return rxD_.selected(); }
    void dabSelectJournaline(int sub, int packetAddress, int ext) { rxD_.selectJournaline(sub, packetAddress, ext); }
    dect2::JournalineDecoder& dabJournaline() { return rxD_.journaline(); }
    void dabSelectSpi(int sub, int packetAddress) { rxD_.selectSpi(sub, packetAddress); }   // the programme guide (SPI): the sub-channel to collect it from (-1 off, address -2: the PAD)
    dect2::DabSpiDecoder& dabSpi() { return rxD_.spi(); }
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
    Vdl2Receiver& vdl2() { return rxVdl2_; }   // VDL Mode 2: the receiver's own controls and results (the interface reads telemetry through latestRx)
    GsmrReceiver& gsmr() { return rxGsmr_; }   // GSM-R: the receiver's own controls and results (the interface reads telemetry through latestRx)
    CellReceiver& cell() { return rxCell_; }   // Cell towers: the receiver's own controls and results (the interface reads telemetry through latestRx)
    IsmReceiver& ism() { return rxIsm_; }   // ISM sensors: the receiver's own controls and results (the interface reads telemetry through latestRx)
    // Whole-span decoding (the "wide decode" switch): the narrowband HF modes (CW, PSK, HF digital: FT8 / FT4 / WSPR slots and RTTY) decode
    // the visible span (up to 250 kHz around the dial frequency) instead of the tuned transmission (widespan.h). Off by default.
    void setWideDecode(bool on, double spanHz = 250000);
    bool wideDecode() const { return wideDecode_.load(); }
    CwReceiver& cw() { return rxCw_; }   // CW: the receiver's own controls and results (the interface reads telemetry through latestRx)
    MacReceiver& mac() { return rxMac_; }   // MAC television: pictures, sound (the interface reads telemetry through latestRx)
    DpmrReceiver& dpmr() { return rxDpmr_; }   // dPMR: scan, sound (the interface reads telemetry through latestRx)
    NxdnReceiver& nxdn() { return rxNxdn_; }   // NXDN: the receiver's own controls and results (the interface reads telemetry through latestRx)
    SpecxReceiver& specx() { return rxSpecx_; }   // spectrum explorer: FFT size and averaging
    sigid::Capture& sigid() { return sigid_; }   // signal identifier: start() a capture of a region of what the receivers see, in any mode
    AstroReceiver& astro() { return rxAstro_; }   // radio astronomy (hydrogen line): FFT size, integration, frequency switching
    SpecanReceiver& specan() { return rxSpecan_; }   // spectrum analyser: range, resolution, averaging (telemetry through latestRx)
    Js8Receiver& js8() { return rxJs8_; }   // JS8Call: the receiver's own controls and results (the interface reads telemetry through latestRx)
    OrbcommReceiver& orbcomm() { return rxOrb_; }   // Orbcomm: downlinks at once (the interface reads telemetry through latestRx)
    TetraReceiver& tetra() { return rxTetra_; }   // TETRA network information: the interface reads telemetry through latestRx
    TetrapolReceiver& tetrapol() { return rxTetrapol_; }   // TETRAPOL control channel (receive only)
    AleReceiver& ale() { return rxAle_; }   // HF ALE: the receiver's own controls and results (the interface reads telemetry through latestRx)
    P25Receiver& p25() { return rxP25_; }   // P25 Phase 1: sound (the interface reads telemetry through latestRx)
    UplinkReceiver& uplink() { return rxUplink_; }   // Mode S uplink (1030 MHz): telemetry through latestRx
    BleReceiver& ble() { return rxBle_; }   // Bluetooth LE advertising: clear the device table (telemetry through latestRx)
    IonoReceiver& iono() { return rxIono_; }   // ionospheric sounders: timing lock, the sounder shown (telemetry through latestRx)
    // More channels for the general receiver: up to kGeneralExtra further receivers on the same capture, each with its own channel offset,
    // demodulator, squelch and recording. They are silent (the one that is listened to is general()); n = how many run (0 .. kGeneralExtra).
    static constexpr int kGeneralExtra = 3;
    GeneralReceiver& generalExtra(int i) { return extraGeneral_[(size_t)i]; }
    int generalExtraCount() const { return extraGeneralN_.load(); }
    void setGeneralExtraCount(int n) { extraGeneralN_ = n < 0 ? 0 : n > kGeneralExtra ? kGeneralExtra : n; }
    GeneralReceiver& general() { return rxGeneral_; }   // general receiver: demodulator, filter, squelch, sound and recording (telemetry through latestRx)
    GlobalstarReceiver& globalstar() { return rxGs_; }   // Globalstar activity monitor: clear the event log (telemetry through latestRx)
    VdesReceiver& vdes() { return rxVdes_; }   // VDES: clear the ASM and burst lists (telemetry through latestRx)
    NbiotReceiver& nbiot() { return rxNb_; }   // NB-IoT cells: clear the cell table (telemetry through latestRx)
    ZigbeeReceiver& zigbee() { return rxZb_; }   // Zigbee / 802.15.4: clear the PAN and device tables (telemetry through latestRx)
    FlarmReceiver& flarm() { return rxFlarm_; }   // FLARM: receiver position, clear (telemetry through latestRx)
    WmbusLiveReceiver& wmbus() { return rxWmb_; }   // Wireless M-Bus meters: clear the device table (telemetry through latestRx)
    TonesReceiver& tones() { return rxTones_; }   // tone decoders: audio input, the gate, listen (telemetry through latestRx)
    SarsatReceiver& sarsat() { return rxSar_; }   // COSPAS-SARSAT 406 MHz beacons: clear the log (telemetry through latestRx)
    FsqReceiver& fsq() { return rxFsq_; }   // FSQ: speed, own callsign, clear the text (telemetry through latestRx)
    KaalLiveReceiver& kaal() { return rxKaal_; }   // KAAL: volume, mute (telemetry through latestRx)
    KaitLiveReceiver& kait() { return rxKait_; }   // KAIT: picture bursts (telemetry through latestRx)
    KavbLiveReceiver& kavb() { return rxKavb_; }   // KAVB: the symbol rate (profile) to listen to, the output file, clear the counts (telemetry through latestRx)
    KawsLiveReceiver& kaws() { return rxKaws_; }   // KAWS: clear the lists, the clock, listen only (telemetry through latestRx)
    KappLiveReceiver& kapp() { return rxKapp_; }   // KAPP: clear the packet list (telemetry through latestRx)
    KaftLiveReceiver& kaft() { return rxKaft_; }   // KAFT: the folder received files are saved to, clear the list (telemetry through latestRx)
    IfkpReceiver& ifkp() { return rxIfkp_; }   // IFKP: speed, clear the text (telemetry through latestRx)
    ArdopReceiver& ardop() { return rxArd_; }   // ARDOP: clear the frames, Listen (telemetry through latestRx)
    Mt63Receiver& mt63() { return rxMt63_; }   // MT63: bandwidth and interleave (automatic by default), clear the text (telemetry through latestRx)
    CquamReceiver& cquam() { return rxCq_; }   // AM stereo (C-QUAM): stereo mode, audio bandwidth, squelch, channel offset (telemetry through latestRx)
    EczasLiveReceiver& eczas() { return rxEcz_; }   // e-Czas (225 kHz, Poland): audio input, receiver position, clear the log (telemetry through latestRx)
    FpvReceiver& fpv() { return rxFpv_; }   // FPV video: picture controls, sound, clear the channel levels (telemetry through latestRx)
    ZwaveReceiver& zwave() { return rxZw_; }   // Z-Wave: region, clear the tables (telemetry through latestRx)
    GoesDcsReceiver& goesdcs() { return rxGd_; }   // GOES DCS: CSV log file, carrier threshold, clear the lists (telemetry through latestRx)
    ModeAcReceiver& modeac() { return rxModeAc_; }   // Mode A/C replies: reference position, code confirmation and expiry, clear (telemetry through latestRx)
    BletrkLiveReceiver& bletrk() { return rxBt_; }   // BLE tracker detector: alert time, near level, own trackers, clear (telemetry through latestRx)
    SatbeaconLiveReceiver& satbeacon() { return rxSb_; }   // satellite beacon: CSV log file (telemetry through latestRx)
    MscatterLiveReceiver& mscatter() { return rxMs_; }   // meteor scatter: threshold, CSV log file, clear the list (telemetry through latestRx)
    WisunReceiver& wisun() { return rxWs_; }   // Wi-SUN FAN: region, clear the tables (telemetry through latestRx)
    LoraRawReceiver& loraraw() { return rxLr_; }   // LoRa (raw): settings, clear the frame log (telemetry through latestRx)
    HfbReceiver& hfbeacon() { return rxHfb_; }   // HF beacons: the list, receiver position, ppm, clock offset (telemetry through latestRx)
    DjiReceiver& dji() { return rxDji_; }   // DJI drones: clear the results (telemetry through latestRx)
    WbReceiver& wb() { return rxWb_; }   // Signal workbench: nothing to clear (the tool works on files)
    DatawellReceiver& datawell() { return rxDw_; }   // Datawell buoy HF link: clear the frames, force the format (telemetry through latestRx)
    MultitoneReceiver& multitone() { return rxMt_; }   // Multitone paging: clear the transmission list (telemetry through latestRx)
    CodarReceiver& codar() { return rxCd_; }   // CODAR / HF ocean radar: clear the station table and the events (telemetry through latestRx)
    DectReceiver& dect() { return rxDect_; }   // DECT: the base station table and log (telemetry through latestRx)
    // Plugin modes (standard codes of mode_tuning.h kPluginStdBase and up): the samples of the running plugin mode go to this function on the analysis
    // thread (interleaved float I, Q; it must not block: the plugin host queues them). Empty function removes it.
    using PluginIqSink = std::function<void(const float* iq, size_t nComplex, double sampleRate)>;
    void setPluginSink(PluginIqSink f);
    AntReceiver& ant() { return rxAnt_; }   // ANT+ sensors: the sensor table and frame log (telemetry through latestRx)
    SameReceiver& same() { return rxSame_; }   // NOAA weather radio SAME: the alert list, country, Listen (telemetry through latestRx)
    HalowReceiver& halow() { return rxHl_; }   // Wi-Fi HaLow / 802.11ah: clear the network table and the frame log (telemetry through latestRx)
    CubesatReceiver& cubesat() { return rxCube_; }   // CubeSat telemetry: satellite table, tracked satellite, KISS (telemetry through latestRx)
    TimeReceiver& timesig() { return rxTime_; }   // time signals: station, receiver position (telemetry through latestRx)
    RidReceiver& rid() { return rxRid_; }   // Drone Remote ID: clear the drone table (telemetry through latestRx)
    HrptReceiver& hrpt() { return rxHrpt_; }   // HRPT / AHRPT: variant, new picture (telemetry through latestRx)
    GoesReceiver& goes() { return rxGoes_; }   // GOES HRIT: clear the products (telemetry through latestRx)
    ElektroReceiver& elektro() { return rxEl_; }   // Elektro-L HRIT / LRIT / GGAK (telemetry through latestRx)
    ElLoranReceiver& eloran() { return rxEloran_; }   // eLoran / Loran-C (WIP): GRI, stations, arrival times (telemetry through latestRx)
    MptReceiver& mpt1327() { return rxMpt_; }   // MPT1327 trunking control channel (WIP): messages and channel table (telemetry through latestRx)
    FusionReceiver& fusion() { return rxFusion_; }   // Yaesu Fusion (WIP): callsigns and slow data, the speech codec plug-in (telemetry through latestRx)
    LtrReceiver& ltr() { return rxLtr_; }   // LTR control channel (telemetry through latestRx)
    EdacsReceiver& edacs() { return rxEdacs_; }   // EDACS control channel (telemetry through latestRx)
    MilhfReceiver& milhf() { return rxMilhf_; }   // HF waveform identifier: the audio clip for sigidwiki (telemetry through latestRx)
    DgpsReceiver& dgps() { return rxDgps_; }   // DGPS beacons: the station, the frames (telemetry through latestRx)
    RailwayReceiver& klw() { return rxKlw_; }
    RailwayReceiver& tdcs() { return rxTdcs_; }
    LbjReceiver& lbj() { return rxLbj_; }   // LBJ railway warnings (telemetry through latestRx)
    Vdl4Receiver& vdl4() { return rxVdl4_; }   // VDL Mode 4 (telemetry through latestRx)
    UatReceiver& uat() { return rxUat_; }         // UAT 978 MHz ADS-B / FIS-B / TIS-B (telemetry through latestRx)
    MeteorReceiver& meteor() { return rxMeteor_; }   // Meteor-M LRPT: modulation, a new picture (telemetry through latestRx)
    NrReceiver& nr() { return rxNr_; }         // 5G NR cells: clear the cell table (telemetry through latestRx)
    WifiReceiver& wifi() { return rxWifi_; }   // Wi-Fi networks: clear the network table (telemetry through latestRx)
    Qo100Receiver& qo100() { return rxQo100_; }   // QO-100: LNB oscillator, SSB dial and listening (telemetry through latestRx)
    DvbcReceiver& dvbc() { return rxDvbc_; }   // DVB-C: symbol rate, QAM, annex (the interface reads telemetry through latestRx)
    PskReceiver& psk() { return rxPsk_; }   // PSK31: the receiver's own controls and results (the interface reads telemetry through latestRx)
    DstarReceiver& dstar() { return rxDstar_; }   // D-STAR: the receiver's own controls and results (the interface reads telemetry through latestRx)
    HfdlReceiver& hfdl() { return rxHfdl_; }   // HFDL: the receiver's own controls and results (the interface reads telemetry through latestRx)
    M17Receiver& m17() { return rxM17_; }   // M17: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AirbandReceiver& airband() { return rxAirband_; }   // Airband: the receiver's own controls and results (the interface reads telemetry through latestRx)
    VorReceiver& vor() { return rxVor_; }   // VOR / ILS: the receiver's own controls and results (the interface reads telemetry through latestRx)
    DmeLiveReceiver& dme() { return rxDme_; }   // DME (WIP): channel X or Y, pair rate, the Morse ident (telemetry through latestRx)
    NdbReceiver& ndb() { return rxNdb_; }   // NDB: long-wave non-directional beacons, the Morse ident (telemetry through latestRx)
    TeletextDecoder& teletext() { return ttx_; }
    DvbSubDecoder& dvbSubtitles() { return dvbsub_; }
    // ATSC 3.0: the service list and statistics, and which service to receive (-1 = the first video service)
    bool atsc3Telemetry(Atsc3Telemetry& t) const { std::lock_guard<std::mutex> lk(atsc3Mu_); t = atsc3Tel_; return atsc3Tel_.seq != 0; }
    void atsc3Select(int serviceId) { rxA3_.selectService(serviceId); }

    // Decoded results: the lists every mode keeps (messages, aircraft, vessels, cells, pictures...) live through mode switches, retunes
    // and restarts of the receiver, and are saved to a folder (one file per receiver) so the next run starts with them. Only a mode's
    // Clear empties them. Any thread.
    // Whole-span decoding (DMR, M17, D-STAR, NXDN, dPMR, P25): one decoder per carrier found in the captured span instead of the tuned channel only.
    // Takes effect at once; the single-channel receiver is untouched while it is on. Needs a restart-free toggle: the carriers appear within a second.
    void setWideVoice(bool on) { wideOn_.store(on); for (auto& v : wideSeq_) v = 0; }
    bool wideVoice() const { return wideOn_.load(); }
    void setWideMuted(bool m) { wide_.setMuted(m); }
    void setWideSelected(int carrierId) { wide_.setSelected(carrierId); }   // the carrier whose sound plays and whose detail shows (0: the best locked)
    static bool wideSupported(int analysisMode) { return WideModes::supports(analysisMode); }
    void clearAllResults();                              // the lists of every mode at once (the top bar's Clear all)
    // the files of the DSM-CC carousels of the stream received, saved to dir/pid_XXXX/path (clear broadcast data); returns the number of files written
    size_t saveCarouselFiles(const std::string& dir) { std::lock_guard<std::mutex> lk(tsMu_); return demux_.saveCarouselFiles(dir); }
    void clearResults(int stdMode);                      // the Clear of a mode's screen: every list of its receiver
    int loadResults(const std::string& dir);             // the lists of the last run (a file of another build is discarded); the receivers restored
    int saveResults(const std::string& dir);             // the receivers whose lists changed since the last save (or load); the files written
    void setResultsDir(const std::string& dir);          // load from dir now, then save to it every 30 s on a thread of its own (empty: stop)
    void flushResults();                                 // save now (at exit; also done by the destructor) and stop the 30 s saver
    std::vector<std::string> resultNames() const;        // the file names (without .bin) of the receivers that keep results
    // With no radio running: the active mode's report again, so its kept lists show (after a load, a Clear or a change of mode)
    void refreshIdle();

    // Log
    void log(const std::string& line);
    std::vector<std::string> logSnapshot(size_t& total);
    void clearLog();

private:
    struct ResultSlot {
        const char* name;                                // file name: results/<name>.bin
        int act;                                         // the receiver's active standard (activeStandard())
        std::function<std::string()> save;
        std::function<bool(const std::string&)> load;
        std::function<void()> clear;
    };
    std::vector<ResultSlot> resultSlots();
    std::mutex resIoMu_;                                 // one save or load at a time; resHash_
    std::mutex resMu_;                                   // the saver thread: resDir_, resStop_
    std::string resDir_;
    std::map<std::string, uint64_t> resHash_;            // what each file holds now (fnv1a of the blob)
    std::thread resTh_;
    std::condition_variable resCv_;
    bool resStop_ = false;
    void stopResultsSaver();
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
    // multi-channel TV (setMultiChannel)
    std::shared_ptr<MultiTv> mtv_;
    mutable std::mutex mtvMu_;               // mtv_ (the analysis thread replaces it, the interface reads it)
    std::shared_ptr<MultiTv> mtvGet() const { std::lock_guard<std::mutex> lk(mtvMu_); return mtv_; }
    std::atomic<bool> mtvOn_{false}, mtvActive_{false};
    std::atomic<int> mtvMax_{0}, mtvGrid_{0};
    double mtvKeyRate_ = -1, mtvKeyBw_ = 0;
    int mtvKeyStd_ = -1, mtvKeyMax_ = -1, mtvKeyGrid_ = -1;
    bool mtvKeyOn_ = false;
    mutable std::mutex mtvNoteMu_;
    std::string mtvNote_;
    void syncMultiTv();                       // the analysis thread: starts, changes or stops the multi-channel receivers when the settings or the capture changed
    void onMultiPackets(const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap);
    std::mutex tsMu_;
    BbUnpacker unpack_;
    TsDemux demux_;
    TsSnapshot tsSnap_;
    OutputManager outputs_;
    std::function<void(const uint8_t*, size_t, const TsSnapshot*, double secs)> tap_;
    std::function<void(const uint8_t*, size_t, const TsSnapshot*, double secs)> rtap_;
    std::atomic<bool> playerExternal_{false};
    Player player_;
    IqCorrector iqFix_;
    // offset tuning (offset_tune.h): the radio is tuned offHz_ below the channel at offRate_, and offMix_ brings the channel back to the centre;
    // offHz_ is 0 when it is not in use. offsetDc_: offset tuning was asked for but is not possible, the DC spike is removed instead
    std::atomic<bool> autoOffset_{false}, offsetDc_{false};
    std::atomic<double> offHz_{0}, offRate_{0};
    OffsetMixer offMix_;
    std::atomic<double> decodeShiftHz_{0};
    OffsetMixer decodeMix_;
    // with offset tuning the radio runs faster than the mode needs; right after the shift the samples go back to the mode's own rate, so the
    // receivers, the spectrum and the waterfall see exactly what they see without the offset (only the radio's USB stream is larger)
    ExactResampler offRs_;
    bool offResample_ = false;
    std::vector<cf32> offBuf_;
    double srcRate_ = 0;   // the radio's rate (rate_ is what the receivers get: the same, or the mode's rate with offset tuning)
    bool offsetAllowed() const;
    // the steps of start() (engine_start.cpp), called in this order
    void configureTvReceivers(const DeviceInfo& dev, const TuneSettings& tune);
    void placeNarrowReceivers(const DeviceInfo& dev, const TuneSettings& tune);
    void configureDecoderReceivers(const DeviceInfo& dev, const TuneSettings& tune);
    void configureDataReceivers(const DeviceInfo& dev, const TuneSettings& tune);
    void checkRateAndResetState(const DeviceInfo& dev, const TuneSettings& tune);
    void announceAndRun(const DeviceInfo& dev, const TuneSettings& tune);
    // the radio's samples into the receivers: clean-up, offset shift and resampling, spectrum, receiver, accounting
    void ingest(cf32* x, size_t n);
    TeletextDecoder ttx_;
    DvbSubDecoder dvbsub_;
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
    // The log is declared before every receiver: members are destroyed in reverse order, and receiver worker
    // threads (joined in the receivers' destructors) may still log until then.
    std::mutex logMu_;
    std::vector<std::string> log_;
    std::chrono::steady_clock::time_point t0_;
    IqRecorder rec_;
    TimeMachine tm_;
    double tmSeconds_ = 0;
    int tmDecim_ = 1, tmBudgetMB_ = 512;
    std::mutex iqTapMu_;
    std::function<void(const std::complex<float>*, size_t, double)> iqTap_;
    std::atomic<bool> iqTapOn_{false};
    bool recSigmf_ = true;
    std::unique_ptr<IqSource> src_;
    SpectrumAnalyzer analyzer_{8192};
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
    WideModes wide_;
    std::atomic<bool> wideOn_{false};
    uint64_t wideSeq_[64] = {};
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
    AirbandReceiver rxAirband_;
    Vdl2Receiver rxVdl2_;
    IsmReceiver rxIsm_;
    VorReceiver rxVor_;
    NdbReceiver rxNdb_;
    DmeLiveReceiver rxDme_;
    M17Receiver rxM17_;
    HfdlReceiver rxHfdl_;
    CellReceiver rxCell_;
    CwReceiver rxCw_;
    std::atomic<bool> wideDecode_{false};
    DstarReceiver rxDstar_;
    NxdnReceiver rxNxdn_;
    DpmrReceiver rxDpmr_;
    MacReceiver rxMac_;
    P25Receiver rxP25_;
    GsmrReceiver rxGsmr_;
    PskReceiver rxPsk_;
    Js8Receiver rxJs8_;
    SpecxReceiver rxSpecx_;
    sigid::Capture sigid_;
    SpecanReceiver rxSpecan_;
    AstroReceiver rxAstro_;
    bool specanSweep_ = false;   // the HackRF sweeps by itself (spectrum analyser)
    DvbcReceiver rxDvbc_;
    OrbcommReceiver rxOrb_;
    UplinkReceiver rxUplink_;
    TetraReceiver rxTetra_;
    AleReceiver rxAle_;
    BleReceiver rxBle_;
    IonoReceiver rxIono_;
    Qo100Receiver rxQo100_;
    WifiReceiver rxWifi_;
    GeneralReceiver rxGeneral_;
    GeneralReceiver extraGeneral_[3];
    std::atomic<int> extraGeneralN_{0};
    TimeReceiver rxTime_;
    CubesatReceiver rxCube_;
    GlobalstarReceiver rxGs_;
    VdesReceiver rxVdes_;
    ZigbeeReceiver rxZb_;
    HfbReceiver rxHfb_;
    DjiReceiver rxDji_;
    NbiotReceiver rxNb_;
    HalowReceiver rxHl_;
    CodarReceiver rxCd_;
    WbReceiver rxWb_;
    DatawellReceiver rxDw_;
    SameReceiver rxSame_;
    AntReceiver rxAnt_;
    std::shared_ptr<const PluginIqSink> pluginSink_;   // read with std::atomic_load
    DectReceiver rxDect_;
    TetrapolReceiver rxTetrapol_;
    MultitoneReceiver rxMt_;
    LoraRawReceiver rxLr_;
    WisunReceiver rxWs_;
    ZwaveReceiver rxZw_;
    EczasLiveReceiver rxEcz_;
    FpvReceiver rxFpv_;
    WmbusLiveReceiver rxWmb_;
    TonesReceiver rxTones_;
    SarsatReceiver rxSar_;
    Mt63Receiver rxMt63_;
    FsqReceiver rxFsq_;
    IfkpReceiver rxIfkp_;
    KaalLiveReceiver rxKaal_;
    KaitLiveReceiver rxKait_;
    KappLiveReceiver rxKapp_;
    KavbLiveReceiver rxKavb_;
    KawsLiveReceiver rxKaws_;
    KaftLiveReceiver rxKaft_;
    CquamReceiver rxCq_;
    ArdopReceiver rxArd_;
    MscatterLiveReceiver rxMs_;
    GoesDcsReceiver rxGd_;
    ModeAcReceiver rxModeAc_;
    BletrkLiveReceiver rxBt_;
    SatbeaconLiveReceiver rxSb_;
    RidReceiver rxRid_;
    MeteorReceiver rxMeteor_;
    NrReceiver rxNr_;
    GoesReceiver rxGoes_;
    ElektroReceiver rxEl_;
    LbjReceiver rxLbj_;
    RailwayReceiver rxKlw_;
    RailwayReceiver rxTdcs_{railway::Protocol::Tdcs};
    DgpsReceiver rxDgps_;
    ElLoranReceiver rxEloran_;
    MptReceiver rxMpt_;
    FusionReceiver rxFusion_;
    LtrReceiver rxLtr_;
    EdacsReceiver rxEdacs_;
    MilhfReceiver rxMilhf_;
    Vdl4Receiver rxVdl4_;
    UatReceiver rxUat_;
    HrptReceiver rxHrpt_;
    FlarmReceiver rxFlarm_;
    uint64_t modeSeq_[kPollSeqSlots] = {};      // the last report taken from each of the modes added after FM (same order as above)
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
    void feedReceiver(int a, const cf32* x, size_t n);   // the receiver of active standard a
    void resetReceiver(int a);
    // the report of the receiver of active standard a for the modes from DVB-S on: 1 = a new one in t, 0 = none new, -1 = not one of them
    int pollMode(int a, RxTelemetry& t, uint64_t* seqs);
    // the second mode (setSecondary): the queue to its thread, the receiver's placement in the capture
    void secondaryLoop(int a);
    void startSecondary();                    // start(): the second mode asked for, suspended when it does not fit
    void stopSecondary();                     // stop(): stays asked for
    bool startSecondaryLocked(bool keep);     // under secCtlMu_; keep: a pair that does not fit stays asked for (suspended)
    void stopSecondaryLocked();
    void refreshSecondary(bool force);        // after a retune: placed again (force: the radio moved), suspended or resumed
    bool placeSecondary(std::string& why);   // checks the pair with the current tuning and shifts the receiver there; false = refused
    void placeReceiver(int a, double sigOffHz, double centreHz, double dialHz, double chanHz);
    void placeReceiverAsPrimary(int a);      // back to what start() gave it
    void configureReceiver(int a, double rateHz);
    void muteReceiver(int a, bool muted);
    std::atomic<bool> secSound_{false};
    AudioMonitor monitor_;
    std::atomic<double> monitorOffsetHz_{0};
    std::atomic<double> secRate_{0};          // the second receiver's rate: its mode's own, the capture resampled down to it
    std::atomic<double> secChanHz_{0}, secWantRate_{0}, secSym_{0};   // setSecondaryDvbs: the channel's width, the rate it wants, its symbol rate (0 = the mode's own)
    bool setSecondaryImpl(int sm, double hz, double chanHz, double wantRate, double sym);
    void pushSecondary(const cf32* x, size_t n);
    void setSecNote(const std::string& s) { std::lock_guard<std::mutex> lk(secNoteMu_); secNote_ = s; }
    std::thread secTh_;
    std::mutex secQMu_;
    std::condition_variable secQCv_;
    std::deque<std::vector<cf32>> secQ_;
    size_t secQN_ = 0;                  // samples in secQ_
    bool secStop_ = false;
    std::mutex secFeedMu_;              // held while the second receiver takes a block: nothing resets or moves it meanwhile
    std::mutex secCtlMu_;               // setSecondary / start / stop of the second mode
    std::atomic<int> secStd_{0};        // asked for: engine standard code, 0 = none
    std::atomic<double> secHz_{0};      // its frequency
    std::atomic<int> secAct_{-1};       // the receiver (active standard) running as the second mode, -1 = none
    std::atomic<double> secShift_{0};   // how far its frequency lies above the centre of the capture
    std::atomic<bool> secFlush_{false};
    std::atomic<uint64_t> secDropped_{0};
    uint64_t modeSeq2_[kPollSeqSlots] = {};
    std::mutex rx2Mu_;
    RxTelemetry rxTel2_;
    uint64_t rx2Pub_ = 0;
    mutable std::mutex secNoteMu_;
    std::string secNote_;
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
    std::atomic<uint64_t> startEpoch_{0};
    void watchRadio();
    std::atomic<bool> radioLost_{false};
    TuneSettings hwTune(const TuneSettings& t) const;      // radioTune() through the converter (converter.h): what the radio itself is tuned to
    mutable std::atomic<bool> convSwap_{false};            // the converter mirrors the spectrum: ingest() swaps I and Q
    std::atomic<double> specShiftHz_{0};
    double specPhase_ = 0;                                // the spectrum thread's mixer
    std::atomic<bool> noTuneOffset_{false};               // a sound card or a recording: the channel is where it was recorded, the mode's tuneOffsetHz does not apply
    TuneSettings radioTune(const TuneSettings& t) const;  // what the radio is tuned to: the user's frequency plus the mode's tuneOffsetHz
    mutable std::mutex tuneMu_;                 // lastDev_ / lastTune_: what to reopen the radio with
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
    std::atomic<float> deliveredPct_{-1};   // checkDelivery(): samples the library offered over wall time, percent of srcRate_; -1 until a window completes
    std::atomic<bool> live_{false};   // the source is a live radio (sampleLoss() is read by the interface without touching src_)
    double loadT_ = 0, loadRx_ = 0, loadSamp_ = 0;   // analysis thread: where the last load measurement started
    int64_t rateT0_ = 0;       // analysis thread: start of the delivered-rate window (steady ns); 0 = not started
    uint64_t rateOff0_ = 0;    // offered-sample count at rateT0_
    std::string reconnectErr_;
    mutable std::mutex noteMu_;
    std::string note_, retuneNote_;     // sourceNote(): the driver's note at the start, at the last retune
    void setNote(const std::string& n, bool retune);
    std::atomic<double> rate_{0};

    std::mutex specMu_;
    SpectrumFrame spec_;

};

// One protocol of a shared receiver as its own mode (mode_tuning.h): runs that receiver with only this protocol's decoder.
// nullptr: every protocol of HF digital and pagers (mesh and marine keep their own settings). Any thread.
void applySubMode(Engine& e, const SubMode* s);

} // namespace dect2
