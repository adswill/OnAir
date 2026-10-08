// A fake "vendor library" that exports the functions of librtlsdr, libairspy, libbladeRF, LimeSuite, libiio, UHD and the SDRplay API that OnAir uses, and
// streams a known tone. The test copies this one file under the seven real library names. It checks OnAir's driver code (what it asks the
// libraries to do and how it converts the samples), not the real libraries.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

static std::mutex gMu;
static std::map<std::string, double> gState;
static void rec(const char* k, double v) { std::lock_guard<std::mutex> lk(gMu); gState[k] = v; }
EXPORT double fake_state(const char* k) { std::lock_guard<std::mutex> lk(gMu); auto it = gState.find(k); return it == gState.end() ? -1e300 : it->second; }

static void pace() { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }

// ---- native-robust: the test sets state through this. "rtl.gone" = 1: the RTL-SDR is unplugged (the stream ends, opens fail with
// LIBUSB_ERROR_NO_DEVICE); "rtl.busyOpens" = n: the next n opens fail with LIBUSB_ERROR_BUSY; "rtl.opens" counts the opens
EXPORT void fake_set(const char* k, double v) { rec(k, v); }
static int rtlOpenRefusal() {
    rec("rtl.opens", std::max(0.0, fake_state("rtl.opens")) + 1);
    if (fake_state("rtl.gone") == 1) return -4;
    const double busy = fake_state("rtl.busyOpens");
    if (busy > 0) { rec("rtl.busyOpens", busy - 1); return -6; }
    return 0;
}
// ---- end native-robust
// the test tone: I = 0.5 (constant), Q = -0.25
static const float kI = 0.5f, kQ = -0.25f;

// ------------------------------------------------------------------ RTL-SDR
struct rtl_dev { std::atomic<bool> cancel{false}; };
EXPORT uint32_t rtlsdr_get_device_count(void) { return 1; }
EXPORT const char* rtlsdr_get_device_name(uint32_t) { return "Generic RTL2832U"; }
EXPORT int rtlsdr_get_device_usb_strings(uint32_t, char* m, char* p, char* s) {   // ---- radios: rtl.v4 = an RTL-SDR Blog V4, rtl.v3 = a Blog V3, rtl.v4l = a Blog V4 Lite
    const bool v4 = fake_state("rtl.v4") == 1, v3 = fake_state("rtl.v3") == 1, v4l = fake_state("rtl.v4l") == 1;
    strcpy(m, v4 || v3 || v4l ? "RTLSDRBlog" : "Realtek"); strcpy(p, v4 ? "Blog V4" : v3 ? "Blog V3" : v4l ? "Blog V4L" : "RTL2838UHIDIR"); strcpy(s, "00000001"); return 0;
}
EXPORT int rtlsdr_open(rtl_dev** d, uint32_t) { if (const int r = rtlOpenRefusal()) return r; if (getenv("FAKE_RTL_OPEN_FAIL")) return -6; *d = new rtl_dev; return 0; }   // ---- start failure: the app shows the error of a failed start
EXPORT int rtlsdr_close(rtl_dev* d) { delete d; return 0; }
EXPORT int rtlsdr_set_center_freq(rtl_dev*, uint32_t f) { rec("rtl.freq", f); return 0; }
EXPORT int rtlsdr_set_sample_rate(rtl_dev*, uint32_t r) { rec("rtl.rate", r); return 0; }
EXPORT uint32_t rtlsdr_get_sample_rate(rtl_dev*) { return (uint32_t)fake_state("rtl.rate"); }
EXPORT int rtlsdr_set_tuner_gain_mode(rtl_dev*, int m) { rec("rtl.gainmode", m); return 0; }
EXPORT int rtlsdr_get_tuner_gains(rtl_dev*, int* g) { static const int v[] = {0, 90, 140, 270, 370, 496}; if (g) memcpy(g, v, sizeof v); return 6; }
EXPORT int rtlsdr_set_tuner_gain(rtl_dev*, int g) { rec("rtl.gain", g); return 0; }
EXPORT int rtlsdr_set_agc_mode(rtl_dev*, int) { return 0; }
EXPORT int rtlsdr_reset_buffer(rtl_dev*) { return 0; }
EXPORT int rtlsdr_read_async(rtl_dev* d, void (*cb)(unsigned char*, uint32_t, void*), void* ctx, uint32_t, uint32_t) {
    std::vector<unsigned char> b(16384);
    for (size_t i = 0; i + 1 < b.size(); i += 2) { b[i] = (unsigned char)lround(127.4 + 128 * kI); b[i + 1] = (unsigned char)lround(127.4 + 128 * kQ); }
    d->cancel = false;
    while (!d->cancel) { if (fake_state("rtl.gone") == 1) return -5; cb(b.data(), (uint32_t)b.size(), ctx); pace(); }   // unplugged: librtlsdr ends the stream
    return 0;
}
EXPORT int rtlsdr_cancel_async(rtl_dev* d) { d->cancel = true; return 0; }
// ---- radio settings: frequency correction and offset tuning (counted, so that the test sees a default that calls nothing)
static void count(const char* k) { const double v = fake_state(k); rec(k, v < 0 ? 1 : v + 1); }
EXPORT int rtlsdr_set_freq_correction(rtl_dev*, int ppm) { rec("rtl.ppm", ppm); count("rtl.ppmCalls"); return 0; }
EXPORT int rtlsdr_set_offset_tuning(rtl_dev*, int on) { rec("rtl.offset", on); count("rtl.offsetCalls"); return 0; }
// (OnAir reads block by block on macOS, see RtlSource::streamLoop)
EXPORT int rtlsdr_read_sync(rtl_dev*, void* buf, int len, int* n) {
    if (fake_state("rtl.gone") == 1) return -4;   // LIBUSB_ERROR_NO_DEVICE
    auto* b = static_cast<unsigned char*>(buf);
    const int m = std::min(len, 16384) & ~1;
    for (int i = 0; i + 1 < m; i += 2) { b[i] = (unsigned char)lround(127.4 + 128 * kI); b[i + 1] = (unsigned char)lround(127.4 + 128 * kQ); }
    count("rtl.syncReads");
    pace();
    *n = m;
    return 0;
}

// ------------------------------------------------------------------ Airspy
struct airspy_transfer_t { void* device; void* ctx; void* samples; int sample_count; uint64_t dropped_samples; int sample_type; };
struct as_dev { std::atomic<bool> run{false}; std::thread th; };
EXPORT int airspy_init(void) { return 0; }
EXPORT int airspy_exit(void) { return 0; }
EXPORT int airspy_list_devices(uint64_t* s, int) { s[0] = 0x1234567890ABCDEFull; return 1; }
EXPORT int airspy_open_sn(as_dev** d, uint64_t sn) { rec("airspy.serial", (double)(sn & 0xFFFFFFFF)); if (fake_state("airspy.gone") == 1) return -1000; *d = new as_dev; return 0; }   // airspy.gone = unplugged
EXPORT int airspy_open(as_dev** d) { *d = new as_dev; return 0; }
EXPORT int airspy_close(as_dev* d) { if (d->run) { d->run = false; if (d->th.joinable()) d->th.join(); } delete d; return 0; }
EXPORT int airspy_get_samplerates(as_dev*, uint32_t* b, uint32_t len) {   // airspy.mini: a Mini (6 / 3 Msps)
    const bool mini = fake_state("airspy.mini") == 1;
    if (len == 0) { b[0] = 2; return 0; }
    b[0] = mini ? 6000000 : 10000000; if (len > 1) b[1] = mini ? 3000000 : 2500000; return 0;
}
EXPORT int airspy_version_string_read(as_dev*, char* v, uint8_t len) { snprintf(v, len, "%s", fake_state("airspy.mini") == 1 ? "AirSpy MINI v1.0.0-rc10-6-g4008185 2020-05-08" : "AirSpy NOS v1.0.0-rc10-6-g4008185 2020-05-08"); return 0; }
EXPORT int airspy_set_samplerate(as_dev*, uint32_t r) { rec("airspy.rate", r); return 0; }
EXPORT int airspy_set_sample_type(as_dev*, int t) { rec("airspy.type", t); return 0; }
EXPORT int airspy_set_freq(as_dev*, uint32_t f) { rec("airspy.freq", f); return 0; }
EXPORT int airspy_set_linearity_gain(as_dev*, uint8_t g) { rec("airspy.gain", g); rec("airspy.table", 0); return 0; }
EXPORT int airspy_set_sensitivity_gain(as_dev*, uint8_t g) { rec("airspy.gain", g); rec("airspy.table", 1); return 0; }   // ---- radio settings
EXPORT int airspy_start_rx(as_dev* d, int (*cb)(airspy_transfer_t*), void* ctx) {
    d->run = true;
    d->th = std::thread([d, cb, ctx] {
        std::vector<float> s(16384 * 2);
        for (size_t i = 0; i < 16384; i++) { s[2 * i] = kI; s[2 * i + 1] = kQ; }
        airspy_transfer_t t{d, ctx, s.data(), 16384, 0, 0};
        while (d->run && fake_state("airspy.gone") != 1) { cb(&t); pace(); }   // unplugged: libairspy stops streaming
    });
    return 0;
}
EXPORT int airspy_stop_rx(as_dev* d) { d->run = false; if (d->th.joinable()) d->th.join(); return 0; }
EXPORT int airspy_is_streaming(as_dev* d) { return d->run && fake_state("airspy.gone") != 1 ? 1 : 0; }

// ------------------------------------------------------------------ bladeRF
struct bladerf_devinfo_t { int backend; char serial[33]; uint8_t usb_bus, usb_addr; unsigned int instance; char manufacturer[33]; char product[33]; };
struct bladerf_range_t { int64_t min, max, step; float scale; };
struct bl_dev { int dummy; };
EXPORT int bladerf_get_device_list(bladerf_devinfo_t** l) { auto* a = (bladerf_devinfo_t*)calloc(1, sizeof(bladerf_devinfo_t)); strcpy(a->serial, "0123456789ABCDEF0123456789ABCDEF"); strcpy(a->product, fake_state("blade.v1") == 1 ? "bladeRF" : "bladeRF 2.0 micro"); *l = a; return 1; }   // ---- radios: blade.v1 = a bladeRF 1
EXPORT void bladerf_free_device_list(bladerf_devinfo_t* l) { free(l); }
EXPORT int bladerf_open(bl_dev** d, const char* id) { rec("blade.openSerialGiven", id ? 1 : 0); const double oe = fake_state("blade.openErr"); if (oe < 0 && oe > -100) return (int)oe; rec("blade.fpgaLoaded", 0); *d = new bl_dev; return 0; }   // ---- radios: blade.openErr = the code bladerf_open fails with
EXPORT void bladerf_close(bl_dev* d) { delete d; }
EXPORT int bladerf_set_frequency(bl_dev*, int ch, uint64_t f) { rec("blade.freq", (double)f); rec("blade.freqCh", ch); return 0; }
EXPORT int bladerf_set_sample_rate(bl_dev*, int, unsigned r, unsigned* actual) { rec("blade.rate", r); *actual = r; return 0; }
EXPORT int bladerf_set_bandwidth(bl_dev*, int, unsigned b, unsigned* actual) { rec("blade.bw", b); *actual = b; return 0; }
EXPORT int bladerf_set_gain(bl_dev*, int ch, int g) { rec("blade.gain", g); rec("blade.gainCh", ch); return 0; }
EXPORT int bladerf_set_gain_mode(bl_dev*, int, int m) { rec("blade.gainmode", m); return 0; }
EXPORT int bladerf_get_gain_range(bl_dev*, int, const bladerf_range_t** r) { static const bladerf_range_t g{-15, 60, 1, 1.f}; *r = &g; return 0; }
EXPORT int bladerf_enable_module(bl_dev*, int ch, bool e) { rec("blade.enabled", e ? 1 : 0); rec("blade.enabledCh", ch); return 0; }
EXPORT int bladerf_sync_config(bl_dev*, int layout, int fmt, unsigned, unsigned, unsigned, unsigned) { rec("blade.layout", layout); rec("blade.format", fmt); return 0; }
EXPORT int bladerf_sync_rx(bl_dev*, void* s, unsigned n, void*, unsigned) {
    int16_t* p = (int16_t*)s;
    for (unsigned i = 0; i < n; i++) { p[2 * i] = (int16_t)lround(kI * 2048); p[2 * i + 1] = (int16_t)lround(kQ * 2048); }
    pace(); return 0;
}

// ---- bias-tee: what the drivers ask for; built with -DFAKE_NO_BIAS the functions are missing, as in a library that is too old
#ifndef FAKE_NO_BIAS
EXPORT int rtlsdr_set_bias_tee(rtl_dev*, int on) { rec("rtl.bias", on); rec("rtl.biasCalls", std::max(0.0, fake_state("rtl.biasCalls")) + 1); return 0; }
EXPORT int airspy_set_rf_bias(as_dev*, uint8_t on) { rec("airspy.bias", on); return 0; }
EXPORT int bladerf_set_bias_tee(bl_dev*, int ch, bool on) { rec("blade.bias", on ? 1 : 0); rec("blade.biasCh", ch); return 0; }
#endif
// ---- end bias-tee

// ------------------------------------------------------------------ LimeSuite
struct lms_range_t { double min, max, step; };
struct lms_stream_t { size_t handle; bool isTx; uint32_t channel; uint32_t fifoSize; float throughputVsLatency; int dataFmt; int linkFmt; };
struct lms_meta_t { uint64_t timestamp; bool waitForTimestamp; bool flushPartialPacket; };
EXPORT int LMS_GetDeviceList(char (*l)[256]) { if (l) strcpy(l[0], "LimeSDR-USB, media=USB 3.0, module=FX3, addr=1d50:6108, serial=0009060B00000001"); return 1; }
EXPORT int LMS_Open(void** d, const char* info, void*) { rec("lime.openInfoOk", info && strstr(info, "LimeSDR") ? 1 : 0); *d = new int(1); return 0; }
EXPORT int LMS_Close(void* d) { delete (int*)d; return 0; }
EXPORT int LMS_Init(void*) { return 0; }
EXPORT int LMS_EnableChannel(void*, bool tx, size_t ch, bool e) { rec("lime.chanEnabled", (!tx && ch == 0 && e) ? 1 : 0); return 0; }
EXPORT int LMS_SetSampleRate(void*, double r, size_t) { rec("lime.rate", r); return 0; }
EXPORT int LMS_GetSampleRateRange(void*, bool, lms_range_t* r) { r->min = 100e3; r->max = 61.44e6; r->step = 0; return 0; }
EXPORT int LMS_SetLOFrequency(void*, bool, size_t, double f) { rec("lime.freq", f); return 0; }
EXPORT int LMS_GetAntennaList(void*, bool, size_t, char (*l)[16]) { strcpy(l[0], "NONE"); strcpy(l[1], "LNAH"); strcpy(l[2], "LNAL"); strcpy(l[3], "LNAW"); return 4; }
EXPORT int LMS_SetAntenna(void*, bool, size_t, size_t i) { rec("lime.antenna", (double)i); return 0; }
EXPORT int LMS_SetLPFBW(void*, bool, size_t, double b) { rec("lime.bw", b); return 0; }
EXPORT int LMS_SetGaindB(void*, bool, size_t, unsigned g) { rec("lime.gain", g); return 0; }
EXPORT int LMS_Calibrate(void*, bool, size_t, double, unsigned) { rec("lime.calibrated", 1); rec("lime.calibrations", std::max(0.0, fake_state("lime.calibrations")) + 1); return 0; }   // ---- radios: counted
EXPORT int LMS_SetupStream(void*, lms_stream_t* s) { rec("lime.fmt", s->dataFmt); s->handle = 7; return 0; }
EXPORT int LMS_DestroyStream(void*, lms_stream_t*) { return 0; }
EXPORT int LMS_StartStream(lms_stream_t*) { rec("lime.streaming", 1); return 0; }
EXPORT int LMS_StopStream(lms_stream_t*) { rec("lime.streaming", 0); return 0; }
EXPORT int LMS_RecvStream(lms_stream_t*, void* s, size_t n, lms_meta_t*, unsigned) {
    float* p = (float*)s;
    for (size_t i = 0; i < n; i++) { p[2 * i] = kI; p[2 * i + 1] = kQ; }
    pace(); return (int)n;
}

// ------------------------------------------------------------------ libiio (PlutoSDR)
struct iio_chan { std::string name; bool out; };
EXPORT void* iio_create_scan_context(const char*, unsigned) { return new int(1); }
EXPORT void iio_scan_context_destroy(void* c) { delete (int*)c; }
EXPORT long long iio_scan_context_get_info_list(void*, void*** info) { *info = (void**)calloc(2, sizeof(void*)); (*info)[0] = (void*)"x"; return 1; }
EXPORT void iio_context_info_list_free(void** i) { free(i); }
EXPORT const char* iio_context_info_get_description(const void*) { return "0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO)), serial=1044"; }
EXPORT const char* iio_context_info_get_uri(const void*) { return "usb:1.2.5"; }
EXPORT void* iio_create_context_from_uri(const char* uri) { rec("pluto.connected", strstr(uri, "usb:") ? 1 : 0); return new int(1); }
EXPORT void iio_context_destroy(void* c) { delete (int*)c; }
EXPORT void* iio_context_find_device(const void*, const char* name) { return strcmp(name, "ad9361-phy") == 0 ? (void*)"phy" : strcmp(name, "cf-ad9361-lpc") == 0 ? (void*)"rx" : nullptr; }
EXPORT int iio_context_set_timeout(void*, unsigned) { return 0; }
EXPORT void* iio_device_find_channel(const void* dev, const char* name, bool out) { auto* c = new iio_chan{std::string((const char*)dev) + ":" + name, out}; return c; }
EXPORT long long iio_channel_attr_write(const void* c, const char* a, const char* v) { rec(("pluto." + ((iio_chan*)c)->name + "." + a + "=" + v).c_str(), 1); return (long long)strlen(v); }
EXPORT int iio_channel_attr_write_longlong(const void* c, const char* a, long long v) { rec(("pluto." + ((iio_chan*)c)->name + "." + a).c_str(), (double)v); return 0; }
EXPORT void iio_channel_enable(void*) {}
EXPORT void iio_channel_disable(void*) {}
struct iio_buf { std::vector<int16_t> d; std::atomic<bool> cancel{false}; };
EXPORT void* iio_device_create_buffer(const void*, size_t n, bool) { auto* b = new iio_buf; b->d.resize(n * 2); for (size_t i = 0; i < n; i++) { b->d[2 * i] = (int16_t)lround(kI * 2048); b->d[2 * i + 1] = (int16_t)lround(kQ * 2048); } return b; }
EXPORT void iio_buffer_destroy(void* b) { delete (iio_buf*)b; }
EXPORT long long iio_buffer_refill(void* b) { auto* x = (iio_buf*)b; if (x->cancel) return -1; pace(); return (long long)(x->d.size() / 2 * 4); }
EXPORT void iio_buffer_cancel(void* b) { ((iio_buf*)b)->cancel = true; }
EXPORT void* iio_buffer_start(const void* b) { return ((iio_buf*)b)->d.data(); }
EXPORT void* iio_buffer_end(const void* b) { auto* x = (iio_buf*)b; return x->d.data() + x->d.size(); }

// ------------------------------------------------------------------ UHD (USRP)
struct uhd_tune_request_t { double target_freq; int rf_freq_policy; double rf_freq; int dsp_freq_policy; double dsp_freq; char* args; };
struct uhd_tune_result_t { double clipped_rf_freq, target_rf_freq, actual_rf_freq, target_dsp_freq, actual_dsp_freq; };
struct uhd_stream_args_t { char* cpu_format; char* otw_format; char* args; size_t* channel_list; int n_channels; };
struct uhd_stream_cmd_t { int stream_mode; size_t num_samps; bool stream_now; int64_t time_spec_full_secs; double time_spec_frac_secs; };
struct strvec { std::vector<std::string> v; };
EXPORT int uhd_string_vector_make(strvec** h) { *h = new strvec; return 0; }
EXPORT int uhd_string_vector_free(strvec** h) { delete *h; *h = nullptr; return 0; }
EXPORT int uhd_string_vector_size(strvec* h, size_t* n) { *n = h->v.size(); return 0; }
EXPORT int uhd_string_vector_at(strvec* h, size_t i, char* out, size_t len) { snprintf(out, len, "%s", h->v[i].c_str()); return 0; }
EXPORT int uhd_usrp_find(const char*, strvec** h) { (*h)->v.push_back(fake_state("usrp.b210") == 1 ? "type=b200,name=,serial=30C6E4D,product=B210" : "type=b200,name=,serial=30C6E4D,product=B200"); return 0; }   // ---- radios: usrp.b210 = a B210
EXPORT int uhd_usrp_make(void** h, const char* args) { rec("usrp.argsOk", args && strstr(args, "b200") ? 1 : 0); *h = new int(1); return 0; }
EXPORT int uhd_usrp_free(void** h) { delete (int*)*h; *h = nullptr; return 0; }
EXPORT int uhd_usrp_set_rx_rate(void*, double r, size_t) { rec("usrp.rate", r); return 0; }
EXPORT int uhd_usrp_get_rx_rate(void*, size_t, double* r) { *r = fake_state("usrp.rate"); return 0; }
struct uhd_range { double lo, hi; };
EXPORT int uhd_meta_range_make(uhd_range** r) { *r = new uhd_range{0, 0}; return 0; }
EXPORT int uhd_meta_range_free(uhd_range** r) { delete *r; *r = nullptr; return 0; }
EXPORT int uhd_meta_range_start(uhd_range* r, double* v) { *v = r->lo; return 0; }
EXPORT int uhd_meta_range_stop(uhd_range* r, double* v) { *v = r->hi; return 0; }
EXPORT int uhd_usrp_get_rx_rates(void*, size_t, uhd_range* r) { r->lo = 200e3; r->hi = 56e6; return 0; }
EXPORT int uhd_usrp_get_rx_gain_range(void*, const char*, size_t, uhd_range* r) { r->lo = 0; r->hi = 76; return 0; }
EXPORT int uhd_usrp_set_rx_freq(void*, uhd_tune_request_t* q, size_t ch, uhd_tune_result_t* res) { rec("usrp.freqCh", (double)ch); rec("usrp.freq", q->target_freq); rec("usrp.policy", q->rf_freq_policy); res->actual_rf_freq = q->target_freq; return 0; }
EXPORT int uhd_usrp_set_rx_gain(void*, double g, size_t, const char*) { rec("usrp.gain", g); return 0; }
EXPORT int uhd_usrp_set_rx_bandwidth(void*, double b, size_t) { rec("usrp.bw", b); return 0; }
EXPORT int uhd_usrp_get_rx_stream(void*, uhd_stream_args_t* a, void*) { rec("usrp.cpuFmtFc32", strcmp(a->cpu_format, "fc32") == 0 ? 1 : 0); rec("usrp.streamCh", a->n_channels == 1 ? (double)a->channel_list[0] : -1); return 0; }
EXPORT int uhd_rx_streamer_make(void** h) { *h = new int(1); return 0; }
EXPORT int uhd_rx_streamer_free(void** h) { delete (int*)*h; *h = nullptr; return 0; }
EXPORT int uhd_rx_streamer_max_num_samps(void*, size_t* n) { *n = 4096; return 0; }
EXPORT int uhd_rx_streamer_issue_stream_cmd(void*, const uhd_stream_cmd_t* c) { rec("usrp.cmd", c->stream_mode); return 0; }
EXPORT int uhd_rx_metadata_make(void** h) { *h = new int(0); return 0; }
EXPORT int uhd_rx_metadata_free(void** h) { delete (int*)*h; *h = nullptr; return 0; }
EXPORT int uhd_rx_metadata_error_code(void*, int* c) { *c = 0; return 0; }
EXPORT int uhd_rx_streamer_recv(void*, void** bufs, size_t n, void**, double, bool, size_t* got) {
    float* p = (float*)bufs[0];
    for (size_t i = 0; i < n; i++) { p[2 * i] = kI; p[2 * i + 1] = kQ; }
    *got = n; pace(); return 0;
}

// ------------------------------------------------------------------ SDRplay API 3.x (an RSP1B; the structs come from OnAir's own copy of
// the API's layouts, which sdrplay_api_min.h checks against the official headers' sizes)
#include "../core/src/sdrplay_api_min.h"
namespace sp = dect2::sdrplay;
namespace fakesdrplay {
static sp::DevParamsT devParams;
static sp::RxChannelParamsT chA;
static sp::DeviceParamsT params{&devParams, &chA, nullptr};
static int handle = 1;
static std::atomic<bool> run{false};
static std::atomic<bool> locked{false};
static std::thread th;
static int inits = 0, acks = 0;
static void defaults() {   // the API's documented defaults
    memset(&devParams, 0, sizeof devParams);
    memset(&chA, 0, sizeof chA);
    devParams.fsFreq.fsHz = 2e6;
    chA.tunerParams.bwType = sp::BW_0_200; chA.tunerParams.ifType = sp::IF_Zero; chA.tunerParams.loMode = sp::LO_Auto;
    chA.tunerParams.gain.gRdB = 50; chA.tunerParams.gain.minGr = sp::NORMAL_MIN_GR; chA.tunerParams.rfFreq.rfHz = 200e6;
    chA.ctrlParams.dcOffset.DCenable = 1; chA.ctrlParams.dcOffset.IQenable = 1; chA.ctrlParams.decimation.decimationFactor = 1;
    chA.ctrlParams.agc.enable = sp::AGC_50HZ;
}
static void record() {
    rec("sdrplay.fs", devParams.fsFreq.fsHz);
    rec("sdrplay.rf", chA.tunerParams.rfFreq.rfHz);
    rec("sdrplay.bw", chA.tunerParams.bwType);
    rec("sdrplay.if", chA.tunerParams.ifType);
    rec("sdrplay.lna", chA.tunerParams.gain.LNAstate);
    rec("sdrplay.grdb", chA.tunerParams.gain.gRdB);
    rec("sdrplay.agc", chA.ctrlParams.agc.enable);
    rec("sdrplay.dc", chA.ctrlParams.dcOffset.DCenable);
    rec("sdrplay.iq", chA.ctrlParams.dcOffset.IQenable);
    rec("sdrplay.bias", chA.rsp1aTunerParams.biasTEnable);   // ---- bias-tee
    rec("sdrplay.decim", chA.ctrlParams.decimation.enable ? chA.ctrlParams.decimation.decimationFactor : 0);
    // ---- radio settings: frequency correction, notch filters, HDR, the Hi-Z inputs
    rec("sdrplay.ppm", devParams.ppm);
    rec("sdrplay.rfNotch1a", devParams.rsp1aParams.rfNotchEnable); rec("sdrplay.dabNotch1a", devParams.rsp1aParams.rfDabNotchEnable);
    rec("sdrplay.rfNotch2", chA.rsp2TunerParams.rfNotchEnable); rec("sdrplay.amPort2", chA.rsp2TunerParams.amPortSel);
    rec("sdrplay.rfNotchDuo", chA.rspDuoTunerParams.rfNotchEnable); rec("sdrplay.amNotchDuo", chA.rspDuoTunerParams.tuner1AmNotchEnable);
    rec("sdrplay.dabNotchDuo", chA.rspDuoTunerParams.rfDabNotchEnable); rec("sdrplay.amPortDuo", chA.rspDuoTunerParams.tuner1AmPortSel);
    rec("sdrplay.rfNotchDx", devParams.rspDxParams.rfNotchEnable); rec("sdrplay.dabNotchDx", devParams.rspDxParams.rfDabNotchEnable);
    rec("sdrplay.hdr", devParams.rspDxParams.hdrEnable);
}
}   // namespace fakesdrplay
EXPORT sp::ErrT sdrplay_api_Open(void) { fakesdrplay::defaults(); rec("sdrplay.open", 1); return sp::Success; }
EXPORT sp::ErrT sdrplay_api_Close(void) { rec("sdrplay.open", 0); return sp::Success; }
EXPORT sp::ErrT sdrplay_api_ApiVersion(float* v) { *v = 3.15f; return sp::Success; }
EXPORT sp::ErrT sdrplay_api_LockDeviceApi(void) { fakesdrplay::locked = true; return sp::Success; }
EXPORT sp::ErrT sdrplay_api_UnlockDeviceApi(void) { fakesdrplay::locked = false; return sp::Success; }
EXPORT const char* sdrplay_api_GetErrorString(sp::ErrT) { return "fake error"; }
EXPORT sp::ErrT sdrplay_api_GetDevices(sp::DeviceT* d, unsigned int* n, unsigned int max) {
    rec("sdrplay.listLocked", fakesdrplay::locked ? 1 : 0);
    *n = 0;
    if (fake_state("sdrplay.selected") == 1 || max < 1) return sp::Success;   // a selected radio is not listed
    memset(d, 0, sizeof *d);
    strcpy(d->SerNo, "2305012345");
    const double model = fake_state("sdrplay.model");   // ---- radio settings: another model (2 RSP2, 3 RSPduo, 4 RSPdx)
    d->hwVer = model > 0 ? (unsigned char)model : sp::kRsp1B; d->tuner = sp::Tuner_A; d->rspDuoMode = d->hwVer == sp::kRspDuo ? sp::RspDuoMode_Single_Tuner : sp::RspDuoMode_Unknown; d->valid = 1;
    *n = 1;
    return sp::Success;
}
EXPORT sp::ErrT sdrplay_api_SelectDevice(sp::DeviceT* d) {
    if (strcmp(d->SerNo, "2305012345") != 0 || !fakesdrplay::locked) return sp::Fail;
    d->dev = &fakesdrplay::handle;
    fakesdrplay::defaults();   // a radio is selected with the API's defaults
    rec("sdrplay.selected", 1);
    return sp::Success;
}
EXPORT sp::ErrT sdrplay_api_ReleaseDevice(sp::DeviceT* d) { rec("sdrplay.selected", d->dev == &fakesdrplay::handle ? 0 : -1); return sp::Success; }
EXPORT sp::ErrT sdrplay_api_GetDeviceParams(void* dev, sp::DeviceParamsT** p) { if (dev != &fakesdrplay::handle) return sp::NotInitialised; *p = &fakesdrplay::params; return sp::Success; }
EXPORT sp::ErrT sdrplay_api_Init(void* dev, sp::CallbackFnsT* cb, void* ctx) {
    using namespace fakesdrplay;
    if (dev != &handle || !cb || !cb->StreamACbFn || !cb->EventCbFn) return sp::InvalidParam;
    if (run) return sp::AlreadyInitialised;
    record();
    rec("sdrplay.inits", ++inits);
    rec("sdrplay.init", 1);
    run = true;
    const sp::CallbackFnsT fns = *cb;
    th = std::thread([fns, ctx] {
        sp::EventParamsT ev;
        memset(&ev, 0, sizeof ev);
        ev.powerOverloadParams.powerOverloadChangeType = sp::Overload_Detected;
        fns.EventCbFn(sp::PowerOverloadChange, sp::Tuner_A, &ev, ctx);   // one overload report: the driver must acknowledge it
        std::vector<short> xi(2016, (short)lround(kI * 32768)), xq(2016, (short)lround(kQ * 32768));
        sp::StreamCbParamsT p;
        memset(&p, 0, sizeof p);
        unsigned int reset = 1;
        while (run) {
            for (int k = 0; k < 8 && run; k++) { p.numSamples = 2016; fns.StreamACbFn(xi.data(), xq.data(), &p, 2016, reset, ctx); reset = 0; p.firstSampleNum += 2016; }
            pace();
        }
    });
    return sp::Success;
}
EXPORT sp::ErrT sdrplay_api_Uninit(void* dev) {
    using namespace fakesdrplay;
    if (dev != &handle) return sp::NotInitialised;
    run = false;
    if (th.joinable()) th.join();
    rec("sdrplay.init", 0);
    return sp::Success;
}
EXPORT sp::ErrT sdrplay_api_Update(void* dev, sp::TunerSelectT tuner, sp::ReasonForUpdateT why, sp::ReasonForUpdateExtension1T why1) {
    using namespace fakesdrplay;
    if (dev != &handle) return sp::NotInitialised;
    if (!run) return sp::NotInitialised;
    rec("sdrplay.updTuner", tuner);
    if (why & sp::Update_Ctrl_OverloadMsgAck) rec("sdrplay.overloadAcks", ++acks);
    if (why & ~(unsigned)sp::Update_Ctrl_OverloadMsgAck) { rec("sdrplay.reasons", (double)why); record(); }
    if (why1) { rec("sdrplay.reasons1", (double)why1); record(); }
    return sp::Success;
}

// ---- airspyhf: libairspyhf 1.6.8 (a standard HF+: the Discovery would add 912 kHz). The stream is a tone at 1/8 of the sample rate with amplitude 0.5,
// so the test can read the sample rate back from the samples.
struct ahf_transfer_t { void* device; void* ctx; float* samples; int sample_count; uint64_t dropped_samples; };
static void ahfCount(const char* k) { const double v = fake_state(k); rec(k, v < 0 ? 1 : v + 1); }
struct ahf_dev { std::atomic<bool> run{false}; std::thread th; uint32_t rate = 768000; };
EXPORT int airspyhf_list_devices(uint64_t* s, int n) { if (s && n > 0) s[0] = 0x3652A8D8C9E1F001ull; return 1; }
EXPORT int airspyhf_open_sn(ahf_dev** d, uint64_t sn) {
    rec("airspyhf.serialOk", sn == 0x3652A8D8C9E1F001ull ? 1 : 0);
    ahfCount("airspyhf.opens");
    rec("airspyhf.open", 1);
    *d = new ahf_dev;
    return 0;
}
EXPORT int airspyhf_close(ahf_dev* d) {
    if (d->run) { d->run = false; if (d->th.joinable()) d->th.join(); }
    delete d;
    ahfCount("airspyhf.closes");
    rec("airspyhf.open", 0);
    return 0;
}
EXPORT int airspyhf_get_samplerates(ahf_dev*, uint32_t* b, uint32_t len) {
    static const uint32_t v[] = {912000, 768000, 456000, 384000, 256000, 192000};
    if (len == 0) { b[0] = 6; return 0; }
    for (uint32_t i = 0; i < len && i < 6; i++) b[i] = v[i];
    return 0;
}
EXPORT int airspyhf_set_samplerate(ahf_dev* d, uint32_t r) { if (d->run) rec("airspyhf.rateWhileStreaming", 1); d->rate = r; rec("airspyhf.rate", r); return 0; }
EXPORT int airspyhf_set_freq(ahf_dev*, uint32_t f) { rec("airspyhf.freq", f); return 0; }
EXPORT int airspyhf_set_hf_agc(ahf_dev*, uint8_t f) { rec("airspyhf.agc", f); return 0; }
EXPORT int airspyhf_set_hf_att(ahf_dev*, uint8_t v) { rec("airspyhf.att", v); return 0; }
EXPORT int airspyhf_set_hf_lna(ahf_dev*, uint8_t f) { rec("airspyhf.lna", f); return 0; }
EXPORT int airspyhf_set_lib_dsp(ahf_dev*, uint8_t f) { rec("airspyhf.dsp", f); return 0; }
// ---- radio settings: the AGC threshold, the calibration (a factory value of 1234 ppb), antenna power on radios that have it (airspyhf.biasCount)
EXPORT int airspyhf_set_hf_agc_threshold(ahf_dev*, uint8_t f) { rec("airspyhf.agcThr", f); return 0; }
EXPORT int airspyhf_get_calibration(ahf_dev*, int32_t* ppb) { *ppb = 1234; return 0; }
EXPORT int airspyhf_set_calibration(ahf_dev*, int32_t ppb) { rec("airspyhf.cal", ppb); ahfCount("airspyhf.calCalls"); return 0; }
EXPORT int airspyhf_get_bias_tee_count(ahf_dev*, int32_t* n) { *n = fake_state("airspyhf.biasCount") > 0 ? (int32_t)fake_state("airspyhf.biasCount") : 0; return 0; }
EXPORT int airspyhf_set_bias_tee(ahf_dev*, int8_t v) { rec("airspyhf.bias", v); return 0; }
EXPORT int airspyhf_is_streaming(ahf_dev* d) { return d->run ? 1 : 0; }
EXPORT int airspyhf_start(ahf_dev* d, int (*cb)(ahf_transfer_t*), void* ctx) {
    d->run = true;
    rec("airspyhf.streaming", 1);
    d->th = std::thread([d, cb, ctx] {
        const int n = 4096;
        std::vector<float> s(n * 2);
        ahf_transfer_t t{d, ctx, s.data(), n, 0};
        uint64_t k = 0;
        while (d->run) {
            for (int i = 0; i < n; i++, k++) { const double ph = 2 * M_PI * (double)(k % 8) / 8; s[2 * i] = (float)(0.5 * cos(ph)); s[2 * i + 1] = (float)(0.5 * sin(ph)); }
            cb(&t);
            pace();
        }
    });
    return 0;
}
EXPORT int airspyhf_stop(ahf_dev* d) { d->run = false; if (d->th.joinable()) d->th.join(); rec("airspyhf.streaming", 0); return 0; }

// ---- radios: the optional functions the drivers use for error texts, tuning ranges, direct sampling and the bladeRF 1's FPGA
EXPORT int rtlsdr_get_tuner_type(rtl_dev*) { return fake_state("rtl.tuner") > 0 ? (int)fake_state("rtl.tuner") : fake_state("rtl.v4") == 1 ? 6 : 5; }   // R828D on the V4, else R820T (rtl.tuner: another)
EXPORT int rtlsdr_set_direct_sampling(rtl_dev*, int on) { rec("rtl.ds", on); rec("rtl.dsCalls", std::max(0.0, fake_state("rtl.dsCalls")) + 1); return 0; }
EXPORT const char* bladerf_strerror(int e) { return e == -7 ? "No device(s) available" : e == -2 ? "Value out of range" : "Unexpected error"; }
EXPORT const char* bladerf_get_board_name(bl_dev*) { return fake_state("blade.v1") == 1 ? "bladerf1" : "bladerf2"; }
EXPORT int bladerf_is_fpga_configured(bl_dev*) { return fake_state("blade.v1") == 1 ? (fake_state("blade.fpgaLoaded") == 1 ? 1 : 0) : 1; }
EXPORT int bladerf_get_fpga_size(bl_dev*, int* size) { *size = fake_state("blade.v1") == 1 ? 40 : 49; return 0; }
EXPORT int bladerf_load_fpga(bl_dev*, const char* path) {
    const char* base = strrchr(path, '/');
    rec("blade.fpgaLoaded", 1);
    rec("blade.fpgaFileX40", base && strcmp(base + 1, "hostedx40.rbf") == 0 ? 1 : 0);
    return 0;
}
EXPORT int bladerf_get_frequency_range(bl_dev*, int, const bladerf_range_t** r) {
    static const bladerf_range_t v1{237500000, 3800000000LL, 1, 1.f}, v1xb{0, 3800000000LL, 1, 1.f}, v2{70000000, 6000000000LL, 1, 1.f};   // libbladeRF: BLADERF_FREQUENCY_MIN, _MIN_XB200
    *r = fake_state("blade.v1") == 1 ? (fake_state("blade.xb") == 2 ? &v1xb : &v1) : &v2;
    return 0;
}
EXPORT int bladerf_expansion_attach(bl_dev*, int xb) { rec("blade.xb", xb); return 0; }   // ---- radio settings
EXPORT const char* LMS_GetLastErrorMessage(void) { return "fake LimeSuite error"; }
EXPORT int LMS_GetLOFrequencyRange(void*, bool, lms_range_t* r) { r->min = 30e6; r->max = 3.8e9; r->step = 1; return 0; }
EXPORT long long iio_channel_attr_read(const void* c, const char* a, char* dst, size_t len) {
    if (strcmp(a, "frequency_available") != 0 || ((iio_chan*)c)->name != "phy:altvoltage0") return -22;
    snprintf(dst, len, "[325000000 1 3800000000]");   // a stock Pluto (AD9363)
    return (long long)strlen(dst) + 1;
}
// ---- radio settings: the AD9361 reference clock (a device attribute of ad9361-phy)
EXPORT long long iio_device_attr_read(const void* dev, const char* a, char* dst, size_t len) {
    if (strcmp((const char*)dev, "phy") != 0 || strcmp(a, "xo_correction") != 0) return -22;
    snprintf(dst, len, "40000000");
    return (long long)strlen(dst) + 1;
}
EXPORT int iio_device_attr_write_longlong(const void* dev, const char* a, long long v) { rec((std::string("pluto.") + (const char*)dev + "." + a).c_str(), (double)v); count("pluto.xoWrites"); return 0; }
EXPORT int uhd_usrp_set_clock_source(void*, const char* src, size_t mb) { rec("usrp.clock", strcmp(src, "internal") == 0 ? 1 : strcmp(src, "external") == 0 ? 2 : strcmp(src, "gpsdo") == 0 ? 3 : 0); rec("usrp.clockMb", (double)mb); count("usrp.clockCalls"); return 0; }
EXPORT int uhd_get_last_error(char* out, size_t len) { snprintf(out, len, "%s", "fake UHD error"); return 0; }
EXPORT int uhd_usrp_last_error(void*, char* out, size_t len) { snprintf(out, len, "%s", "fake USRP error"); return 0; }
EXPORT int uhd_usrp_get_rx_freq_range(void*, size_t, uhd_range* r) { r->lo = 70e6; r->hi = 6e9; return 0; }
// the inputs of a B200 / B210 as UHD lists them (b200_impl: TX/RX, RX2 on each channel); usrp.antenna: 1 = RX2, 2 = TX/RX
EXPORT int uhd_usrp_get_rx_num_channels(void*, size_t* n) { *n = fake_state("usrp.b210") == 1 ? 2 : 1; return 0; }
EXPORT int uhd_usrp_get_rx_antennas(void*, size_t, strvec** h) { (*h)->v = {"TX/RX", "RX2"}; return 0; }
EXPORT int uhd_usrp_set_rx_antenna(void*, const char* a, size_t ch) {
    rec("usrp.antenna", strcmp(a, "RX2") == 0 ? 1 : strcmp(a, "TX/RX") == 0 ? 2 : 0); rec("usrp.antennaCh", (double)ch);
    rec("usrp.antennaCalls", std::max(0.0, fake_state("usrp.antennaCalls")) + 1);
    return strcmp(a, "RX2") == 0 || strcmp(a, "TX/RX") == 0 ? 0 : 1;
}
// ---- end radios
