// A fake "vendor library" that exports the functions of librtlsdr, libairspy, libbladeRF, LimeSuite, libiio and UHD that OnAir uses, and
// streams a known tone. The test copies this one file under the six real library names. It checks OnAir's driver code (what it asks the
// libraries to do and how it converts the samples), not the real libraries.
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
// the test tone: I = 0.5 (constant), Q = -0.25
static const float kI = 0.5f, kQ = -0.25f;

// ------------------------------------------------------------------ RTL-SDR
struct rtl_dev { std::atomic<bool> cancel{false}; };
EXPORT uint32_t rtlsdr_get_device_count(void) { return 1; }
EXPORT const char* rtlsdr_get_device_name(uint32_t) { return "Generic RTL2832U"; }
EXPORT int rtlsdr_get_device_usb_strings(uint32_t, char* m, char* p, char* s) { strcpy(m, "Realtek"); strcpy(p, "RTL2838UHIDIR"); strcpy(s, "00000001"); return 0; }
EXPORT int rtlsdr_open(rtl_dev** d, uint32_t) { *d = new rtl_dev; return 0; }
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
    while (!d->cancel) { cb(b.data(), (uint32_t)b.size(), ctx); pace(); }
    return 0;
}
EXPORT int rtlsdr_cancel_async(rtl_dev* d) { d->cancel = true; return 0; }

// ------------------------------------------------------------------ Airspy
struct airspy_transfer_t { void* device; void* ctx; void* samples; int sample_count; uint64_t dropped_samples; int sample_type; };
struct as_dev { std::atomic<bool> run{false}; std::thread th; };
EXPORT int airspy_init(void) { return 0; }
EXPORT int airspy_exit(void) { return 0; }
EXPORT int airspy_list_devices(uint64_t* s, int) { s[0] = 0x1234567890ABCDEFull; return 1; }
EXPORT int airspy_open_sn(as_dev** d, uint64_t sn) { rec("airspy.serial", (double)(sn & 0xFFFFFFFF)); *d = new as_dev; return 0; }
EXPORT int airspy_open(as_dev** d) { *d = new as_dev; return 0; }
EXPORT int airspy_close(as_dev* d) { if (d->run) { d->run = false; if (d->th.joinable()) d->th.join(); } delete d; return 0; }
EXPORT int airspy_get_samplerates(as_dev*, uint32_t* b, uint32_t len) { if (len == 0) { b[0] = 2; return 0; } b[0] = 10000000; if (len > 1) b[1] = 2500000; return 0; }
EXPORT int airspy_set_samplerate(as_dev*, uint32_t r) { rec("airspy.rate", r); return 0; }
EXPORT int airspy_set_sample_type(as_dev*, int t) { rec("airspy.type", t); return 0; }
EXPORT int airspy_set_freq(as_dev*, uint32_t f) { rec("airspy.freq", f); return 0; }
EXPORT int airspy_set_linearity_gain(as_dev*, uint8_t g) { rec("airspy.gain", g); return 0; }
EXPORT int airspy_start_rx(as_dev* d, int (*cb)(airspy_transfer_t*), void* ctx) {
    d->run = true;
    d->th = std::thread([d, cb, ctx] {
        std::vector<float> s(16384 * 2);
        for (size_t i = 0; i < 16384; i++) { s[2 * i] = kI; s[2 * i + 1] = kQ; }
        airspy_transfer_t t{d, ctx, s.data(), 16384, 0, 0};
        while (d->run) { cb(&t); pace(); }
    });
    return 0;
}
EXPORT int airspy_stop_rx(as_dev* d) { d->run = false; if (d->th.joinable()) d->th.join(); return 0; }

// ------------------------------------------------------------------ bladeRF
struct bladerf_devinfo_t { int backend; char serial[33]; uint8_t usb_bus, usb_addr; unsigned int instance; char manufacturer[33]; char product[33]; };
struct bladerf_range_t { int64_t min, max, step; float scale; };
struct bl_dev { int dummy; };
EXPORT int bladerf_get_device_list(bladerf_devinfo_t** l) { auto* a = (bladerf_devinfo_t*)calloc(1, sizeof(bladerf_devinfo_t)); strcpy(a->serial, "0123456789ABCDEF0123456789ABCDEF"); strcpy(a->product, "bladeRF 2.0 micro"); *l = a; return 1; }
EXPORT void bladerf_free_device_list(bladerf_devinfo_t* l) { free(l); }
EXPORT int bladerf_open(bl_dev** d, const char* id) { rec("blade.openSerialGiven", id ? 1 : 0); *d = new bl_dev; return 0; }
EXPORT void bladerf_close(bl_dev* d) { delete d; }
EXPORT int bladerf_set_frequency(bl_dev*, int, uint64_t f) { rec("blade.freq", (double)f); return 0; }
EXPORT int bladerf_set_sample_rate(bl_dev*, int, unsigned r, unsigned* actual) { rec("blade.rate", r); *actual = r; return 0; }
EXPORT int bladerf_set_bandwidth(bl_dev*, int, unsigned b, unsigned* actual) { rec("blade.bw", b); *actual = b; return 0; }
EXPORT int bladerf_set_gain(bl_dev*, int, int g) { rec("blade.gain", g); return 0; }
EXPORT int bladerf_set_gain_mode(bl_dev*, int, int m) { rec("blade.gainmode", m); return 0; }
EXPORT int bladerf_get_gain_range(bl_dev*, int, const bladerf_range_t** r) { static const bladerf_range_t g{-15, 60, 1, 1.f}; *r = &g; return 0; }
EXPORT int bladerf_enable_module(bl_dev*, int, bool e) { rec("blade.enabled", e ? 1 : 0); return 0; }
EXPORT int bladerf_sync_config(bl_dev*, int layout, int fmt, unsigned, unsigned, unsigned, unsigned) { rec("blade.layout", layout); rec("blade.format", fmt); return 0; }
EXPORT int bladerf_sync_rx(bl_dev*, void* s, unsigned n, void*, unsigned) {
    int16_t* p = (int16_t*)s;
    for (unsigned i = 0; i < n; i++) { p[2 * i] = (int16_t)lround(kI * 2048); p[2 * i + 1] = (int16_t)lround(kQ * 2048); }
    pace(); return 0;
}

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
EXPORT int LMS_Calibrate(void*, bool, size_t, double, unsigned) { rec("lime.calibrated", 1); return 0; }
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
EXPORT int uhd_usrp_find(const char*, strvec** h) { (*h)->v.push_back("type=b200,name=,serial=30C6E4D,product=B200"); return 0; }
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
EXPORT int uhd_usrp_set_rx_freq(void*, uhd_tune_request_t* q, size_t, uhd_tune_result_t* res) { rec("usrp.freq", q->target_freq); rec("usrp.policy", q->rf_freq_policy); res->actual_rf_freq = q->target_freq; return 0; }
EXPORT int uhd_usrp_set_rx_gain(void*, double g, size_t, const char*) { rec("usrp.gain", g); return 0; }
EXPORT int uhd_usrp_set_rx_bandwidth(void*, double b, size_t) { rec("usrp.bw", b); return 0; }
EXPORT int uhd_usrp_get_rx_stream(void*, uhd_stream_args_t* a, void*) { rec("usrp.cpuFmtFc32", strcmp(a->cpu_format, "fc32") == 0 ? 1 : 0); return 0; }
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
