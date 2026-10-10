// Native driver for the SDRplay RSPs (RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx, RSPdx-R2) through the SDRplay API 3.x. The API is a
// service plus a library that the user installs from sdrplay.com (every SDRplay program needs it; OnAir does not ship it); the library is
// loaded at run time like the other radios' libraries (native_common.h). The API's interface is in sdrplay_api_min.h.
#include "native_common.h"
#define DECT2_SDRPLAY_CALL DECT2_CALL
#include "sdrplay_api_min.h"

namespace dect2 {
namespace native {

namespace sp = dect2::sdrplay;

// Where the SDRplay API installer puts the library (SDRplay API Specification 3.15, section 6)
static std::vector<std::string> sdrplayLibNames() {
    std::vector<std::string> v;
#ifdef _WIN32
#ifdef _WIN64
    const char* arch = "\\x64\\sdrplay_api.dll";
#else
    const char* arch = "\\x86\\sdrplay_api.dll";
#endif
    // the installer records its folder (it can be changed during the install)
    for (const char* key : {"SOFTWARE\\SDRplay\\Service\\API", "SOFTWARE\\WOW6432Node\\SDRplay\\Service\\API"}) {
        HKEY h = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) continue;
        char dir[MAX_PATH * 2] = {0};
        DWORD size = sizeof dir - 1, type = 0;
        if (RegQueryValueExA(h, "Install_Dir", nullptr, &type, (LPBYTE)dir, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && dir[0]) {
            std::string d(dir, strnlen(dir, sizeof dir));
            while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
            v.push_back(d + arch);
        }
        RegCloseKey(h);
    }
    for (const char* env : {"ProgramW6432", "ProgramFiles"})
        if (const char* pf = getenv(env)) v.push_back(std::string(pf) + "\\SDRplay\\API" + arch);
    v.push_back(std::string("C:\\Program Files\\SDRplay\\API") + arch);
    {   // a copy next to the program
        char path[MAX_PATH * 2] = {0};
        if (GetModuleFileNameA(nullptr, path, (DWORD)sizeof path - 1)) {
            std::string dir = path;
            const size_t cut = dir.find_last_of("\\/");
            if (cut != std::string::npos) v.push_back(dir.substr(0, cut + 1) + "sdrplay_api.dll");
        }
    }
    v.push_back("sdrplay_api.dll");
#else
    // the API installers for macOS and Linux both install libsdrplay_api.so.3 (also on macOS: .so, not .dylib)
    for (const char* dir : {"/usr/local/lib/", "/usr/lib/", "/opt/homebrew/lib/"}) {
        v.push_back(std::string(dir) + "libsdrplay_api.so.3");
        v.push_back(std::string(dir) + "libsdrplay_api.so");
    }
    v.push_back("libsdrplay_api.so.3");
    v.push_back("libsdrplay_api.so");
#endif
    return v;
}

struct SdrplayApi {
    sp::ErrT (DECT2_CALL* Open)(void) = nullptr;
    sp::ErrT (DECT2_CALL* Close)(void) = nullptr;
    sp::ErrT (DECT2_CALL* ApiVersion)(float*) = nullptr;
    sp::ErrT (DECT2_CALL* LockDeviceApi)(void) = nullptr;
    sp::ErrT (DECT2_CALL* UnlockDeviceApi)(void) = nullptr;
    sp::ErrT (DECT2_CALL* GetDevices)(sp::DeviceT*, unsigned int*, unsigned int) = nullptr;
    sp::ErrT (DECT2_CALL* SelectDevice)(sp::DeviceT*) = nullptr;
    sp::ErrT (DECT2_CALL* ReleaseDevice)(sp::DeviceT*) = nullptr;
    const char* (DECT2_CALL* GetErrorString)(sp::ErrT) = nullptr;
    sp::ErrT (DECT2_CALL* GetDeviceParams)(void*, sp::DeviceParamsT**) = nullptr;
    sp::ErrT (DECT2_CALL* Init)(void*, sp::CallbackFnsT*, void*) = nullptr;
    sp::ErrT (DECT2_CALL* Uninit)(void*) = nullptr;
    sp::ErrT (DECT2_CALL* Update)(void*, sp::TunerSelectT, sp::ReasonForUpdateT, sp::ReasonForUpdateExtension1T) = nullptr;
    bool ok = false;        // the library is there with every function
    bool opened = false;    // sdrplay_api_Open() worked: the service answers
    float version = 0;
    int selected = 0;       // radios OnAir has selected now
    std::mutex mu;
    DynLib lib;
    SdrplayApi() {
        if (nativeDisabled() || !lib.open(sdrplayLibNames())) return;
        ok = lib.get(Open, "sdrplay_api_Open") && lib.get(Close, "sdrplay_api_Close") && lib.get(ApiVersion, "sdrplay_api_ApiVersion") &&
             lib.get(LockDeviceApi, "sdrplay_api_LockDeviceApi") && lib.get(UnlockDeviceApi, "sdrplay_api_UnlockDeviceApi") &&
             lib.get(GetDevices, "sdrplay_api_GetDevices") && lib.get(SelectDevice, "sdrplay_api_SelectDevice") &&
             lib.get(ReleaseDevice, "sdrplay_api_ReleaseDevice") && lib.get(GetErrorString, "sdrplay_api_GetErrorString") &&
             lib.get(GetDeviceParams, "sdrplay_api_GetDeviceParams") && lib.get(Init, "sdrplay_api_Init") && lib.get(Uninit, "sdrplay_api_Uninit") &&
             lib.get(Update, "sdrplay_api_Update");
        if (!ok) { fprintf(stderr, "SDRplay API library %s lacks functions OnAir needs (an API older than 3.x?)\n", lib.path.c_str()); fflush(stderr); }
    }
    // The API wants Open() first and Close() last; it stays open while OnAir runs (a service that was not running yet is tried again on the
    // next rescan)
    ~SdrplayApi() { if (opened && selected == 0) Close(); }
    std::string err(sp::ErrT e) { const char* s = GetErrorString ? GetErrorString(e) : nullptr; return s ? s : "error " + std::to_string((int)e); }
    bool ensureOpen() {
        std::lock_guard<std::mutex> lk(mu);
        if (opened || !ok) return opened;
        const sp::ErrT e = Open();
        if (e != sp::Success) {
            if (!warnedService) fprintf(stderr, "SDRplay API found but its service is not running (sdrplay_api_Open: %s): start the \"SDRplay API\" service, or reinstall the SDRplay API from sdrplay.com\n", err(e).c_str());
            warnedService = true;
            fflush(stderr);
            return false;
        }
        float v = 0;
        const sp::ErrT ve = ApiVersion(&v);
        if (ve != sp::Success) {   // InvalidServiceVersion: the library and the running service come from different installs
            fprintf(stderr, "SDRplay API: the library does not work with the running service (%s): reinstall the SDRplay API from sdrplay.com\n", err(ve).c_str());
            fflush(stderr);
            Close();
            return false;
        }
        // 3.x keeps the layouts of what OnAir uses (sdrplay_api_min.h); 3.07 is the oldest the layouts were checked against
        if (v < 3.065f || v >= 4.0f) {
            fprintf(stderr, "SDRplay API %.2f is not supported: OnAir needs SDRplay API 3.07 or a newer 3.x (install the current one from sdrplay.com)\n", v);
            fflush(stderr);
            Close();
            return false;
        }
        if (v > sp::kHeaderVersion + 0.005f) fprintf(stderr, "SDRplay API %.2f (newer than the %.2f OnAir was written for; 3.x keeps its layouts, so it should work)\n", v, sp::kHeaderVersion);
        else fprintf(stderr, "SDRplay API %.2f\n", v);
        fflush(stderr);
        version = v;
        opened = true;
        return true;
    }
    bool hasValidFlag() const { return version > 3.075f; }   // DeviceT.valid exists from 3.08 on

private:
    bool warnedService = false;
};
static SdrplayApi& sdrplayApi() { static SdrplayApi a; return a; }

// ------------------------------------------------------------------ the models

static const char* sdrplayModel(unsigned char hw) {
    switch (hw) {
    case sp::kRsp1: return "RSP1";
    case sp::kRsp1A: return "RSP1A";
    case sp::kRsp1B: return "RSP1B";
    case sp::kRsp2: return "RSP2";
    case sp::kRspDuo: return "RSPduo";
    case sp::kRspDx: return "RSPdx";
    case sp::kRspDxR2: return "RSPdx-R2";
    default: return nullptr;
    }
}

// LNA gain reduction in dB of each LNAstate, by frequency band: the tables in section 5 of the SDRplay API Specification 3.15
// (RSP2: ports A/B; RSPduo: the 50 ohm ports; RSPdx: HDR mode off). A band holds frequencies below upToHz.
struct LnaBand { double upToHz; int n; int gr[28]; };
static const LnaBand kLnaRsp1[] = {{420e6, 4, {0, 24, 19, 43}}, {1000e6, 4, {0, 7, 19, 26}}, {1e12, 4, {0, 5, 19, 24}}};
static const LnaBand kLnaRsp1A[] = {{60e6, 7, {0, 6, 12, 18, 37, 42, 61}},
                                    {420e6, 10, {0, 6, 12, 18, 20, 26, 32, 38, 57, 62}},
                                    {1000e6, 10, {0, 7, 13, 19, 20, 27, 33, 39, 45, 64}},
                                    {1e12, 9, {0, 6, 12, 20, 26, 32, 38, 43, 62}}};
static const LnaBand kLnaRsp1B[] = {{50e6, 7, {0, 6, 12, 18, 37, 42, 61}},
                                    {420e6, 10, {0, 6, 12, 18, 20, 26, 32, 38, 57, 62}},   // 50-60 and 60-420 MHz are the same
                                    {1000e6, 10, {0, 7, 13, 19, 20, 27, 33, 39, 45, 64}},
                                    {1e12, 9, {0, 6, 12, 20, 26, 32, 38, 43, 62}}};
static const LnaBand kLnaRsp2[] = {{420e6, 9, {0, 10, 15, 21, 24, 34, 39, 45, 64}}, {1000e6, 6, {0, 7, 10, 17, 22, 41}}, {1e12, 6, {0, 5, 21, 15, 15, 34}}};
static const LnaBand kLnaRspDx[] = {
    {12e6, 19, {0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60}},
    {50e6, 20, {0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60}},
    {60e6, 25, {0, 3, 6, 9, 12, 20, 23, 26, 29, 32, 35, 38, 44, 47, 50, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80}},
    {250e6, 27, {0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84}},
    {420e6, 28, {0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84}},
    {1000e6, 21, {0, 7, 10, 13, 16, 19, 22, 25, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67}},
    {1e12, 19, {0, 5, 8, 11, 14, 17, 20, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65}}};

static void lnaTable(unsigned char hw, const LnaBand*& first, size_t& count) {
    switch (hw) {
    case sp::kRsp1: first = kLnaRsp1; count = 3; return;
    case sp::kRsp1B: first = kLnaRsp1B; count = 4; return;
    case sp::kRsp2: first = kLnaRsp2; count = 3; return;
    case sp::kRspDx: case sp::kRspDxR2: first = kLnaRspDx; count = 7; return;
    default: first = kLnaRsp1A; count = 4; return;   // RSP1A, RSPduo (the same table) and models newer than this code
    }
}
static const LnaBand& lnaBand(unsigned char hw, double hz) {
    const LnaBand* t; size_t n;
    lnaTable(hw, t, n);
    for (size_t i = 0; i < n; i++) if (hz < t[i].upToHz) return t[i];
    return t[n - 1];
}

// The app has one gain in dB, 0..sdrplayMaxGain(): the top is the least gain reduction the radio has (LNAstate 0 and an IF gain
// reduction of 20 dB), every dB below it is one dB more reduction. The IF stage takes the first 30 dB of extra reduction (gRdB 20..50); past
// that the LNA steps in with the smallest reduction that keeps gRdB at 50 or below, so the LNA keeps its gain (and the low noise figure)
// as long as possible while the IF stage keeps some room either way.
constexpr int kGrIfMin = 20, kGrIfMax = 59, kGrIfSoft = 50;
static int sdrplayMaxGain(unsigned char hw) {
    const LnaBand* t; size_t n;
    lnaTable(hw, t, n);
    int top = 0;
    for (size_t i = 0; i < n; i++) for (int k = 0; k < t[i].n; k++) top = std::max(top, t[i].gr[k]);
    return top + (kGrIfMax - kGrIfMin);
}
static void sdrplayGain(unsigned char hw, double hz, double gainDb, int& lnaState, int& gRdB) {
    const int gMax = sdrplayMaxGain(hw);
    const int g = (int)std::lround(std::min<double>(gMax, std::max(0.0, gainDb)));
    const int reduce = gMax - g + kGrIfMin;   // total gain reduction wanted
    const LnaBand& b = lnaBand(hw, hz);
    int pick = -1;
    for (int k = 0; k < b.n; k++)   // the smallest LNA reduction that leaves the IF stage at kGrIfSoft or below (the tables are not all sorted)
        if (b.gr[k] >= reduce - kGrIfSoft && (pick < 0 || b.gr[k] < b.gr[pick])) pick = k;
    if (pick < 0) for (int k = 0; k < b.n; k++) if (pick < 0 || b.gr[k] > b.gr[pick]) pick = k;   // past the end: the most the LNA can do
    lnaState = pick;
    gRdB = std::min(kGrIfMax, std::max(kGrIfMin, reduce - b.gr[pick]));
}

// Antenna power (bias-tee): the RSP1A/1B, RSP2, RSPduo and RSPdx/dx-R2 have it, each in its own part of the parameters; the RSP1 has none
static bool sdrplayHasBiasT(unsigned char hw) {
    return hw == sp::kRsp1A || hw == sp::kRsp1B || hw == sp::kRsp2 || hw == sp::kRspDuo || hw == sp::kRspDx || hw == sp::kRspDxR2;
}
// The notch filters (API: rfNotchEnable, the FM/MW broadcast notch; rfDabNotchEnable, the DAB band III notch; the RSPduo's Hi-Z input has its
// own MW notch, tuner1AmNotchEnable): every model but the RSP1 has the broadcast notch, the RSP2 has no DAB notch. All are off by default
// (the API's default, which OnAir left alone before these settings).
static bool sdrplayHasNotch(unsigned char hw) { return sdrplayHasBiasT(hw); }
static bool sdrplayHasDabNotch(unsigned char hw) { return sdrplayHasNotch(hw) && hw != sp::kRsp2; }
static bool sdrplayIsDx(unsigned char hw) { return hw == sp::kRspDx || hw == sp::kRspDxR2; }
constexpr double kHdrBelowHz = 2e6;     // the RSPdx's HDR mode is for the bands below 2 MHz
constexpr double kHiZTopHz = 30e6;      // the Hi-Z inputs of the RSP2 and RSPduo: 1 kHz - 30 MHz (data sheets)

static std::vector<RadioSetting> sdrplaySettings(unsigned char hw) {
    std::vector<RadioSetting> v = {ppmSetting(0.01)};
    if (sdrplayHasNotch(hw))
        v.push_back(boolSetting("rfnotch", "FM/MW notch", "The radio's notch filter for the FM broadcast band (and on most models medium wave, and the MW notch of the\nRSPduo's Hi-Z input): cuts strong broadcast stations that overload the radio elsewhere. Leave it off to receive FM or MW."));
    if (sdrplayHasDabNotch(hw))
        v.push_back(boolSetting("dabnotch", "DAB notch", "The radio's notch filter for DAB (band III, about 155-260 MHz): cuts strong DAB multiplexes. Leave it off to receive DAB."));
    if (sdrplayIsDx(hw))
        v.push_back(boolSetting("hdr", "HDR mode (below 2 MHz)", "The RSPdx's high dynamic range mode for the bands below 2 MHz (LF, MW): fewer intermodulation products and spurs.\nUsed only while tuned below 2 MHz."));
    return v;
}

// The analogue IF filter: the narrowest that holds the channel and what the app wants its filter to pass (a narrow channel can sit off the
// radio's centre), but never wider than the sample rate
static sp::Bw_MHzT sdrplayBandwidth(double channelHz, double filterHz, double rate) {
    static const sp::Bw_MHzT bws[] = {sp::BW_0_200, sp::BW_0_300, sp::BW_0_600, sp::BW_1_536, sp::BW_5_000, sp::BW_6_000, sp::BW_7_000, sp::BW_8_000};
    const double want = std::max(channelHz, filterHz);
    size_t i = 0;
    while (i + 1 < 8 && (int)bws[i] * 1e3 < want * 0.999) i++;
    while (i > 0 && (int)bws[i] * 1e3 > rate * 1.001) i--;
    return bws[i];
}

// ------------------------------------------------------------------ the source

// the radios OnAir has open: GetDevices() no longer lists those, so a rescan while streaming keeps them from this list
static std::mutex gInUseMu;
static std::vector<DeviceInfo> gInUse;

class SdrplaySource : public NativeSource {
public:
    // args: the serial, then "#" and the antenna input for the radios that have more than one (listSdrplay)
    SdrplaySource(const std::string& args, const DeviceInfo& info) : info_(info) {
        const size_t cut = args.find('#');
        serial_ = args.substr(0, cut);
        if (cut != std::string::npos) port_ = atoi(args.c_str() + cut + 1);
    }
    ~SdrplaySource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        initState_ = 0;
        if (!NativeSource::start(s, ring, err)) return false;
        // sdrplay_api_Init() runs on the stream thread: wait for its answer so that a refusal reaches the user
        for (int i = 0; i < 1000 && initState_.load() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (initState_.load() < 0) { err = initErr_; stop(); return false; }
        return true;
    }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        SdrplayApi& api = sdrplayApi();
        if (!api.ensureOpen()) { err = "SDRplay: the SDRplay API service is not running (start it, or reinstall the SDRplay API from sdrplay.com)"; return false; }
        sp::DeviceT devs[sp::kMaxDevices];
        sp::ErrT e = sp::Success;
        int found = -1;
        // A radio that was just released (the receiver stopping before a scan) comes back in the service's list only after a moment
        // (up to a few seconds on Windows): look again for up to 4 s before calling it gone. The stream thread opening it again after an
        // unplug (run_ is set then) looks once: it comes back every second anyway, and stop() waits for it
        for (int tries = 0;; tries++) {
            memset(devs, 0, sizeof devs);
            unsigned int n = 0;
            api.LockDeviceApi();   // nobody else may pick a radio between the list and the selection
            e = api.GetDevices(devs, &n, sp::kMaxDevices);
            for (unsigned int i = 0; e == sp::Success && i < n && i < (unsigned)sp::kMaxDevices; i++)
                if (trimmed(devs[i].SerNo, sizeof devs[i].SerNo) == serial_ && (!api.hasValidFlag() || devs[i].valid)) found = (int)i;
            if (found >= 0 || tries >= 40 || run_) break;
            api.UnlockDeviceApi();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (found < 0) {
            api.UnlockDeviceApi();
            err = "SDRplay: the radio " + serial_ + " is not available (in use by another program, or unplugged)";
            openFailure_ = OpenFailure::Final;   // the 4 s above already waited for it: start() trying again would only make the user wait longer
            return false;
        }
        dev_ = devs[found];
        if (dev_.hwVer == sp::kRspDuo) {   // the RSPduo: tuner A on its own, like a single-tuner RSP
            if (!(dev_.rspDuoMode & sp::RspDuoMode_Single_Tuner)) {
                api.UnlockDeviceApi();
                err = "SDRplay: the RSPduo is in use by another program (only its second tuner is free, which OnAir does not use)";
                return false;
            }
            dev_.tuner = port_ == 1 ? sp::Tuner_B : sp::Tuner_A;   // the socket the user picked: "Tuner 1", "Tuner 2" or "Tuner 1 Hi-Z"
            dev_.rspDuoMode = sp::RspDuoMode_Single_Tuner;
            dev_.rspDuoSampleFreq = 0;
        }
        e = api.SelectDevice(&dev_);
        api.UnlockDeviceApi();
        if (e != sp::Success) { err = "SDRplay: cannot open the radio (in use by another program?): " + api.err(e); return false; }
        selected_ = true;
        bias_ = false;
        { std::lock_guard<std::mutex> lk(api.mu); api.selected++; }
        { std::lock_guard<std::mutex> lk(gInUseMu); gInUse.push_back(info_); }
        if (dev_.tuner != sp::Tuner_A && dev_.tuner != sp::Tuner_B) dev_.tuner = sp::Tuner_A;
        params_ = nullptr;
        e = api.GetDeviceParams(dev_.dev, &params_);
        // The settings of the tuner in use: an RSPduo opened on tuner 2 gets only rxChannelB (rxChannelA is null), so requiring
        // rxChannelA refused tuner 2 with "cannot read the radio's settings: sdrplay_api_Success" (issue #31)
        sp::RxChannelParamsT* chan = nullptr;
        if (e == sp::Success && params_) chan = dev_.tuner == sp::Tuner_B ? (params_->rxChannelB ? params_->rxChannelB : params_->rxChannelA) : params_->rxChannelA;
        if (e != sp::Success || !chan) {
            err = "SDRplay: cannot read the radio's settings" + (e != sp::Success ? ": " + api.err(e) : std::string(dev_.tuner == sp::Tuner_B ? " (no settings for tuner 2)" : ""));
            params_ = nullptr;
            return false;
        }
        ch_ = chan;
        conv_.assign(8192, cf32(0, 0));   // the stream callback converts into this; no allocation there
        overloads_ = 0;
        removed_ = false;
        return configure(s, err, false);
    }

    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!params_ || !ch_) return false;
        sp::RxChannelParamsT& ch = *ch_;
        unsigned int why = sp::Update_None;
        if (!live) {
            // ADC rate 2..10.66 MHz, zero IF; below 2 Msps the API's decimation (2..32) brings it down
            const double want = std::min(10.66e6, std::max(62.5e3, s.sampleRate > 0 ? s.sampleRate : 8e6));
            int dec = 1;
            while (dec < 32 && want * dec < 2e6 - 1) dec *= 2;
            const double fs = std::max(2e6, want * dec);
            if (params_->devParams) params_->devParams->fsFreq.fsHz = fs;
            ch.ctrlParams.decimation.enable = dec > 1 ? 1 : 0;
            ch.ctrlParams.decimation.decimationFactor = (unsigned char)dec;
            ch.ctrlParams.decimation.wideBandSignal = 1;   // zero IF: the wide-band filters, as the API's own programs use them
            ch.tunerParams.ifType = sp::IF_Zero;
            ch.tunerParams.loMode = sp::LO_Auto;
            ch.tunerParams.gain.minGr = sp::NORMAL_MIN_GR;
            ch.ctrlParams.agc.enable = sp::AGC_DISABLE;   // the app runs its own gain control
            ch.ctrlParams.dcOffset.DCenable = 1;
            ch.ctrlParams.dcOffset.IQenable = 1;
            // the antenna socket of the radios with more than one (the RSPduo picks its tuner in openDevice). The Hi-Z entries (port 2):
            // the RSP2's and the RSPduo tuner 1's high-impedance HF input (AMPORT_1), as SoapySDRPlay3 selects it
            if (dev_.hwVer == sp::kRspDuo) {
                ch.rspDuoTunerParams.tuner1AmPortSel = port_ == 2 ? sp::RspDuo_AMPORT_1 : sp::RspDuo_AMPORT_2;   // 50 ohm unless the Hi-Z entry
            } else if (dev_.hwVer == sp::kRsp2) {
                ch.rsp2TunerParams.antennaSel = port_ == 1 ? sp::Rsp2_ANTENNA_B : sp::Rsp2_ANTENNA_A;
                ch.rsp2TunerParams.amPortSel = port_ == 2 ? sp::Rsp2_AMPORT_1 : sp::Rsp2_AMPORT_2;   // the 50 ohm sockets unless the Hi-Z entry
            } else if ((dev_.hwVer == sp::kRspDx || dev_.hwVer == sp::kRspDxR2) && params_->devParams) {
                params_->devParams->rspDxParams.antennaSel = port_ == 2 ? sp::RspDx_ANTENNA_C : port_ == 1 ? sp::RspDx_ANTENNA_B : sp::RspDx_ANTENNA_A;
            }
            rate_ = fs / dec;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const sp::Bw_MHzT bw = sdrplayBandwidth(s.bandwidthMhz * 1e6, s.basebandFilterHz, rate_);
        if (bw != ch.tunerParams.bwType) { ch.tunerParams.bwType = bw; why |= sp::Update_Tuner_BwType; }
        // the RSPs tune 1 kHz (RSP1: 10 kHz) to 2 GHz
        const double fMin = dev_.hwVer == sp::kRsp1 ? 10e3 : 1e3;
        const double f = std::min(2e9, std::max(fMin, s.centerHz));
        if (f != s.centerHz && err.empty()) err = dev_.hwVer == sp::kRsp1 ? "the SDRplay RSP1 tunes from 10 kHz to 2 GHz" : "SDRplay RSPs tune from 1 kHz to 2 GHz";
        if (std::fabs(f - ch.tunerParams.rfFreq.rfHz) > 0.5) { ch.tunerParams.rfFreq.rfHz = f; why |= sp::Update_Tuner_Frf; }
        int lna = 0, gr = 40;
        sdrplayGain(dev_.hwVer, f, s.gainDb, lna, gr);
        if (lna != ch.tunerParams.gain.LNAstate || gr != ch.tunerParams.gain.gRdB) {
            ch.tunerParams.gain.LNAstate = (unsigned char)lna;
            ch.tunerParams.gain.gRdB = gr;
            why |= sp::Update_Tuner_Gr;
        }
        unsigned int why1 = sp::Update_Ext1_None;
        if (sdrplayHasBiasT(dev_.hwVer) && (!live || s.biasTee != bias_)) {
            if (setBias(s.biasTee, why, why1)) bias_ = s.biasTee;
        }
        radioSettings(s, f, why, why1);
        if (port_ == 2 && (dev_.hwVer == sp::kRsp2 || dev_.hwVer == sp::kRspDuo) && f > kHiZTopHz && err.empty())
            err = "the SDRplay Hi-Z input is for 1 kHz to 30 MHz: use a 50 ohm input above that";
        if (!live || (why == sp::Update_None && why1 == sp::Update_Ext1_None)) return true;
        // one update for everything that changed; the API refuses a new one while the last is still being applied
        sp::ErrT e = sp::Success;
        for (int tries = 0; tries < 5; tries++) {
            e = sdrplayApi().Update(dev_.dev, dev_.tuner, (sp::ReasonForUpdateT)why, (sp::ReasonForUpdateExtension1T)why1);
            if (e != sp::RfUpdateError && e != sp::GainUpdateError && e != sp::FsUpdateError) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (e != sp::Success) { err = "SDRplay: the radio did not take the new settings: " + sdrplayApi().err(e); return false; }
        return true;
    }

    void closeDevice() override {
        if (!selected_) return;
        SdrplayApi& api = sdrplayApi();
        api.ReleaseDevice(&dev_);
        selected_ = false;
        params_ = nullptr;
        ch_ = nullptr;
        { std::lock_guard<std::mutex> lk(api.mu); api.selected--; }
        std::lock_guard<std::mutex> lk(gInUseMu);
        for (auto it = gInUse.begin(); it != gInUse.end(); ++it) if (it->nativeArgs == info_.nativeArgs) { gInUse.erase(it); break; }
    }

    void streamLoop() override {
        sp::CallbackFnsT cb;
        cb.StreamACbFn = &SdrplaySource::streamA;
        cb.StreamBCbFn = &SdrplaySource::streamB;
        cb.EventCbFn = &SdrplaySource::event;
        const sp::ErrT e = sdrplayApi().Init(dev_.dev, &cb, this);
        if (e != sp::Success) {
            initErr_ = "SDRplay: the radio did not start: " + sdrplayApi().err(e);
            fprintf(stderr, "%s\n", initErr_.c_str());
            fflush(stderr);
            initState_ = -1;
            return;
        }
        initState_ = 1;
        while (run_) {
            if (removed_) {   // returning lets the base class release the radio and open it again every second until it is back
                fprintf(stderr, "SDRplay: the radio was unplugged or stopped working; the stream has ended\n");
                fflush(stderr);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (bias_) {   // antenna power off while the radio can still take an update (not after Uninit)
            unsigned int why = sp::Update_None, why1 = sp::Update_Ext1_None;
            if (setBias(false, why, why1)) {
                sdrplayApi().Update(dev_.dev, dev_.tuner, (sp::ReasonForUpdateT)why, (sp::ReasonForUpdateExtension1T)why1);
                bias_ = false;
            }
        }
        sdrplayApi().Uninit(dev_.dev);
    }

private:
    // The radio settings (TuneSettings::radio): frequency correction, the notch filters, the RSPdx's HDR mode. Each field is written, with its
    // update flag, only when it differs from what the parameters hold: a setting the user never touched leaves the API's default alone.
    void radioSettings(const TuneSettings& s, double f, unsigned int& why, unsigned int& why1) {
        if (!params_ || !ch_) return;
        sp::RxChannelParamsT& ch = *ch_;
        sp::DevParamsT* dp = params_->devParams;
        auto put = [](unsigned char& field, bool on, unsigned int& flags, unsigned int flag) {
            const unsigned char v = on ? 1 : 0;
            if (field != v) { field = v; flags |= flag; }
        };
        if (dp) {   // devParams->ppm, as SoapySDRPlay3 sets it (Update_Dev_Ppm)
            const double ppm = radioPpm(s);
            if (std::fabs(dp->ppm - ppm) > 1e-9) { dp->ppm = ppm; why |= sp::Update_Dev_Ppm; }
        }
        const bool rf = radioFlag(s, "rfnotch"), dab = radioFlag(s, "dabnotch");
        switch (dev_.hwVer) {
        case sp::kRsp1A: case sp::kRsp1B:
            if (dp) { put(dp->rsp1aParams.rfNotchEnable, rf, why, sp::Update_Rsp1a_RfNotchControl); put(dp->rsp1aParams.rfDabNotchEnable, dab, why, sp::Update_Rsp1a_RfDabNotchControl); }
            break;
        case sp::kRsp2:
            put(ch.rsp2TunerParams.rfNotchEnable, rf, why, sp::Update_Rsp2_RfNotchControl);
            break;
        case sp::kRspDuo:   // the Hi-Z input has its own (MW) notch
            if (port_ == 2) put(ch.rspDuoTunerParams.tuner1AmNotchEnable, rf, why, sp::Update_RspDuo_Tuner1AmNotchControl);
            else put(ch.rspDuoTunerParams.rfNotchEnable, rf, why, sp::Update_RspDuo_RfNotchControl);
            put(ch.rspDuoTunerParams.rfDabNotchEnable, dab, why, sp::Update_RspDuo_RfDabNotchControl);
            break;
        case sp::kRspDx: case sp::kRspDxR2:
            if (dp) {
                put(dp->rspDxParams.rfNotchEnable, rf, why1, sp::Update_RspDx_RfNotchControl);
                put(dp->rspDxParams.rfDabNotchEnable, dab, why1, sp::Update_RspDx_RfDabNotchControl);
                put(dp->rspDxParams.hdrEnable, radioFlag(s, "hdr") && f < kHdrBelowHz, why1, sp::Update_RspDx_HdrEnable);
            }
            break;
        default: break;
        }
    }
    // writes the bias-tee flag into the part of the parameters this model uses and adds the matching update flag
    bool setBias(bool on, unsigned int& why, unsigned int& why1) {
        if (!params_ || !ch_) return false;
        const unsigned char v = on ? 1 : 0;
        switch (dev_.hwVer) {
        case sp::kRsp1A: case sp::kRsp1B: ch_->rsp1aTunerParams.biasTEnable = v; why |= sp::Update_Rsp1a_BiasTControl; return true;
        case sp::kRsp2: ch_->rsp2TunerParams.biasTEnable = v; why |= sp::Update_Rsp2_BiasTControl; return true;
        case sp::kRspDuo: ch_->rspDuoTunerParams.biasTEnable = v; why |= sp::Update_RspDuo_BiasTControl; return true;
        case sp::kRspDx: case sp::kRspDxR2:
            if (!params_->devParams) return false;
            params_->devParams->rspDxParams.biasTEnable = v;
            why1 |= sp::Update_RspDx_BiasTControl;
            return true;
        default: return false;
        }
    }
    // runs on the API's thread: converts the int16 I and Q arrays to complex floats of +-1 into the preallocated buffer
    static void DECT2_CALL streamA(short* xi, short* xq, sp::StreamCbParamsT*, unsigned int numSamples, unsigned int, void* ctx) {
        auto* self = static_cast<SdrplaySource*>(ctx);
        if (!self || !self->run_ || !xi || !xq || self->conv_.empty()) return;
        const float k = 1.0f / 32768.0f;
        while (numSamples) {
            const unsigned int m = std::min<unsigned int>(numSamples, (unsigned int)self->conv_.size());
            cf32* o = self->conv_.data();
            for (unsigned int i = 0; i < m; i++) o[i] = cf32(xi[i] * k, xq[i] * k);
            self->push(o, m);
            xi += m; xq += m; numSamples -= m;
        }
    }
    // the RSPduo's tuner 2 on its own: the API may deliver it through either callback, so both convert
    static void DECT2_CALL streamB(short* xi, short* xq, sp::StreamCbParamsT* p, unsigned int n, unsigned int reset, void* ctx) { streamA(xi, xq, p, n, reset, ctx); }
    static void DECT2_CALL event(sp::EventT id, sp::TunerSelectT tuner, sp::EventParamsT* p, void* ctx) {
        auto* self = static_cast<SdrplaySource*>(ctx);
        if (!self) return;
        if (id == sp::PowerOverloadChange) {
            if (p && p->powerOverloadParams.powerOverloadChangeType == sp::Overload_Detected) {
                const unsigned n = ++self->overloads_;
                if (n <= 5 || n % 100 == 0) {
                    fprintf(stderr, "SDRplay: power overload (%u so far): the signal is too strong for the gain; lower the gain\n", n);
                    fflush(stderr);
                }
            }
            // the API reports the next overload change only after this acknowledgement
            sdrplayApi().Update(self->dev_.dev, tuner, sp::Update_Ctrl_OverloadMsgAck, sp::Update_Ext1_None);
        } else if (id == sp::DeviceRemoved || id == sp::DeviceFailure) {
            self->removed_ = true;
        }
    }

    std::string serial_;
    int port_ = 0;   // antenna input: RSPduo tuner 1/2/tuner 1 Hi-Z, RSP2 A/B/Hi-Z, RSPdx A/B/C
    DeviceInfo info_;
    sp::DeviceT dev_{};
    sp::DeviceParamsT* params_ = nullptr;
    sp::RxChannelParamsT* ch_ = nullptr;
    bool selected_ = false;
    bool bias_ = false;               // the antenna power is on
    std::atomic<int> initState_{0};   // 0 waiting for Init(), 1 streaming, -1 refused
    std::string initErr_;
    std::atomic<unsigned> overloads_{0};
    std::atomic<bool> removed_{false};
};

void listSdrplay(std::vector<DeviceInfo>& out) {
    SdrplayApi& api = sdrplayApi();
    if (!api.ok || !api.ensureOpen()) return;
    sp::DeviceT devs[sp::kMaxDevices];
    memset(devs, 0, sizeof devs);
    unsigned int n = 0;
    api.LockDeviceApi();
    const sp::ErrT e = api.GetDevices(devs, &n, sp::kMaxDevices);
    api.UnlockDeviceApi();
    if (e != sp::Success) { fprintf(stderr, "SDRplay API: cannot list the radios: %s\n", api.err(e).c_str()); fflush(stderr); n = 0; }
    std::vector<DeviceInfo> found;
    for (unsigned int i = 0; i < n && i < (unsigned)sp::kMaxDevices; i++) {
        const sp::DeviceT& dv = devs[i];
        if (api.hasValidFlag() && !dv.valid) continue;   // not ready to use (the API says so from 3.08 on)
        const std::string serial = trimmed(dv.SerNo, sizeof dv.SerNo);
        const char* model = sdrplayModel(dv.hwVer);
        if (dv.hwVer == sp::kRspDuo && !(dv.rspDuoMode & sp::RspDuoMode_Single_Tuner)) {
            fprintf(stderr, "SDRplay RSPduo %s is in use by another program (only its second tuner is free)\n", serial.c_str());
            fflush(stderr);
            continue;
        }
        // one entry per antenna socket on the radios that have several, so the user picks the one the antenna is on (the first is what
        // OnAir used before); the RSP2's and the RSPduo's high-impedance HF inputs (1 kHz - 30 MHz) come last
        std::vector<std::string> ports;
        if (dv.hwVer == sp::kRspDuo) ports = {"Tuner 1", "Tuner 2", "Tuner 1 Hi-Z"};
        else if (dv.hwVer == sp::kRsp2) ports = {"antenna A", "antenna B", "Hi-Z"};
        else if (dv.hwVer == sp::kRspDx || dv.hwVer == sp::kRspDxR2) ports = {"antenna A", "antenna B", "antenna C"};
        if (ports.empty()) ports = {""};
        for (size_t pi = 0; pi < ports.size(); pi++) {
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "sdrplay";
        d.serial = serial;
        d.nativeArgs = ports.size() > 1 ? serial + "#" + std::to_string(pi) : serial;
        d.name = "SDRplay " + (model ? std::string(model) : "RSP (model " + std::to_string((int)dv.hwVer) + ")") + (serial.empty() ? "" : " " + serial) + (ports[pi].empty() ? "" : " " + ports[pi]) + " (native, experimental)";
        d.maxRateHz = 10e6; d.minRateHz = 2e6;
        d.gainMinDb = 0; d.gainMaxDb = sdrplayMaxGain(dv.hwVer);
        d.hasBiasTee = sdrplayHasBiasT(dv.hwVer);
        d.settings = sdrplaySettings(dv.hwVer);
        // the RSPs tune 1 kHz (RSP1: 10 kHz) to 2 GHz (configure clamps to that): listed, so that the app warns instead
        d.minFreqHz = dv.hwVer == sp::kRsp1 ? 10e3 : 1e3; d.maxFreqHz = 2e9;
        if (ports[pi].find("Hi-Z") != std::string::npos) d.maxFreqHz = kHiZTopHz;   // the high-impedance HF input
        fprintf(stderr, "SDRplay: found %s\n", d.name.c_str());
        found.push_back(d);
        }
    }
    {   // the ones OnAir is streaming from are not in the API's list
        std::lock_guard<std::mutex> lk(gInUseMu);
        for (const auto& u : gInUse) {
            bool dup = false;
            for (const auto& f : found) if (f.nativeArgs == u.nativeArgs) dup = true;
            if (!dup) found.push_back(u);
        }
    }
    fflush(stderr);
    for (auto& d : found) out.push_back(d);
}

std::unique_ptr<IqSource> makeSdrplay(const DeviceInfo& d) {
    return std::make_unique<SdrplaySource>(d.nativeArgs, d);
}

} // namespace native
} // namespace dect2
