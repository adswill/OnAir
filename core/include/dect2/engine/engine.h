// Engine: owns the source, the sample ring and the analysis thread. UI talks only to this.
#pragma once
#include "dect2/dsp/gpu_policy.h"
#include "dect2/common/cpu_meter.h"
#include "dect2/dsp/iq_correct.h"
#include "dect2/source/offset_tune.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/audio/audio_monitor.h"
#include "dect2/source/delivery_check.h"
#include "dect2/source/gain_meter.h"
#include "dect2/source/source.h"
#include "dect2/source/iq_record.h"
#include "dect2/source/time_machine.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/dsp/noise_blanker.h"
#include "dect2/t2/t2rx.h"
#include "dect2/t2/t2mi_out.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/atsc/atsc_rx.h"
#include "dect2/dab/dab.h"
#include "dect2/atsc3/atsc3_rx.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "dect2/tv/mpe_fec.h"
#include "dect2/tv/ip_out.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/fm/fm_census.h"
#include "dect2/dab/dab_census.h"
#include "dect2/general/am_census.h"
#include "dect2/general/am_diversity.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/atv/atv_rx.h"
#include "dect2/dmr/dmr_rx.h"
#include "wide_modes.h"
#include "wide_session.h"
#include "vfo.h"
#include "dect2/audio/audioout.h"
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
#include "dect2/sitor/sitor_live.h"
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
#include "dect2/dvbc2/dvbc2_rx.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include "dect2/uplink/uplink_rx.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/btc/btc_rx.h"
#include "dect2/iono/iono_rx.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/specx/specx_rx.h"
#include "sig_ident.h"
#include "dect2/autodet/autodet.h"
#include "dect2/specan/specan_rx.h"
#include "dect2/astro/astro_rx.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/general/general_rx.h"
#include "dect2/time/time_rx.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_smart.h"
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/vdes/vdes_rx.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/adsl/adsl_rx.h"
#include "dect2/avroad/avroad_rx.h"
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
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/lorasat/lorasat_rx.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/roshf/roshf_rx.h"
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/inmsat/inmsat_rx.h"
#include "dect2/rclink/rc_rx.h"
#include "dect2/xlink/xlink_rx.h"
#include "dect2/cishf/cishf_rx.h"
#include "dect2/w1a/w1a_rx.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/sensors/sensors_rx.h"
#include "dect2/bgan/bgan_rx.h"
#include "dect2/sferics/sferics_rx.h"
#include "dect2/sferics/ltpos_rx.h"
#include "dect2/fbr/fbr_rx.h"
#include "dect2/atis/atis_rx.h"
#include "dect2/selcal/selcal_rx.h"
#include "dect2/umts/umts_rx.h"
#include "dect2/cdma/cdma_rx.h"
#include "dect2/tdscdma/tdscdma_rx.h"
#include "dect2/vdl3/vdl3_rx.h"
#include "dect2/tv/tr101290.h"
#include "dect2/t2/t2mi_in.h"
#include "dect2/t2/t2plp_merge.h"
#include "dect2/fy3/fy3_rx.h"
#include "dect2/pactorhf/pactorhf_rx.h"
#include "dect2/shortwave/shortwave_rx.h"
#include "dect2/pskdsp/pskdsp_rx.h"
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
#include "dect2/goesdcs/hrdcp_live.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/satbeacon/satbeacon_live.h"
#include "dect2/satmisc/satdetect_live.h"
#include "dect2/navic/navic.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/meteor/meteor_rx.h"
#include "dect2/nr/nr_rx.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/elektro/elektro_rx.h"
#include "dect2/gk2a/gk2a_rx.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/railway/railway_rx.h"
#include "dect2/horus/horus_rx.h"
#include "dect2/dgps/dgps_engine.h"
#include "dect2/eloran/eloran_engine.h"
#include "dect2/nfc/nfc_engine.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/isdbs/isdbs_rx.h"
#include "dect2/dvbsh/dvbsh_engine.h"
#include "dect2/fusion/fusion_rx.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/tetradmo/tetradmo_rx.h"
#include "dect2/opensky/opensky_rx.h"
#include "dect2/marinevhf/marinevhf_rx.h"
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/milhf/milhf_engine.h"
#include "dect2/sbas/sbas_engine.h"
#include "dect2/asterix/asterix_engine.h"
#include "dect2/argos/argos_rx.h"
#include "dect2/sigfox/sigfox_rx.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/sd3/sd3_rx.h"
#include "dect2/hamsci/hamsci_rx.h"
#include "dect2/xband/xband_rx.h"
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
#include <memory>
#include "dect2/power/power.h"
#include "dect2/dsp/dc_notch.h"
#include "dect2/dsp/spur_notch.h"
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
    double centreHz = 0;   // the recording's own centre frequency (0: the tuned frequency at start is it). The spectrum stays on it; tune.centerHz is then the VFO
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
    float cpuPct = 0;                  // the same with the sample thread's CPU time instead of the wall clock: a machine busy with other programs does not raise it
    float deliveredPct = -1;           // samples the radio handed over, percent of the rate it is set to, over the last seconds. -1 = not measured yet
    bool rateLow = false;              // a live radio under about 97% of that rate (the radio or driver, not the receiver's CPU load)
    bool network = false;              // the radio is reached over a network link (rtl_tcp, SpyServer, ...), not USB
};

class Engine {
    int receiverId_ = 1;
public:
    Engine();
    ~Engine();

    bool start(const DeviceInfo& dev, const TuneSettings& tune, const FileOptions& file);
    // Why the last start() returned false, in the driver's own words (empty when it succeeded): the app shows it when a rate or bandwidth the user typed is refused
    std::string startError() const { std::lock_guard<std::mutex> lk(noteMu_); return startErr_; }
    // The number of this receiver (RX 1, RX 2 ...): the radio it opens is claimed for it (device_claims.h), another receiver cannot open it too
    void setReceiverId(int id) { receiverId_ = id; }
    int receiverId() const { return receiverId_; }
    // CPU use of this receiver in percent of one core (100 = one core busy), averaged over about the last second (common/cpu_meter.h):
    // its sample path (ring, clean-up, offset mixer, recording copies), its spectrum and its decoder (the mode's receiver, including
    // the jobs it runs on the shared pool and the second mode's thread). cpuUsage() gives the parts, named "rx<N> <part>"; cpu::processPercent()
    // is the whole process (GUI, drivers and audio included) and cpu::usage() every account of every receiver.
    double cpuPercent() const;
    std::vector<cpu::Usage> cpuUsage() const;
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
    void setComputeMode(int m) { computeMode_ = m; applyCompute(); }
    // GPU on / off per decoder family (dect2/dsp/gpu_policy.h Domain): off forces that decoder's CPU path
    void setGpuAllowed(int domain, bool on) { if (domain >= 0 && domain < gpupol::kDomains && gpu_.allowed[domain].exchange(on) != on) applyCompute(); }
    bool gpuAllowed(int domain) const { return domain >= 0 && domain < gpupol::kDomains && gpu_.allowed[domain].load(); }
    int computeMode() const { return computeMode_; }
    void selectPlp(int id) { rx_.selectPlp(id); }
    void selectT2Layer(int layer) { rx_.selectLayer(layer); }   // 0 = automatic, 1 = T2-Base, 2 = T2-Lite (mixed signals)
    // Which standard to decode: 0 = automatic (alternates between DVB-T2 and DVB-T until one locks), 1 = DVB-T2, 2 = DVB-T, 3 = ATSC, 4 = DAB, 5 = ATSC 3.0, 6 = ISDB-T, 7 = FM,
    // 8 = DVB-S/S2, 9 = DTMB, 10 = analog TV, 11 = DMR, 12 = DRM, 13 = ADS-B, 14 = GNSS,
    // 15 = radiosonde, 16 = AIS, 17 = marine, 18 = ACARS, 19 = Inmarsat-C, 20 = Inmarsat Aero, 21 = Iridium, 22 = mesh,
    // 23 = HD Radio, 24 = CDR, 25 = pagers, 26 = APRS / packet, 27 = HF digital, 28 = airband, 29 = VDL Mode 2, 30 = ISM sensors, 32 = VOR / ILS, 33 = M17, 34 = cell towers, 35 = D-STAR, 36 = CW, 37 = HFDL, 38 = PSK31, 39 = JS8Call, 42 = DVB-C, 46 = Orbcomm, 40 = NXDN, 41 = GSM-R, 45 = Mode S uplink, 47 = dPMR, 51 = HF ALE, 48 = TETRA, 49 = MAC, 50 = radio astronomy, 51 = HF ALE, 52 = P25, 53 = Bluetooth LE, 54 = ionospheric sounders
    static constexpr int kPollSeqSlots = 901;   // pollMode() seqs[] indices; LBJ uses slot 76
    void setStandard(int m) { stdMode_ = m; stdReq_ = true; refreshIdle(); }
    int standardMode() const { return stdMode_.load(); }
    int activeStandard() const { return activeStd_.load(); }  // 0 = DVB-T2, 1 = DVB-T, 2 = ATSC, 3 = DAB, 4 = ATSC 3.0, 5 = ISDB-T, 6 = FM, 7 = DVB-S/S2, 8 = DTMB, 9 = analog TV, 10 = DMR, 11 = DRM, 12 = ADS-B, 13 = GNSS, 14 to 27 = radiosonde, AIS, marine, ACARS, Inmarsat-C, Inmarsat Aero, Iridium, mesh, HD Radio, CDR, pagers, APRS / packet, HF digital, airband, VDL Mode 2, ISM sensors, VOR / ILS, M17, 34 = D-STAR, 35 = CW, 36 = HFDL, 37 = PSK31, 38 = JS8Call, 41 = DVB-C, 44 = Mode S uplink, 45 = Orbcomm, 46 = dPMR, 39 = NXDN, 47 = TETRA, 48 = MAC, 49 = radio astronomy, 50 = HF ALE, 51 = P25, 52 = Bluetooth LE, 53 = ionospheric sounders , 54 = QO-100, 55 = Wi-Fi networks, 56 = general receiver (always the standard code minus one)
    double sampleRate() const { return rate_; }
    // The automatic gain's sensors (every sample of the radio's block, on the engine thread; take() from any thread): docs/AUTOGAIN.md
    ag::GainMeter& gainMeter() { return gainMeter_; }
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
    // Extra SigMF fields and a size cap for the next recording (the Android app: radio name, location only if the user allows it, cap in bytes)
    void setRecordExtra(const std::string& hw, bool hasGeo, double lat, double lon) { rec_.setSigmfExtra(hw, hasGeo, lat, lon); }
    void setRecordMaxBytes(uint64_t b) { rec_.setMaxBytes(b); }
    bool recordingCapReached() const { return rec_.capReached(); }
    // A file name for a recording of what the radio is tuned to right now (see recordingFileName())
    std::string recordingName(FileFormat fmt) const;
    uint64_t droppedSamples() const { return ring_.dropped(); }   // the OnAir side only, in samples (see sampleLoss())
    double backlogSec() const { const double r = srcRate_; return r > 0 ? (double)ring_.available() / r : 0; }   // samples waiting in the input ring, in seconds of signal
    SampleLoss sampleLoss() const;
    std::string loadProfile() const;   // seconds the analysis thread spent on the spectrum and on the receiver, and the signal it has processed (diagnostics)

    // Latest spectrum snapshot; returns true if newer than `lastSeq`.
    bool latestSpectrum(SpectrumFrame& out, uint64_t lastSeq);
    // no spectrum fft when headless, autobw needs it
    void setSpectrumEnabled(bool on) { analyzer_.setTransform(on); }
    // display cost: FFT blocks averaged per spectrum frame (16 = every sample at 10 Msps; 4 = a quarter of the FFT work, a noisier trace)
    void setSpectrumBlocksPerFrame(int n) { analyzer_.setBlocksPerFrame((size_t)(n < 1 ? 1 : n)); }
    // The impulse noise blanker on the samples, before the spectrum and every decoder (#264; off by default). threshold: times the mean
    // magnitude; widthUs: the blank after a click. nbBlankedPct: the share of samples blanked over the last second or so.
    void setNoiseBlanker(bool on, float threshold, float widthUs) { nbThr_ = threshold; nbWidthUs_ = widthUs; nbOn_ = on; nbDirty_ = true; }
    bool noiseBlankerOn() const { return nbOn_.load(); }
    float nbBlankedPct() const { return nbPct_.load(); }
    // Fast waterfall rows (SpectrumAnalyzer::setFastRows): 0 = off. The analyser is told the sample rate again after a restart (applyReset).
    void setSpectrumFastRows(double rowsPerSec) { fastRps_ = rowsPerSec; analyzer_.setFastRows(rowsPerSec, rate_.load()); }
    size_t takeFastRows(std::vector<float>& rows, size_t& bins, double& dt, uint64_t& dropped) { return analyzer_.takeFastRows(rows, bins, dt, dropped); }
    int spectrumFft() const { return specFft_.load(); }   // the choice (0: automatic)
    void setSpectrumFft(int n) { specFft_ = n; analyzer_.setSampleRate(rate_.load()); if (n > 0) analyzer_.resize((size_t)n); else analyzer_.resize(SpectrumAnalyzer::autoSize(rate_.load())); }   // 0: automatic from the sample rate

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
    // DVB-T2: export the BB frames of the decoded PLP as raw frames or T2-MI, to a file and/or UDP (t2mi_out.h). Empty config: off. Returns "" or the reason it failed.
    std::string setT2Export(const T2ExportConfig& c);
    uint64_t t2ExportFrames() const { return t2Export_ ? t2Export_->frames() : 0; }

    // Multi-channel TV (DVB-T / DVB-T2, multitv.h): when the capture is wide enough for more than one channel of the chosen width, one receiver runs for
    // every occupied channel of the raster (up to maxChannels), each with its own telemetry and service list; one channel plays at a time while the others
    // keep decoding. grid: 0 finds out whether the capture is centred on a channel or between two, 1 on a channel, 2 between two. Off: the single receiver as before.
    void setMultiChannel(bool on, int maxChannels = 0, int grid = 0);
    bool multiChannel() const { return mtvOn_.load(); }                  // asked for
    bool multiChannelActive() const { return mtvActive_.load(); }        // running now (the mode, the span and the width allow it)
    static bool multiChannelSupported(int stdMode) { return stdMode == 1 || stdMode == 2 || stdMode == 8 || stdMode == 3 || stdMode == 5 || stdMode == 6 || stdMode == 9 || stdMode == 42; }   // DVB-T2, DVB-T, DVB-S/S2 ("Whole span")
    // the capture can hold more than one signal of the standard: T/T2 two channels of the width, S/S2 any capture of 3 Msps and up (transponders sit anywhere)
    static bool wholeSpanFits(int stdMode, double spanRate, double widthMhz, int grid) { return stdMode == 8 ? spanRate >= 3e6 : slotsInSpan(spanRate, widthMhz, grid) >= 2; }
    static int defaultMaxChannels();                                      // 2 on a laptop, 3 on a desktop
    static int slotsInSpan(double spanRate, double widthMhz, int grid);   // channels of the raster that fit the capture
    void selectTvChannel(int slot);                                       // the channel that plays and shows in detail
    int selectedTvChannel() const { auto m = mtvGet(); return m ? m->selected() : 0; }
    std::vector<TvChannelRow> tvChannels();
    std::vector<MultiService> multiServices();                            // every service of every channel, with the channel it comes from
    std::string multiChannelNote() const { std::lock_guard<std::mutex> lk(mtvNoteMu_); return mtvNote_; }
    // Transport stream (phase 4)
    TsSnapshot tsSnapshot();
    void clearServices();   // empties the live service list (the next PAT / SDT fills it again): another transponder was tuned, or the user asked
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
    // A recording (file source): the spectrum stays on its centre; the tune frequency of start()/retune() is the VFO, whose offset from the centre
    // becomes the decode shift (clamped to the usable span). 0 = not a file / not running.
    double fileCentreHz() const { return fileCentreHz_.load(); }
    void setFileCentreHz(double hz);   // the File info dialog: the centre the header lacked (the VFO keeps its offset)
    void setFileVfoHz(double hz);      // the VFO (dial) frequency: the shift follows, nothing else is touched (no hold, no reset of the radio)
    // "Ignore DC spike": a notch at the capture's centre in front of the receiver (the spectrum still shows the spike)
    void setDcNotch(bool on) { dcNotch_ = on; }
    bool dcNotch() const { return dcNotch_.load(); }
    // "Clean the input" (auto gain, 3.6): an adaptive notch for the radio's own spurs (dsp/spur_notch.h), off by default. Cuts persistent narrow lines
    // outside the wanted channel, and inside it only when the same baseband line was seen under another radio centre (instrumental, not a signal).
    void setSpurNotch(bool on) { spurNotch_ = on; }
    bool spurNotch() const { return spurNotch_.load(); }
    int spurNotchLines() const { return spurLines_.load(); }
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
    // FM / AM census: every station of the capture, beside the mode's own receiver (docs/SMART-fmam-3.7.md). Rows from any thread.
    void setFmCensus(bool on) { if (!on) fmCensusReady_ = false; fmCensusOn_ = on; }
    bool fmCensusOn() const { return fmCensusOn_.load(); }
    std::vector<FmCensusRow> fmCensusRows() const { return fmCensus_.rows(); }
    void setDabCensus(bool on) { if (!on) dabCensusReady_ = false; dabCensusOn_ = on; }
    bool dabCensusOn() const { return dabCensusOn_.load(); }
    std::vector<DabCensusRow> dabCensusRows() const { return dabCensus_.rows(); }
    void setAmCensus(bool on) { if (!on) amCensusReady_ = false; amCensusOn_ = on; }
    bool amCensusOn() const { return amCensusOn_.load(); }
    std::vector<AmCensusRow> amCensusRows() const { return amCensus_.rows(); }
    double amCensusStepHz() const { return amCensus_.stepHz(); }
    // AM frequency diversity: general() and generalExtra(0) (the partner frequency) play through AmDiversity; offsetHz is the partner from the dial, the extra channel carries the same demodulator
    void setAmDiversity(bool on, double offsetHz = 0);
    bool amDiversityOn() const { return amDivOn_.load(); }
    AmDivPlayer& amDiversity() { return amDiv_; }
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
    autodet::Capture& autodet() { return autodet_; }   // "TV (auto)" / "Digital voice (auto)": start() a capture of what the receivers see, in any mode (autodet.h)
    AstroReceiver& astro() { return rxAstro_; }   // radio astronomy (hydrogen line): FFT size, integration, frequency switching
    SpecanReceiver& specan() { return rxSpecan_; }   // spectrum analyser: range, resolution, averaging (telemetry through latestRx)
    Js8Receiver& js8() { return rxJs8_; }   // JS8Call: the receiver's own controls and results (the interface reads telemetry through latestRx)
    OrbcommReceiver& orbcomm() { return rxOrb_; }   // Orbcomm: downlinks at once (the interface reads telemetry through latestRx)
    TetraReceiver& tetra() { return rxTetra_; }   // TETRA network information: the interface reads telemetry through latestRx
    TetrapolReceiver& tetrapol() { return rxTetrapol_; }   // TETRAPOL control channel (receive only)
    AleReceiver& ale() { return rxAle_; }   // HF ALE: the receiver's own controls and results (the interface reads telemetry through latestRx)
    P25Receiver& p25() { return rxP25_; }   // P25 Phase 1: sound (the interface reads telemetry through latestRx)
    UplinkReceiver& uplink() { return rxUplink_; }   // Mode S uplink (1030 MHz): telemetry through latestRx
    BtcReceiver& btc() { return rxBtc_; }   // Bluetooth Classic discovery: clear the device table (telemetry through latestRx)
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
    ZigbeeReceiver& thread() { return rxTh_; }   // Thread: clear the network and device tables (telemetry through latestRx)
    ZigbeeReceiver& zigbee() { return rxZb_; }   // Zigbee / 802.15.4: clear the PAN and device tables (telemetry through latestRx)
    AvRoadReceiver& avroad() { return rxAvRoad_; }   // aviation, rail and road modes (families 400 - 411; telemetry through latestRx)
    AdslReceiver& adsl() { return rxAdsl_; }   // ADS-L: receiver position, clear (telemetry through latestRx)
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
    PskdReceiver& pskd() { return rxPskd_; }       // PSK demodulator: settings, analyse again, clear
    RoshfReceiver& roshf() { return rxRoshf_; }   // ROS (HF): clear the detections, sideband (telemetry through latestRx)
    SsdvReceiver& ssdv() { return rxSsdv_; }   // SSDV: speed, save folder, clear the pictures (telemetry through latestRx)
    Fy3Receiver& fy3Ahrpt() { return rxFy3a_; }   // FY-3 AHRPT: satellite preset, symbol rate, roll-off, new pictures (telemetry through latestRx)
    Fy3Receiver& fy3Mpt() { return rxFy3m_; }     // FY-3 MPT: the same
    HfUtilReceiver& hfUtil(int kind) { return kind >= kHfuKinds ? rxHfm_[kind - kHfuKinds] : rxHfu_[kind]; }   // HF utility modes (families 430 .. 439, 603, 604)
    PactorHfReceiver& pactorHf(int variant) { return variant == 3 ? rxPh3_ : variant == 4 ? rxPh4_ : rxPh2_; }   // PACTOR-II / -III / -IV: clear the results (telemetry through latestRx)
    std::vector<Tr290Check> tr290Checks() { std::lock_guard<std::mutex> k(tsMu_); return tr290_.checks(); }
    double tr290JitterMs() { std::lock_guard<std::mutex> k(tsMu_); return tr290_.pcrJitterMs(); }
    bool t2miFound() { std::lock_guard<std::mutex> k(tsMu_); return t2miIn_.found(); }
    int t2miPid() { std::lock_guard<std::mutex> k(tsMu_); return t2miIn_.pid(); }
    std::vector<int> t2miPlps() { std::lock_guard<std::mutex> k(tsMu_); return t2miIn_.plps(); }
    void setT2miPlp(int plp) { t2miUse_ = plp; }   // -1: back to the outer stream
    int t2miPlp() const { return t2miUse_.load(); }
    UmtsReceiver& umts() { return rxUmts_; }   // UMTS cells (family 480)
    CdmaReceiver& cdma() { return rxCdma_; }   // CDMA2000 cells (family 660)
    TdscdmaReceiver& tdscdma() { return rxTds_; }   // TD-SCDMA cells (family 661)
    Vdl3Receiver& vdl3() { return rxVdl3_; }   // VDL Mode 3 (family 640)
    SelcalReceiver& selcal() { return rxSelcal_; }   // SELCAL: call log, aircraft table, Listen volume / mute (telemetry through latestRx)
    AtisReceiver& atis() { return rxAtis_; }   // ATIS (inland waterways): clear the lists, Listen volume / mute (telemetry through latestRx)
    ShortwaveReceiver& shortwave() { return rxSw_; }   // Shortwave: the general receiver and the one-click decoders (telemetry through latestRx)
    SensorsReceiver& sensors() { return rxSens_; }   // Sub-GHz sensors: protocols, custom decoders, clear (telemetry through latestRx)
    BganReceiver& bgan() { return rxBgan_; }   // Inmarsat BGAN (telemetry through latestRx)
    FbrReceiver& fbr() { return rxFbr_; }   // Fleet Broadcast (UHF SATCOM), family 870: bit polarity, clear the log
    LtPosReceiver& ltpos() { return rxLtpos_; }   // Lightning position (network), family 790
    SfericsReceiver& sferics() { return rxSfer_; }   // Sferics (lightning): threshold, mains filter, clear counts (telemetry through latestRx)
    XlinkReceiver& xlink() { return rxXlink_; }   // Extra links (family 600)
    CisHfReceiver& cisHf(int kind) { return rxCis_[kind >= 0 && kind < 7 ? kind : 0]; }   // CIS HF modes (families 620 - 626): Advanced settings, clear
    W1aReceiver& w1a(int kind) { return rxW1a_[kind >= 0 && kind < 3 ? kind : 0]; }   // HF packet 300 Bd, voice inversion, ELT (families 800 - 802): Advanced settings, clear (telemetry through latestRx)
    RcLinkReceiver& rclink() { return rxRc_; }   // RC and mesh links (family 360): lists, phrase, plan (telemetry through latestRx)
    Nrf24Receiver& nrf24() { return rxNrf_; }   // nRF24L01: clear the lists (telemetry through latestRx)
    InmsatReceiver& inmsat() { return rxInmsat_; }   // Inmarsat all carriers: budget, focus, voice mixer (telemetry through latestRx)
    EbicabReceiver& ebicab() { return rxEbi_; }   // EBICab / Eurobalise: clear the lists (telemetry through latestRx)
    EsubghzReceiver& esubghz() { return rxEsg_; }   // Flipper Zero Sub-GHz chat: the key, the lines sent, clear the chat (telemetry through latestRx)
    PactorReceiver& pactor() { return rxPac_; }   // PACTOR-I monitor: clear the packets, Listen (telemetry through latestRx)
    ArdopReceiver& ardop() { return rxArd_; }   // ARDOP: clear the frames, Listen (telemetry through latestRx)
    Mt63Receiver& mt63() { return rxMt63_; }   // MT63: bandwidth and interleave (automatic by default), clear the text (telemetry through latestRx)
    CquamReceiver& cquam() { return rxCq_; }   // AM stereo (C-QUAM): stereo mode, audio bandwidth, squelch, channel offset (telemetry through latestRx)
    EczasLiveReceiver& eczas() { return rxEcz_; }   // e-Czas (225 kHz, Poland): audio input, receiver position, clear the log (telemetry through latestRx)
    FpvReceiver& fpv() { return rxFpv_; }   // FPV video: picture controls, sound, clear the channel levels (telemetry through latestRx)
    ZwaveReceiver& zwave() { return rxZw_; }   // Z-Wave: region, clear the tables (telemetry through latestRx)
    HrdcpReceiver& hrdcp() { return rxHrdcp_; }   // Meteosat HRDCP (family 700): CSV log file, carrier threshold, clear the lists
    GoesDcsReceiver& goesdcs() { return rxGd_; }   // GOES DCS: CSV log file, carrier threshold, clear the lists (telemetry through latestRx)
    ModeAcReceiver& modeac() { return rxModeAc_; }   // Mode A/C replies: reference position, code confirmation and expiry, clear (telemetry through latestRx)
    BletrkLiveReceiver& bletrk() { return rxBt_; }   // BLE tracker detector: alert time, near level, own trackers, clear (telemetry through latestRx)
    SatDetectReceiver& satdetect() { return rxSatd_; }   // satellite carrier and burst detector modes (families 550 on)
    SatbeaconLiveReceiver& satbeacon() { return rxSb_; }   // satellite beacon: CSV log file (telemetry through latestRx)
    MscatterLiveReceiver& mscatter() { return rxMs_; }   // meteor scatter: threshold, CSV log file, clear the list (telemetry through latestRx)
    WisunReceiver& wisun() { return rxWs_; }   // Wi-SUN FAN: region, clear the tables (telemetry through latestRx)
    LorawanReceiver& lorawan() { return rxLw_; }   // LoRaWAN, all channels of a plan: settings, clear (telemetry through latestRx)
    LoraSatReceiver& lorasat() { return rxLs_; }   // LoRa satellites: settings, the pass planner names the satellite
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
    Sd3Receiver& sd3(int p) { return rxSd3_[p]; }      // SatDump tier 3 links (sd3_tel.h), telemetry through latestRx
    HamsciReceiver& hamsci(int kind) { return rxHs_[kind]; }   // ham science modes by kind (hamsci_tel.h): Auto, options, clear (telemetry through latestRx)
    XbandReceiver& xband(int p) { return rxXb_[p]; }   // direct broadcast links by profile (xband_tel.h)
    WxsatReceiver& wxsat(int p) { return rxWx_[p]; }   // weather links by profile (wxsat_tel.h), telemetry through latestRx
    HrptReceiver& hrpt() { return rxHrpt_; }   // HRPT / AHRPT: variant, new picture (telemetry through latestRx)
    GoesReceiver& goes() { return rxGoes_; }   // GOES HRIT: clear the products (telemetry through latestRx)
    Gk2aReceiver& gk2aLrit() { return rxGkL_; }   // GK-2A LRIT (family 230): clear, keys (telemetry through latestRx)
    Gk2aReceiver& gk2aHrit() { return rxGkH_; }   // GK-2A HRIT (family 231)
    FygeoReceiver& fygeo() { return rxFy_; }   // FengYun geostationary: FY-4A / FY-4B LRIT, FY-4A HRIT, FY-2 S-VISSR (telemetry through latestRx)
    ElektroReceiver& elektro() { return rxEl_; }   // Elektro-L HRIT / LRIT / GGAK (telemetry through latestRx)
    ElLoranReceiver& eloran() { return rxEloran_; }   // eLoran / Loran-C: GRI, stations, arrival times (telemetry through latestRx)
    NfcReceiver& nfc() { return rxNfc_; }   // NFC / RFID 13.56 MHz: frame log, tag table, NDEF (telemetry through latestRx)
    IsdbsReceiver& isdbs() { return rxIsdbs_; }   // ISDB-S (BS satellite TV): stream selection (telemetry through latestRx; the packets go to the TV player like DVB-S)
    DvbshEngineReceiver& dvbsh() { return rxDvbsh_; }   // DVB-SH: channel width and HP / LP stream (telemetry through latestRx; the packets go to the TV player)
    MptReceiver& mpt1327() { return rxMpt_; }   // MPT1327 trunking control channel: messages and channel table (telemetry through latestRx)
    FusionReceiver& fusion() { return rxFusion_; }   // Yaesu Fusion: callsigns and slow data, the speech codec plug-in (telemetry through latestRx)
    LtrReceiver& ltr() { return rxLtr_; }   // LTR control channel (telemetry through latestRx)
    ChanVoiceReceiver& chanVoice() { return rxChv_; }   // CB, FRS / GMRS, PMR446, ham FM repeaters, HF voice, MDC-1200, wireless mic finder (families 340 - 346, docs/CHANNELS-3.8.md)
    MarineVhfReceiver& marineVhf() { return rxMarineVhf_; }   // Marine VHF voice: all channels of the span (telemetry through latestRx)
    EdacsReceiver& edacs() { return rxEdacs_; }   // EDACS control channel (telemetry through latestRx)
    SmartnetReceiver& smartnet() { return rxSmartnet_; }   // Motorola Type II (family 320), telemetry through latestRx
    TetraDmoReceiver& tetradmo() { return rxTetraDmo_; }   // TETRA Direct Mode (family 321)
    OpenSkyReceiver& opensky() { return rxOpenSky_; }      // OpenSky identifier (family 322)
    SbasReceiver& sbas() { return rxSbas_; }   // SBAS L1: the decoded messages are cleared from here (telemetry through latestRx)
    AsterixReceiver& asterix() { return rxAsterix_; }   // ASTERIX: UDP / file input, demo, origin; its own telemetry (it runs without a radio)
    ArgosReceiver& argos() { return rxArgos_; }   // Argos uplink / downlink: link choice, clear (telemetry through latestRx)
    SigfoxReceiver& sigfox() { return rxSfx_; }   // Sigfox uplinks: clear (telemetry through latestRx)
    P25p2Receiver& p25p2() { return rxP25p2_; }   // P25 Phase 2 (family 180, engine standard 182)
    MilhfReceiver& milhf() { return rxMilhf_; }   // HF waveform identifier: the audio clip for sigidwiki (telemetry through latestRx)
    DgpsReceiver& dgps() { return rxDgps_; }   // DGPS beacons: the station, the frames (telemetry through latestRx)
    RailwayReceiver& klw() { return rxKlw_; }
    RailwayReceiver& tdcs() { return rxTdcs_; }
    HorusReceiver& horus() { return rxHorus_; }   // Project Horus balloons: tables, channels (telemetry through latestRx)
    LbjReceiver& lbj() { return rxLbj_; }   // LBJ railway warnings (telemetry through latestRx)
    Vdl4Receiver& vdl4() { return rxVdl4_; }   // VDL Mode 4 (telemetry through latestRx)
    UatReceiver& uat() { return rxUat_; }         // UAT 978 MHz ADS-B / FIS-B / TIS-B (telemetry through latestRx)
    MeteorReceiver& meteor() { return rxMeteor_; }   // Meteor-M LRPT: modulation, a new picture (telemetry through latestRx)
    NrReceiver& nr() { return rxNr_; }         // 5G NR cells: clear the cell table (telemetry through latestRx)
    WifiReceiver& wifi() { return rxWifi_; }   // Wi-Fi networks: clear the network table (telemetry through latestRx)
    Qo100Receiver& qo100() { return rxQo100_; }   // QO-100: LNB oscillator, SSB dial and listening (telemetry through latestRx)
    Dvbc2Receiver& dvbc2() { return rxC2_; }   // DVB-C2 (active standard 742): data slice and PLP choice
    IpOut& ipOutput() { return ipOut_; }       // IP datagrams of DVB-H (MPE / MPE-FEC) and DVB-C2 GSE PLPs: pcap file, UDP forward (off by default)
    DvbcReceiver& dvbc() { return rxDvbc_; }   // DVB-C: symbol rate, QAM, annex (the interface reads telemetry through latestRx)
    PskReceiver& psk() { return rxPsk_; }   // PSK31: the receiver's own controls and results (the interface reads telemetry through latestRx)
    DstarReceiver& dstar() { return rxDstar_; }   // D-STAR: the receiver's own controls and results (the interface reads telemetry through latestRx)
    HfdlReceiver& hfdl() { return rxHfdl_; }   // HFDL: the receiver's own controls and results (the interface reads telemetry through latestRx)
    M17Receiver& m17() { return rxM17_; }   // M17: the receiver's own controls and results (the interface reads telemetry through latestRx)
    AirbandReceiver& airband() { return rxAirband_; }   // Airband: the receiver's own controls and results (the interface reads telemetry through latestRx)
    VorReceiver& vor() { return rxVor_; }   // VOR / ILS: the receiver's own controls and results (the interface reads telemetry through latestRx)
    SitorLiveReceiver& sitor() { return rxSitor_; }   // SITOR-B (FEC) text from NAVTEX and HF coast stations (telemetry through latestRx)
    DmeLiveReceiver& dme() { return rxDme_; }   // DME: channel X or Y, pair rate, the Morse ident (telemetry through latestRx)
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
    // "Decode everything in view": one wide capture, every signal in it found, identified and decoded at once (wide_session.h). While on it takes
    // the capture before the mode's own receiver (the mode's receiver is idle); off: nothing here runs.
    // Smart CubeSat (docs/CUBESAT-SMART.md): in the CubeSat mode the whole capture goes to the smart decoder (any thread to switch and read)
    void setSmartCube(bool on) { smartCubeOn_.store(on); }
    bool smartCube() const { return smartCubeOn_.load(); }
    cubesat::SmartCubesat& smartCubeDecoder() { return smartCube_; }
    void setWideAll(bool on) { wideAllOn_.store(on); }
    bool wideAll() const { return wideAllOn_.load(); }
    WideSession& wideAllSession() { return wideAll_; }   // entries(), status() and the controls (any thread)
    void setWideAllBudget(double frac) { wideAllBudget_.store(frac); }   // CPU limit as a share of one core (0: none); default 0.8
    void setWideMuted(bool m) { wide_.setMuted(m); }
    void setWideVolume(float v) { wide_.setVolume(v); }   // the volume slider reaches every carrier receiver of the whole-span modes
    void setWideSelected(int carrierId) { wide_.setSelected(carrierId); }   // the carrier whose sound plays and whose detail shows (0: the best locked)
    static bool wideSupported(int analysisMode) { return WideModes::supports(analysisMode); }
    // the whole-span detector's settings (threshold, hysteresis, minimum SNR, dwell, hold, cap, CPU budget, known channels)
    void setWideTuning(const WideTuning& t) { wide_.setTuning(t); }
    WideTuning wideTuning() const { return wide_.tuning(); }
    void setWideExtraPriors(const std::vector<double>& absHz) { wide_.setExtraPriors(absHz); }
    void setWideFollow(bool on, const std::vector<double>& absHz) { wide_.setFollowOnly(on, absHz); }   // only these channels get a decoder (WideModes::setFollowOnly)
    // IQ of one carrier of the running mode to an IQ WAV file in dir (the path, "" on failure)
    std::string startWideRecording(int carrierId, const std::string& dir) { return wide_.startRecording(activeStd_.load(), carrierId, dir); }
    void stopWideRecording(int carrierId) { wide_.stopRecording(activeStd_.load(), carrierId); }
    // Multi-VFO (vfo.h): extra channels on the same capture, each with its own frequency, mode, squelch and mixer settings; their sound is mixed
    // and played beside the mode's own. Offsets are from the capture centre (captureCentreHz()). With no channels nothing runs. Any thread.
    void setVfoChannels(const std::vector<VfoConfig>& c) { vfo_.setChannels(c); vfoN_.store((int)c.size()); if (c.empty()) vfoCloseSound(); }
    size_t vfoChannelCount() const { return (size_t)vfoN_.load(); }
    std::vector<VfoStatus> vfoStatus() const { return vfo_.status(); }
    bool vfoPager(int id, PagerTelemetry& t) const { return vfo_.pagerTelemetry(id, t); }
    bool vfoPacket(int id, PacketTelemetry& t) const { return vfo_.packetTelemetry(id, t); }
    double vfoLoad() const { return vfo_.load(); }                       // above 1 the channels fall behind: warn, never refuse
    std::string vfoWarning(int id) const { return vfo_.warning(id); }
    void vfoClearResults() { vfo_.clearResults(); }
    void setVfoSound(bool on) { vfoSoundOn_.store(on); }                // false: decode and measure, never open the sound device (tests, --mute)
    void clearAllResults();                              // the lists of every mode at once (the top bar's Clear all)
    // the files of the DSM-CC carousels of the stream received, saved to dir/pid_XXXX/path (clear broadcast data); returns the number of files written
    size_t saveCarouselFiles(const std::string& dir) { std::lock_guard<std::mutex> lk(tsMu_); return demux_.saveCarouselFiles(dir); }
    std::map<int, std::vector<DsmccFile>> carouselFiles(size_t maxBytes = 32u << 20) { std::lock_guard<std::mutex> lk(tsMu_); return demux_.carouselFiles(maxBytes); }
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
    void rxLog(const std::string& line) { if (!rxLogQuiet_.load(std::memory_order_relaxed)) log(line); }   // a receiver's own message: dropped while start() configures every receiver
    std::atomic<bool> rxLogQuiet_{false};
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
    std::vector<std::vector<cf32>> specFree_;   // blocks the spectrum thread is done with, reused by feedSpectrum (no allocation per block)
    bool specStop_ = false;
    void catchUp();
    void logRxEvents(const RxTelemetry& t);
    std::function<void(const PlpResult&)> plpDump_;
    std::unique_ptr<T2Export> t2Export_;
    std::mutex t2ExportMu_;
    void onPlp(const PlpResult& r);
    // multi-channel TV (setMultiChannel)
    std::shared_ptr<MultiTv> mtv_;
    mutable std::mutex mtvMu_;               // mtv_ (the analysis thread replaces it, the interface reads it)
    std::shared_ptr<MultiTv> mtvGet() const { std::lock_guard<std::mutex> lk(mtvMu_); return mtv_; }
    std::atomic<bool> mtvOn_{false}, mtvActive_{false}, mtvSat_{false};
    std::atomic<int> mtvOther_{-1};     // Whole span of ATSC, ATSC 3.0, ISDB-T, DTMB or DVB-C: its receiver number
    bool mtvMatches(int act) const { const int o = mtvOther_.load(std::memory_order_relaxed); return o >= 0 ? act == o : mtvSat_.load(std::memory_order_relaxed) ? act == 7 : act <= 1; }
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
    BbUnpacker commonUnpack_; std::vector<uint8_t> commonFrame_; T2PlpMerge plpMerge_;   // the common PLP of the selected group
    TsDemux demux_;
    TsSnapshot tsSnap_;
    // declared before the receivers: their worker threads call onTsPackets until they are joined in the receivers' destructors
    Tr290Monitor tr290_; double tr290T_ = 0;
    T2miIngest t2miIn_; std::vector<uint8_t> t2miInner_; std::atomic<int> t2miUse_{-1};   // -1 off, else the PLP of the T2-MI stream shown instead of the outer stream
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
    std::atomic<double> fileCentreHz_{0};
    std::atomic<bool> dcNotch_{false};
    DcNotch dcn_;
    std::atomic<bool> spurNotch_{false};
    std::atomic<int> spurLines_{0};
    SpurNotch spurn_;
    double spurRate_ = 0;
    void fileFollowVfo(const TuneSettings& t);
    OffsetMixer decodeMix_;
    // with offset tuning the radio runs faster than the mode needs; right after the shift the samples go back to the mode's own rate, so the
    // receivers, the spectrum and the waterfall see exactly what they see without the offset (only the radio's USB stream is larger)
    ExactResampler offRs_;
    bool offResample_ = false;
    std::vector<cf32> offBuf_;
    ag::GainMeter gainMeter_;
    double gmRate_ = 0;
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
    mpe::MpeDemux mpe_;      // DVB-H: MPE / MPE-FEC IP datagrams of the stream (under tsMu_)
    IpOut ipOut_;            // where those datagrams and the DVB-C2 GSE packets go (thread-safe)
    int lastT2Frame_ = -1;
    std::atomic<bool> resetReq_{false};
    std::atomic<uint64_t> resets_{0};
    int computeMode_ = 2;
    gpupol::Policy gpu_;
    void applyCompute() {
        gpupol::live().compute = computeMode_;
        for (int d = 0; d < gpupol::kDomains; d++) gpupol::live().allowed[d] = gpu_.allowed[d].load();
        rx_.setComputeMode(gpupol::effectiveMode(computeMode_, gpu_.allowed[gpupol::T2]));
        rxT_.setComputeMode(gpupol::effectiveMode(computeMode_, gpu_.allowed[gpupol::T]));
        rxDvbs_.setComputeMode(gpupol::effectiveMode(computeMode_, gpu_.allowed[gpupol::DvbS]));
        rxDtmb_.setComputeMode(gpupol::effectiveMode(computeMode_, gpu_.allowed[gpupol::Dtmb]));
    }
    void applyReset();
    std::vector<uint8_t> frameBuf_;
    double tSpec_ = 0, tRx_ = 0, tRxCpu_ = 0;   // analysis thread: wall and CPU seconds in the receiver
    double nSamp_ = 0;
    uint64_t logP1Count_ = 0;
    int logS1_ = -1, logS2_ = -1, logGi_ = -2, logState_ = -1, logFrameSyms_ = 0;

    IqRing ring_;
    // CPU accounts (cpu_meter.h): "rx<N> front" (the sample path), "rx<N> spectrum", "rx<N> decoder", "rx<N> second"; made from receiverId_
    std::atomic<int> cpuFront_{0}, cpuSpec_{0}, cpuDec_{0}, cpuSec_{0};
    void cpuAccounts();
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
    std::atomic<double> fastRps_{0};   // the fast waterfall rows wanted (setSpectrumFastRows)
    std::atomic<int> specFft_{0};
    NoiseBlanker nb_;   // the analysis thread's; set from the atomics below when nbDirty_
    std::atomic<bool> nbOn_{false}, nbDirty_{true};
    std::atomic<float> nbThr_{8.f}, nbWidthUs_{20.f}, nbPct_{0.f};
    double nbRate_ = 0; uint64_t nbSeen0_ = 0, nbBlank0_ = 0; bool nbWas_ = false;
    T2Receiver rx_;
    DvbtReceiver rxT_;
    AtscReceiver rxA_;
    DabReceiver rxD_;
    Atsc3Rx rxA3_;
    IsdbtReceiver rxI_;
    IsdbtReceiver rxTsb_;   // ISDB-TSB (active standard 731): the narrow-band one- and three-segment receiver
    FmReceiver rxFm_;
    FmCensus fmCensus_;
    DabCensus dabCensus_; std::atomic<bool> dabCensusOn_{false}; bool dabCensusReady_ = false; double dabCensusC_ = 0, dabCensusR_ = 0;
    AmCensus amCensus_;
    std::atomic<bool> fmCensusOn_{false}, amCensusOn_{false}, amDivOn_{false};
    bool fmCensusReady_ = false, amCensusReady_ = false;
    double fmCensusC_ = 0, fmCensusR_ = 0, amCensusC_ = 0, amCensusR_ = 0;
    AmDivPlayer amDiv_;
    DvbsReceiver rxDvbs_;
    DtmbReceiver rxDtmb_;
    AtvReceiver rxAtv_;
    DmrReceiver rxDmr_;
    WideModes wide_;
    WideSession wideAll_;
    std::atomic<bool> wideAllOn_{false};
    std::atomic<double> wideAllCentre_{0}, wideAllBudget_{0.8};
    double wideAllKeyRate_ = 0, wideAllKeyCentre_ = -1; bool wideAllKeyOn_ = false;
    void syncWideAll();
    cubesat::SmartCubesat smartCube_;
    std::atomic<bool> smartCubeOn_{false};
    double smartKeyRate_ = 0, smartKeyCentre_ = -1; bool smartKeyOn_ = false;
    void syncSmartCube();
    VfoBank vfo_;
    std::atomic<int> vfoN_{0};
    std::atomic<bool> vfoSoundOn_{true};
    std::unique_ptr<AudioOut> vfoOut_;
    std::mutex vfoOutMu_;
    void vfoFeed(const cf32* x, size_t n);
    void vfoCloseSound();
    std::atomic<bool> wideOn_{false};
    uint64_t wideSeq_[256] = {};
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
    SitorLiveReceiver rxSitor_;
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
    autodet::Capture autodet_;
    SpecanReceiver rxSpecan_;
    AstroReceiver rxAstro_;
    bool specanSweep_ = false;   // the HackRF sweeps by itself (spectrum analyser)
    DvbcReceiver rxDvbc_;
    Dvbc2Receiver rxC2_;   // DVB-C2 (active standard 742, family 741)
    OrbcommReceiver rxOrb_;
    UplinkReceiver rxUplink_;
    TetraReceiver rxTetra_;
    AleReceiver rxAle_;
    BleReceiver rxBle_;
    BtcReceiver rxBtc_;
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
    ZigbeeReceiver rxTh_;   // Thread (the 802.15.4 receiver in Thread mode)
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
    LorawanReceiver rxLw_;
    LoraSatReceiver rxLs_;
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
    PactorReceiver rxPac_;
    HfUtilReceiver rxHfu_[kHfuKinds] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint64_t hfuSeq_[kHfuKinds] = {};
    HfUtilReceiver rxHfm_[kHfuKindsAll - kHfuKinds] = {kHfuKindHdr, kHfuKindAle3g, kHfuKindLink11, kHfuKind4481};   // 3.9 HF utility kinds (families 603 STANAG 4539, 604 3G-ALE, 605 Link 11, 606 STANAG 4481)
    uint64_t hfmSeq_[kHfuKindsAll - kHfuKinds] = {};
    EsubghzReceiver rxEsg_;
    RoshfReceiver rxRoshf_;
    SsdvReceiver rxSsdv_;
    EbicabReceiver rxEbi_;
    InmsatReceiver rxInmsat_;
    XlinkReceiver rxXlink_;   // Extra links
    CisHfReceiver rxCis_[7]{0, 1, 2, 3, 4, 5, 6};   // CIS HF modes, families 620 - 626 (engine standards 621 - 627)
    W1aReceiver rxW1a_[3]{0, 1, 2};   // families 800 - 802 (engine standards 802 - 804)
    RcLinkReceiver rxRc_;   // RC and mesh links
    Nrf24Receiver rxNrf_;
    SensorsReceiver rxSens_;
    BganReceiver rxBgan_;
    SfericsReceiver rxSfer_;
    LtPosReceiver rxLtpos_;
    FbrReceiver rxFbr_;
    AtisReceiver rxAtis_;
    SelcalReceiver rxSelcal_;
    UmtsReceiver rxUmts_;
    CdmaReceiver rxCdma_;
    TdscdmaReceiver rxTds_;
    Vdl3Receiver rxVdl3_;
    Fy3Receiver rxFy3a_{0}, rxFy3m_{1};
    PactorHfReceiver rxPh2_{2}, rxPh3_{3}, rxPh4_{4};   // PACTOR-II (family 237), -III (238), -IV (239)
    ShortwaveReceiver rxSw_;
    PskdReceiver rxPskd_;
    MscatterLiveReceiver rxMs_;
    GoesDcsReceiver rxGd_;
    HrdcpReceiver rxHrdcp_;   // Meteosat HRDCP
    ModeAcReceiver rxModeAc_;
    BletrkLiveReceiver rxBt_;
    SatDetectReceiver rxSatd_;
    NavicReceiver rxNavic_;
    SatbeaconLiveReceiver rxSb_;
    RidReceiver rxRid_;
    MeteorReceiver rxMeteor_;
    NrReceiver rxNr_;
    GoesReceiver rxGoes_;
    ElektroReceiver rxEl_;
    Gk2aReceiver rxGkL_{0};
    Gk2aReceiver rxGkH_{1};
    FygeoReceiver rxFy_;
    LbjReceiver rxLbj_;
    RailwayReceiver rxKlw_;
    RailwayReceiver rxTdcs_{railway::Protocol::Tdcs};
    HorusReceiver rxHorus_;
    DgpsReceiver rxDgps_;
    ElLoranReceiver rxEloran_;
    NfcReceiver rxNfc_;
    MptReceiver rxMpt_;
    IsdbsReceiver rxIsdbs_;
    DvbshEngineReceiver rxDvbsh_;   // DVB-SH (active standard 747)
    FusionReceiver rxFusion_;
    LtrReceiver rxLtr_;
    EdacsReceiver rxEdacs_;
    SmartnetReceiver rxSmartnet_;
    TetraDmoReceiver rxTetraDmo_;
    OpenSkyReceiver rxOpenSky_;
    MarineVhfReceiver rxMarineVhf_;
    ChanVoiceReceiver rxChv_;   // one receiver for the seven channelised voice modes (active standards 341 - 347)
    P25p2Receiver rxP25p2_;
    MilhfReceiver rxMilhf_;
    SbasReceiver rxSbas_;
    AsterixReceiver rxAsterix_;
    ArgosReceiver rxArgos_;
    SigfoxReceiver rxSfx_;
    Vdl4Receiver rxVdl4_;
    UatReceiver rxUat_;
    HrptReceiver rxHrpt_;
    Sd3Receiver rxSd3_[kSd3NumProfiles]{{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10}};   // families 290 - 299, 510
    WxsatReceiver rxWx_[kWxNumProfiles]{{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11}};   // the SatDump-list weather links, families 260 - 271 (slots 220 - 231)
    HamsciReceiver rxHs_[kHamsciKinds]{{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}};   // QRSS, DFCW, FSK441, ISCAT, PI4, Throb, SuperSID, riometer, pulsar: families 460 - 465, 500 - 502 (slots 461 - 466, 501 - 503)
    XbandReceiver rxXb_[kXbNumProfiles]{{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18},{19}};   // the X-band links, families 530 - 549 (slots 530 - 549)
    FlarmReceiver rxFlarm_;
    AdslReceiver rxAdsl_;
    AvRoadReceiver rxAvRoad_;
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
    int logTMode_ = -1, logTGi_ = -1, logTTps_ = -1; bool logTFec_ = false; uint64_t logTFecDropped_ = 0; std::chrono::steady_clock::time_point logTFecAt_{};
    std::mutex rxMu_;
    RxTelemetry rxTel_;
    // Every receiver counts its reports from 1, so after a change of mode (or a restart) its numbers would lie below the last one the interface
    // took and latestRx() would hold them back. The published report therefore carries a number of the engine that only grows. Under rxMu_.
    uint64_t rxPub_ = 0;
    void publishRx(RxTelemetry&& t) { t.seq = ++rxPub_; rxTel_ = std::move(t); }
    std::thread th_;
    std::unique_ptr<power::ReceivingActivity> nap_;   // held while this engine receives: keeps macOS App Nap off only then (power/power.h)
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
    std::atomic<bool> lastNetwork_{false};   // lastDev_.isNetwork(), set with lastDev_; read without tuneMu_ by sampleLoss() (interface thread) and watchRadio()
    TuneSettings lastTune_;
    std::chrono::steady_clock::time_point lastSamples_{}, nextReconnect_{}, lastSkipLog_{}, lostSince_{};
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
    std::atomic<float> loadPct_{0}, cpuPct_{0};
    std::atomic<float> deliveredPct_{-1};   // checkDelivery(): samples the library offered over wall time, percent of srcRate_; -1 until a window completes
    std::atomic<bool> live_{false};   // the source is a live radio (sampleLoss() is read by the interface without touching src_)
    double loadT_ = 0, loadRx_ = 0, loadSamp_ = 0, loadRxCpu_ = 0;   // analysis thread: where the last load measurement started
    int64_t rateT0_ = 0;       // analysis thread: start of the delivered-rate window (steady ns); 0 = not started
    uint64_t rateOff0_ = 0;    // offered-sample count at rateT0_
    std::string reconnectErr_;
    mutable std::mutex noteMu_;
    std::string startErr_;
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
