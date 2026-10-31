// JNI bridge: the Android app talks to the desktop receiver library (dect2::Engine) through these functions only.
// One engine for the whole process. Receive only: nothing here transmits.
#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "dect2/common/verbose_log.h"
#include "dect2/engine/engine.h"
#include "dect2/source/gain.h"
#include "dect2/source/iq_file.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/net/netradio.h"
#include "dect2/net/remote_api.h"
#include "dect2/general/general_rx.h"
#include "dect2/fm/fm_rx.h"
#include "results_json.h"
#include "jni_radio.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnAir", __VA_ARGS__)

namespace dect2 { void applySubMode(Engine& e, const SubMode* s); }

using namespace dect2;

namespace {

// 4 USB radio, 5 network radio, 6 sound card: onair::RadioSource (jni_radio.h)
enum Source { kSynth = 0, kFile = 1, kRtlTcp = 2, kSpyServer = 3 };
bool isRadioSource(int k) { return k == onair::kSrcUsb || k == onair::kSrcNet || k == onair::kSrcSoundCard; }

struct Bridge {
    Engine eng;
    std::mutex mu;                // the control calls (start/stop/retune) one at a time
    TuneSettings tune;
    DeviceInfo dev;
    FileOptions file;
    int std = 7;                  // engine standard asked for
    int sourceKind = kSynth;
    const SubMode* sub = nullptr;
    // AGC (the desktop's AutoGain on the spectrum statistics)
    AutoGain agc;
    bool agcOn = false;
    int gainMax = 0;
    uint64_t specSeq = 0, rxSeq = 0;
    std::string lastError;
};
Bridge* gB = nullptr;

std::string jstr(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}
jstring mk(JNIEnv* e, const std::string& s) {
    // JNI wants modified UTF-8: build UTF-16 by hand so that any byte string (decoded text with odd bytes) cannot crash NewStringUTF
    std::u16string u;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD; int n = 1;
        if (c < 0x80) cp = c;
        else if (c >= 0xC2 && c < 0xE0 && i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; }
        else if (c >= 0xE0 && c < 0xF0 && i + 2 < s.size() && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if (c >= 0xF0 && c < 0xF5 && i + 3 < s.size() && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4;
        }
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; u.push_back((char16_t)(0xD800 + (cp >> 10))); u.push_back((char16_t)(0xDC00 + (cp & 0x3FF))); }
        else u.push_back((char16_t)cp);
    }
    return e->NewString(reinterpret_cast<const jchar*>(u.data()), (jsize)u.size());
}

std::string q(const std::string& s) { return '"' + jsonEscape(s) + '"'; }
std::string n(double v) { return jsonNum(v); }

int engineStdForFamily(int f) { return f >= 6 ? f + 2 : f == 1 ? 3 : f == 2 ? 4 : f == 3 ? 5 : f == 4 ? 6 : f == 5 ? 7 : 0; }

std::string tuningJson(int std, const SubMode* sm) {
    const ModeTuning* mt = modeTuning(std);
    double def = 100, mn = 0.01, mx = 6000, rate = 2e6, bw = 1, minRate = 0;
    std::string name;
    switch (std) {   // the modes before the tuning table
    case 0: case 1: case 2: def = 522; mn = 170; mx = 870; rate = 10e6; bw = 8; minRate = 7.9e6; name = "DVB"; break;
    case 3: def = 473; mn = 170; mx = 870; rate = 8e6; bw = 6; minRate = 6.5e6; name = "ATSC"; break;
    case 4: def = 218.64; mn = 174; mx = 240; rate = 2.048e6; bw = 1.7; minRate = 1.6e6; name = "DAB"; break;
    case 5: def = 522; mn = 170; mx = 870; rate = 8e6; bw = 6; minRate = 6.5e6; name = "ATSC 3.0"; break;
    case 6: def = 473.143; mn = 170; mx = 770; rate = 8e6; bw = 6; minRate = 6e6; name = "ISDB-T"; break;
    case 7: def = 100; mn = 64; mx = 108; rate = 2.4e6; bw = 0.25; minRate = 0.5e6; name = "FM"; break;
    default: break;
    }
    if (mt) { def = mt->defMhz; mn = mt->minMhz; mx = mt->maxMhz; rate = mt->sampleRate; bw = mt->bandwidthMhz; minRate = mt->minSampleRate; name = mt->name; }
    if (sm) { def = sm->defMhz; name = sm->name; }
    return "{\"std\":" + std::to_string(std) + ",\"name\":" + q(name) + ",\"sub\":" + q(sm ? sm->id : "") + ",\"defMhz\":" + n(def) + ",\"minMhz\":" + n(mn) +
           ",\"maxMhz\":" + n(mx) + ",\"rate\":" + n(rate) + ",\"bwMhz\":" + n(bw) + ",\"minRate\":" + n(minRate) + "}";
}

// what the radio and the sample rate should be for the active mode (the desktop's applyBandwidth, without its screens)
void applyModeTuning(Bridge& b, double wantRate) {
    TuneSettings& t = b.tune;
    t.synth.atsc = b.std == 3;
    t.synth.dab = b.std == 4;
    t.synth.mode = b.std >= 4 ? b.std : 0;
    t.basebandFilterHz = 0;
    switch (b.std) {
    case 0: case 1: case 2: t.bandwidthMhz = 8; t.sampleRate = 10e6; break;
    case 3: t.bandwidthMhz = 6; t.sampleRate = 8e6; break;
    case 4: t.bandwidthMhz = 1.7; t.sampleRate = 2.048e6; t.basebandFilterHz = 1.75e6; break;
    case 5: t.bandwidthMhz = 6; t.sampleRate = 8e6; break;
    case 6: t.bandwidthMhz = 8; t.sampleRate = 10e6; t.isdbtBwMhz = 6; t.synth.isdbtBwMhz = 6; break;
    case 7: t.bandwidthMhz = 0.25; t.sampleRate = b.sourceKind == kSynth ? 4e6 : 2.4e6; t.basebandFilterHz = 0; break;
    default:
        if (const ModeTuning* mt = modeTuning(b.std)) {
            t.bandwidthMhz = mt->bandwidthMhz; t.sampleRate = mt->sampleRate; t.basebandFilterHz = mt->basebandHz;
            const bool rtl = b.sourceKind == kRtlTcp || (b.sourceKind == onair::kSrcUsb && b.dev.board == "rtlsdr");
            if (rtl && mt->minSampleRate <= 2.56e6 && t.sampleRate > 2.56e6) t.sampleRate = 2.4e6;
            if (b.sourceKind == onair::kSrcSoundCard) t.sampleRate = 48000;   // the card's rate; the source interpolates for modes that want more
        }
        if (b.sub) applySubModeSynth(t.synth, *b.sub);
        break;
    }
    if (wantRate > 0) t.sampleRate = wantRate;
}

std::string deviceJson(const DeviceInfo& d) {
    std::string s = "{\"name\":" + q(d.name) + ",\"gainMin\":" + n(d.gainMinDb) + ",\"gainMax\":" + n(d.gainMaxDb) + ",\"maxRate\":" + n(d.maxRateHz) +
                    ",\"minRate\":" + n(d.minRateHz) + ",\"biasTee\":" + (d.hasBiasTee ? "true" : "false") + ",\"settings\":[";
    bool first = true;
    for (const RadioSetting& r : d.settings) {
        if (!first) s += ','; first = false;
        const char* type = r.type == RadioSetting::Bool ? "bool" : r.type == RadioSetting::Choice ? "choice" : "number";
        s += "{\"key\":" + q(r.key) + ",\"label\":" + q(r.label) + ",\"help\":" + q(r.help) + ",\"type\":\"" + type + "\",\"def\":" + q(r.def) +
             ",\"min\":" + n(r.minV) + ",\"max\":" + n(r.maxV) + ",\"step\":" + n(r.step) + ",\"unit\":" + q(r.unit) + ",\"values\":[";
        for (size_t i = 0; i < r.values.size(); i++) s += (i ? "," : "") + q(r.values[i]);
        s += "],\"names\":[";
        for (size_t i = 0; i < r.names.size(); i++) s += (i ? "," : "") + q(r.names[i]);
        s += "]}";
    }
    return s + "]}";
}

DeviceInfo makeDevice(int kind, const std::string& host, int port, std::string* err = nullptr) {
    if (isRadioSource(kind)) {   // USB (host = the descriptor's registry name), network (host = the radio's settings text), sound card
        DeviceInfo d;
        std::string e;
        if (onair::radioDevice(kind, host, d, e)) return d;
        if (err) *err = e;
        d.kind = DeviceInfo::Synthetic; d.name = "";
        return d;
    }
    if (kind == kRtlTcp || kind == kSpyServer) {
        NetRadio r; r.type = kind == kRtlTcp ? NetRadio::RtlTcp : NetRadio::SpyServer; r.host = host; r.port = port;
        auto v = netRadioDevices({r});
        if (!v.empty()) return v[0];
    }
    DeviceInfo d;
    d.kind = kind == kFile ? DeviceInfo::File : DeviceInfo::Synthetic;
    d.name = kind == kFile ? "IQ file" : "Test signal";
    return d;
}

void parseRadio(const std::string& s, std::map<std::string, std::string>& out) {   // "k=v;k=v"
    out.clear();
    std::stringstream ss(s); std::string item;
    while (std::getline(ss, item, ';')) { auto p = item.find('='); if (p != std::string::npos && p > 0) out[item.substr(0, p)] = item.substr(p + 1); }
}

void pushGain(Bridge& b) {
    const bool generic = b.dev.isGeneric();
    if (generic) b.tune.gainDb = std::max(0.0, std::min((double)b.gainMax, b.tune.gainDb));
    if (b.dev.kind == DeviceInfo::HackRF) {   // one slider on the phone: the total split over LNA, VGA and the amplifier as the desktop does
        const GainSetting g = gainForTotal((int)std::lround(std::max(0.0, std::min(116.0, b.tune.gainDb))));
        b.tune.lnaDb = g.lna; b.tune.vgaDb = g.vga; b.tune.ampOn = g.amp;
    }
}

} // namespace

std::string onair::deviceJsonOf(const DeviceInfo& d) { return deviceJson(d); }

#define FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_io_github_adswill_onair_OnAirNative_##name

FN(jboolean, nativeInit)(JNIEnv* e, jobject, jstring resultsDir) {
    if (gB) return JNI_TRUE;
    gB = new Bridge();
    gB->eng.setSpectrumEnabled(true);
    dect2::setDiagSink([](const std::string& s) { if (gB) gB->eng.log(s); });   // driver messages (radio lost, stream restarted, verbose lines) into the log the app shows: stderr goes nowhere on Android
    const std::string dir = jstr(e, resultsDir);
    if (!dir.empty()) gB->eng.setResultsDir(dir);
    return JNI_TRUE;
}

// every mode of the engine's tuning table + the protocols that are their own entries, as the desktop lists them
FN(jstring, nativeModeTable)(JNIEnv* e, jobject) {
    std::string s = "[";
    bool first = true;
    for (int st = 0; st < 120; st++) {
        const ModeTuning* mt = modeTuning(st);
        if (!mt) continue;
        if (!first) s += ','; first = false;
        s += tuningJson(st, nullptr);
    }
    s += "],";
    std::string subs = "[";
    int cnt = 0;
    const SubMode* t = subModes(cnt);
    for (int i = 0; i < cnt; i++) subs += (i ? "," : "") + tuningJson(t[i].stdMode, &t[i]);
    subs += "]";
    return mk(e, "{\"modes\":" + s.substr(0, s.size() - 1) + ",\"subs\":" + subs + "}");
}

FN(jstring, nativeSetMode)(JNIEnv* e, jobject, jint family, jstring subId) {
    Bridge& b = *gB;
    std::lock_guard<std::mutex> lk(b.mu);
    const std::string sid = jstr(e, subId);
    b.sub = sid.empty() ? nullptr : subModeById(sid);
    b.std = b.sub ? b.sub->stdMode : engineStdForFamily(family);
    b.eng.setStandard(b.std);
    applySubMode(b.eng, b.sub);
    return mk(e, tuningJson(b.std, b.sub));
}

FN(jstring, nativeDeviceInfo)(JNIEnv* e, jobject, jint kind, jstring host, jint port) {
    DeviceInfo d = makeDevice(kind, jstr(e, host), port);
    return mk(e, deviceJson(d));
}

// radio: "key=value;key=value" (the settings DeviceInfo lists), file: path (/proc/self/fd/N), fmt: 0 cs8, 1 cu8, 2 cf32, 3 ci16, 4 cu16
FN(jstring, nativeStart)(JNIEnv* e, jobject, jint kind, jstring host, jint port, jstring path, jint fmt, jdouble fileRate, jdouble freqHz,
                         jdouble rate, jdouble gainDb, jboolean agc, jboolean bias, jstring radio) {
    Bridge& b = *gB;
    std::lock_guard<std::mutex> lk(b.mu);
    if (b.eng.running()) b.eng.stop();
    b.sourceKind = kind;
    std::string devErr;
    b.dev = makeDevice(kind, jstr(e, host), port, &devErr);
    if (!devErr.empty()) { b.lastError = devErr; return mk(e, devErr); }
    if (b.dev.kind == DeviceInfo::HackRF && b.dev.gainMaxDb <= 0) { b.dev.gainMinDb = 0; b.dev.gainMaxDb = 116; }   // LNA 40 + VGA 62 + amp 14
    b.tune.centerHz = freqHz;
    b.tune.gainDb = gainDb;
    b.tune.biasTee = bias && b.dev.hasBiasTee;
    parseRadio(jstr(e, radio), b.tune.radio);
    applyModeTuning(b, rate);
    b.gainMax = b.dev.gainMaxDb > 0 ? (int)b.dev.gainMaxDb : 0;
    pushGain(b);
    b.agcOn = agc; b.agc.reset(); b.agc.setGenericMax(b.gainMax);
    b.file = FileOptions();
    if (kind == kFile) {
        b.file.path = jstr(e, path);
        static const FileFormat f[] = {FileFormat::CS8, FileFormat::CU8, FileFormat::CF32, FileFormat::CI16, FileFormat::CU16};
        b.file.format = f[fmt < 0 || fmt > 4 ? 0 : fmt];
        b.file.sampleRate = fileRate > 0 ? fileRate : 2e6;
        b.file.loop = true;
        b.dev.serial = b.file.path;
    }
    b.eng.setStandard(b.std);
    applySubMode(b.eng, b.sub);
    b.specSeq = 0; b.rxSeq = 0;
    const bool ok = b.eng.start(b.dev, b.tune, b.file);
    b.lastError = ok ? "" : (b.eng.sourceNote().empty() ? "could not start the source" : b.eng.sourceNote());
    return mk(e, b.lastError);
}

FN(void, nativeStop)(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(gB->mu);
    gB->eng.stop();
}
FN(jboolean, nativeRunning)(JNIEnv*, jobject) { return gB && gB->eng.running(); }
FN(jstring, nativeNote)(JNIEnv* e, jobject) { return mk(e, gB->eng.sourceNote()); }

FN(void, nativeRetune)(JNIEnv*, jobject, jdouble hz) {
    Bridge& b = *gB;
    std::lock_guard<std::mutex> lk(b.mu);
    b.tune.centerHz = hz;
    if (b.eng.running()) b.eng.retune(b.tune);
}
FN(void, nativeSetGain)(JNIEnv*, jobject, jdouble db, jboolean agc) {
    Bridge& b = *gB;
    std::lock_guard<std::mutex> lk(b.mu);
    b.tune.gainDb = db;
    if (agc && !b.agcOn) b.agc.reset();
    b.agcOn = agc;
    pushGain(b);
    if (b.eng.running()) b.eng.retune(b.tune);
}
FN(void, nativeSetRadio)(JNIEnv* e, jobject, jstring radio, jboolean bias) {
    Bridge& b = *gB;
    std::lock_guard<std::mutex> lk(b.mu);
    parseRadio(jstr(e, radio), b.tune.radio);
    b.tune.biasTee = bias && b.dev.hasBiasTee;
    if (b.eng.running()) b.eng.retune(b.tune);
}

// meta: [centreHz, spanHz, rmsDbfs, clipFraction]. true when a new line was copied into out (max-decimated to out.size() bins)
FN(jboolean, nativeSpectrum)(JNIEnv* e, jobject, jfloatArray out, jdoubleArray meta) {
    Bridge& b = *gB;
    SpectrumFrame f;
    if (!b.eng.latestSpectrum(f, b.specSeq)) return JNI_FALSE;
    b.specSeq = f.seq;
    const jsize m = e->GetArrayLength(out);
    std::vector<float> v((size_t)m);
    const size_t N = f.dbfs.size();
    for (jsize i = 0; i < m; i++) {
        const size_t a = (size_t)i * N / (size_t)m, c = std::max(a + 1, (size_t)(i + 1) * N / (size_t)m);
        float mx = f.dbfs[a];
        for (size_t k = a + 1; k < c && k < N; k++) mx = std::max(mx, f.dbfs[k]);
        v[(size_t)i] = mx;
    }
    e->SetFloatArrayRegion(out, 0, m, v.data());
    const double md[4] = {b.eng.captureCentreHz(), b.eng.sampleRate(), f.stats.rmsDbfs, f.stats.clipFraction};
    e->SetDoubleArrayRegion(meta, 0, 4, md);
    if (b.agcOn && b.eng.running() && !b.dev.isGeneric() == false) {
        GainSetting g = genericGain((int)std::lround(b.tune.gainDb), b.gainMax);
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (b.agc.update(now, f.stats, g)) {
            std::lock_guard<std::mutex> lk(b.mu);
            b.tune.gainDb = g.vga;
            b.eng.retune(b.tune);
        }
    }
    return JNI_TRUE;
}

FN(jdouble, nativeGainNow)(JNIEnv*, jobject) { return gB->tune.gainDb; }

// the telemetry JSON of the active mode (rx_json header + the mode's cards and tables); empty when nothing new since the last call and !force
FN(jstring, nativeTelemetry)(JNIEnv* e, jobject, jboolean force) {
    Bridge& b = *gB;
    RxTelemetry rx;
    if (!b.eng.latestRx(rx, force ? 0 : b.rxSeq)) {
        if (!force) return mk(e, "");
        return mk(e, std::string("{\"running\":") + (b.eng.running() ? "true" : "false") + ",\"available\":false,\"cards\":[],\"tables\":[]}");
    }
    b.rxSeq = rx.seq;
    if (rx.standard == 3) { dect2::DabEnsemble ens = b.eng.dabEnsemble(); return mk(e, onair::resultsJson(rx, b.eng.running(), &ens)); }
    return mk(e, onair::resultsJson(rx, b.eng.running()));
}

// "total\nline\nline..." : the log lines after the first `since` ones
FN(jstring, nativeLog)(JNIEnv* e, jobject, jlong since) {
    size_t total = 0;
    auto lines = gB->eng.logSnapshot(total);
    std::string s = std::to_string(total);
    const size_t want = total > (size_t)since ? total - (size_t)since : 0;
    const size_t from = lines.size() > want ? lines.size() - want : 0;
    for (size_t i = from; i < lines.size(); i++) s += "\n" + lines[i];
    return mk(e, s);
}
FN(void, nativeClearResults)(JNIEnv*, jobject) { gB->eng.clearResults(gB->std); }

// Verbose logging (Settings): extra diagnostic lines (USB transfer counts, stall restarts ...) into the core log
FN(void, nativeSetVerbose)(JNIEnv*, jobject, jboolean on) { dect2::setVerboseLogging(on); }

// sound: volume 0..1 and mute, for the receivers that play it
FN(void, nativeSetVolume)(JNIEnv*, jobject, jfloat v, jboolean mute) {
    Bridge& b = *gB;
    b.eng.fm().setVolume(v); b.eng.fm().setMuted(mute);
    b.eng.general().setVolume(v); b.eng.general().setMuted(mute);
}
// general receiver: demod 0 NFM 1 WFM 2 AM 3 USB 4 LSB 5 CW, filter width (0 = default), squelch (auto or level dBFS)
FN(void, nativeGeneral)(JNIEnv*, jobject, jint demod, jdouble widthHz, jboolean sqOn, jboolean sqAuto, jfloat sqDb) {
    GeneralReceiver& g = gB->eng.general();
    g.setDemod(demod);
    if (widthHz > 0) g.setWidthHz(widthHz);
    g.setSquelch(sqOn, sqAuto, sqDb);
}
FN(void, nativeFmDeemphasis)(JNIEnv*, jobject, jdouble us) { gB->eng.fm().setDeemphasis(us); }

FN(jstring, nativeVersion)(JNIEnv* e, jobject) { return mk(e, ONAIR_VERSION); }
