// A fake Aaronia RTSA API (AaroniaRTSAAPI) for tests/source/test_spectran.cpp: the functions source_spectran.cpp loads, with the same names
// and the declarations of core/src/source/aartsa_min.h. It lists SPECTRAN V6 and V6 ECO devices, keeps their config trees, plays a tone in
// timed IQ packets (an eighth of a turn per sample: +rate/8, at half the reference level) and records what is sent to the transmitter.
// fake_spectran_num / fake_spectran_set / fake_spectran_str let the test look at the state and steer it.
#include "../../core/src/source/aartsa_min.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define FAKE_EXPORT extern "C" __declspec(dllexport)
#else
#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace dect2::aartsa;

namespace {

constexpr double kPi = 3.14159265358979323846;
std::recursive_mutex mu;
std::map<std::string, double> num;        // counters and settings
std::map<std::string, std::string> str;   // strings: "cfg:<path>" = what was set, "initpath", "opentype"

double get(const std::string& k, double def = 0) { auto it = num.find(k); return it == num.end() ? def : it->second; }
void bump(const std::string& k) { num[k] = get(k) + 1; }
std::string nar(const wchar_t* w) { std::string s; if (w) for (; *w; w++) s += (char)(*w < 128 ? *w : '?'); return s; }

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const char* kClockNames[] = {"46MHz", "61MHz", "76MHz", "92MHz", "122MHz", "184MHz", "245MHz"};
const double kClockHz[] = {46.08e6, 61.44e6, 76.8e6, 92.16e6, 122.88e6, 184.32e6, 245.76e6};
const char* kDecim[] = {"Full", "1 / 2", "1 / 4", "1 / 8", "1 / 16", "1 / 32", "1 / 64", "1 / 128", "1 / 256", "1 / 512"};

struct FakeDev {
    bool eco = false;
    std::string serial, mode;   // mode: raw, iqreceiver, iqtransmitter
    bool connected = false, started = false;
    std::map<std::string, std::string> cfg;   // path -> value as text
    double startT = 0;
    long long produced = 0, consumedSamples = 0;
    std::vector<float> buf;
    bool pending = false;       // a packet was handed out and not consumed yet
    double lastEnd = -1;
};
FakeDev* gOpen[32] = {nullptr};   // open devices, by the order the test lists them

std::vector<std::string> itemsOf(bool eco) {
    std::vector<std::string> v = {"main/centerfreq", "main/spanfreq", "main/reflevel", "main/decimation", "main/transgain",
                                  "device/outputformat", "device/usbcompression", "device/sclksource", "calibration/preamp", "calibration/rffilter"};
    if (!eco) for (const char* s : {"device/receiverclock", "device/receiverchannel", "device/gaincontrol"}) v.push_back(s);
    return v;
}
std::vector<std::string> gPaths;   // Config.d = index + 1 into this
void* pathId(const std::string& p) {
    for (size_t i = 0; i < gPaths.size(); i++) if (gPaths[i] == p) return (void*)(uintptr_t)(i + 1);
    gPaths.push_back(p);
    return (void*)(uintptr_t)gPaths.size();
}
std::string pathOf(void* id) { const size_t i = (size_t)(uintptr_t)id; return i >= 1 && i <= gPaths.size() ? gPaths[i - 1] : std::string(); }

double clockHz(FakeDev& d) {
    if (d.eco) return 61.44e6;
    const auto it = d.cfg.find("device/receiverclock");
    const std::string c = it == d.cfg.end() ? "92MHz" : it->second;
    for (int i = 0; i < 7; i++) if (c == kClockNames[i]) return kClockHz[i];
    return 92.16e6;
}
double numCfg(FakeDev& d, const char* p, double def) { const auto it = d.cfg.find(p); return it == d.cfg.end() ? def : atof(it->second.c_str()); }
double rateOf(FakeDev& d) {
    if (d.mode == "raw") {
        const auto it = d.cfg.find("main/decimation");
        int k = 0;
        if (it != d.cfg.end()) for (int i = 0; i < 10; i++) if (it->second == kDecim[i]) k = i;
        return clockHz(d) / double(1 << k);
    }
    return numCfg(d, "main/spanfreq", 1e6) * 1.5;
}

} // namespace

FAKE_EXPORT double fake_spectran_num(const char* k) { std::lock_guard<std::recursive_mutex> l(mu); return get(k); }
FAKE_EXPORT void fake_spectran_set(const char* k, double v) { std::lock_guard<std::recursive_mutex> l(mu); num[k] = v; }
// the value last set for a config path ("device/receiverclock"), or "initpath" / "opentype"
FAKE_EXPORT const char* fake_spectran_str(const char* k) {
    std::lock_guard<std::recursive_mutex> l(mu);
    static std::string out;
    const auto it = str.find(k);
    out = it == str.end() ? std::string() : it->second;
    return out.c_str();
}

FAKE_EXPORT Result AARTSAAPI_Init(uint32_t memory) { std::lock_guard<std::recursive_mutex> l(mu); bump("init"); num["memory"] = memory; return get("fail_init") ? kErrorMissingPathsFile : kOk; }
FAKE_EXPORT Result AARTSAAPI_Init_With_Path(uint32_t memory, const wchar_t* path) {
    std::lock_guard<std::recursive_mutex> l(mu);
    str["initpath"] = nar(path);
    bump("init_with_path");
    num["memory"] = memory;
    return get("fail_init") ? kErrorMissingPathsFile : kOk;
}
FAKE_EXPORT Result AARTSAAPI_Shutdown(void) { std::lock_guard<std::recursive_mutex> l(mu); bump("shutdown"); return kOk; }
FAKE_EXPORT uint32_t AARTSAAPI_Version(void) { return 0x00010203; }
FAKE_EXPORT Result AARTSAAPI_Open(Handle* h) { std::lock_guard<std::recursive_mutex> l(mu); if (!h) return kErrorInvalidParameter; h->d = (void*)&num; bump("open"); return kOk; }
FAKE_EXPORT Result AARTSAAPI_Close(Handle* h) { std::lock_guard<std::recursive_mutex> l(mu); if (h) h->d = nullptr; bump("close"); return kOk; }
FAKE_EXPORT Result AARTSAAPI_RescanDevices(Handle* h, int) { std::lock_guard<std::recursive_mutex> l(mu); if (!h || !h->d) return kErrorNotInitialized; bump("rescan"); return kOk; }

static std::string serialFor(bool eco, int i) { return std::string(eco ? "ECO-" : "V6-") + std::to_string(1000 + i); }
static int slotFor(bool eco, int i) { return (eco ? 16 : 0) + i; }

FAKE_EXPORT Result AARTSAAPI_EnumDevice(Handle* h, const wchar_t* type, int32_t index, DeviceInfo* di) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!h || !h->d || !di) return kErrorInvalidParameter;
    if (di->cbsize != (int64_t)sizeof(DeviceInfo)) { bump("bad_cbsize"); return kErrorInvalidSize; }
    const std::string t = nar(type);
    bool eco;
    if (t == "spectranv6") eco = false;
    else if (t == "spectranv6eco") eco = true;
    else return kErrorNotFound;
    if (index < 0 || index >= (int)get(eco ? "ecocount" : "v6count", 1)) return kErrorNotFound;
    const std::string s = serialFor(eco, index);
    for (size_t i = 0; i < s.size(); i++) di->serialNumber[i] = (wchar_t)s[i];
    di->serialNumber[s.size()] = 0;
    di->ready = gOpen[slotFor(eco, index)] == nullptr;
    di->superspeed = true;
    return kOk;
}

FAKE_EXPORT Result AARTSAAPI_OpenDevice(Handle* h, Device* d, const wchar_t* type, const wchar_t* serial) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!h || !h->d || !d) return kErrorInvalidParameter;
    const std::string t = nar(type), s = nar(serial);
    str["opentype"] = t;
    const size_t slash = t.find('/');
    if (slash == std::string::npos) return kErrorInvalidParameter;
    const std::string kind = t.substr(0, slash), mode = t.substr(slash + 1);
    if (kind != "spectranv6" && kind != "spectranv6eco") return kErrorNotFound;
    if (mode != "raw" && mode != "iqreceiver" && mode != "iqtransmitter" && mode != "iqtransceiver") return kErrorInvalidParameter;
    if (mode == "iqtransmitter" && get("no_tx")) return kErrorNotFound;
    const bool eco = kind == "spectranv6eco";
    int idx = -1;
    for (int i = 0; i < (int)get(eco ? "ecocount" : "v6count", 1); i++) if (serialFor(eco, i) == s) idx = i;
    if (idx < 0) return kErrorNotFound;
    if (gOpen[slotFor(eco, idx)]) { bump("busy"); return kErrorBusy; }
    auto* f = new FakeDev;
    f->eco = eco; f->serial = s; f->mode = mode;
    gOpen[slotFor(eco, idx)] = f;
    d->d = f;
    bump("opendevice");
    return kOk;
}
FAKE_EXPORT Result AARTSAAPI_CloseDevice(Handle*, Device* d) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!d || !d->d) return kErrorNotOpen;
    auto* f = (FakeDev*)d->d;
    if (f->started) bump("close_while_started");
    for (auto& g : gOpen) if (g == f) g = nullptr;
    delete f;
    d->d = nullptr;
    bump("closedevice");
    return kOk;
}
static FakeDev* devOf(Device* d) { return d ? (FakeDev*)d->d : nullptr; }

FAKE_EXPORT Result AARTSAAPI_ConnectDevice(Device* d) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f) return kErrorNotOpen;
    if (get("fail_connect")) return kErrorNotConnected;
    f->connected = true;
    bump("connect");
    return kOk;
}
FAKE_EXPORT Result AARTSAAPI_DisconnectDevice(Device* d) { std::lock_guard<std::recursive_mutex> l(mu); FakeDev* f = devOf(d); if (!f) return kErrorNotOpen; f->connected = false; bump("disconnect"); return kOk; }
FAKE_EXPORT Result AARTSAAPI_StartDevice(Device* d) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f) return kErrorNotOpen;
    if (!f->connected) return kErrorNotConnected;
    f->started = true;
    f->startT = nowSec();
    f->produced = 0;
    f->pending = false;
    bump("start");
    num["rate"] = rateOf(*f);
    return kOk;
}
FAKE_EXPORT Result AARTSAAPI_StopDevice(Device* d) { std::lock_guard<std::recursive_mutex> l(mu); FakeDev* f = devOf(d); if (!f) return kErrorNotOpen; f->started = false; bump("stop"); return kOk; }
FAKE_EXPORT Result AARTSAAPI_GetDeviceState(Device* d) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f) return kErrorNotOpen;
    return f->started ? kRunning : f->connected ? kConnected : kIdle;
}

FAKE_EXPORT Result AARTSAAPI_ConfigRoot(Device* d, Config* c) { std::lock_guard<std::recursive_mutex> l(mu); if (!devOf(d) || !c) return kErrorNotOpen; c->d = pathId(""); return kOk; }
FAKE_EXPORT Result AARTSAAPI_ConfigFind(Device* d, Config* group, Config* out, const wchar_t* path) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f || !group || !out) return kErrorNotOpen;
    if (pathOf(group->d) != "") return kErrorInvalidConfig;   // the driver always searches from the root
    const std::string p = nar(path);
    for (const auto& it : itemsOf(f->eco)) if (it == p) { out->d = pathId(p); return kOk; }
    return kErrorNotFound;
}
static bool rateItem(const std::string& p) { return p == "main/spanfreq" || p == "main/decimation" || p == "device/receiverclock"; }
FAKE_EXPORT Result AARTSAAPI_ConfigSetFloat(Device* d, Config* c, double v) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f || !c) return kErrorNotOpen;
    const std::string p = pathOf(c->d);
    if (p.empty()) return kErrorInvalidConfig;
    if (f->started && rateItem(p)) { bump("rate_change_while_streaming"); return kErrorBusy; }
    Result r = kOk;
    if (p == "main/reflevel") {
        const double lo = f->eco ? -40 : -20, hi = 10;
        if (v < lo || v > hi) { v = v < lo ? lo : hi; r = kWarningValueAdjusted; }
    }
    if (p == "main/spanfreq" && f->mode != "iqtransmitter") {
        const double top = clockHz(*f) / 1.5;
        if (v > top) { v = top; r = kWarningValueAdjusted; }
    }
    char b[64];
    snprintf(b, sizeof b, "%.17g", v);
    f->cfg[p] = b;
    str["cfg:" + p] = b;
    num["cfg:" + p] = v;
    bump("set:" + p);
    return r;
}
FAKE_EXPORT Result AARTSAAPI_ConfigGetFloat(Device* d, Config* c, double* v) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f || !c || !v) return kErrorNotOpen;
    const auto it = f->cfg.find(pathOf(c->d));
    if (it == f->cfg.end()) return kErrorNotFound;
    *v = atof(it->second.c_str());
    return kOk;
}
FAKE_EXPORT Result AARTSAAPI_ConfigSetString(Device* d, Config* c, const wchar_t* w) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f || !c) return kErrorNotOpen;
    const std::string p = pathOf(c->d), v = nar(w);
    if (p.empty()) return kErrorInvalidConfig;
    if (f->started && rateItem(p)) { bump("rate_change_while_streaming"); return kErrorBusy; }
    if (p == "device/receiverclock") {
        bool okv = false;
        for (const char* n : kClockNames) okv |= v == n;
        if (!okv || (v == "245MHz" && get("no245"))) return kErrorValueInvalid;
    }
    if (p == "main/decimation") {
        bool okv = false;
        for (const char* n : kDecim) okv |= v == n;
        if (!okv) return kErrorValueInvalid;
    }
    f->cfg[p] = v;
    str["cfg:" + p] = v;
    bump("set:" + p);
    return kOk;
}

// 200 packets of 4096 samples a second, whatever the rate (the test needs the logic, not the speed). fail_after: the stream breaks once
// after that many packets (an unplug: the driver must open the device again)
FAKE_EXPORT Result AARTSAAPI_GetPacket(Device* d, int32_t channel, int32_t index, Packet* p) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f || !p) return kErrorNotOpen;
    if (p->cbsize != (int64_t)sizeof(Packet)) { bump("bad_cbsize"); return kErrorInvalidSize; }
    if (channel != 0 || index != 0) return kErrorInvalidChannel;
    if (!f->started) return kErrorNotConnected;
    if (get("fail_after") > 0 && f->produced >= (long long)get("fail_after")) {
        num["fail_after"] = 0;
        f->started = f->connected = false;
        bump("failed_stream");
        return kErrorNotConnected;
    }
    constexpr int64_t n = 4096;
    if (!f->pending) {
        if ((nowSec() - f->startT) * 200.0 < double(f->produced)) return kEmpty;
        const double ref = numCfg(*f, "main/reflevel", 0);
        const double amp = 0.5 * std::sqrt(0.05) * std::pow(10.0, ref / 20.0);
        f->buf.resize(2 * n);
        const double base = double(f->produced) * double(n);
        for (int64_t k = 0; k < n; k++) {
            const double ph = 2 * kPi * std::fmod((base + double(k)) / 8.0, 1.0);
            f->buf[2 * k] = float(amp * std::cos(ph));
            f->buf[2 * k + 1] = float(amp * std::sin(ph));
        }
        f->pending = true;
    }
    const double rate = rateOf(*f), cf = numCfg(*f, "main/centerfreq", 0);
    memset(p, 0, sizeof *p);
    p->cbsize = sizeof *p;
    p->startTime = f->startT + double(f->produced * n) / rate;
    p->endTime = p->startTime + double(n) / rate;
    p->startFrequency = cf - rate / 2;
    p->stepFrequency = rate;
    p->spanFrequency = f->mode == "raw" ? rate : rate / 1.5;
    p->num = n;
    p->size = 2;
    p->stride = 2;
    p->fp32 = f->buf.data();
    return kOk;
}
FAKE_EXPORT Result AARTSAAPI_ConsumePackets(Device* d, int32_t channel, int32_t count) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f) return kErrorNotOpen;
    if (channel != 0 || count != 1 || !f->pending) { bump("bad_consume"); return kErrorInvalidParameter; }
    f->pending = false;
    f->produced++;
    bump("packets");
    return kOk;
}

FAKE_EXPORT Result AARTSAAPI_GetMasterStreamTime(Device* d, double* t) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!devOf(d) || !t) return kErrorNotOpen;
    *t = nowSec() * get("clock_speed", 1.0);
    return kOk;
}
FAKE_EXPORT Result AARTSAAPI_SendPacket(Device* d, int32_t channel, const Packet* p) {
    std::lock_guard<std::recursive_mutex> l(mu);
    FakeDev* f = devOf(d);
    if (!f || !p) return kErrorNotOpen;
    if (p->cbsize != (int64_t)sizeof(Packet)) { bump("bad_cbsize"); return kErrorInvalidSize; }
    if (channel != 0) return kErrorInvalidChannel;
    if (f->mode != "iqtransmitter" && f->mode != "iqtransceiver") { bump("send_wrong_mode"); return kErrorInvalidConfig; }
    if (!f->started) return kErrorNotConnected;
    if (p->num <= 0 || !p->fp32 || p->stride < 2 || !(p->stepFrequency > 0)) { bump("bad_packet"); return kErrorInvalidParameter; }
    if (f->lastEnd >= 0 && std::fabs(p->startTime - f->lastEnd) > 1e-9) bump("tx_gaps");
    if (p->startTime < nowSec()) bump("tx_late");
    if (std::fabs((p->endTime - p->startTime) - double(p->num) / p->stepFrequency) > 1e-9) bump("tx_bad_end");
    f->lastEnd = p->endTime;
    double peak = get("tx_peak");
    for (int64_t k = 0; k < p->num; k++) {
        const double a = std::hypot(p->fp32[k * p->stride], p->fp32[k * p->stride + 1]);
        if (a > peak) peak = a;
    }
    num["tx_peak"] = peak;
    num["tx_samples"] = get("tx_samples") + double(p->num);
    num["tx_flags"] = double((uint64_t)get("tx_flags") | p->flags);
    if (p->flags & kPacketStreamStart) bump("tx_starts");
    if (p->flags & kPacketStreamEnd) bump("tx_ends");
    num["tx_startfreq"] = p->startFrequency;
    num["tx_rate"] = p->stepFrequency;
    num["tx_ahead"] = p->startTime - nowSec();
    bump("tx_packets");
    return kOk;
}
