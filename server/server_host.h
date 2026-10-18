// The headless server's radio: an Engine with no window, behind the remote-control API (core/src/net/remote_api.cpp). It owns the device list, the
// chosen mode / frequency / gain, the receiver's on-off intent (so it comes back after a radio was unplugged), the browser page, the spectrum and
// telemetry snapshots and the audio hub for the browser's Listen button. The settings that the browser changes are written back to the config file.
#pragma once
#include "dect2/engine/engine.h"
#include "dect2/engine/quality.h"
#include "dect2/net/remote_api.h"
#include "dect2/source/gain.h"
#include "server_config.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    std::string devicesJson() override;
    std::string modesJson() override;
    std::string telemetryJson(uint64_t& seq) override;
    std::string spectrumJson(int maxBins, uint64_t& seq) override;
    bool start(int deviceIndex, std::string& err) override;
    bool stop(std::string& err) override;
    bool setMode(int family, const std::string& subId, std::string& err) override;
    bool setFrequencyHz(double hz, std::string& err) override;
    bool setSampleRateHz(double hz, std::string& err) override;
    bool setGainDb(double db, std::string& err) override;
    std::string indexHtml() override;
    bool spectrumBinary(int bins, uint64_t& seq, std::string& frame) override;
    int audioOpen(int rateHz) override;
    bool audioRead(int id, std::string& pcm) override;
    void audioClose(int id) override;
    int handleExtra(const std::string& method, const std::string& path, const std::map<std::string, dect2::JsonVal>& body, std::string& json) override;

    // Resolves a mode id or number text ("fm", "dvbs", "ft8", "7") to a mode entry; nullptr when it is none.
    const ModeEntry* findMode(const std::string& idOrNumber) const;

private:
    ConfigFile& cfg_;
    dect2::Engine e_;
    mutable std::mutex mu_;                 // the chosen settings and the engine calls
    std::vector<dect2::DeviceInfo> devices_;
    double devicesAt_ = -1e9;
    std::vector<ModeEntry> modes_;
    int devIdx_ = 0, modeIdx_ = 0;
    double freqHz_ = 100e6, rateHz_ = 0, gainDb_ = 30;
    int demod_ = 4;                         // HfDemod: 0 USB, 1 LSB, 2 NFM, 3 AM, 4 automatic
    dect2::TuneSettings tune_;
    std::atomic<bool> wantRun_{false}, dirty_{false};
    std::string err_;
    double lastTry_ = -1e9;

    // published by the snapshot thread
    std::mutex snapMu_;
    std::string telJson_ = "{\"running\":false,\"available\":false}";
    uint64_t telSeq_ = 0;
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
