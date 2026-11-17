// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native driver for Tektronix's USB real-time spectrum analysers (RSA306, RSA306B, RSA500A, RSA600A) as receivers, through Tektronix's
// RSA API (RSA_API.dll on Windows, libRSA_API.so on Linux). OnAir does not ship it: the user installs the API from tek.com, and the
// library is loaded at run time (native_common.h). The functions and structures used are declared in rsa_api_min.h.
//
// The API's "IQ streaming" hands out float I/Q in volts into 50 ohm at one of thirteen rates, 56 Msps divided by 1, 2, 4 ... 4096, each
// with an acquisition bandwidth of 40 MHz divided by the same. OnAir takes the slowest of them at or above the rate that was asked for.
// The gain slider sets the reference level (0 dB = +30 dBm, each dB more is 1 dB lower), and the samples are scaled so that a signal at
// the reference level is 1.0.
//
// What this driver relies on was measured on an RSA306 (firmware V1.7, FPGA V1.1, hardware V5) with the RSA API 5.6.0295 on Windows 11
// and a USB 3 port (docs/TEKTRONIX-RSA.md has the numbers):
// - DEVICE_Connect takes 1 to 3.5 s and DEVICE_Disconnect 4 s, while DEVICE_Run + IQSTREAM_Start deliver the first samples after 50 ms
//   and a stop takes 30 ms. So the device stays connected from its first start to the end of the program: a stop only ends the stream.
//   The API has one device connected at a time, in one program.
// - Nothing changes while it streams: a new centre frequency is accepted and the device stays where it was (the carriers do not move)
//   until it was stopped and run again, and a new bandwidth is accepted and the samples that follow are garbage. So every retune stops
//   the stream and the device, sets the values and runs them again, which takes about 50 ms.
// - The rates are exact, and the set functions take any value while the device is stopped (what it cannot do shows at the run).
// - An RSA306 has no preamplifier: CONFIG_SetRFPreampEnable succeeds and the value reads back as off. The setting is there for the
//   models that have one (RSA500A, RSA600A), and the start message says when the device did not take it.
// Not tried: the other models, Linux, and more than one device.
#include "native_common.h"
#include "rsa_api_min.h"

namespace dect2 {
namespace native {

namespace rsa = dect2::tekrsa;

namespace {

constexpr double kTopRateHz = 56e6, kTopBandwidthHz = 40e6;   // the widest step; every further one is half of both
constexpr int kRateSteps = 13;                                // down to 13671.875 sps (9765.625 Hz)
constexpr double kRefTopDbm = 30;          // gain 0 dB = this reference level (the API's highest)
constexpr double kGainMaxDb = 130;         // -100 dBm
constexpr double kZeroDbmAmp = 0.22360679774997896;   // sqrt(0.05): 0 dBm into 50 ohm, as the API counts its volts
constexpr const char* kMissingMsg =
    "Tektronix RSA API is not installed: the RSA306 / RSA500A / RSA600A need Tektronix's RSA API library (RSA_API), a free download "
    "from tek.com. Install it, then restart OnAir. docs/TEKTRONIX-RSA.md";

double ampForDbm(double dbm) { return kZeroDbmAmp * std::pow(10.0, dbm / 20.0); }
std::string mhz(double hz) { char b[40]; snprintf(b, sizeof b, "%.6g MHz", hz / 1e6); return b; }

// Where the RSA API installer puts the library. ONAIR_RSA_DIR: the folder that holds it when it is somewhere else.
#ifdef _WIN32
constexpr const char* kLibFile = "RSA_API.dll";
constexpr char kSep = '\\';
#elif defined(__APPLE__)
constexpr const char* kLibFile = "libRSA_API.dylib";   // (Tektronix offers none for macOS: the name is for the tests)
constexpr char kSep = '/';
#else
constexpr const char* kLibFile = "libRSA_API.so";
constexpr char kSep = '/';
#endif
std::vector<std::string> rsaLibNames() {
    std::vector<std::string> v;
    if (const char* e = getenv("ONAIR_RSA_DIR")) if (*e) v.push_back(std::string(e) + kSep + kLibFile);
#ifdef _WIN32
    v.push_back("C:\\Tektronix\\RSA_API\\lib\\x64\\RSA_API.dll");
#else
    for (const char* dir : {"/opt/tektronix/RSA_API/lib", "/usr/local/lib", "/usr/lib"}) v.push_back(std::string(dir) + kSep + kLibFile);
#endif
    v.push_back(kLibFile);
    return v;
}

// The highest frequency of each model, by the start of the type the API reports ("RSA306B", "RSA507A" ...): for the note in the list only
double maxHzOf(const std::string& type) {
    struct M { const char* start; double hz; };
    static const M k[] = {{"RSA306", 6.2e9}, {"RSA503", 3e9}, {"RSA507", 7.5e9}, {"RSA513", 13.6e9}, {"RSA518", 18e9}, {"RSA603", 3e9}, {"RSA607", 7.5e9}};
    for (const auto& m : k) if (type.rfind(m.start, 0) == 0) return m.hz;
    return 6.2e9;
}

// The library, loaded once. It talks to one device at a time, so its calls take turns under `mu` and the connection is kept here.
struct RsaApi {
    rsa::GetErrorStringFn GetErrorString = nullptr;
    rsa::SearchFn Search = nullptr;
    rsa::ConnectFn Connect = nullptr;
    rsa::VoidFn Disconnect = nullptr, Run = nullptr, Stop = nullptr, StreamStart = nullptr, StreamStop = nullptr;
    rsa::GetInfoFn GetInfo = nullptr;                      // optional: for the log
    rsa::SetDoubleFn SetCenterFreq = nullptr, SetReferenceLevel = nullptr, SetAcqBandwidth = nullptr;
    rsa::GetDoubleFn GetReferenceLevel = nullptr;          // optional: reads back what the device took
    rsa::SetBoolFn SetPreamp = nullptr;                    // optional
    rsa::GetBoolFn GetPreamp = nullptr;                    // optional
    rsa::GetAcqParametersFn GetAcqParameters = nullptr;
    rsa::SetOutputConfigurationFn SetOutputConfiguration = nullptr;
    rsa::GetBufferSizeFn GetBufferSize = nullptr;
    rsa::GetIqDataFn GetIqData = nullptr;

    bool ok = false;
    std::mutex mu;                 // every call of the library, and the fields below
    std::string connected;         // the serial of the device the library is connected to ("" when none)
    bool streaming = false;        // a source of this program is streaming from it
    std::vector<DeviceInfo> known; // the last list: it stands while the device is connected (a search then would disturb it)
    DynLib lib;

    RsaApi() {
        if (nativeDisabled()) return;
        if (!load()) return;
        ok = lib.get(GetErrorString, "DEVICE_GetErrorString") && lib.get(Search, "DEVICE_Search") && lib.get(Connect, "DEVICE_Connect") &&
             lib.get(Disconnect, "DEVICE_Disconnect") && lib.get(Run, "DEVICE_Run") && lib.get(Stop, "DEVICE_Stop") &&
             lib.get(SetCenterFreq, "CONFIG_SetCenterFreq") && lib.get(SetReferenceLevel, "CONFIG_SetReferenceLevel") &&
             lib.get(SetAcqBandwidth, "IQSTREAM_SetAcqBandwidth") && lib.get(GetAcqParameters, "IQSTREAM_GetAcqParameters") &&
             lib.get(SetOutputConfiguration, "IQSTREAM_SetOutputConfiguration") && lib.get(GetBufferSize, "IQSTREAM_GetIQDataBufferSize") &&
             lib.get(StreamStart, "IQSTREAM_Start") && lib.get(StreamStop, "IQSTREAM_Stop") && lib.get(GetIqData, "IQSTREAM_GetIQData");
        lib.opt(GetInfo, "DEVICE_GetInfo");
        lib.opt(GetReferenceLevel, "CONFIG_GetReferenceLevel");
        lib.opt(SetPreamp, "CONFIG_SetRFPreampEnable");
        lib.opt(GetPreamp, "CONFIG_GetRFPreampEnable");
    }
    // Kept for the life of the program, and the device is not disconnected at its end (4 s): the connection ends with the program.
    ~RsaApi() = default;

    std::string text(rsa::Status s) const {
        const char* t = GetErrorString ? GetErrorString(s) : nullptr;
        char b[32];
        snprintf(b, sizeof b, " (%d)", (int)s);
        return std::string(t && *t ? t : "error") + b;
    }
    // (mu) the devices the library finds: {type, serial}
    std::vector<std::pair<std::string, std::string>> search() {
        std::vector<std::pair<std::string, std::string>> v;
        int n = 0;
        static int ids[rsa::kMaxDevices];
        static char serials[rsa::kMaxDevices][rsa::kSerialLen], types[rsa::kMaxDevices][rsa::kTypeLen];
        memset(serials, 0, sizeof serials);
        memset(types, 0, sizeof types);
        if (Search(&n, ids, serials, types) != rsa::kNoError) return v;
        for (int i = 0; i < n && i < rsa::kMaxDevices; i++) {
            serials[i][rsa::kSerialLen - 1] = 0;
            types[i][rsa::kTypeLen - 1] = 0;
            v.emplace_back(types[i], serials[i]);
            ids_.push_back({serials[i], ids[i]});
        }
        return v;
    }
    // (mu) connects to the device with this serial, unless it already is; another one is let go of first
    bool connect(const std::string& serial, std::string& err) {
        if (connected == serial) return true;
        if (!connected.empty()) { Disconnect(); connected.clear(); }
        ids_.clear();
        search();
        int id = -1;
        for (const auto& p : ids_) if (p.first == serial) id = p.second;
        if (id < 0) { err = "Tektronix RSA " + serial + " not found (unplugged, or in use by SignalVu-PC?)"; return false; }
        const rsa::Status s = Connect(id);
        if (s != rsa::kNoError) { err = "Tektronix RSA " + serial + ": the device did not connect: " + text(s) + ". Is it in use by SignalVu-PC?"; return false; }
        connected = serial;
        if (GetInfo) {
            rsa::DeviceInfo di;
            memset(&di, 0, sizeof di);
            if (GetInfo(&di) == rsa::kNoError) {
                di.nomenclature[rsa::kInfoLen - 1] = di.apiVersion[rsa::kInfoLen - 1] = di.fwVersion[rsa::kInfoLen - 1] = di.fpgaVersion[rsa::kInfoLen - 1] = di.hwVersion[rsa::kInfoLen - 1] = 0;
                fprintf(stderr, "Tektronix RSA: connected to %s %s (API %s, firmware %s, FPGA %s, hardware %s)\n", di.nomenclature, serial.c_str(), di.apiVersion, di.fwVersion,
                        di.fpgaVersion, di.hwVersion);
                fflush(stderr);
            }
        }
        return true;
    }
    // (mu) after an error of the stream: the connection is given up, so that the next open looks for the device again
    void drop() {
        if (connected.empty()) return;
        Disconnect();
        connected.clear();
    }

private:
    std::vector<std::pair<std::string, int>> ids_;   // (mu) serial -> the id of the last search

    bool load() {
        if (getenv("DECT2_NATIVE_LIBDIR")) return lib.open(rsaLibNames());   // tests: the fake library
#ifdef _WIN32
        // RSA_API.dll needs the libraries next to it (RSA300API.dll, SharedUtils.dll ...): that folder is added to the search for them
        // (AddDllDirectory is looked up at run time, and the flags are spelled out: older MinGW headers lack them)
        typedef void* (WINAPI * AddDirFn)(const wchar_t*);
        const AddDirFn addDir = reinterpret_cast<AddDirFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory")));
        constexpr DWORD kSearchDllLoadDir = 0x00000100, kSearchUserDirs = 0x00000400, kSearchDefaultDirs = 0x00001000;
        for (const auto& n : rsaLibNames()) {
            const size_t cut = n.find_last_of('\\');
            if (cut == std::string::npos) continue;
            if (GetFileAttributesA(n.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const std::string dir = n.substr(0, cut);
            const std::wstring wdir(dir.begin(), dir.end()), wlib(n.begin(), n.end());
            HMODULE m = nullptr;
            if (addDir) {
                addDir(wdir.c_str());
                m = LoadLibraryExW(wlib.c_str(), nullptr, kSearchDefaultDirs | kSearchUserDirs | kSearchDllLoadDir);
            } else {
                SetDllDirectoryW(wdir.c_str());   // (Windows 7 without the update: the older way)
                m = LoadLibraryExW(wlib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                SetDllDirectoryW(nullptr);
            }
            if (m) { lib.h = (void*)m; lib.path = n; fprintf(stderr, "radio library: %s\n", n.c_str()); fflush(stderr); return true; }
            fprintf(stderr, "radio library %s is there but does not load (Windows error %lu)\n", n.c_str(), (unsigned long)GetLastError());
            fflush(stderr);
        }
#endif
        return lib.open(rsaLibNames());
    }
};
RsaApi& rsaApi() { static RsaApi a; return a; }

std::vector<RadioSetting> rsaSettings() {
    std::vector<RadioSetting> v = {ppmSetting(0.01)};
    v.push_back(choiceSetting("preamp", "RF preamplifier",
                              "The preamplifier of the models that have one (RSA500A, RSA600A; an RSA306 has none). Keep: as the device is.\n"
                              "On for weak signals; the start message says when the device did not take it.",
                              {"keep", "off", "on"}, {"Keep", "Off", "On"}, "keep"));
    return v;
}

bool splitArgs(const std::string& a, std::string& type, std::string& serial) {
    const size_t bar = a.find('|');
    if (bar == std::string::npos) return false;
    type = a.substr(0, bar);
    serial = a.substr(bar + 1);
    return !serial.empty();
}

// The step for a rate: the slowest of the thirteen at or above it (the fastest when it is beyond that)
int stepFor(double want) {
    int k = 0;
    while (k + 1 < kRateSteps && kTopRateHz / double(1 << (k + 1)) >= want * 0.999) k++;
    return k;
}

class RsaSource : public NativeSource {
public:
    RsaSource(std::string type, std::string serial) : type_(std::move(type)), serial_(std::move(serial)) {}
    ~RsaSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        auto& api = rsaApi();
        if (!api.ok) { err = kMissingMsg; openFailure_ = OpenFailure::Final; return false; }
        std::lock_guard<std::mutex> lk(api.mu);
        if (api.streaming && !mine_) { err = "Tektronix RSA: the device is already in use in this program (the RSA API has one device at a time)"; openFailure_ = OpenFailure::Final; return false; }
        if (!api.connect(serial_, err)) return false;
        return setUp(s, err) && run(err);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        auto& api = rsaApi();
        std::lock_guard<std::mutex> lk(api.mu);
        if (!mine_) return false;
        if (live) {
            // (the stream thread runs and waits for api.mu: the device is stopped and run again under it, see the top of this file)
            if (!changed(s)) return true;
            api.StreamStop();
            api.Stop();
            if (!tune(s, err)) return false;
            rsa::Status st = api.Run();
            if (st == rsa::kNoError) st = api.StreamStart();
            if (st != rsa::kNoError) { err = "Tektronix RSA: the device did not start again after the retune: " + api.text(st); return false; }
            return true;
        }
        // a new rate (the stream thread is stopped): the device is stopped, set up again and run
        halt();
        return setUp(s, err) && run(err);
    }
    void closeDevice() override {
        auto& api = rsaApi();
        if (!api.ok) return;
        std::lock_guard<std::mutex> lk(api.mu);
        halt();
        if (lost_) { api.drop(); lost_ = false; }
    }
    void streamLoop() override {
        auto& api = rsaApi();
        uint32_t said = 0;
        while (run_) {
            int n = 0;
            rsa::IqInfo info{};
            rsa::Status st;
            {
                std::lock_guard<std::mutex> lk(api.mu);
                if (!mine_) return;
                st = api.GetIqData(buf_.data(), &n, &info);
                if (st == rsa::kNoError && n > 0) {
                    const float sc = scale_;
                    conv_.resize((size_t)n);
                    for (int k = 0; k < n; k++) conv_[(size_t)k] = cf32(buf_[2 * (size_t)k] * sc, buf_[2 * (size_t)k + 1] * sc);
                }
            }
            if (st != rsa::kNoError) {
                if (run_) { fprintf(stderr, "Tektronix RSA: the stream ended: %s\n", api.text(st).c_str()); fflush(stderr); }
                lost_ = true;
                return;   // (NativeSource opens it again every second until it is back)
            }
            if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            scrubNonFinite(conv_.data(), conv_.size());
            push(conv_.data(), conv_.size());
            // lost samples are said once per start and kind (an overdriven input is the level meter's to show)
            const uint32_t lostBits = info.acqStatus & (rsa::kStatusDiscontinuity | rsa::kStatusInputOverflow | rsa::kStatusOutputOverflow);
            if (lostBits & ~said) {
                said |= lostBits;
                fprintf(stderr, "Tektronix RSA: samples were lost (%s%s%s): at this rate the USB port or the computer does not keep up\n",
                        lostBits & rsa::kStatusDiscontinuity ? "between the device and the computer " : "", lostBits & rsa::kStatusInputOverflow ? "in the API's processing " : "",
                        lostBits & rsa::kStatusOutputOverflow ? "in the API's output buffer" : "");
                fflush(stderr);
            }
        }
    }

private:
    // (api.mu, the device stopped) the rate, then what tune() sets
    bool setUp(const TuneSettings& s, std::string& err) {
        auto& api = rsaApi();
        const double want = s.sampleRate > 0 ? s.sampleRate : 3.5e6;
        const int k = stepFor(want);
        rsa::Status st = api.SetAcqBandwidth(kTopBandwidthHz / double(1 << k));
        if (st != rsa::kNoError && st != rsa::kErrorBandwidthOutOfRange) { err = "Tektronix RSA: the bandwidth for " + mhz(want) + " was refused: " + api.text(st); return false; }
        double bw = 0, sr = 0;
        if (api.GetAcqParameters(&bw, &sr) != rsa::kNoError || !(sr > 0)) sr = kTopRateHz / double(1 << k);
        if ((st = api.SetOutputConfiguration(rsa::kOutputClient, rsa::kTypeSingle)) != rsa::kNoError) { err = "Tektronix RSA: the output format was refused: " + api.text(st); return false; }
        int block = 0;
        if (api.GetBufferSize(&block) != rsa::kNoError || block <= 0) block = 131072;
        buf_.assign(2 * (size_t)block + 16, 0.f);
        rate_ = sr;
        requested_ = s.sampleRate;
        checkRate(s.sampleRate, err);
        const std::string pre = radioOption(s, "preamp", "keep");
        if (pre != "keep" && api.SetPreamp) {
            const bool on = pre == "on";
            api.SetPreamp(on);
            bool got = on;
            if (on && api.GetPreamp && api.GetPreamp(&got) == rsa::kNoError && !got) addNote(err, "Tektronix RSA: this device has no preamplifier (the setting was not taken)");
        }
        return tune(s, err);
    }
    // the frequency and the reference level a setting asks for
    static double centreOf(const TuneSettings& s) { return ppmCorrectedHz(std::max(s.centerHz, 0.0), radioPpm(s)); }   // (the API has no frequency correction)
    static double refOf(const TuneSettings& s) { return kRefTopDbm - std::clamp(s.gainDb, 0.0, kGainMaxDb); }
    // (api.mu) whether a retune asks for another frequency or level than the device runs with
    bool changed(const TuneSettings& s) const { return std::fabs(centreOf(s) - centreSet_) > 0.5 || std::fabs(refOf(s) - refSet_) > 0.01; }
    // (api.mu, the device stopped) the frequency and the reference level
    bool tune(const TuneSettings& s, std::string& err) {
        auto& api = rsaApi();
        const double f = centreOf(s);
        rsa::Status st = api.SetCenterFreq(f);
        if (st != rsa::kNoError) { err = "Tektronix RSA: the device refused " + mhz(f) + ": " + api.text(st); return false; }
        addNote(err, freqSpecNote("Tektronix RSA", s.centerHz, 9e3, maxHzOf(type_)));
        centreSet_ = f;
        double ref = refOf(s);
        refSet_ = ref;
        st = api.SetReferenceLevel(ref);
        if (st != rsa::kNoError) { addNote(err, "Tektronix RSA: the device refused the reference level: " + api.text(st) + ": the gain slider has no effect"); return true; }
        if (api.GetReferenceLevel) { double took = ref; if (api.GetReferenceLevel(&took) == rsa::kNoError) ref = took; }
        scale_ = float(1.0 / ampForDbm(ref));
        return true;
    }
    // (api.mu) the device runs and the stream is on
    bool run(std::string& err) {
        auto& api = rsaApi();
        rsa::Status st = api.Run();
        if (st != rsa::kNoError) { err = "Tektronix RSA: the device did not start: " + api.text(st); api.drop(); return false; }
        if ((st = api.StreamStart()) != rsa::kNoError) { api.Stop(); err = "Tektronix RSA: the IQ stream did not start: " + api.text(st); api.drop(); return false; }
        mine_ = true;
        api.streaming = true;
        fprintf(stderr, "Tektronix RSA: %s streams at %.6g Msps (bandwidth %.6g MHz)\n", serial_.c_str(), rate_ / 1e6, rate_ / kTopRateHz * kTopBandwidthHz / 1e6);
        fflush(stderr);
        return true;
    }
    // (api.mu) the stream and the device are stopped; the connection stays (see the top of this file). Safe twice.
    void halt() {
        auto& api = rsaApi();
        if (!mine_) return;
        api.StreamStop();
        api.Stop();
        mine_ = false;
        api.streaming = false;
    }

    std::string type_, serial_;
    bool mine_ = false;             // (api.mu) this source has the device running
    bool lost_ = false;             // the stream ended by itself: the connection is given up at the close
    float scale_ = 1;               // (api.mu) volts -> 1.0 at the reference level
    double centreSet_ = -1, refSet_ = 1e9;   // (api.mu) what the device was last set to
    std::vector<float> buf_;        // (api.mu) one block of I/Q pairs
};

} // namespace

bool rsaApiInstalled() { return rsaApi().ok; }

// ------------------------------------------------------------------ the list

void listRsa(std::vector<DeviceInfo>& out) {
    auto& api = rsaApi();
    if (!api.ok) return;
    std::lock_guard<std::mutex> lk(api.mu);
    // the library is not asked to search while it is connected to a device: the last list stands
    if (!api.connected.empty() && !api.known.empty()) { for (const auto& d : api.known) out.push_back(d); return; }
    std::vector<DeviceInfo> list;
    for (const auto& ts : api.search()) {
        if (ts.second.empty()) continue;
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "rsa";
        d.serial = ts.second;
        d.nativeArgs = ts.first + "|" + ts.second;
        d.name = "Tektronix " + (ts.first.empty() ? std::string("RSA") : ts.first) + " " + ts.second + " (experimental)";
        d.minRateHz = kTopRateHz / double(1 << (kRateSteps - 1));
        d.maxRateHz = kTopRateHz;
        d.gainMinDb = 0; d.gainMaxDb = kGainMaxDb;
        d.gainNote = "Tektronix RSA: the gain sets the reference level, 0 dB = +30 dBm, each dB more is 1 dB lower (60 dB = -30 dBm)";
        d.minFreqHz = 9e3; d.maxFreqHz = maxHzOf(ts.first);
        d.settings = rsaSettings();
        list.push_back(d);
    }
    static size_t logged = (size_t)-1;
    if (list.size() != logged) {
        logged = list.size();
        fprintf(stderr, "Tektronix RSA: the RSA API lists %zu device(s)\n", list.size());
        fflush(stderr);
    }
    api.known = list;
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makeRsa(const DeviceInfo& d) {
    std::string type, serial;
    if (!splitArgs(d.nativeArgs, type, serial)) return nullptr;
    return std::make_unique<RsaSource>(type, serial);
}

} // namespace native
} // namespace dect2
