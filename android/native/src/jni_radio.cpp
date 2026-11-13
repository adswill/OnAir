// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JNI of io.github.adswill.onair.usb.RadioNative: radio access on Android. Receive only.
//  - USB radios: the app opens each one with UsbManager (permission dialog, its own UsbDeviceConnection) and hands the descriptor over here
//    (usbFdAttach); the core's drivers open it from that registry (dect2/source/usb_fd.h), never by scanning the bus.
//  - Network radios: the core's own sources (rtl_tcp, SpyServer, KiwiSDR, TCI, SDR++ server ...), described by the settings' text form.
//  - Sound card: Android's AudioRecord runs in the app and pushes frames in (dect2/source/audio_push.h).
// Threading: like the rest of the bridge these run on the receiver thread, except nativeAudioPush (the app's recording thread; the push
// buffer has its own lock and touches nothing else).
#include <jni.h>
#include <dlfcn.h>
#include <algorithm>
#include <string>
#include <vector>

#include "dect2/source/audio_push.h"
#include "dect2/source/source.h"
#include "dect2/source/usb_fd.h"
#include "dect2/net/netradio.h"
#include "dect2/net/remote_api.h"   // jsonEscape, jsonNum
#include "jni_radio.h"

using namespace dect2;

namespace {

std::string jstr(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}
// (ASCII-safe: every string here is built from USB descriptors, device names and our own texts; jsonEscape escapes the rest)
jstring mk(JNIEnv* e, const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (unsigned char c : s) t += c < 0x80 ? (char)c : '?';
    return e->NewStringUTF(t.c_str());
}
std::string q(const std::string& s) { return '"' + jsonEscape(s) + '"'; }
std::string n(double v) { return jsonNum(v); }

// The gain stages a radio has, for the device sheet (name, min, max, step in dB). The bridge itself takes one overall gain and splits it
// (HackRF: gainForTotal), so these are information and slider limits, not separate controls.
std::string stagesJson(const DeviceInfo& d) {
    auto st = [](const char* name, double lo, double hi, double step) {
        return std::string("{\"name\":") + q(name) + ",\"min\":" + n(lo) + ",\"max\":" + n(hi) + ",\"step\":" + n(step) + "}";
    };
    if (d.kind == DeviceInfo::HackRF) return "[" + st("LNA", 0, 40, 8) + "," + st("VGA", 0, 62, 2) + "," + st("Amp", 0, 14, 14) + "]";
    if (d.board == "rtlsdr") return "[" + st("Tuner", 0, 49.6, 0.1) + "]";
    if (d.board == "airspy") return "[" + st("Linearity", 0, 21, 1) + "]";
    if (d.board == "airspyhf") return "[" + st("Attenuator", 0, d.gainMaxDb > 0 ? d.gainMaxDb : 48, 6) + "]";
    if (d.board == "mirics") return "[" + st("LNA", 0, 24, 24) + "," + st("Mixer", 0, 19, 19) + "," + st("IF", 0, 59, 1) + "]";
    if (d.gainMaxDb > d.gainMinDb) return "[" + st("Gain", d.gainMinDb, d.gainMaxDb, 1) + "]";
    return "[]";
}

// One radio for the app's device list: the bridge's device JSON plus where it is, its USB key, rates, range and stages
std::string radioJson(const DeviceInfo& d, const std::string& usbName, const std::string& kind) {
    std::string base = onair::deviceJsonOf(d);   // "{...}"
    std::string rates = "[";
    for (size_t i = 0; i < d.rateRanges.size(); i++) rates += (i ? "," : "") + std::string("[") + n(d.rateRanges[i].first) + "," + n(d.rateRanges[i].second) + "]";
    rates += "]";
    const std::string extra = ",\"usbName\":" + q(usbName) + ",\"driver\":" + q(kind) + ",\"board\":" + q(d.board) + ",\"serial\":" + q(d.serial) +
                              ",\"minFreq\":" + n(d.minFreqHz) + ",\"maxFreq\":" + n(d.maxFreqHz) + ",\"rates\":" + rates +
                              ",\"stages\":" + stagesJson(d) + ",\"network\":" + (d.isNetwork() ? "true" : "false");
    return base.substr(0, base.size() - 1) + extra + "}";
}

// The drivers' entries of the radios in the registry, each paired with the registry name it came from
struct UsbEntry { DeviceInfo dev; UsbFdRadio fd; };
std::vector<UsbEntry> usbEntries(std::string& err) {
    std::vector<UsbEntry> out;
    std::vector<DeviceInfo> all;
    if (!usbFdList("hackrf").empty()) for (auto& d : listHackrfDevices(err)) all.push_back(d);
    std::string e2;
    for (auto& d : listNativeDevices(e2)) if (d.board == "rtlsdr" || d.board == "airspy" || d.board == "airspyhf" || d.board == "mirics") all.push_back(d);
    if (err.empty()) err = e2;
    const auto rtl = usbFdList("rtlsdr");
    for (const UsbFdRadio& r : usbFdList()) {
        for (const DeviceInfo& d : all) {
            bool match = false;
            if (r.kind == "hackrf") match = d.kind == DeviceInfo::HackRF && d.serial == r.serial;
            else if (r.kind == "rtlsdr") {   // the RTL driver numbers the registry's RTL radios in order
                const auto it = std::find_if(rtl.begin(), rtl.end(), [&](const UsbFdRadio& x) { return x.name == r.name; });
                match = d.board == "rtlsdr" && it != rtl.end() && d.nativeArgs == std::to_string(it - rtl.begin());
            } else if (r.kind == "airspy" || r.kind == "airspyhf")
                match = d.board == r.kind && d.nativeArgs == std::to_string((unsigned long long)usbFdAirspySerial(r.serial));
            else if (r.kind == "mirics")   // the Mirics driver names the radio by its registry name ("<name>#vid:pid")
                match = d.board == "mirics" && d.nativeArgs.rfind(r.name + "#", 0) == 0;
            if (match) { out.push_back({d, r}); break; }
        }
    }
    return out;
}

} // namespace

namespace onair {

bool radioDevice(int kind, const std::string& key, DeviceInfo& out, std::string& err) {
    if (kind == kSrcUsb) {
        for (auto& e : usbEntries(err)) if (e.fd.name == key) { out = e.dev; return true; }
        UsbFdRadio r;
        bool known = false;
        for (const auto& x : usbFdList()) if (x.name == key) { r = x; known = true; }
        if (!known) err = "The USB radio is not connected any more, or Android has not given OnAir access to it: plug it in and tap Allow";
        else if (err.empty()) err = "The " + (r.product.empty() ? std::string("USB radio") : r.product) + " is connected but its driver did not answer: "
                                    "unplug it, plug it in again, and check the log";
        return false;
    }
    if (kind == kSrcNet) {
        const auto v = netRadiosFromString(key);
        if (v.empty()) { err = "The network radio's address is not valid"; return false; }
        const auto d = netRadioDevices({v[0]});
        if (d.empty()) {
            err = v[0].type == NetRadio::SoapyRemote ? "SoapyRemote needs SoapySDR, which the Android app does not have: use rtl_tcp or SpyServer on the server instead"
                                                    : std::string("OnAir for Android cannot use this kind of network radio (") + netRadioTypeName(v[0].type) + ")";
            return false;
        }
        out = d[0];
        out.network = true;
        return true;
    }
    if (kind == kSrcSoundCard) {
        DeviceInfo d;
        d.kind = DeviceInfo::Audio;
        d.serial = "push";
        d.board = "audio";
        d.name = "Sound card (phone microphone / USB audio input)";
        d.minRateHz = 8000; d.maxRateHz = 48000; d.steadyRateHz = audioPushRate() > 0 ? audioPushRate() : 48000;
        out = d;
        return true;
    }
    err = "unknown source";
    return false;
}

} // namespace onair

#define RFN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_io_github_adswill_onair_usb_RadioNative_##name

// what this build of the library can drive: {"rtlsdr":true,"hackrf":bool,"airspy":bool,"airspyhf":bool,"mirics":bool}
RFN(jstring, nativeUsbFeatures)(JNIEnv* e, jobject) {
#ifdef ONAIR_REAL_LIBUSB
    const bool lu = true;
#else
    const bool lu = false;
#endif
    // the Airspy libraries are loaded at run time by the core: present when the app ships them (native/CMakeLists.txt AIRSPY_SRC)
    auto has = [](const char* lib) { void* h = dlopen(lib, RTLD_LAZY | RTLD_LOCAL); if (h) dlclose(h); return h != nullptr; };
    const bool as = lu && has("libairspy.so"), ahf = lu && has("libairspyhf.so");
    std::string s = std::string("{\"rtlsdr\":true,\"hackrf\":") + (lu ? "true" : "false") + ",\"airspy\":" + (as ? "true" : "false") +
                    ",\"airspyhf\":" + (ahf ? "true" : "false") + ",\"mirics\":" + (lu ? "true" : "false") + "}";
    return mk(e, s);
}

RFN(jboolean, nativeUsbAttach)(JNIEnv* e, jobject, jstring name, jstring kind, jint fd, jint vid, jint pid, jstring manufacturer, jstring product, jstring serial) {
    UsbFdRadio r;
    r.name = jstr(e, name); r.kind = jstr(e, kind); r.fd = fd; r.vid = vid; r.pid = pid;
    r.manufacturer = jstr(e, manufacturer); r.product = jstr(e, product); r.serial = jstr(e, serial);
    if (r.kind.empty()) r.kind = usbFdKindForId(vid, pid);
    if (r.kind.empty()) return JNI_FALSE;
    return usbFdAttach(r) ? JNI_TRUE : JNI_FALSE;
}

RFN(void, nativeUsbDetach)(JNIEnv* e, jobject, jstring name) { usbFdDetach(jstr(e, name)); }

// {"radios":[radio JSON...],"error":"..."}: the drivers' view of the attached radios (opens each for a moment: not while it streams)
RFN(jstring, nativeUsbRadios)(JNIEnv* e, jobject) {
    std::string err;
    std::string s = "{\"radios\":[";
    bool first = true;
    for (auto& x : usbEntries(err)) { s += (first ? "" : ",") + radioJson(x.dev, x.fd.name, x.fd.kind); first = false; }
    return mk(e, s + "],\"error\":" + q(err) + "}");
}

// a network radio ("rtltcp,host,port"): its radio JSON, or {"error":"..."}
RFN(jstring, nativeNetDevice)(JNIEnv* e, jobject, jstring spec) {
    DeviceInfo d;
    std::string err;
    if (!onair::radioDevice(onair::kSrcNet, jstr(e, spec), d, err)) return mk(e, "{\"error\":" + q(err) + "}");
    return mk(e, radioJson(d, "", "net"));
}

// "" when the radio answers within timeoutMs, else why not (plain words). Blocks: the receiver thread only.
RFN(jstring, nativeNetProbe)(JNIEnv* e, jobject, jstring spec, jint timeoutMs) {
    const auto v = netRadiosFromString(jstr(e, spec));
    if (v.empty()) return mk(e, "The address is not valid");
    std::string err;
    return mk(e, probeNetRadio(v[0], timeoutMs > 0 ? timeoutMs : 8000, err) ? "" : (err.empty() ? "The radio did not answer" : err));
}

// the settings' text form of one radio (the core's own format, so the app never writes it by hand)
RFN(jstring, nativeNetSpec)(JNIEnv* e, jobject, jint type, jstring host, jint port, jstring remote) {
    NetRadio r;
    r.type = (NetRadio::Type)type; r.host = jstr(e, host); r.port = port; r.remote = jstr(e, remote);
    return mk(e, netRadiosToString({r}));
}

RFN(void, nativeAudioConfigure)(JNIEnv*, jobject, jint rate, jdouble seconds) { audioPushConfigure(rate, seconds); }

// interleaved stereo float frames from AudioRecord; returns the frames kept. Called on the app's recording thread.
RFN(jint, nativeAudioPush)(JNIEnv* e, jobject, jfloatArray samples, jint frames) {
    if (!samples || frames <= 0) return 0;
    const jsize len = e->GetArrayLength(samples);
    const jint nf = std::min<jint>(frames, len / 2);
    std::vector<float> tmp((size_t)nf * 2);
    e->GetFloatArrayRegion(samples, 0, nf * 2, tmp.data());
    return (jint)audioPushWrite(tmp.data(), (size_t)nf);
}
