// Native driver for the Aaronia SPECTRAN V6 and V6 ECO (receive and transmit) through Aaronia's RTSA API, the C library that comes with
// the RTSA-Suite PRO (AaroniaRTSAAPI.dll on Windows, libAaroniaRTSAAPI.so on Linux). OnAir does not ship it: the user installs the
// RTSA-Suite PRO from aaronia.com/downloads, and the library is loaded at run time (native_common.h). The functions and structures used are
// declared in aartsa_min.h. Written from Aaronia's public documentation without a device: docs/SPECTRAN-V6.md says what is untested.
//
// Receive: the device is opened as "<type>/iqreceiver" (the default: the SDK's own resampler gives any rate; the rate is 1.5 x
// main/spanfreq) or "<type>/raw" (radio setting "Mode": receiver clock / a power-of-two decimation, the exact rates of the converter).
// The gain slider sets the reference level (main/reflevel, the level that fills the converter): 0 dB = +10 dBm, each dB more is 1 dB lower.
// The samples arrive in volts (|IQ|^2 / 50 ohm = watts: 0 dBm is an amplitude of sqrt(0.05)); they are scaled so that the reference
// level is 1.0, the full scale the rest of OnAir counts with.
// Transmit: "<type>/iqtransmitter"; the samples go out in timed packets (AARTSAAPI_SendPacket) kept a little ahead of the device's
// stream clock. Full scale 1.0 is sent as 0 dBm before main/transgain.
#include "native_common.h"
#include "aartsa_min.h"
#include <cwchar>

namespace dect2 {
namespace native {

namespace aa = dect2::aartsa;

namespace {

std::wstring widen(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
#else
    std::wstring w;
    for (unsigned char c : s) w += (wchar_t)c;
    return w;
#endif
}
std::string narrow(const wchar_t* w, size_t max) {
    std::string s;
    for (size_t i = 0; i < max && w[i]; i++) s += (w[i] > 0 && w[i] < 127) ? (char)w[i] : '?';
    return s;
}
std::string hexResult(aa::Result r) {
    switch (r) {
    case aa::kErrorNotInitialized: return "not initialised";
    case aa::kErrorNotFound: return "not found";
    case aa::kErrorBusy: return "busy (in use by another program, such as the RTSA-Suite PRO itself)";
    case aa::kErrorNotOpen: return "not open";
    case aa::kErrorNotConnected: return "not connected (unplugged?)";
    case aa::kErrorInvalidConfig: return "invalid configuration";
    case aa::kErrorMissingPathsFile: return "paths.xml of the RTSA-Suite PRO not found";
    case aa::kErrorValueInvalid: return "value not accepted";
    default: break;
    }
    char b[24];
    snprintf(b, sizeof b, "0x%08x", (unsigned)r);
    return b;
}

// Where the RTSA-Suite PRO puts the library. ONAIR_RTSA_DIR: the install folder when it is somewhere else.
// Windows: <install>\sdk\AaroniaRTSAAPI.dll, the libraries it needs in <install>; Linux: <install>/libAaroniaRTSAAPI.so.
constexpr const char* kLinuxDir = "/opt/aaronia-rtsa-suite/Aaronia-RTSA-Suite-PRO";
std::vector<std::string> rtsaInstallDirs() {
    std::vector<std::string> v;
    if (const char* e = getenv("ONAIR_RTSA_DIR")) if (*e) v.push_back(e);
#ifdef _WIN32
    for (const char* env : {"ProgramW6432", "ProgramFiles"})
        if (const char* pf = getenv(env)) v.push_back(std::string(pf) + "\\Aaronia AG\\Aaronia RTSA-Suite PRO");
    v.push_back("C:\\Program Files\\Aaronia AG\\Aaronia RTSA-Suite PRO");
#else
    v.push_back(kLinuxDir);
#endif
    return v;
}
#ifdef _WIN32
constexpr const char* kLibFile = "AaroniaRTSAAPI.dll";
constexpr char kSep = '\\';
#elif defined(__APPLE__)
constexpr const char* kLibFile = "libAaroniaRTSAAPI.dylib";
constexpr char kSep = '/';
#else
constexpr const char* kLibFile = "libAaroniaRTSAAPI.so";
constexpr char kSep = '/';
#endif
std::vector<std::string> rtsaLibNames() {
    std::vector<std::string> v;
    for (const auto& d : rtsaInstallDirs()) {
        v.push_back(d + kSep + "sdk" + kSep + kLibFile);
        v.push_back(d + kSep + kLibFile);
    }
#ifdef __APPLE__
    v.push_back("libAaroniaRTSAAPI.so");
#endif
    v.push_back(kLibFile);
    return v;
}
// the install folder of a library path: the folder of the file, or its parent when that is "sdk" (where paths.xml is looked for)
std::string installDirOf(const std::string& lib) {
    const size_t cut = lib.find_last_of("/\\");
    if (cut == std::string::npos) return std::string();
    std::string dir = lib.substr(0, cut);
    const size_t c2 = dir.find_last_of("/\\");
    std::string last = c2 == std::string::npos ? dir : dir.substr(c2 + 1);
    for (auto& ch : last) ch = (char)tolower((unsigned char)ch);
    if (last == "sdk" && c2 != std::string::npos) dir.resize(c2);
    return dir;
}

} // namespace

static const char* const kRtsaMissingMsg =
    "Aaronia RTSA-Suite PRO is not installed: the SPECTRAN V6 needs Aaronia's RTSA API library (AaroniaRTSAAPI), which comes with the "
    "RTSA-Suite PRO. Install it from aaronia.com/downloads (Windows or Linux), then restart OnAir. docs/SPECTRAN-V6.md";

struct RtsaApi {
    aa::InitFn Init = nullptr;
    aa::InitWithPathFn InitWithPath = nullptr;
    aa::ShutdownFn Shutdown = nullptr;
    aa::VersionFn Version = nullptr;
    aa::OpenFn Open = nullptr;
    aa::CloseFn Close = nullptr;
    aa::RescanDevicesFn RescanDevices = nullptr;
    aa::EnumDeviceFn EnumDevice = nullptr;
    aa::OpenDeviceFn OpenDevice = nullptr;
    aa::CloseDeviceFn CloseDevice = nullptr;
    aa::DeviceFn ConnectDevice = nullptr, DisconnectDevice = nullptr, StartDevice = nullptr, StopDevice = nullptr, GetDeviceState = nullptr;
    aa::GetPacketFn GetPacket = nullptr;
    aa::ConsumePacketsFn ConsumePackets = nullptr;
    aa::SendPacketFn SendPacket = nullptr;                  // transmit only
    aa::GetMasterStreamTimeFn GetMasterStreamTime = nullptr; // transmit only
    aa::ConfigRootFn ConfigRoot = nullptr;
    aa::ConfigFindFn ConfigFind = nullptr;
    aa::ConfigSetFloatFn ConfigSetFloat = nullptr;
    aa::ConfigSetStringFn ConfigSetString = nullptr;
    aa::ConfigGetFloatFn ConfigGetFloat = nullptr;          // optional: reads back a value the device adjusted
    bool ok = false;        // the library is there with every receive function
    bool ready = false;     // initialised and a library handle is open
    std::string why;        // why not ready
    aa::Handle h{nullptr};
    std::mutex mu;          // the library handle: listing, opening and closing take turns
    DynLib lib;

    RtsaApi() {
        if (nativeDisabled()) { why = "native radios are switched off (DECT2_NO_NATIVE)"; return; }
        if (!load()) { why = kRtsaMissingMsg; return; }
        ok = lib.get(Init, "AARTSAAPI_Init") && lib.get(Shutdown, "AARTSAAPI_Shutdown") && lib.get(Open, "AARTSAAPI_Open") &&
             lib.get(Close, "AARTSAAPI_Close") && lib.get(RescanDevices, "AARTSAAPI_RescanDevices") && lib.get(EnumDevice, "AARTSAAPI_EnumDevice") &&
             lib.get(OpenDevice, "AARTSAAPI_OpenDevice") && lib.get(CloseDevice, "AARTSAAPI_CloseDevice") &&
             lib.get(ConnectDevice, "AARTSAAPI_ConnectDevice") && lib.get(DisconnectDevice, "AARTSAAPI_DisconnectDevice") &&
             lib.get(StartDevice, "AARTSAAPI_StartDevice") && lib.get(StopDevice, "AARTSAAPI_StopDevice") &&
             lib.get(GetPacket, "AARTSAAPI_GetPacket") && lib.get(ConsumePackets, "AARTSAAPI_ConsumePackets") &&
             lib.get(ConfigRoot, "AARTSAAPI_ConfigRoot") && lib.get(ConfigFind, "AARTSAAPI_ConfigFind") &&
             lib.get(ConfigSetFloat, "AARTSAAPI_ConfigSetFloat") && lib.get(ConfigSetString, "AARTSAAPI_ConfigSetString");
        lib.opt(InitWithPath, "AARTSAAPI_Init_With_Path");
        lib.opt(Version, "AARTSAAPI_Version");
        lib.opt(GetDeviceState, "AARTSAAPI_GetDeviceState");
        lib.opt(ConfigGetFloat, "AARTSAAPI_ConfigGetFloat");
        lib.opt(SendPacket, "AARTSAAPI_SendPacket");
        lib.opt(GetMasterStreamTime, "AARTSAAPI_GetMasterStreamTime");
        if (!ok) { why = "the RTSA API library " + lib.path + " lacks functions OnAir needs: install the current RTSA-Suite PRO from aaronia.com/downloads"; return; }
        // paths.xml (where the SDK finds its firmware and calibration files) is in the install folder; the older libraries without
        // Init_With_Path look for it themselves
        const std::string dir = installDirOf(lib.path);
        aa::Result r;
        if (InitWithPath && !dir.empty()) r = InitWithPath(aa::kMemoryMedium, widen(dir).c_str());
        else r = Init(aa::kMemoryMedium);
        if (aa::failed(r)) {
            why = "the RTSA API did not start (" + hexResult(r) + ")" + (dir.empty() ? std::string() : " with the install folder " + dir) +
                  ": reinstall the RTSA-Suite PRO, or set ONAIR_RTSA_DIR to its folder";
            fprintf(stderr, "SPECTRAN: %s\n", why.c_str());
            fflush(stderr);
            return;
        }
        if (aa::failed(r = Open(&h))) {
            why = "the RTSA API did not open (" + hexResult(r) + ")";
            fprintf(stderr, "SPECTRAN: %s\n", why.c_str());
            fflush(stderr);
            return;
        }
        ready = true;
        fprintf(stderr, "SPECTRAN: RTSA API %s loaded (version %u)%s\n", lib.path.c_str(), Version ? (unsigned)Version() : 0u,
                SendPacket && GetMasterStreamTime ? "" : ", without the transmit functions");
        fflush(stderr);
    }
    // Kept for the life of the program (as the SDRplay API): the library's own teardown at exit is not something to race with.
    ~RtsaApi() = default;

private:
    bool load() {
        if (getenv("DECT2_NATIVE_LIBDIR")) return lib.open(rtsaLibNames());   // tests: the fake library
#ifdef _WIN32
        // AaroniaRTSAAPI.dll is in <install>\sdk and needs the libraries in <install>: that folder is added to the search for them
        // (AddDllDirectory is looked up at run time, and the flags are spelled out: older MinGW headers lack them)
        typedef void* (WINAPI * AddDirFn)(const wchar_t*);
        const AddDirFn addDir = reinterpret_cast<AddDirFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory")));
        constexpr DWORD kSearchDllLoadDir = 0x00000100, kSearchUserDirs = 0x00000400, kSearchDefaultDirs = 0x00001000;
        for (const auto& n : rtsaLibNames()) {
            if (n.find('\\') == std::string::npos) continue;
            if (GetFileAttributesA(n.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const std::wstring wdir = widen(installDirOf(n)), wlib = widen(n);
            HMODULE m = nullptr;
            if (addDir) {
                if (!wdir.empty()) addDir(wdir.c_str());
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
        return lib.open(rtsaLibNames());
    }
};
static RtsaApi& rtsa() { static RtsaApi a; return a; }

bool spectranApiInstalled() { return rtsa().ok; }

namespace {

// ------------------------------------------------------------------ the models and their settings

struct Clock { const wchar_t* name; double hz; };
// device/receiverclock of the V6 (the RTSA-API-Samples README and SDR++ list them); the V6 ECO has no such item: it runs at 61.44 MHz
const Clock kClocks[] = {{L"46MHz", 46.08e6}, {L"61MHz", 61.44e6}, {L"76MHz", 76.8e6}, {L"92MHz", 92.16e6},
                         {L"122MHz", 122.88e6}, {L"184MHz", 184.32e6}, {L"245MHz", 245.76e6}};
constexpr double kEcoClockHz = 61.44e6;
const wchar_t* const kDecimation[] = {L"Full", L"1 / 2", L"1 / 4", L"1 / 8", L"1 / 16", L"1 / 32", L"1 / 64", L"1 / 128", L"1 / 256", L"1 / 512"};
constexpr int kDecimations = 10;
constexpr double kIqRateOverSpan = 1.5;    // iqreceiver: the packets come at 1.5 x main/spanfreq (RTSA-API-Samples README)
constexpr double kRefTopDbm = 10;          // gain 0 dB = this reference level
constexpr double kGainMaxDb = 60;
constexpr double kZeroDbmAmp = 0.22360679774997896;   // sqrt(0.05): 0 dBm into 50 ohm, as the SDK's samples count
constexpr int64_t kTxBlock = 16384;        // samples per transmit packet (as Aaronia's IQTransmitter sample)

double ampForDbm(double dbm) { return kZeroDbmAmp * std::pow(10.0, dbm / 20.0); }

struct Model { bool eco; const char* type; const char* name; double maxRateHz, minHz, maxHz; };
const Model kModels[] = {
    {false, "spectranv6", "Aaronia SPECTRAN V6", 245.76e6, 9e3, 20e9},    // 9 kHz with the option, 6/8 GHz standard, up to 20 GHz (PLUS)
    {true, "spectranv6eco", "Aaronia SPECTRAN V6 ECO", 61.44e6, 9e3, 8e9},
};
const Model* modelOf(const std::string& type) {
    for (const auto& m : kModels) if (type == m.type) return &m;
    return nullptr;
}

RadioSetting numberSetting(const std::string& key, const std::string& label, const std::string& help, double lo, double hi, double step,
                           const std::string& unit, const std::string& def, bool restart) {
    RadioSetting r;
    r.type = RadioSetting::Number;
    r.key = key; r.label = label; r.help = help;
    r.minV = lo; r.maxV = hi; r.step = step; r.unit = unit; r.def = def; r.restart = restart;
    return r;
}

std::vector<RadioSetting> spectranSettings(bool eco) {
    std::vector<RadioSetting> v = {ppmSetting(0.01)};
    v.push_back(choiceSetting("mode", "Mode",
                              "IQ receiver (the default): any sample rate; the RTSA API filters and resamples (the rate is 1.5 x its span).\n"
                              "Raw: the converter's clock divided by 1 to 512, the exact rates of the hardware and the least work for the computer;\n"
                              "OnAir takes the smallest of those at or above the rate the mode asks for.",
                              {"iq", "raw"}, {"IQ receiver (any rate)", "Raw (clock / decimation)"}, "iq", true));
    if (eco)
        v.push_back(choiceSetting("preamp", "RF amplifier",
                                  "calibration/preamp of the V6 ECO. Keep: what the RTSA API chooses. Off for strong signals, Amp 1 / Amp 2 for weak ones.",
                                  {"keep", "auto", "off", "amp1", "amp2"}, {"Keep", "Auto", "Off", "Amp 1", "Amp 2"}, "keep"));
    else
        v.push_back(choiceSetting("preamp", "RF amplifier",
                                  "calibration/preamp of the V6: the amplifier and the preamplifier in front of the converter. Keep: what the RTSA API chooses.",
                                  {"keep", "Auto", "None", "Amp", "Preamp", "Both"}, {"Keep", "Auto", "None", "Amp", "Preamp", "Both"}, "keep"));
    if (!eco) {
        v.push_back(choiceSetting("clock", "Receiver clock",
                                  "device/receiverclock: the converter's rate. Auto: the slowest one that carries the sample rate (the least USB load).",
                                  {"auto", "46MHz", "61MHz", "76MHz", "92MHz", "122MHz", "184MHz", "245MHz"},
                                  {"Auto", "46.08 MHz", "61.44 MHz", "76.80 MHz", "92.16 MHz", "122.88 MHz", "184.32 MHz", "245.76 MHz"}, "auto", true));
        v.push_back(choiceSetting("rxchan", "Input", "device/receiverchannel: the RF input the samples come from.", {"Rx1", "Rx2"}, {"RX 1", "RX 2"}, "Rx1", true));
        v.push_back(choiceSetting("agc", "Gain control",
                                  "device/gaincontrol. Manual: the reference level follows the gain slider. Peak / Power: the device sets it itself.",
                                  {"keep", "manual", "peak", "power"}, {"Keep", "Manual", "Peak", "Power"}, "keep"));
    }
    v.push_back(choiceSetting("usbcomp", "USB compression",
                              "device/usbcompression. Compressed: less USB traffic for the high rates; Raw: the samples as the converter makes them.",
                              {"keep", "auto", "compressed", "raw"}, {"Keep", "Auto", "Compressed", "Raw"}, "keep", true));
    v.push_back(choiceSetting("sclk", "Stream clock",
                              "device/sclksource: what the device's time stamps and clock follow. GPS / PPS / 10 MHz need that signal on the device.",
                              {"keep", "Consumer", "Oscillator", "GPS", "PPS", "10MHz"}, {"Keep", "Consumer", "Oscillator", "GPS", "PPS", "10 MHz reference"}, "keep", true));
    v.push_back(numberSetting("txlead", "TX lead time",
                              "How far ahead of the device's clock OnAir queues transmit packets. More: no gaps on a busy computer; less: a quicker\n"
                              "key-up. Aaronia's samples use 50 ms for the V6 and 500 ms for the V6 ECO.",
                              0.02, 2.0, 0.01, "s", eco ? "0.5" : "0.05", false));
    return v;
}

// Every radio this program has open (by serial): the API's list does not show those as ready, so a rescan keeps the last entries
std::mutex gKnownMu;
std::vector<DeviceInfo> gKnown;
int gOpenCount = 0;

// nativeArgs: "<type>|<serial>"
bool splitArgs(const std::string& a, std::string& type, std::string& serial) {
    const size_t bar = a.find('|');
    if (bar == std::string::npos) return false;
    type = a.substr(0, bar);
    serial = a.substr(bar + 1);
    return modelOf(type) != nullptr && !serial.empty();
}

std::string mhz(double hz) { char b[40]; snprintf(b, sizeof b, "%.6g MHz", hz / 1e6); return b; }

// An opened device and its config tree (the receiver and the transmitter both use it)
struct Dev {
    aa::Device d{nullptr};
    aa::Config root{nullptr};
    bool open = false, connected = false, started = false;
    std::string warn;   // values the API adjusted, for the start message

    bool openAs(const std::string& type, const std::string& mode, const std::string& serial, std::string& err) {
        auto& api = rtsa();
        if (!api.ready) { err = api.why.empty() ? std::string(kRtsaMissingMsg) : "SPECTRAN: " + api.why; return false; }
        std::lock_guard<std::mutex> lk(api.mu);
        const std::wstring wtype = widen(type + "/" + mode), wserial = widen(serial);
        aa::Result r = api.OpenDevice(&api.h, &d, wtype.c_str(), wserial.c_str());
        if (aa::failed(r)) { err = "SPECTRAN " + serial + ": cannot open it as " + type + "/" + mode + " (" + hexResult(r) + ")"; return false; }
        open = true;
        { std::lock_guard<std::mutex> g(gKnownMu); gOpenCount++; }
        if (aa::failed(r = api.ConfigRoot(&d, &root))) { err = "SPECTRAN " + serial + ": no configuration tree (" + hexResult(r) + ")"; return false; }
        return true;
    }
    // false only when the item exists and refused the value
    bool setStr(const wchar_t* path, const std::wstring& value, std::string* err = nullptr) {
        auto& api = rtsa();
        aa::Config c{nullptr};
        if (aa::failed(api.ConfigFind(&d, &root, &c, path))) return true;   // this model has no such item
        const aa::Result r = api.ConfigSetString(&d, &c, value.c_str());
        if (r == aa::kWarningValueAdjusted || r == aa::kWarningValueDisabled) addNote(warn, narrow(path, 64) + " adjusted by the device");
        if (aa::failed(r)) { if (err) *err = narrow(path, 64) + " = " + narrow(value.c_str(), 64) + " refused (" + hexResult(r) + ")"; return false; }
        return true;
    }
    bool has(const wchar_t* path) {
        aa::Config c{nullptr};
        return !aa::failed(rtsa().ConfigFind(&d, &root, &c, path));
    }
    // got: the value the device took (read back when it says it adjusted it and the library can tell)
    aa::Result setNum(const wchar_t* path, double value, double* got = nullptr) {
        auto& api = rtsa();
        aa::Config c{nullptr};
        if (got) *got = value;
        aa::Result r = api.ConfigFind(&d, &root, &c, path);
        if (aa::failed(r)) return r;
        r = api.ConfigSetFloat(&d, &c, value);
        if (r == aa::kWarningValueAdjusted || r == aa::kWarningValueDisabled) {
            double v = value;
            if (got && api.ConfigGetFloat && !aa::failed(api.ConfigGetFloat(&d, &c, &v))) *got = v;
            char b[160];
            if (got && v != value) snprintf(b, sizeof b, "%s %.6g adjusted by the device to %.6g", narrow(path, 64).c_str(), value, v);
            else snprintf(b, sizeof b, "%s %.6g adjusted by the device", narrow(path, 64).c_str(), value);
            addNote(warn, b);
        }
        return r;
    }
    bool connectStart(std::string& err) {
        auto& api = rtsa();
        aa::Result r = api.ConnectDevice(&d);
        if (aa::failed(r)) { err = "SPECTRAN: the device did not connect (" + hexResult(r) + "): is it on a USB 3 port, and not in use by the RTSA-Suite PRO?"; return false; }
        connected = true;
        if (aa::failed(r = api.StartDevice(&d))) { err = "SPECTRAN: the device did not start (" + hexResult(r) + ")"; return false; }
        started = true;
        return true;
    }
    void stopDisconnect() {
        auto& api = rtsa();
        if (started) api.StopDevice(&d);
        if (connected) api.DisconnectDevice(&d);
        started = connected = false;
    }
    void close() {
        if (!open) return;
        stopDisconnect();
        auto& api = rtsa();
        { std::lock_guard<std::mutex> lk(api.mu); api.CloseDevice(&api.h, &d); }
        open = false;
        d.d = nullptr; root.d = nullptr;
        std::lock_guard<std::mutex> g(gKnownMu);
        if (gOpenCount > 0) gOpenCount--;
    }
};

// the raw mode's clock and decimation for a rate: the smallest rate at or above it (the slowest clock among equals); the fastest there is
// when none reaches it
struct RawPlan { int clock = -1; int decim = 0; double rate = 0; };
RawPlan rawPlan(double want, bool eco, int fixedClock) {
    RawPlan best, fastest;
    const int n = eco ? 1 : (int)(sizeof kClocks / sizeof kClocks[0]);
    for (int c = 0; c < n; c++) {
        if (fixedClock >= 0 && c != fixedClock) continue;
        const double hz = eco ? kEcoClockHz : kClocks[c].hz;
        for (int k = 0; k < kDecimations; k++) {
            const double r = hz / double(1 << k);
            if (r > fastest.rate) fastest = {eco ? -1 : c, k, r};
            if (r >= want * 0.999 && (best.rate == 0 || r < best.rate - 0.5)) best = {eco ? -1 : c, k, r};
        }
    }
    return best.rate > 0 ? best : fastest;
}
int clockIndex(const std::string& name) {
    for (int c = 0; c < (int)(sizeof kClocks / sizeof kClocks[0]); c++) if (widen(name) == kClocks[c].name) return c;
    return -1;
}

} // namespace

// Tests (tests/source/test_spectran.cpp): the raw mode's clock and decimation for a rate
double spectranRawRateForTest(double want, bool eco, int* clockIdx, int* decimLog2) {
    const RawPlan p = rawPlan(want, eco, -1);
    if (clockIdx) *clockIdx = p.clock;
    if (decimLog2) *decimLog2 = p.decim;
    return p.rate;
}

// ------------------------------------------------------------------ receive

class SpectranSource : public NativeSource {
public:
    SpectranSource(std::string type, std::string serial) : type_(std::move(type)), serial_(std::move(serial)) {
        const Model* m = modelOf(type_);
        eco_ = m && m->eco;
    }
    ~SpectranSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!rtsa().ok) { err = kRtsaMissingMsg; openFailure_ = OpenFailure::Final; return false; }
        if (!rtsa().ready) { err = "SPECTRAN: " + rtsa().why; openFailure_ = OpenFailure::Final; return false; }
        std::lock_guard<std::mutex> lk(api_);
        mode_ = radioOption(s, "mode", "iq") == "raw" ? "raw" : "iq";
        if (!dev_.openAs(type_, mode_ == "raw" ? "raw" : "iqreceiver", serial_, err)) return false;
        fprintf(stderr, "SPECTRAN: opened %s %s (%s mode)\n", type_.c_str(), serial_.c_str(), mode_.c_str());
        fflush(stderr);
        return apply(s, err, false) && dev_.connectStart(err);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_.open) return false;
        if (live) return apply(s, err, true);
        // a new rate (the stream thread is stopped): the device is stopped, set up again and started
        dev_.stopDisconnect();
        return apply(s, err, false) && dev_.connectStart(err);
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> lk(api_);
        dev_.close();
    }
    bool waitForStreamStart(std::string& err) override {
        // the API says the real rate in every packet (stepFrequency): the first one is waited for, up to 3 s
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end && firstRate_.load() <= 0 && run_)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const double got = firstRate_.load();
        if (got > 0) {
            if (std::fabs(got - rate_) > 1.0) {
                fprintf(stderr, "SPECTRAN: the device delivers %.6g Msps (%.6g were planned)\n", got / 1e6, rate_ / 1e6);
                fflush(stderr);
            }
            rate_ = got;
        } else if (run_) {
            addNote(err, "the SPECTRAN sent no samples within 3 s of its start");
        }
        return true;
    }
    void streamLoop() override {
        aa::Packet pk;
        while (run_) {
            aa::Result r;
            {
                std::lock_guard<std::mutex> lk(api_);
                if (!dev_.started) return;
                memset(&pk, 0, sizeof pk);
                pk.cbsize = sizeof pk;
                r = rtsa().GetPacket(&dev_.d, 0, 0, &pk);
                if (r == aa::kOk) {
                    convert(pk);
                    rtsa().ConsumePackets(&dev_.d, 0, 1);
                }
            }
            if (r == aa::kOk) continue;
            if (r == aa::kEmpty || r == aa::kRetry) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            if (run_) { fprintf(stderr, "SPECTRAN: the stream ended (%s)\n", hexResult(r).c_str()); fflush(stderr); }
            return;   // (NativeSource opens it again every second until it is back)
        }
    }

private:
    // (under api_) live: the stream runs, only the frequency, the reference level and the amplifier change
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        dev_.warn.clear();
        const double f = ppmCorrectedHz(std::max(s.centerHz, 0.0), radioPpm(s));
        if (!live) {
            if (radioOption(s, "rxchan", "Rx1") == "Rx2") dev_.setStr(L"device/receiverchannel", L"Rx2");
            else dev_.setStr(L"device/receiverchannel", L"Rx1");
            dev_.setStr(L"device/outputformat", L"iq");
            for (const char* key : {"usbcomp", "sclk"}) {
                const std::string v = radioOption(s, key, "keep");
                if (v != "keep") dev_.setStr(std::string(key) == "usbcomp" ? L"device/usbcompression" : L"device/sclksource", widen(v));
            }
            const double want = s.sampleRate > 0 ? s.sampleRate : 2e6;
            const int fixed = eco_ ? -1 : clockIndex(radioOption(s, "clock", "auto"));
            double planned = want;
            if (mode_ == "raw") {
                const RawPlan p = rawPlan(want, eco_, fixed);
                if (!setClock(p.clock, err)) return false;
                if (!dev_.setStr(L"main/decimation", kDecimation[p.decim], &err)) { err = "SPECTRAN: " + err; return false; }
                planned = p.rate;
            } else {
                // iqreceiver: the span is the rate / 1.5, and must stay within the receiver clock / 1.5 (V6: the slowest clock that does)
                if (!eco_) {
                    int c = fixed;
                    if (c < 0) {
                        const int n = (int)(sizeof kClocks / sizeof kClocks[0]);
                        c = n - 1;
                        for (int i = 0; i < n; i++) if (kClocks[i].hz >= want * 0.999) { c = i; break; }
                    }
                    if (!setClock(c, err)) return false;
                }
                const aa::Result r = dev_.setNum(L"main/spanfreq", want / kIqRateOverSpan);
                if (aa::failed(r)) { err = "SPECTRAN: a span of " + mhz(want / kIqRateOverSpan) + " was refused (" + hexResult(r) + ")"; return false; }
            }
            rate_ = planned;
            requested_ = s.sampleRate;
            firstRate_ = 0;
            checkRate(s.sampleRate, err);
        }
        aa::Result r = dev_.setNum(L"main/centerfreq", f);
        if (aa::failed(r)) { err = "SPECTRAN: the device refused " + mhz(f) + " (" + hexResult(r) + ")"; return false; }
        if (const Model* m = modelOf(type_)) addNote(err, freqSpecNote(eco_ ? "SPECTRAN V6 ECO" : "SPECTRAN V6", s.centerHz, m->minHz, m->maxHz));
        const std::string agc = radioOption(s, "agc", "keep");
        if (agc != "keep") dev_.setStr(L"device/gaincontrol", widen(agc));
        const std::string pre = radioOption(s, "preamp", "keep");
        if (pre != "keep") dev_.setStr(L"calibration/preamp", widen(pre));
        // the gain slider is the reference level; it can be set while streaming (as SDR++ does)
        const double ref = kRefTopDbm - std::clamp(s.gainDb, 0.0, kGainMaxDb);
        double took = ref;
        r = dev_.setNum(L"main/reflevel", ref, &took);
        if (!aa::failed(r)) {
            std::lock_guard<std::mutex> g(scaleMu_);
            scale_ = float(1.0 / ampForDbm(took));
        }
        if (!dev_.warn.empty() && !live) addNote(err, "SPECTRAN: " + dev_.warn);
        return true;
    }
    bool setClock(int c, std::string& err) {
        if (c < 0 || !dev_.has(L"device/receiverclock")) return true;
        std::string e;
        if (dev_.setStr(L"device/receiverclock", kClocks[c].name, &e)) return true;
        err = "SPECTRAN: " + e;
        return false;
    }
    // (under api_) one packet into the ring: I/Q in volts, scaled so that the reference level is 1.0
    void convert(const aa::Packet& pk) {
        if (!pk.fp32 || pk.num <= 0) return;
        if (pk.stepFrequency > 0 && firstRate_.load() <= 0) firstRate_ = pk.stepFrequency;
        const int64_t stride = pk.stride >= 2 ? pk.stride : 2;
        float sc;
        { std::lock_guard<std::mutex> g(scaleMu_); sc = scale_; }
        conv_.resize((size_t)pk.num);
        for (int64_t k = 0; k < pk.num; k++) conv_[(size_t)k] = cf32(pk.fp32[k * stride] * sc, pk.fp32[k * stride + 1] * sc);
        push(conv_.data(), conv_.size());
    }

    std::string type_, serial_, mode_ = "iq";
    bool eco_ = false;
    Dev dev_;
    std::mutex api_;                // the device's calls: the stream thread's GetPacket against a retune
    std::atomic<double> firstRate_{0};
    std::mutex scaleMu_;
    float scale_ = 1;
};

// ------------------------------------------------------------------ transmit

class SpectranTx : public TxSink {
public:
    SpectranTx(std::string type, std::string serial) : type_(std::move(type)), serial_(std::move(serial)) {
        const Model* m = modelOf(type_);
        eco_ = m && m->eco;
    }
    ~SpectranTx() override { close(); }

    bool open(const TxTune& t, std::string& err) override {
        close();
        if (!rtsa().ok) { err = kRtsaMissingMsg; return false; }
        if (!rtsa().SendPacket || !rtsa().GetMasterStreamTime) { err = "SPECTRAN: this RTSA API library has no transmit functions (AARTSAAPI_SendPacket): install the current RTSA-Suite PRO"; return false; }
        if (!dev_.openAs(type_, "iqtransmitter", serial_, err)) { dev_.close(); return false; }
        rate_ = t.sampleRate > 0 ? t.sampleRate : 1e6;
        const double f = ppmCorrectedHz(txRadioHz(t), ppmOf(t));
        aa::Result r = dev_.setNum(L"main/centerfreq", f);
        if (aa::failed(r)) { err = "SPECTRAN: the transmitter refused " + mhz(f) + " (" + hexResult(r) + ")"; dev_.close(); return false; }
        // the span is the band the SDK's modulator covers: the signal's rate (or the radio setting), the device widens it where it must
        double span = rate_;
        {
            const auto it = t.radio.find("txspan");
            if (it != t.radio.end()) { const double v = atof(it->second.c_str()); if (v > 0) span = v; }
        }
        if (aa::failed(r = dev_.setNum(L"main/spanfreq", span))) { err = "SPECTRAN: a transmit span of " + mhz(span) + " was refused (" + hexResult(r) + ")"; dev_.close(); return false; }
        if (aa::failed(r = dev_.setNum(L"main/transgain", t.gainDb))) { err = "SPECTRAN: the transmit gain was refused (" + hexResult(r) + ")"; dev_.close(); return false; }
        {
            const auto it = t.radio.find("txlead");
            lead_ = it != t.radio.end() && atof(it->second.c_str()) > 0 ? atof(it->second.c_str()) : (eco_ ? 0.5 : 0.05);
        }
        centerHz_ = f;
        if (!dev_.connectStart(err)) { dev_.close(); return false; }
        // the transmitter takes a moment to run (Aaronia's sample waits for the state)
        if (rtsa().GetDeviceState) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (rtsa().GetDeviceState(&dev_.d) != aa::kRunning) {
                if (std::chrono::steady_clock::now() > end) { err = "SPECTRAN: the transmitter did not start running within 5 s (is the TX option enabled on this device?)"; dev_.close(); return false; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!dev_.warn.empty()) addNote(err, "SPECTRAN: " + dev_.warn);
        first_ = true;
        return true;
    }
    bool write(const cf32* x, size_t n, std::string& err) override {
        if (!dev_.started) { err = "SPECTRAN: the transmitter is not open"; return false; }
        while (n > 0) {
            const size_t m = std::min<size_t>(n, (size_t)kTxBlock);
            buf_.resize(2 * m);
            for (size_t i = 0; i < m; i++) {
                // 1.0 = 0 dBm before main/transgain; beyond full scale is clipped (TxSink)
                buf_[2 * i] = std::clamp(x[i].real(), -1.f, 1.f) * (float)kZeroDbmAmp;
                buf_[2 * i + 1] = std::clamp(x[i].imag(), -1.f, 1.f) * (float)kZeroDbmAmp;
            }
            if (!send(buf_.data(), (int64_t)m, first_ ? aa::kPacketStreamStart | aa::kPacketSegmentStart : 0, err)) return false;
            first_ = false;
            x += m; n -= m;
        }
        return true;
    }
    void close() override {
        if (!dev_.open) return;
        if (dev_.started && !first_) {
            // the end of the stream: 1 ms of silence that carries the end flags, then wait for it to have gone out (at most 2 s)
            const int64_t m = std::max<int64_t>(1, (int64_t)(rate_ / 1000));
            buf_.assign((size_t)(2 * m), 0.f);
            std::string e;
            if (send(buf_.data(), m, aa::kPacketSegmentEnd | aa::kPacketStreamEnd, e)) {
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                double now = 0;
                while (std::chrono::steady_clock::now() < end && !aa::failed(rtsa().GetMasterStreamTime(&dev_.d, &now)) && now < next_)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        dev_.close();
        first_ = true;
    }
    double sampleRate() const override { return rate_; }
    std::string name() const override { return eco_ ? "SPECTRAN V6 ECO" : "SPECTRAN V6"; }

private:
    static double ppmOf(const TxTune& t) {
        const auto it = t.radio.find("ppm");
        if (it == t.radio.end()) return 0;
        const double p = atof(it->second.c_str());
        return p > -1000 && p < 1000 ? p : 0;
    }
    // one timed packet; paced so that the queue stays about lead_ ahead of the device's clock
    bool send(float* iq, int64_t m, uint64_t flags, std::string& err) {
        double now = 0;
        if (aa::failed(rtsa().GetMasterStreamTime(&dev_.d, &now))) { err = "SPECTRAN: the device's stream clock is not readable (unplugged?)"; return false; }
        if (first_ || next_ < now) next_ = now + std::max(lead_, 0.02);   // the start, or after a gap (late writes): a new time base
        aa::Packet p;
        memset(&p, 0, sizeof p);
        p.cbsize = sizeof p;
        p.flags = flags;
        p.startTime = next_;
        p.endTime = next_ + double(m) / rate_;
        p.startFrequency = centerHz_ - rate_ / 2;   // the packet's band: centre - rate/2 .. + rate/2 (Aaronia's IQTransceiver sample)
        p.stepFrequency = rate_;
        p.spanFrequency = rate_;
        p.num = m;
        p.size = 2;
        p.stride = 2;
        p.fp32 = iq;
        // wait while the queue is more than lead_ ahead (the device plays the packets at their times)
        for (int guard = 0; guard < 10000 && now + lead_ < p.startTime; guard++) {
            const double ahead = p.startTime - now - lead_;
            std::this_thread::sleep_for(std::chrono::microseconds((long long)std::clamp(ahead * 1e6, 200.0, 50000.0)));
            if (aa::failed(rtsa().GetMasterStreamTime(&dev_.d, &now))) { err = "SPECTRAN: the device's stream clock is not readable (unplugged?)"; return false; }
        }
        const aa::Result r = rtsa().SendPacket(&dev_.d, 0, &p);
        if (aa::failed(r)) { err = "SPECTRAN: the transmitter refused the samples (" + hexResult(r) + ")"; return false; }
        next_ = p.endTime;
        return true;
    }

    std::string type_, serial_;
    bool eco_ = false;
    Dev dev_;
    double rate_ = 0, centerHz_ = 0, lead_ = 0.05, next_ = 0;
    bool first_ = true;
    std::vector<float> buf_;
};

// ------------------------------------------------------------------ the list

void listSpectran(std::vector<DeviceInfo>& out) {
    auto& api = rtsa();
    if (!api.ready) return;
    {   // a device this program has open is not shown as ready: the last list stands while one is in use
        std::lock_guard<std::mutex> g(gKnownMu);
        if (gOpenCount > 0 && !gKnown.empty()) { for (const auto& d : gKnown) out.push_back(d); return; }
    }
    std::vector<DeviceInfo> list;
    {
        std::lock_guard<std::mutex> lk(api.mu);
        // a rescan also brings a newly plugged device's firmware up to date (it can take the 2 s)
        const aa::Result rr = api.RescanDevices(&api.h, 2000);
        if (aa::failed(rr)) { fprintf(stderr, "SPECTRAN: rescan failed (%s)\n", hexResult(rr).c_str()); fflush(stderr); }
        for (const auto& m : kModels) {
            const std::wstring wtype = widen(m.type);
            for (int32_t i = 0; i < 16; i++) {
                aa::DeviceInfo di;
                memset(&di, 0, sizeof di);
                di.cbsize = sizeof di;
                if (api.EnumDevice(&api.h, wtype.c_str(), i, &di) != aa::kOk) break;
                const std::string serial = narrow(di.serialNumber, 120);
                if (serial.empty()) continue;
                DeviceInfo d;
                d.kind = DeviceInfo::Native;
                d.board = "spectran";
                d.serial = serial;
                d.nativeArgs = std::string(m.type) + "|" + serial;
                d.name = std::string(m.name) + " " + serial + (di.ready ? "" : " (not ready)") + " (experimental)";
                d.minRateHz = 1e3;
                d.maxRateHz = m.maxRateHz;
                d.gainMinDb = 0; d.gainMaxDb = kGainMaxDb;
                d.gainNote = "SPECTRAN: the gain sets the reference level, 0 dB = +10 dBm, each dB more is 1 dB lower (30 dB = -20 dBm)";
                d.minFreqHz = m.minHz; d.maxFreqHz = m.maxHz;
                d.settings = spectranSettings(m.eco);
                // transmit: models with the TX option (the API refuses the transmitter on the others, and says so)
                d.canTransmit = api.SendPacket != nullptr && api.GetMasterStreamTime != nullptr;
                d.txMinRateHz = 1e3; d.txMaxRateHz = m.maxRateHz;
                d.txGainMinDb = -60; d.txGainMaxDb = 10;
                d.txGainNote = "SPECTRAN: main/transgain in dB; full scale is 0 dBm at 0 dB, the device sets the nearest level it has";
                list.push_back(d);
            }
        }
    }
    static size_t logged = (size_t)-1;
    if (list.size() != logged) {
        logged = list.size();
        fprintf(stderr, "SPECTRAN: the RTSA API lists %zu device(s)\n", list.size());
        fflush(stderr);
    }
    { std::lock_guard<std::mutex> g(gKnownMu); gKnown = list; }
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makeSpectran(const DeviceInfo& d) {
    std::string type, serial;
    if (!splitArgs(d.nativeArgs, type, serial)) return nullptr;
    return std::make_unique<SpectranSource>(type, serial);
}

std::unique_ptr<TxSink> makeSpectranTx(const DeviceInfo& d) {
    std::string type, serial;
    if (!splitArgs(d.nativeArgs, type, serial)) return nullptr;
    return std::make_unique<SpectranTx>(type, serial);
}

} // namespace native
} // namespace dect2
