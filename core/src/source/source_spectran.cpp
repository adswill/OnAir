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
#include "dect2/common/diag.h"
#include <atomic>
#include "native_realddc.h"
#include <cwchar>
#include <map>

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
    aa::OpenFn ResetDevices = nullptr;                      // optional (the same signature as Open): cures a start that delivers nothing
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
        lib.opt(ResetDevices, "AARTSAAPI_ResetDevices");
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
// transmit: with a narrower main/spanfreq the device takes the packets and sends nothing. Measured on a V6 ECO 150XA-6 (RTSA-Suite PRO
// 3.0.3, a 1 Msps tone at 433.92 MHz heard by a second receiver on the TX output): silent at 1 and 10 MHz, there at 20 and 50 MHz.
constexpr double kTxMinSpanHz = 20e6;
// receive below the lowest main/centerfreq of the iqreceiver mode (5 MHz on that V6 ECO: a lower value comes back "adjusted" to it and the
// device stays at 5 MHz): the device stays at its lowest centre and runs `decim` times the rate, and the driver shifts the wanted
// frequency to zero, filters and decimates. kEcoIqTopRateHz is where the ECO's iqreceiver rate stops growing (measured: 59.2137 Msps).
constexpr int kLowMaxDecim = 4096;
// A V6 ECO that was started above this and is then tuned below it while it streams often stays set up for where it started: what is
// below about 4 MHz is then gone or weak and other carriers stand in its place (measured: started at 10.1 or 9 MHz and tuned to 5.5 or
// 5 MHz, a broadcast carrier on 3.955 MHz was missing in 11 of 21 tries; started at 8 MHz or lower, or on the frequency itself, in
// none of 19). Small steps afterwards do not cure it. So a retune down across it closes the device and starts it on the new frequency.
constexpr double kEcoLowBandHz = 8e6;
constexpr double kEcoIqTopRateHz = 59.2e6;
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
                                  "device/gaincontrol. Manual (default): the reference level follows the gain slider. Peak / Power: the device sets the reference level itself and the gain slider has no effect.",
                                  {"keep", "manual", "peak", "power"}, {"Keep", "Manual", "Peak", "Power"}, "manual"));
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
    bool resetFirst = false;   // the next openAs() resets the device first (after a start that did not run)

    bool openAs(const std::string& type, const std::string& mode, const std::string& serial, std::string& err) {
        auto& api = rtsa();
        if (!api.ready) { err = api.why.empty() ? std::string(kRtsaMissingMsg) : "SPECTRAN: " + api.why; return false; }
        std::lock_guard<std::mutex> lk(api.mu);
        const std::wstring wtype = widen(type + "/" + mode), wserial = widen(serial);
        // The library looks for the devices again before every open (measured on a V6 ECO: a second open without it either fails with
        // "not found" or connects and never streams). Not while this program has another device open: its stream would be disturbed.
        bool othersOpen;
        { std::lock_guard<std::mutex> g(gKnownMu); othersOpen = gOpenCount > 0; }
        // A V6 ECO that was last opened as a transmitter starts as a receiver and delivers nothing, and one that was last a receiver
        // does not reach the running state as a transmitter at about every second start (both measured); AARTSAAPI_ResetDevices
        // before the open in the other of the two cures it. The device is ready again about 3 s later.
        static std::map<std::string, bool> lastWasTx;   // (api.mu) by serial
        const bool tx = mode == "iqtransmitter";
        const auto last = lastWasTx.find(serial);
        const bool changed = last != lastWasTx.end() && last->second != tx;
        lastWasTx[serial] = tx;
        if ((changed || resetFirst) && !othersOpen && api.ResetDevices) {
            fprintf(stderr, "SPECTRAN: %s is reset before it is opened as a %s\n", serial.c_str(), tx ? "transmitter" : "receiver");
            fflush(stderr);
            api.ResetDevices(&api.h);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        resetFirst = false;
        // The library looks for the devices again, and the device has to be in its list and ready, before every open. Measured on a V6
        // ECO: an open while the library is still looking (kRetry), or while the device is not ready yet (right after it was closed,
        // reset or plugged in), fails with "not found" or connects and delivers nothing. Ready comes 2.7 s after a reset.
        if (!othersOpen) {
            const std::wstring wfamily = widen(type);
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
            for (;;) {
                const bool done = api.RescanDevices(&api.h, 500) != aa::kRetry;
                bool ready = false;
                for (int32_t i = 0; i < 8 && !ready; i++) {
                    aa::DeviceInfo di;
                    memset(&di, 0, sizeof di);
                    di.cbsize = sizeof di;
                    if (api.EnumDevice(&api.h, wfamily.c_str(), i, &di) != aa::kOk) break;
                    if (wserial == di.serialNumber) ready = di.ready;
                }
                if ((done && ready) || std::chrono::steady_clock::now() > end) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
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
        if (got && !aa::failed(r) && api.ConfigGetFloat) {   // what the device took, also when it says nothing (a device-side gain control or a quantised step)
            double v = value;
            if (!aa::failed(api.ConfigGetFloat(&d, &c, &v))) *got = v;
        }
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
        lastSet_ = s;
        mode_ = radioOption(s, "mode", "iq") == "raw" ? "raw" : "iq";
        if (!dev_.openAs(type_, mode_ == "raw" ? "raw" : "iqreceiver", serial_, err)) return false;
        fprintf(stderr, "SPECTRAN: opened %s %s (%s mode)\n", type_.c_str(), serial_.c_str(), mode_.c_str());
        fflush(stderr);
        // the lowest centre the device takes in this mode: asked for 0 Hz, it answers with the value it adjusted that to
        centreMin_ = 0;
        decim_ = 1;
        if (mode_ == "iq") {
            double low = 0;
            const aa::Result r = dev_.setNum(L"main/centerfreq", 0, &low);
            if (!aa::failed(r) && low > 0 && low < 100e6) centreMin_ = low;
        }
        return apply(s, err, false) && dev_.connectStart(err);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        // the stream thread steps aside while a retune waits (a std::mutex is not fair: it would take api_ again at once); released on every return
        struct CfgWait { std::atomic<int>& c; explicit CfgWait(std::atomic<int>& x) : c(x) { c++; } ~CfgWait() { c--; } } cfgWait(cfgWaiting_);
        { std::lock_guard<std::mutex> lk(api_); lastSet_ = s; }
        if (live) {
            bool startAgain = false;
            {
                std::lock_guard<std::mutex> lk(api_);
                if (!dev_.open) return false;
                const Plan p = plan(s);
                startAgain = eco_ && mode_ == "iq" && startedHz_ > kEcoLowBandHz && p.centre < kEcoLowBandHz;
                if (!startAgain && p.decim == decim_) return apply(s, err, true);
            }
            if (startAgain) {
                // (see kEcoLowBandHz; the stream thread itself calls this after it reopened a lost device: then it was just started there)
                const bool self = std::this_thread::get_id() == th_.get_id();
                if (self) { std::lock_guard<std::mutex> lk(api_); return apply(s, err, true); }
                fprintf(stderr, "SPECTRAN: tuned from above %s to below it: the device is started again on the new frequency\n", mhz(kEcoLowBandHz).c_str());
                fflush(stderr);
                stopStream();
                closeDevice();
                // (an open right after the close mostly delivers nothing, which is found out only 6 s later: the reset comes first)
                { std::lock_guard<std::mutex> lk(api_); dev_.resetFirst = true; }
                bool ok = false;
                for (int i = 0; i < 3 && !ok; i++) {
                    err.clear();
                    ok = openDevice(s, err);
                    if (!ok) { closeDevice(); std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
                }
                if (!ok) return false;
                startStream();
                return waitForStreamStart(err);
            }
            // across the device's lowest centre, or far enough below it for another span: the stream is stopped for it, as for a new rate
            // (the stream thread itself calls this after it reopened a lost device: nothing to stop then)
            const bool self = std::this_thread::get_id() == th_.get_id();
            if (!self) stopStream();
            bool ok;
            {
                std::lock_guard<std::mutex> lk(api_);
                ok = setUpAgain(s, err);
            }
            if (!self) startStream();
            return ok;
        }
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_.open) return false;
        return setUpAgain(s, err);   // a new rate (the stream thread is stopped)
    }
    // (under api_, the stream thread stopped) A new rate or span. The V6 ECO takes main/spanfreq and main/centerfreq while it streams (the
    // new rate arrives within 0.1 s, behind a few packets still at the old one), and a disconnect and connect of the open device leaves
    // it without samples (both measured on a V6 ECO 150XA-6): it is left running. The V6 is stopped, set up and started, as documented.
    bool setUpAgain(const TuneSettings& s, std::string& err) {
        if (eco_ && mode_ == "iq" && dev_.started) return apply(s, err, false);   // (the raw mode was not measured)
        dev_.stopDisconnect();
        return apply(s, err, false) && dev_.connectStart(err);
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> lk(api_);
        dev_.close();
    }
    bool waitForStreamStart(std::string& err) override {
        // the API says the real rate in every packet (stepFrequency): the first one is waited for, up to 6 s (a V6 ECO: 1.5 to 3.7 s after the start, measured)
        waitFirstPacket();
        // A V6 ECO now and then starts, says it runs and delivers nothing (measured: about one start in ten, more after it was used in
        // another mode); closing it, AARTSAAPI_ResetDevices and opening it again cures that, at times only the second time. Done twice at
        // most, and only when this program has no other SPECTRAN open (the reset is for all of them).
        for (int attempt = 0; attempt < 2 && firstRate_.load() <= 0 && run_ && rtsa().ResetDevices; attempt++) {
            fprintf(stderr, "SPECTRAN: no samples within %d s of the start: the device is reset and opened again\n", kFirstPacketS);
            fflush(stderr);
            stopStream();
            TuneSettings again;
            bool alone;
            {
                std::lock_guard<std::mutex> lk(api_);
                again = lastSet_;
                dev_.close();
                { std::lock_guard<std::mutex> g(gKnownMu); alone = gOpenCount == 0; }
                if (alone) { std::lock_guard<std::mutex> g(rtsa().mu); rtsa().ResetDevices(&rtsa().h); }
            }
            bool ok = false;
            std::string e;
            if (alone) {
                // the device is back on the bus about 3 s after the reset
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                for (int i = 0; i < 12 && !ok; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    e.clear();
                    ok = openDevice(again, e);
                    if (!ok) closeDevice();
                }
            } else {
                ok = openDevice(again, e);
            }
            if (!ok) { err = e.empty() ? std::string("SPECTRAN: the device did not open again after its reset") : e; return false; }
            startStream();
            waitFirstPacket();
        }
        const double got = firstRate_.load();
        if (got > 0) {
            if (std::fabs(got / decim_ - rate_) > 1.0) {
                fprintf(stderr, "SPECTRAN: the device delivers %.6g Msps (%.6g were planned)\n", got / decim_ / 1e6, rate_ / 1e6);
                fflush(stderr);
            }
            rate_ = got / decim_;
        } else if (run_) {
            addNote(err, "the SPECTRAN sent no samples within 6 s of its start");
        }
        return true;
    }
    // the API says the real rate in every packet (stepFrequency): the first one is waited for (a V6 ECO: 1.5 to 3.7 s after the start)
    static constexpr int kFirstPacketS = 6;
    void waitFirstPacket() {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(kFirstPacketS);
        while (std::chrono::steady_clock::now() < end && firstRate_.load() <= 0 && run_)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
            if (r == aa::kOk) {
                if (cfgWaiting_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (r == aa::kEmpty || r == aa::kRetry) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            if (run_) { fprintf(stderr, "SPECTRAN: the stream ended (%s)\n", hexResult(r).c_str()); fflush(stderr); }
            return;   // (NativeSource opens it again every second until it is back)
        }
    }

private:
    // (under api_) live: the stream runs, only the frequency, the reference level and the amplifier change
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        dev_.warn.clear();
        const Plan lowPlan = plan(s);
        const double f = lowPlan.centre;
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
                        for (int i = 0; i < n; i++) if (kClocks[i].hz >= lowPlan.hwRate * 0.999) { c = i; break; }
                    }
                    if (!setClock(c, err)) return false;
                }
                const aa::Result r = dev_.setNum(L"main/spanfreq", lowPlan.hwRate / kIqRateOverSpan);
                if (aa::failed(r)) { err = "SPECTRAN: a span of " + mhz(lowPlan.hwRate / kIqRateOverSpan) + " was refused (" + hexResult(r) + ")"; return false; }
                if (lowPlan.decim != decim_) {
                    decim_ = lowPlan.decim;
                    if (decim_ > 1) { ddcI_.design(decim_); ddcQ_.design(decim_); }
                }
                if (decim_ > 1) {
                    fprintf(stderr, "SPECTRAN: %s is below the device's lowest centre (%s): the device stays there at %.6g Msps, shifted and decimated by %d\n",
                            mhz(lowPlan.centre + lowPlan.shift).c_str(), mhz(centreMin_).c_str(), lowPlan.hwRate / 1e6, decim_);
                    fflush(stderr);
                }
            }
            rate_ = planned;
            requested_ = s.sampleRate;
            firstRate_ = 0;
            // (packets still at the rate of before a change are not taken for the new rate: for half a second only the planned one counts)
            expectRate_ = mode_ == "raw" ? 0 : lowPlan.hwRate;
            expectUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            checkRate(s.sampleRate, err);
        }
        aa::Result r = dev_.setNum(L"main/centerfreq", f);
        if (aa::failed(r)) { err = "SPECTRAN: the device refused " + mhz(f) + " (" + hexResult(r) + ")"; return false; }
        if (!live && !dev_.started) startedHz_ = f;   // (a set-up before the start: where the device will be started)
        shift_ = lowPlan.shift;
        wantedHz_ = lowPlan.centre + lowPlan.shift;
        deviceHz_ = f;
        ddcRate_ = 0;   // (the next packet tunes the shift)
        saidOff_ = false;
        if (live) expectUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (const Model* m = modelOf(type_)) addNote(err, freqSpecNote(eco_ ? "SPECTRAN V6 ECO" : "SPECTRAN V6", s.centerHz, m->minHz, m->maxHz));
        const std::string agc = radioOption(s, "agc", eco_ ? "keep" : "manual");   // the gain slider needs the device's manual gain control
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
            if (std::fabs(took - ref) > 0.5) {
                char b[200];
                if (agc == "peak" || agc == "power")
                    snprintf(b, sizeof b, "SPECTRAN: the gain control is %s, so the device keeps its own reference level (%.3g dBm) and the gain slider has no effect; choose Manual in the radio settings", agc.c_str(), took);
                else
                    snprintf(b, sizeof b, "SPECTRAN: the reference level is %.3g dBm, not the %.3g dBm the gain slider asked for (the device's range or steps)", took, ref);
                addNote(err, b);
            }
        } else if (r != aa::kErrorNotFound) {
            fprintf(stderr, "SPECTRAN: main/reflevel %.3g dBm was refused (%s)\n", ref, hexResult(r).c_str());
            fflush(stderr);
            DIAG(284064, "The SPECTRAN refused the reference level of %.3g dBm with code %s, so the gain slider has no effect.", ref, hexResult(r).c_str());
            addNote(err, "SPECTRAN: the device refused the reference level (" + hexResult(r) + "): the gain slider has no effect");
        }
        if (!dev_.warn.empty() && !live) addNote(err, "SPECTRAN: " + dev_.warn);
        return true;
    }
    // What the device is asked for. From its lowest centre up: the frequency itself and the rate. Below it (iqreceiver mode): the device
    // stays at its lowest centre and runs `decim` times the rate, wide enough for the flat part of what it delivers (a third of its rate to
    // each side) to reach the wanted frequency and half the output rate beyond it; convert() shifts and decimates.
    struct Plan {
        double centre = 0;   // of the device
        double shift = 0;    // the wanted frequency against it (negative), 0 when the device is on it
        int decim = 1;
        double hwRate = 0;   // the rate asked of the device
    };
    Plan plan(const TuneSettings& s) const {
        Plan p;
        p.centre = ppmCorrectedHz(std::max(s.centerHz, 0.0), radioPpm(s));
        const double want = s.sampleRate > 0 ? s.sampleRate : 2e6;
        p.hwRate = want;
        if (mode_ != "iq" || !(centreMin_ > 0) || p.centre >= centreMin_) return p;
        p.shift = p.centre - centreMin_;
        p.centre = centreMin_;
        const Model* m = modelOf(type_);
        const double top = eco_ ? kEcoIqTopRateHz : (m ? m->maxRateHz : 245.76e6);
        const int most = std::clamp((int)std::floor(top / want), 2, kLowMaxDecim);
        const int least = std::clamp((int)std::ceil(3 * -p.shift / want + 1.5), 2, most);
        // from 2, 3, 4, 6, 8, 12 ... and the one in use kept while it is wide enough and not more than four times too wide: tuning around
        // on short wave then seldom asks for another span
        p.decim = decim_;
        if (p.decim < least || p.decim > most || p.decim > 4 * least) {
            p.decim = 2;
            while (p.decim < least) p.decim = (p.decim & (p.decim - 1)) ? p.decim / 3 * 4 : p.decim / 2 * 3;
            p.decim = std::min(p.decim, most);
        }
        p.hwRate = want * p.decim;
        return p;
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
        if (pk.stepFrequency > 0 && firstRate_.load() <= 0 &&
            (!(expectRate_ > 0) || std::fabs(pk.stepFrequency - expectRate_) < 1.0 || std::chrono::steady_clock::now() > expectUntil_))
            firstRate_ = pk.stepFrequency;
        const int64_t stride = pk.stride >= 2 ? pk.stride : 2;
        float sc;
        { std::lock_guard<std::mutex> g(scaleMu_); sc = scale_; }
        // where the device is, by its own word: the centre and the rate of this packet. Half a second after a set-up or a retune they
        // have to be what was asked for; when they are not, the log says so once (the picture is then on another frequency or scale).
        const double at = pk.spanFrequency > 0 && pk.startFrequency > 0 ? pk.startFrequency + pk.spanFrequency / 2 : 0;
        if (!saidOff_ && mode_ == "iq" && std::chrono::steady_clock::now() > expectUntil_) {
            const bool rateOff = expectRate_ > 0 && pk.stepFrequency > 0 && std::fabs(pk.stepFrequency - expectRate_) > 1.0;
            const bool centreOff = at > 0 && deviceHz_ > 0 && std::fabs(at - deviceHz_) > 1000.0;
            if (rateOff || centreOff) {
                saidOff_ = true;
                fprintf(stderr, "SPECTRAN: the device is at %s and %.6g Msps, where %s and %.6g Msps were asked for\n", mhz(at).c_str(), pk.stepFrequency / 1e6,
                        mhz(deviceHz_).c_str(), expectRate_ / 1e6);
                fflush(stderr);
            }
        }
        if (decim_ > 1) {
            // below the device's lowest centre: shifted to zero and decimated. The filter takes real samples, so I and Q go through it one
            // after the other, each at half the gain: out = F(I) + j F(Q)
            // (the shift is the wanted frequency against the centre this packet says it has, which is the device's lowest one unless
            // the packet is still from before a change)
            const double shift = at > 0 ? wantedHz_ - at : shift_;
            if (pk.stepFrequency > 0 && (pk.stepFrequency != ddcRate_ || std::fabs(shift - ddcShift_) > 0.5)) {
                ddcRate_ = pk.stepFrequency;
                ddcShift_ = shift;
                ddcI_.tune(ddcRate_, shift, 0.5);
                ddcQ_.tune(ddcRate_, shift, 0.5);
            }
            if (!(ddcRate_ > 0)) return;
            ddcI_.process(pk.fp32, (size_t)pk.num, (size_t)stride, partI_);
            ddcQ_.process(pk.fp32 + 1, (size_t)pk.num, (size_t)stride, partQ_);
            const size_t n = std::min(partI_.size(), partQ_.size());
            conv_.resize(n);
            for (size_t i = 0; i < n; i++) conv_[i] = cf32((partI_[i].real() - partQ_[i].imag()) * sc, (partI_[i].imag() + partQ_[i].real()) * sc);
            partI_.clear();
            partQ_.clear();
            if (n) push(conv_.data(), n);
            return;
        }
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
    std::atomic<int> cfgWaiting_{0};
    // below the device's lowest centre (api_): that centre (0: not known, nothing is shifted), the wanted frequency against it, the
    // decimation (1: the device is on the frequency) and the packet rate the shift was last tuned for
    double centreMin_ = 0, shift_ = 0, ddcRate_ = 0;
    double startedHz_ = 0;                                   // the device's centre when it was last started (kEcoLowBandHz)
    double wantedHz_ = 0, deviceHz_ = 0, ddcShift_ = 0;     // the frequency asked for, the device's centre for it, the shift last tuned
    bool saidOff_ = false;                                   // the log already says that the device is not where it was asked to be
    TuneSettings lastSet_;                                   // (api_) what was last asked for: for the open after a reset
    double expectRate_ = 0;                                  // the packet rate planned at the last set-up (0: any)
    std::chrono::steady_clock::time_point expectUntil_{};
    int decim_ = 1;
    RealDdc ddcI_, ddcQ_;
    std::vector<cf32> partI_, partQ_;
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
        stuck_ = false;
        if (openOnce(t, err)) return true;
        if (!stuck_ || !rtsa().ResetDevices) return false;
        // (the same as for the receiver: a start that stays in its starting state is cured by a reset)
        fprintf(stderr, "SPECTRAN: the transmitter did not start running: once more after a reset\n");
        fflush(stderr);
        err.clear();
        dev_.resetFirst = true;
        return openOnce(t, err);
    }
    bool openOnce(const TxTune& t, std::string& err) {
        close();
        if (!rtsa().ok) { err = kRtsaMissingMsg; return false; }
        if (!rtsa().SendPacket || !rtsa().GetMasterStreamTime) { err = "SPECTRAN: this RTSA API library has no transmit functions (AARTSAAPI_SendPacket): install the current RTSA-Suite PRO"; return false; }
        if (!dev_.openAs(type_, "iqtransmitter", serial_, err)) { dev_.close(); return false; }
        rate_ = t.sampleRate > 0 ? t.sampleRate : 1e6;
        const double f = ppmCorrectedHz(txRadioHz(t), ppmOf(t));
        double tookHz = f;
        aa::Result r = dev_.setNum(L"main/centerfreq", f, &tookHz);
        if (aa::failed(r)) { err = "SPECTRAN: the transmitter refused " + mhz(f) + " (" + hexResult(r) + ")"; dev_.close(); return false; }
        // (a V6 ECO's transmitter starts at 22 MHz: a lower frequency comes back adjusted to that, and the signal would go out there)
        if (std::fabs(tookHz - f) > 1.0) {
            err = "SPECTRAN: the transmitter cannot send on " + mhz(f) + ": the nearest frequency it takes is " + mhz(tookHz);
            dev_.close();
            return false;
        }
        // the span is the band the SDK's modulator covers: the signal's rate (or the radio setting), and never less than kTxMinSpanHz
        double span = std::max(rate_, kTxMinSpanHz);
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
                if (std::chrono::steady_clock::now() > end) { err = "SPECTRAN: the transmitter did not start running within 5 s (is the TX option enabled on this device?)"; stuck_ = true; dev_.close(); return false; }
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
    bool stuck_ = false;   // the last openOnce() failed because the device did not reach its running state
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
