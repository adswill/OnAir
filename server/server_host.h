// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The headless server's radio: an Engine with no window, behind the remote-control API (core/src/net/remote_api.cpp). It owns the device list, the
// chosen mode / frequency / gain, the receiver's on-off intent (so it comes back after a radio was unplugged), the browser page, the spectrum and
// telemetry snapshots and the audio hub for the browser's Listen button. The settings that the browser changes are written back to the config file.
#pragma once
#include "dect2/engine/engine.h"
#include "dect2/engine/quality.h"
#include "dect2/net/remote_api.h"
#include "dect2/source/gain.h"
#include "dect2/tetra/tetra_voice_codec.h"
#include "server_config.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace onair_web { class WebHub; }

namespace onair_server {

// A HackRF's total gain as its stages. The amp stays off unless the settings file says amp=1 (it can be damaged by strong signals), the same
// rule as the app: without it the scale ends at 102 dB.
dect2::GainSetting hackrfGain(const dect2::DeviceInfo& d, double db, bool amp);

struct ModeEntry { int family = 0; std::string id, name, sub; double defMhz = 100; };

class ServerHost : public dect2::RemoteApiHost {
public:
    explicit ServerHost(ConfigFile& cfg);
    ~ServerHost() override;
    // Reads the device list and the saved state; does not start the receiver.
    void init();
    // Once a second from the main thread: starts the receiver when it is wanted and not running (every 10 s at most), writes changed settings to `path`.
    void supervise(const std::string& configPath);
    bool wantRun() const { return wantRun_.load(); }
    void shutdown();                        // stops the receiver without changing what is wanted
    std::string lastError() const;
    dect2::Engine& engine() { return e_; }

    // RemoteApiHost
    std::string statusJson() override;
    bool metrics(dect2::apipol::Metrics& m) override;
    std::string devicesJson() override;
    std::string modesJson() override;
    std::string telemetryJson(uint64_t& seq) override;
    std::string panelJson(uint64_t& seq);   // the mode's web panel (dect2::webschema::panelJson), built once per new telemetry report; "null" when not running
    void captureInfo(double& centreHz, double& rateHz);   // what the spectrum frames describe
    std::string spectrumJson(int maxBins, uint64_t& seq) override;
    bool start(int deviceIndex, std::string& err) override;
    bool stop(std::string& err) override;
    bool setMode(int family, const std::string& subId, std::string& err) override;
    bool setFrequencyHz(double hz, std::string& err) override;
    bool setSampleRateHz(double hz, std::string& err) override;
    bool setCaptureBandwidthHz(double hz, std::string& err) override;
    bool setGainDb(double db, std::string& err) override;
    std::string indexHtml() override;
    bool spectrumBinary(int bins, uint64_t& seq, std::string& frame) override;
    // Waterfall zoom: the part of the capture zoom times narrower than the whole, centred on centreHz (moved inside the capture), as the
    // same binary frame (its centre and width are those of the part). zoom 1: the whole capture. At least 64 FFT bins stay visible.
    bool spectrumBinaryZoom(int bins, double zoom, double centreHz, uint64_t& seq, std::string& frame);
    // Mode options (dect2/results/web_options.h): the current mode's own setters, applied now and again at every start, kept in the settings
    // file as opt.<standard>.<name>. optionValuesJson: {"standard":n,"values":{name:value,...}} of the current mode.
    bool setOption(const std::string& name, double value, std::string& err);
    std::string optionValuesJson();
    int telemetryStandard() const;           // RxTelemetry::standard of the chosen mode (-1: a TV / broadcast family)
    // What the browser hears: "fm" (the FM receiver's stereo), "voice" (a digital voice mode's decoded speech), "monitor" (the Listen demodulator)
    std::string audioSource() const;
    int audioOpen(int rateHz) override;
    bool audioRead(int id, std::string& pcm) override;
    void audioClose(int id) override;
    int handleExtra(const std::string& method, const std::string& path, const std::map<std::string, dect2::JsonVal>& body, std::string& json) override;
    bool recordStart(const std::string& kind, std::string& err) override;   // "iq": a .cf32 file in rec_dir (default: ~/OnAir-recordings); audio is not recorded here
    bool recordStop(std::string& err) override;
    std::string recordingJson() override;

    // Resolves a mode id or number text ("fm", "dvbs", "ft8", "7") to a mode entry; nullptr when it is none.
    const ModeEntry* findMode(const std::string& idOrNumber) const;

    // OnAir Web (server/web): the radio's raw samples go to the hub (nullptr: none); can be set and removed at any time
    void setWebHub(onair_web::WebHub* hub);
    std::string modeId() const;                                     // the chosen mode's id ("fm", "dmr", ...)
    void editConfig(const std::function<void(ConfigFile&)>& f);     // changes the settings file (under the lock); supervise() writes it

    // Voice codec plug-ins (docs/CODECS.md). The server has no notice dialog: a codec's notice counts as accepted when the settings file has
    // codec_notice_<id>=1 (the key the desktop app uses), set by hand, by --accept-codec-notice or by the owner in the browser app. Accepted
    // plug-ins are registered at init(), looked for again every 10 s from supervise() (a file dropped in while running), and at once when
    // force is set. TETRA and Yaesu System Fusion get their decoder from here; the AMBE / IMBE modes ask the codec registry themselves.
    void codecTick(bool force);
    // The owner accepts the notice for one patented codec id: stored in the settings file, the plug-in is looked for at once.
    bool acceptCodecNotice(const std::string& id, std::string& err);
    // For the browser app's settings: the notice text, the folders searched and each patented codec's state.
    std::string codecsJson();
    dect2::tetra::TetraVoiceSink& tetraVoice() { return tetraVoice_; }   // tests: the TETRA speech path into the browser's audio

private:
    ConfigFile& cfg_;
    // TETRA and TETRA DMO voice: clear traffic frames to the user's codec plug-in, the speech to the browser. Declared before the engine,
    // so that it outlives the receivers that hold a pointer to it.
    dect2::tetra::TetraVoiceSink tetraVoice_;
    dect2::Engine e_;
    mutable std::mutex mu_;                 // the chosen settings and the engine calls
    std::vector<dect2::DeviceInfo> devices_;
    double devicesAt_ = -1e9;
    std::vector<ModeEntry> modes_;
    int devIdx_ = 0, modeIdx_ = 0;
    double freqHz_ = 100e6, rateHz_ = 0, gainDb_ = 30, captureBwMhz_ = 0;   // captureBwMhz_: the custom capture bandwidth (0 = the mode's own)
    int demod_ = 4;                         // HfDemod: 0 USB, 1 LSB, 2 NFM, 3 AM, 4 automatic
    dect2::TuneSettings tune_;
    std::atomic<bool> wantRun_{false}, dirty_{false};
    std::string err_;
    double lastTry_ = -1e9;

    // published by the snapshot thread
    std::mutex snapMu_;
    std::string telJson_ = "{\"running\":false,\"available\":false}";
    uint64_t telSeq_ = 0;
    std::string recPath_;                   // the IQ recording running now (under mu_), empty: none
    double recStart_ = 0;
    dect2::RxTelemetry rxSnap_;             // the latest report, for the web panel (built on demand under snapMu_)
    uint64_t rxSnapSeq_ = 0, panelSeq_ = 0;
    std::string panelJson_;
    dect2::SpectrumFrame spec_;
    double specCentreHz_ = 0, specRateHz_ = 0;
    std::thread snapTh_;
    std::atomic<bool> quit_{false};
    dect2::QualityMeter quality_;

    // audio hub
    struct Listener { int rate = 16000, step = 3, cnt = 0; double acc = 0; std::string pcm; };
    std::mutex aMu_;
    std::map<int, Listener> listeners_;
    int nextListener_ = 1;
    std::atomic<bool> fmTap_{false};
    std::atomic<bool> voiceTap_{false};     // a digital voice mode: its decoded speech goes to the browser instead of the monitor
    // 8 kHz (or any rate) mono speech -> 48 kHz for audioIn, by linear interpolation. One state per caller: the engine thread's voice modes and
    // TETRA's decoder thread (whose last capture can still be decoded while the next mode already speaks).
    struct Upsampler {
        double phase = 0;
        float last = 0;
        int rate = 0;
        void run(const float* x, size_t n, int rate, std::vector<float>& out);
    };
    Upsampler voiceUp_;                     // engine thread only
    void voiceIn(const float* x, size_t n, int rate);
    // TETRA speech comes a 2.3 s capture at a time, decoded faster than the air: it is held for the end of a capture that starts from silence
    // (CaptureLeadIn, as in the app) and handed to the listeners at the pace of the air when they fetch their audio (pumpPaced). TETRA DMO
    // speaks in real time and never ends a capture: a hold older than kHoldMaxSec is let go (as the app does after a retune). Nothing is kept
    // while nobody listens.
    static constexpr double kHoldMaxSec = 1.0;
    std::mutex pMu_;
    dect2::tetra::CaptureLeadIn tetraLead_{48000, 1, dect2::TetraReceiver::kCaptureSec};
    Upsampler tetraUp_;                     // under pMu_
    std::vector<float> paced_;              // 48 kHz mono, not yet handed out
    double pacedAt_ = 0;                    // the time up to which paced audio has been handed out
    double holdSince_ = 0;                  // when the hold of tetraLead_ began
    void tetraPcm(const int16_t* pcm, size_t n, int rate);
    void tetraCaptureEnd();
    void releaseHoldLocked(double now);     // under pMu_: the held audio with its lead into paced_
    void resetPacedLocked();                // under pMu_: drops the held and the paced audio
    void pumpPaced();
    bool anyListener();
    // codecTick
    std::mutex codecMu_;
    double codecAt_ = -1e9;
    bool fusionCodec_ = false;              // the YSF plug-in was handed to the Fusion receiver (under codecMu_)
    void applyOptionsLocked();              // under mu_: the stored options of the chosen mode

    void snapLoop();
    void audioIn(const float* l, const float* r, size_t n, bool interleaved);
    void applyMonitor();                    // under mu_
    void buildModes();
    bool startLocked(std::string& err);
    void stopLocked();
    bool retuneLocked(std::string& err);
    void pickDevice(const std::string& want);
    void refreshDevices(bool force);        // under mu_
    void saveState();                       // under mu_: into cfg_, marks it for writing
    int effectiveDemod() const;
    dect2::TuneSettings buildTune(const dect2::DeviceInfo& d) const;
};

} // namespace onair_server
