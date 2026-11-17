// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A fake Tektronix RSA API (RSA_API) for tests/source/test_rsa.cpp: the functions source_rsa.cpp loads, with the same names and the
// declarations of core/src/source/rsa_api_min.h. It lists RSA306 devices, keeps what was set, and plays a tone in float IQ blocks (an
// eighth of a turn per sample: +rate/8, at half the reference level). It behaves as the RSA306 that was measured (docs/TEKTRONIX-RSA.md):
// thirteen rates; a centre frequency, a reference level and a bandwidth that only count at the next run; a preamplifier that is not there
// unless "has_preamp" is set.
// fake_rsa_num / fake_rsa_set let the test look at the state and steer it.
#include "../../core/src/source/rsa_api_min.h"
#include <chrono>
#include <cmath>
#include <cstdio>
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

using namespace dect2::tekrsa;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr Status kErrorNotConnected = 101, kErrorStreamNotEnabled = 1304, kErrorConnect = 105;
constexpr int kBlock = 8192;
std::recursive_mutex mu;
std::map<std::string, double> num;   // counters and settings

double get(const std::string& k, double def = 0) { auto it = num.find(k); return it == num.end() ? def : it->second; }
void bump(const std::string& k) { num[k] = get(k) + 1; }
double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool connected = false, running = false, streaming = false, preamp = false;
double centre = 1.5e9, ref = 0, bwSet = 40e6;
double centreRun = 1.5e9, refRun = 0, bwRun = 40e6;           // what the device runs with: taken at the run, not when it is set
double startT = 0;
long long produced = 0;

std::string serialFor(int i) { return "B0" + std::to_string(10000 + i); }
// the rate for a bandwidth: 56 Msps halved until the bandwidth fits
double rateFor(double bw) {
    double r = 56e6, b = 40e6;
    for (int k = 0; k < 12 && bw <= b / 2; k++) { r /= 2; b /= 2; }
    return r;
}

} // namespace

FAKE_EXPORT double fake_rsa_num(const char* k) { std::lock_guard<std::recursive_mutex> l(mu); return get(k); }
FAKE_EXPORT void fake_rsa_set(const char* k, double v) { std::lock_guard<std::recursive_mutex> l(mu); num[k] = v; }

FAKE_EXPORT const char* DEVICE_GetErrorString(Status s) { return s == kNoError ? "No error" : "Fake error"; }
FAKE_EXPORT Status DEVICE_Search(int* found, int ids[], char serials[][kSerialLen], char types[][kTypeLen]) {
    std::lock_guard<std::recursive_mutex> l(mu);
    bump("search");
    if (connected) bump("search_while_connected");
    const int n = (int)get("count", 1);
    if (found) *found = n;
    for (int i = 0; i < n && i < kMaxDevices; i++) {
        ids[i] = i;
        snprintf(serials[i], kSerialLen, "%s", serialFor(i).c_str());
        snprintf(types[i], kTypeLen, "%s", i == 0 ? "RSA306" : "RSA507A");
    }
    return kNoError;
}
FAKE_EXPORT Status DEVICE_Connect(int id) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (get("busy") > 0 || id < 0 || id >= (int)get("count", 1)) return kErrorConnect;
    if (connected) bump("connect_twice");
    connected = true;
    running = streaming = false;
    num["connected_id"] = id;
    bump("connect");
    return kNoError;
}
FAKE_EXPORT Status DEVICE_Disconnect(void) {
    std::lock_guard<std::recursive_mutex> l(mu);
    connected = running = streaming = false;
    bump("disconnect");
    return kNoError;
}
FAKE_EXPORT Status DEVICE_GetInfo(DeviceInfo* di) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected || !di) return kErrorNotConnected;
    memset(di, 0, sizeof *di);
    snprintf(di->nomenclature, kInfoLen, "RSA306");
    snprintf(di->apiVersion, kInfoLen, "0.0.fake");
    snprintf(di->fwVersion, kInfoLen, "V1.7");
    snprintf(di->fpgaVersion, kInfoLen, "V1.1");
    snprintf(di->hwVersion, kInfoLen, "V5");
    return kNoError;
}
FAKE_EXPORT Status DEVICE_Run(void) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    if (get("fail_run") > 0) return kErrorConnect;
    running = true;
    bwRun = bwSet;
    centreRun = centre;
    refRun = ref;
    num["centre_run"] = centreRun;
    bump("run");
    return kNoError;
}
FAKE_EXPORT Status DEVICE_Stop(void) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    running = false;
    bump("stop");
    return kNoError;
}
FAKE_EXPORT Status CONFIG_SetCenterFreq(double hz) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    centre = hz;
    num["centre"] = hz;
    if (running) bump("centre_while_running");   // (the real device takes it and stays where it is)
    bump("set_centre");
    return kNoError;
}
FAKE_EXPORT Status CONFIG_SetReferenceLevel(double dbm) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    ref = dbm;
    num["ref"] = dbm;
    return kNoError;
}
FAKE_EXPORT Status CONFIG_GetReferenceLevel(double* dbm) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected || !dbm) return kErrorNotConnected;
    *dbm = ref;
    return kNoError;
}
// (an RSA306 takes the call and stays off)
FAKE_EXPORT Status CONFIG_SetRFPreampEnable(bool on) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    preamp = on && get("has_preamp") > 0;
    num["preamp"] = preamp ? 1 : 0;
    bump("set_preamp");
    return kNoError;
}
FAKE_EXPORT Status CONFIG_GetRFPreampEnable(bool* on) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected || !on) return kErrorNotConnected;
    *on = preamp;
    return kNoError;
}
FAKE_EXPORT Status IQSTREAM_SetAcqBandwidth(double hz) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    if (running) bump("bandwidth_while_running");   // (the real device then hands out garbage until it is run again)
    Status r = kNoError;
    if (hz > 40e6) { hz = 40e6; r = kErrorBandwidthOutOfRange; }
    if (hz < 40e6 / 4096) { hz = 40e6 / 4096; r = kErrorBandwidthOutOfRange; }
    bwSet = hz;
    num["bandwidth"] = hz;
    return r;
}
FAKE_EXPORT Status IQSTREAM_GetAcqParameters(double* bw, double* rate) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    if (bw) *bw = bwSet;
    if (rate) *rate = rateFor(bwSet);
    return kNoError;
}
FAKE_EXPORT Status IQSTREAM_SetOutputConfiguration(int dest, int dtype) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    num["dest"] = dest;
    num["dtype"] = dtype;
    return kNoError;
}
FAKE_EXPORT Status IQSTREAM_GetIQDataBufferSize(int* samples) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected || !samples) return kErrorNotConnected;
    *samples = kBlock;
    return kNoError;
}
FAKE_EXPORT Status IQSTREAM_Start(void) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    if (!running) { bump("stream_start_while_stopped"); return kErrorStreamNotEnabled; }
    streaming = true;
    startT = nowSec();
    produced = 0;
    bump("stream_start");
    return kNoError;
}
FAKE_EXPORT Status IQSTREAM_Stop(void) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    streaming = false;
    bump("stream_stop");
    return kNoError;
}
FAKE_EXPORT Status IQSTREAM_GetIQData(void* iq, int* samples, IqInfo* info) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!connected) return kErrorNotConnected;
    if (!streaming || !iq || !samples) return kErrorStreamNotEnabled;
    if (get("fail_after") > 0 && produced >= (long long)get("fail_after")) {   // unplugged
        num["fail_after"] = 0;
        streaming = running = false;
        bump("failed_stream");
        return kErrorNotConnected;
    }
    *samples = 0;
    if ((nowSec() - startT) * 200.0 < double(produced)) return kNoError;   // 200 blocks a second, whatever the rate
    const double amp = 0.5 * std::sqrt(0.05) * std::pow(10.0, refRun / 20.0);
    float* out = static_cast<float*>(iq);
    const double base = double(produced) * double(kBlock);
    for (int k = 0; k < kBlock; k++) {
        const double ph = 2 * kPi * std::fmod((base + double(k)) / 8.0, 1.0);
        out[2 * k] = float(amp * std::cos(ph));
        out[2 * k + 1] = float(amp * std::sin(ph));
    }
    *samples = kBlock;
    if (info) {
        memset(info, 0, sizeof *info);
        info->scaleFactor = 1;
        info->acqStatus = (uint32_t)get("status");
    }
    produced++;
    bump("blocks");
    num["run_rate"] = rateFor(bwRun);
    return kNoError;
}
