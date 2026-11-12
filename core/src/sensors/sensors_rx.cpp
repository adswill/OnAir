// Sub-GHz sensors receiver, see sensors_rx.h.
// The pipeline in process() follows rtl_433's push_sdr_flow() (src/r_flow.c, GPL-2.0-or-later, (C) Benjamin Larsson, Tommy Vestermark,
// Christian W. Zuckschwerdt and others) so that its own test recordings give the same pulse packages here.
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_health.h"
#include "dect2/sensors/sensors_names.h"
#include "dect2/sensors/sensors_tel.h"
#include "sensors_analyze.h"
#include "dect2/automation/engine.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

extern "C" {
#include "baseband.h"
#include "bitbuffer.h"
#include "data.h"
#include "list.h"
#include "onair_rtl433.h"
#include "pulse_data.h"
#include "pulse_detect.h"
#include "pulse_slicer.h"
#include "r_device.h"
}

namespace dect2 {

namespace {

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void jsonEscape(std::string& o, const char* s) {
    o += '"';
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    o += '"';
}

std::string num3(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

std::string numText(double v) {
    char b[48];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e12) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

// the value of a data_t as rtl_433's JSON output prints it
void valueJson(std::string& o, data_type_t type, data_value_t v);
void dataJson(std::string& o, const data_t* d) {
    o += '{';
    bool first = true;
    for (; d; d = d->next) {
        if (!first) o += ',';
        first = false;
        jsonEscape(o, d->key);
        o += ':';
        valueJson(o, d->type, d->value);
    }
    o += '}';
}
void valueJson(std::string& o, data_type_t type, data_value_t v) {
    switch (type) {
    case DATA_INT: o += std::to_string(v.v_int); break;
    case DATA_DOUBLE: o += num3(v.v_dbl); break;
    case DATA_STRING: jsonEscape(o, (const char*)v.v_ptr); break;
    case DATA_DATA: dataJson(o, (const data_t*)v.v_ptr); break;
    case DATA_ARRAY: {
        const data_array_t* a = (const data_array_t*)v.v_ptr;
        o += '[';
        for (int i = 0; a && i < a->num_values; i++) {
            if (i) o += ',';
            if (a->type == DATA_INT) o += std::to_string(((const int*)a->values)[i]);
            else if (a->type == DATA_DOUBLE) o += num3(((const double*)a->values)[i]);
            else if (a->type == DATA_STRING) jsonEscape(o, ((const char* const*)a->values)[i]);
            else if (a->type == DATA_ARRAY) {
                data_value_t e; e.v_ptr = ((void* const*)a->values)[i];
                valueJson(o, DATA_ARRAY, e);
            } else o += "null";
        }
        o += ']';
        break;
    }
    default: o += "null"; break;
    }
}

void flatten(std::vector<SensorField>& out, const data_t* d, const std::string& prefix) {
    for (; d; d = d->next) {
        const std::string key = prefix + d->key;
        SensorField f;
        f.key = key;
        switch (d->type) {
        case DATA_INT: f.numeric = true; f.num = d->value.v_int; f.text = std::to_string(d->value.v_int); break;
        case DATA_DOUBLE: f.numeric = true; f.num = d->value.v_dbl; f.text = numText(d->value.v_dbl); break;
        case DATA_STRING: f.text = d->value.v_ptr ? (const char*)d->value.v_ptr : ""; break;
        case DATA_DATA: flatten(out, (const data_t*)d->value.v_ptr, key + "."); continue;
        case DATA_ARRAY: { std::string s; valueJson(s, DATA_ARRAY, d->value); f.text = s; break; }
        default: continue;
        }
        out.push_back(std::move(f));
    }
}

const SensorField* findField(const std::vector<SensorField>& v, const char* k) {
    for (const auto& f : v) if (f.key == k) return &f;
    return nullptr;
}

std::string kindOf(const std::vector<SensorField>& f, const std::string& model) {
    auto has = [&](const char* k) { return findField(f, k) != nullptr; };
    auto lc = model;
    for (auto& c : lc) c = (char)std::tolower((unsigned char)c);
    auto mod = [&](const char* s) { return lc.find(s) != std::string::npos; };
    if (mod("tpms") || mod("tire") || mod("tyre") || has("pressure_kPa") || has("pressure_PSI") || has("pressure_bar")) return "tpms";
    if (mod("honeywell") || mod("visonic") || mod("dsc") || mod("interlogix") || mod("kerui") || mod("alarm") || mod("smoke") || mod("pir") || mod("x10-sec") || mod("chuango") || has("alarm") || has("tamper") || has("opened") || has("motion")) return "security";
    if (has("temperature_C") || has("temperature_F") || has("humidity") || has("wind_avg_km_h") || has("wind_avg_m_s") || has("wind_avg_mi_h") || has("rain_mm") || has("pressure_hPa") || has("uv") || has("lux") || has("temperature_1_C") || has("moisture") || has("light_lux")) return "weather";
    if (has("energy_kWh") || has("power_W") || has("consumption") || has("volume_m3") || has("current_A") || has("meter_type") || has("wmbus") || mod("meter") || mod("ert") || mod("itron") || mod("r900") || mod("neptune")) return "meter";
    if (has("cmd") || has("button") || has("code") || has("command") || mod("remote") || mod("doorbell") || mod("x10") || mod("intertechno") || mod("somfy")) return "remote";
    return "other";
}

// one slicer with the rtl_433 dispatch (r_api.c run_ook_demods / run_fsk_demods)
int runSlicer(const pulse_data_t* pd, r_device* d, bool fsk) {
    switch (d->modulation) {
    case OOK_PULSE_PCM: return fsk ? 0 : pulse_slicer_pcm(pd, d);
    case OOK_PULSE_PPM: return fsk ? 0 : pulse_slicer_ppm(pd, d);
    case OOK_PULSE_PWM: return fsk ? 0 : pulse_slicer_pwm(pd, d);
    case OOK_PULSE_MANCHESTER_ZEROBIT: return fsk ? 0 : pulse_slicer_manchester_zerobit(pd, d);
    case OOK_PULSE_PIWM_RAW: return fsk ? 0 : pulse_slicer_piwm_raw(pd, d);
    case OOK_PULSE_PIWM_DC: return fsk ? 0 : pulse_slicer_piwm_dc(pd, d);
    case OOK_PULSE_DMC: return fsk ? 0 : pulse_slicer_dmc(pd, d);
    case OOK_PULSE_PWM_OSV1: return fsk ? 0 : pulse_slicer_osv1(pd, d);
    case OOK_PULSE_NRZS: return fsk ? 0 : pulse_slicer_nrzs(pd, d);
    case OOK_PULSE_RZI: return fsk ? 0 : pulse_slicer_rzi(pd, d);
    case FSK_PULSE_PCM: return fsk ? pulse_slicer_pcm(pd, d) : 0;
    case FSK_PULSE_PWM: return fsk ? pulse_slicer_pwm(pd, d) : 0;
    case FSK_PULSE_MANCHESTER_ZEROBIT: return fsk ? pulse_slicer_manchester_zerobit(pd, d) : 0;
    default: return 0;
    }
}

} // namespace

struct SensorsReceiver::Impl {
    // settings
    double inRate = 0, offsetHz = 0, centerHz = 433.92e6;
    uint32_t rate = 0;                 // the rate of the pulse detector
    int decim = 1;
    bool isReady = false;
    std::function<void(const std::string&)> logCb;
    std::function<void(const std::string&)> jsonCb;
    std::function<int64_t()> clock = wallNow;
    int fskMode = FSK_PULSE_DETECT_OLD;   // classic: 2946 of 3031 rtl_433 test samples pass with it, 2147 with the auto mode of the vendored version
    bool autoLevel = false;
    float fixedLevel = 0.0f, minLevel = -12.1442f, minSnr = 9.0f;
    float minLevelAuto = 0, noiseLevel = 0;
    int removeAfter = 0;
    std::set<std::string> pinned;      // keys that are never removed (under setMu)
    std::mutex namesMu;
    std::map<std::string, std::string> names;   // the user's names by key (setNames, under namesMu)
    std::map<std::string, int64_t> lastEvent;   // sensor.reading: when each sensor last went out (telMu)

    // decoders
    std::vector<bool> on;              // by device index (protocol number - 1)
    std::vector<std::string> customSpecs;
    std::vector<r_device*> devs, customDevs;
    std::mutex setMu;
    bool dirty = true;
    std::mutex viewMu;
    std::vector<std::string> viewPat;  // setView(): lower-case parts of the model names this view reports (empty: every sensor)

    // pulse pipeline
    pulse_detect_t* pd = nullptr;
    pulse_data_t* pulse = nullptr;     // heap: the arrays are large
    pulse_data_t* fskPulse = nullptr;
    filter_state_t lp;
    demodfm_state_t fm;
    std::vector<uint16_t> tmp;
    std::vector<int16_t> am, fmb;
    uint64_t inputPos = 0;
    bool haveFsk = true;
    std::vector<uint8_t> u8;           // live: the 8-bit IQ gathered for the next block
    double accI = 0, accQ = 0; int accN = 0;
    cf32 rot = cf32(1, 0), rotStep = cf32(1, 0);

    // results
    std::mutex telMu;
    SensorsTelemetry pub;
    std::vector<SensorDevice> devices;
    std::vector<SensorUnknown> unknown;
    std::vector<std::string> recent;
    uint64_t seq = 0, okMsgs = 0, badPk = 0, pk = 0, pkOok = 0, pkFsk = 0;
    float lastSnr = 0, lastRssi = -200;
    uint64_t nIn = 0;
    int eventsInPackage = 0;
    const pulse_data_t* curPd = nullptr;
    int64_t lastPrune = 0;
    uint64_t lastUnknownHash = 0; int64_t lastUnknownUtc = 0;

    Impl() {
        baseband_init();
        pd = pulse_detect_create();
        pulse = (pulse_data_t*)calloc(1, sizeof(pulse_data_t));
        fskPulse = (pulse_data_t*)calloc(1, sizeof(pulse_data_t));
        const unsigned n = onair_rtl433_device_count();
        on.assign(n, false);
        for (unsigned i = 0; i < n; i++) {
            const r_device* d = onair_rtl433_device(i);
            on[i] = d && d->decode_fn && d->disabled == 0;
        }
        std::memset(&lp, 0, sizeof lp);
        std::memset(&fm, 0, sizeof fm);
        baseband_low_pass_filter_reset(&lp);
        baseband_demod_FM_reset(&fm);
        pulse_detect_set_levels(pd, 0, fixedLevel, minLevel, minSnr, 0);
    }
    ~Impl() {
        freeDevs();
        pulse_detect_free(pd);
        free(pulse);
        free(fskPulse);
    }
    void freeDevs() {
        for (auto* d : devs) { free(d->decode_ctx); free(d); }
        for (auto* d : customDevs) { free(d->decode_ctx); free(d); }
        devs.clear(); customDevs.clear();
    }
    void say(const std::string& s) { if (logCb) logCb(s); }

    static void outFn(r_device* d, data_t* data) {
        Impl* self = (Impl*)d->output_ctx;
        if (self) self->onData(d, data);
        data_free(data);
    }

    void rebuild() {
        freeDevs();
        const unsigned n = onair_rtl433_device_count();
        for (unsigned i = 0; i < n; i++) {
            const r_device* src = onair_rtl433_device(i);
            if (!src || !on[i] || !src->decode_fn || src->create_fn) continue;
            r_device* d = (r_device*)malloc(sizeof(r_device));
            *d = *src;
            d->protocol_num = i + 1;
            d->verbose = 0; d->verbose_bits = 0;
            d->log_fn = nullptr;
            d->output_fn = outFn;
            d->output_ctx = this;
            devs.push_back(d);
        }
        for (const auto& s : customSpecs) {
            r_device* d = onair_rtl433_flex_create(s.c_str());
            if (!d) continue;
            d->verbose = 0; d->verbose_bits = 0; d->log_fn = nullptr;
            d->output_fn = outFn; d->output_ctx = this;
            d->priority = 0;
            customDevs.push_back(d);
        }
        dirty = false;
    }

    // ---- results ----
    bool viewAccepts(const std::string& model, const std::string& kind) {
        std::lock_guard<std::mutex> lk(viewMu);
        if (viewPat.empty()) return true;
        std::string a = model;
        for (auto& c : a) c = (char)std::tolower((unsigned char)c);
        for (const auto& p : viewPat) {
            if (p.rfind("kind:", 0) == 0) { if (kind == p.substr(5)) return true; }       // "kind:meter": the kind the decoder's fields say (kindOf)
            else if (a.find(p) != std::string::npos) return true;
        }
        return false;
    }
    void onData(r_device* d, data_t* data) {
        std::vector<SensorField> f;
        flatten(f, data, "");
        {   // a sensor-family entry (setView) reports its own family only; the package still counts as recognised
            const SensorField* vm = findField(f, "model");
            const std::string vmodel = vm ? vm->text : std::string(d && d->name ? d->name : "?");
            if (!viewAccepts(vmodel, kindOf(f, vmodel))) { eventsInPackage++; return; }
        }
        std::string js;
        dataJson(js, data);
        if (jsonCb) jsonCb(js);
        eventsInPackage++;
        const int64_t now = clock();
        std::lock_guard<std::mutex> lk(telMu);
        okMsgs++;
        const SensorField* mf = findField(f, "model");
        std::string model = mf ? mf->text : (d && d->name ? d->name : "?");
        std::string id, ch;
        if (auto* x = findField(f, "id")) id = x->text;
        if (auto* x = findField(f, "channel")) ch = x->text;
        const std::string key = model + "|" + id + "|" + ch;
        SensorDevice* dev = nullptr;
        for (auto& e : devices) if (e.key == key) { dev = &e; break; }
        if (!dev) {
            if (devices.size() >= kSensorsDevicesMax) devices.pop_back();
            devices.insert(devices.begin(), SensorDevice());
            dev = &devices.front();
            dev->key = key; dev->model = model; dev->id = id; dev->channel = ch;
            dev->firstSeen = now;
        } else if (dev != &devices.front()) {
            std::rotate(devices.begin(), devices.begin() + (dev - devices.data()), devices.begin() + (dev - devices.data()) + 1);
            dev = &devices.front();
        }
        dev->count++;
        dev->lastSeen = now;
        dev->json = js;
        if (curPd) { dev->rssiDb = curPd->rssi_db; dev->snrDb = curPd->snr_db; }
        for (const auto& nf : f) {
            bool done = false;
            for (auto& e : dev->latest) if (e.key == nf.key) { e = nf; done = true; break; }
            if (!done) dev->latest.push_back(nf);
            if (nf.numeric && nf.key != "id" && nf.key != "channel" && nf.key != "subtype" && nf.key != "mic") {
                SensorSeries* s = nullptr;
                for (auto& e : dev->history) if (e.key == nf.key) { s = &e; break; }
                if (!s) { dev->history.push_back(SensorSeries()); s = &dev->history.back(); s->key = nf.key; }
                // the radios send each message several times: one history point per second and value
                if (s->pts.empty() || s->pts.back().utc != now) {
                    s->pts.push_back({now, nf.num});
                    if (s->pts.size() > kSensorsHistoryMax) s->pts.erase(s->pts.begin(), s->pts.begin() + (s->pts.size() - kSensorsHistoryMax));
                } else s->pts.back().v = nf.num;
            }
            if (nf.key == "battery_ok" && nf.numeric) dev->batteryOk = nf.num > 0.5 ? 1 : 0;
            if (nf.key == "battery" && !nf.numeric) dev->batteryOk = (nf.text == "OK" || nf.text == "ok") ? 1 : (nf.text == "LOW" || nf.text == "Low" || nf.text == "low") ? 0 : dev->batteryOk;
        }
        dev->kind = kindOf(dev->latest, dev->model);
        sensorHealthMessage(dev->health, *dev, dev->lastSeen);
        recent.insert(recent.begin(), js);
        if (recent.size() > kSensorsRecentMax) recent.pop_back();
        lastSnr = curPd ? curPd->snr_db : lastSnr;
        seq++;
        // sensor.reading for the automations and Integrations: once per transmission (the radio's repeats within 2 s are one reading)
        if (automation::wants(automation::FSensor)) {
            if (lastEvent.size() > 4 * kSensorsDevicesMax) lastEvent.clear();
            int64_t& le = lastEvent[key];
            if (le == 0 || now - le >= 2 || now < le) {
                le = now;
                std::string nm;
                { std::lock_guard<std::mutex> sl(namesMu); auto it = names.find(key); if (it != names.end()) nm = it->second; }
                automation::Event ev = sensorReadingEvent(*dev, nm, (double)now, centerHz);
                automation::emit(std::move(ev));
            }
        }
    }

    void calcRssiSnr(pulse_data_t* p) {
        const float hi = p->ook_high_estimate > 0 ? (float)p->ook_high_estimate : 1.f;
        const float lo = p->ook_low_estimate > 0 ? (float)p->ook_low_estimate : 1.f;
        const int maxHigh = DB_TO_AMP(0);
        const float mx = hi < maxHigh ? hi : (float)maxHigh;
        const float asnr = mx / lo;
        const float f1 = (float)p->fsk_f1_est / INT16_MAX * (float)rate / 2.0f;
        const float f2 = (float)p->fsk_f2_est / INT16_MAX * (float)rate / 2.0f;
        p->freq1_hz = f1 + (float)centerHz;
        p->freq2_hz = f2 + (float)centerHz;
        p->centerfreq_hz = (float)centerHz;
        p->depth_bits = 8;
        p->range_db = 42.1442f;
        p->rssi_db = 10.0f * log10f(hi) - 42.1442f;
        p->noise_db = 10.0f * log10f(lo) - 42.1442f;
        p->snr_db = 10.0f * log10f(asnr);
    }

    int runDemods(const pulse_data_t* p, bool fsk) {
        unsigned nextPrio = 0;
        int events = 0;
        for (unsigned prio = 0; !events && prio < 0xffffffffu; prio = nextPrio) {
            nextPrio = 0xffffffffu;
            auto each = [&](r_device* d) {
                if (d->priority > prio && d->priority < nextPrio) nextPrio = d->priority;
                if (d->priority != prio) return;
                events += runSlicer(p, d, fsk);
            };
            for (auto* d : devs) each(d);
            for (auto* d : customDevs) each(d);
        }
        return events;
    }

    void packageDone(pulse_data_t* p, bool fsk) {
        calcRssiSnr(p);
#ifdef SENS_DEBUG
        fprintf(stderr, "PACKAGE %s\n", fsk ? "FSK" : "OOK"); pulse_data_print(p);
#endif
        pk++; if (fsk) pkFsk++; else pkOok++;
        curPd = p;
        eventsInPackage = 0;
        const int ev = runDemods(p, fsk);
        curPd = nullptr;
        {
            std::lock_guard<std::mutex> lk(telMu);
            lastRssi = p->rssi_db; lastSnr = p->snr_db;
        }
        if (ev <= 0 && eventsInPackage == 0) {
            // a package that nothing took: noise mostly; a clear one goes to the analyser
            if (p->num_pulses >= 24 && p->snr_db >= 8.f) analyseUnknown(p, fsk);
            std::lock_guard<std::mutex> lk(telMu);
            badPk++; seq++;
        }
    }

    void analyseUnknown(const pulse_data_t* p, bool fsk) {
        SensorUnknown u;
        uint64_t h = 1469598103934665603ull;
        for (unsigned i = 0; i < p->num_pulses; i++) {
            h = (h ^ (uint64_t)(p->pulse[i] / 4)) * 1099511628211ull;
            h = (h ^ (uint64_t)(p->gap[i] / 4)) * 1099511628211ull;
        }
        const int64_t now = clock();
        if (h == lastUnknownHash && now - lastUnknownUtc < 3) return;   // the repeat of a package that was just shown
        lastUnknownHash = h; lastUnknownUtc = now;
        sensors::analyse(p, fsk, u);
        u.utc = now;
        u.fsk = fsk;
        u.rssiDb = p->rssi_db; u.snrDb = p->snr_db;
        u.freqMHz = (fsk ? p->freq1_hz : p->centerfreq_hz) / 1e6;
        std::lock_guard<std::mutex> lk(telMu);
        unknown.insert(unknown.begin(), std::move(u));
        if (unknown.size() > kSensorsUnknownMax) unknown.pop_back();
    }

    // rtl_433's push_sdr_flow() for one block of 8-bit IQ (n == 0: flush the package that is still open)
    void process(const uint8_t* iq, size_t n, uint32_t fs) {
        if (dirty) { std::lock_guard<std::mutex> lk(setMu); if (dirty) rebuild(); }
        rate = fs;
        if (n) {
            tmp.resize(n); am.resize(n); fmb.resize(n);
            const float avg = envelope_detect_nolut(iq, tmp.data(), (uint32_t)n);
            if (autoLevel) {
                if (minLevelAuto == 0.0f) minLevelAuto = minLevel;
                if (noiseLevel == 0.0f) noiseLevel = minLevelAuto - 3.0f;
                const bool noiseOnly = avg < noiseLevel + 3.0f;
                if (noiseOnly) {
                    noiseLevel = (noiseLevel * 7 + avg) / 8;
                    if (noiseLevel < minLevel - 3.0f && std::fabs(minLevelAuto - noiseLevel - 3.0f) > 1.0f) {
                        minLevelAuto = noiseLevel + 3.0f;
                        pulse_detect_set_levels(pd, 0, fixedLevel, minLevelAuto, minSnr, 0);
                    }
                } else noiseLevel = (noiseLevel * 31 + avg) / 32;
            }
            baseband_low_pass_filter(&lp, tmp.data(), am.data(), (uint32_t)n);
            const float fmLow = fskMode ? 0.2f : 0.1f;
            baseband_demod_FM(&fm, iq, fmb.data(), (uint32_t)n, fs, fmLow);
        }
        int type = PULSE_DATA_OOK;
        while (type) {
            type = pulse_detect_package(pd, am.data(), fmb.data(), (int)n, fs, inputPos, pulse, fskPulse, (unsigned)fskMode);
            if (type == PULSE_DATA_OOK) packageDone(pulse, false);
            else if (type == PULSE_DATA_FSK) packageDone(fskPulse, true);
        }
        inputPos += n;
        nIn += n;
    }

    void prune(int64_t now) {
        if (removeAfter <= 0) return;
        std::lock_guard<std::mutex> lk(setMu);
        for (auto& d : devices) sensorHealthTick(d.health, now);
        for (size_t i = devices.size(); i-- > 0;)
            if (now - devices[i].lastSeen > removeAfter && !pinned.count(devices[i].key)) devices.erase(devices.begin() + (long)i);
    }

    void publish() {
        std::lock_guard<std::mutex> lk(telMu);
        const int64_t now = clock();
        if (now != lastPrune) { lastPrune = now; prune(now); }
        pub.seq = seq;
        pub.state = okMsgs ? 2 : (pk ? 1 : 0);
        pub.snrDb = lastSnr; pub.levelDb = lastRssi;
        pub.dataValid = okMsgs > 0;
        pub.blocksOk = okMsgs; pub.blocksBad = badPk;
        pub.packages = pk; pub.ookPackages = pkOok; pub.fskPackages = pkFsk;
        pub.inputRate = inRate; pub.workRate = rate;
        pub.timeSec = rate ? (double)nIn / rate : 0;
        pub.protocols = (unsigned)devs.size();
        pub.customProtocols = (unsigned)customDevs.size();
        pub.devices = devices;
        { std::lock_guard<std::mutex> vl(viewMu); if (viewPat.empty()) pub.unknown = unknown; else pub.unknown.clear(); }
        pub.recent = recent;
    }
};

SensorsReceiver::SensorsReceiver() : p_(new Impl) {}
SensorsReceiver::~SensorsReceiver() = default;

void SensorsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void SensorsReceiver::setJsonCallback(std::function<void(const std::string&)> cb) { p_->jsonCb = std::move(cb); }
void SensorsReceiver::setClock(std::function<int64_t()> clk) { p_->clock = std::move(clk); }
void SensorsReceiver::setFskMode(int m) { p_->fskMode = m; }
void SensorsReceiver::setFixedLevelDb(float db) { p_->fixedLevel = db; pulse_detect_set_levels(p_->pd, 0, db, p_->minLevel, p_->minSnr, 0); }
void SensorsReceiver::setMinLevelDb(float db) { p_->minLevel = db; pulse_detect_set_levels(p_->pd, 0, p_->fixedLevel, db, p_->minSnr, 0); }
void SensorsReceiver::setAutoLevel(bool on) { p_->autoLevel = on; }
void SensorsReceiver::setRemoveAfterSec(int s) { p_->removeAfter = s; }
void SensorsReceiver::setPinned(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(p_->setMu);
    p_->pinned = std::set<std::string>(keys.begin(), keys.end());
}
void SensorsReceiver::setNames(const std::map<std::string, std::string>& byKey) {
    std::lock_guard<std::mutex> lk(p_->namesMu);
    p_->names = byKey;
}
void SensorsReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
bool SensorsReceiver::ready() const { return p_->isReady; }

void SensorsReceiver::configure(double inputRateHz) {
    Impl& s = *p_;
    s.inRate = inputRateHz;
    s.isReady = inputRateHz >= sensorsTuning().minSampleRate - 1;
    if (!s.isReady) { if (inputRateHz > 0) s.say("Sensors: the sample rate is too low (200 ksps at least)"); return; }
    // up to 1.1 Msps the input is used as it is (smart meters need the fast clock); faster inputs are averaged down towards 1 Msps
    s.decim = std::max(1, (int)std::lround(inputRateHz / 1.024e6));
    s.rate = (uint32_t)std::lround(inputRateHz / s.decim);
    s.u8.clear(); s.u8.reserve(1 << 17);
    s.accI = s.accQ = 0; s.accN = 0;
    const double w = -2 * math::kPi * s.offsetHz / inputRateHz;
    s.rotStep = cf32((float)std::cos(w), (float)std::sin(w)); s.rot = cf32(1, 0);
    s.say("Sensors: listening, " + std::to_string(s.devs.size()) + " decoders from rtl_433 (OOK and FSK, PWM / PPM / Manchester / PCM), pulse detector at " + std::to_string(s.rate / 1000) + " ksps");
}

void SensorsReceiver::setSignalOffset(double hz) {
    Impl& s = *p_;
    s.offsetHz = hz;
    if (s.inRate > 0) {
        const double w = -2 * math::kPi * hz / s.inRate;
        s.rotStep = cf32((float)std::cos(w), (float)std::sin(w));
    }
}

void SensorsReceiver::reset() {
    Impl& s = *p_;
    baseband_low_pass_filter_reset(&s.lp);
    baseband_demod_FM_reset(&s.fm);
    pulse_detect_reset(s.pd);
    s.u8.clear(); s.accI = s.accQ = 0; s.accN = 0; s.inputPos = 0; s.nIn = 0;
    s.minLevelAuto = s.noiseLevel = 0;
    pulse_detect_set_levels(s.pd, 0, s.fixedLevel, s.minLevel, s.minSnr, 0);
}

void SensorsReceiver::setView(const std::string& patterns) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.viewMu);
    s.viewPat.clear();
    std::string cur;
    for (char c : patterns + "|") {
        if (c == '|') { if (!cur.empty()) s.viewPat.push_back(cur); cur.clear(); }
        else cur += (char)std::tolower((unsigned char)c);
    }
}

void SensorsReceiver::clearResults() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.telMu);
    s.devices.clear(); s.unknown.clear(); s.recent.clear();
    s.okMsgs = s.badPk = s.pk = s.pkOok = s.pkFsk = 0;
    s.seq++;
}

void SensorsReceiver::feedCu8(const uint8_t* iq, size_t n, uint32_t rate) {
    p_->process(iq, n, rate);
    p_->publish();
}

void SensorsReceiver::flush() {
    Impl& s = *p_;
    if (!s.u8.empty() && s.rate) { s.process(s.u8.data(), s.u8.size() / 2, s.rate); s.u8.clear(); }
    if (s.rate) s.process(nullptr, 0, s.rate);
    s.publish();
}

void SensorsReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    if (!s.isReady) return;
    const int D = s.decim;
    const float inv = 127.5f / (float)D;
    for (size_t i = 0; i < n; i++) {
        const cf32 v = x[i] * s.rot;
        s.rot *= s.rotStep;
        s.accI += v.real(); s.accQ += v.imag();
        if (++s.accN == D) {
            const float fi = (float)s.accI * inv + 127.5f, fq = (float)s.accQ * inv + 127.5f;
            s.u8.push_back((uint8_t)std::min(255.f, std::max(0.f, std::floor(fi + 0.5f))));
            s.u8.push_back((uint8_t)std::min(255.f, std::max(0.f, std::floor(fq + 0.5f))));
            s.accI = s.accQ = 0; s.accN = 0;
        }
    }
    if (s.u8.size() / 2 >= 32768) {
        s.process(s.u8.data(), s.u8.size() / 2, s.rate);
        s.u8.clear();
        s.publish();
    }
    // keep the unit-magnitude drift of the rotator in check
    const float m = std::abs(s.rot);
    if (m > 0) s.rot /= m;
}

bool SensorsReceiver::telemetry(SensorsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

int SensorsReceiver::setCustomDecoders(const std::vector<std::string>& specs) {
    Impl& s = *p_;
    std::vector<std::string> ok;
    for (const auto& sp : specs) if (checkFlexSpec(sp).empty()) ok.push_back(sp);
    std::lock_guard<std::mutex> lk(s.setMu);
    s.customSpecs = ok;
    s.dirty = true;
    return (int)ok.size();
}

std::string SensorsReceiver::checkFlexSpec(const std::string& spec) {
    if (spec.empty()) return "empty";
    // the flex parser prints its complaint to stderr and gives nothing back: name the usual mistakes here
    r_device* d = onair_rtl433_flex_create(spec.c_str());
    if (!d) return "not a valid flex decoder definition (needs n=name,m=OOK_PWM|OOK_PPM|OOK_PCM|OOK_MC_ZEROBIT|OOK_DMC|FSK_PCM|FSK_PWM|FSK_MC_ZEROBIT, s=, l=, r= in microseconds)";
    free(d->decode_ctx);
    free(d);
    return "";
}

std::vector<SensorProtocolInfo> SensorsReceiver::protocols() {
    std::vector<SensorProtocolInfo> v;
    const unsigned n = onair_rtl433_device_count();
    for (unsigned i = 0; i < n; i++) {
        const r_device* d = onair_rtl433_device(i);
        if (!d || !d->decode_fn || d->create_fn || d->disabled > 2) continue;
        v.push_back({i + 1, d->name ? d->name : "", d->disabled == 0, d->modulation});
    }
    return v;
}

void SensorsReceiver::enableAll(bool includeDefaultOff) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.setMu);
    const unsigned n = onair_rtl433_device_count();
    for (unsigned i = 0; i < n; i++) {
        const r_device* d = onair_rtl433_device(i);
        s.on[i] = d && d->decode_fn && !d->create_fn && (d->disabled == 0 || (includeDefaultOff && d->disabled <= 2));
    }
    s.dirty = true;
}

void SensorsReceiver::setProtocolEnabled(unsigned num, bool on) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.setMu);
    if (num >= 1 && num <= s.on.size()) { s.on[num - 1] = on; s.dirty = true; }
}

int SensorsReceiver::decodeCode(const std::string& code, std::vector<std::string>* json) {
    Impl& s = *p_;
    if (s.dirty) { std::lock_guard<std::mutex> lk(s.setMu); if (s.dirty) s.rebuild(); }
    auto prev = s.jsonCb;
    s.jsonCb = [&](const std::string& j) { if (json) json->push_back(j); };
    unsigned before = (unsigned)s.okMsgs;
    s.curPd = nullptr;
    unsigned nextPrio = 0;
    int events = 0;
    for (unsigned prio = 0; !events && prio < 0xffffffffu; prio = nextPrio) {
        nextPrio = 0xffffffffu;
        auto each = [&](r_device* d) {
            if (d->priority > prio && d->priority < nextPrio) nextPrio = d->priority;
            if (d->priority != prio) return;
            events += pulse_slicer_string(code.c_str(), d);
        };
        for (auto* d : s.devs) each(d);
        for (auto* d : s.customDevs) each(d);
    }
    s.jsonCb = prev;
    s.publish();
    return (int)((unsigned)s.okMsgs - before);
}

void SensorsReceiver::setProtocolsOnly(const std::vector<unsigned>& nums) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.setMu);
    std::fill(s.on.begin(), s.on.end(), false);
    for (unsigned n : nums) if (n >= 1 && n <= s.on.size()) s.on[n - 1] = true;
    s.dirty = true;
}

// ---- export ----
std::string sensorsCsv(const SensorsTelemetry& t) {
    std::set<std::string> keys;
    for (const auto& d : t.devices)
        for (const auto& f : d.latest)
            if (f.numeric && f.key != "id" && f.key != "channel") keys.insert(f.key);
    auto q = [](const std::string& s) {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c; }
        return o + "\"";
    };
    std::string o = "model,id,channel,kind,count,first_seen,last_seen,rssi_db,snr_db";
    for (const auto& k : keys) o += "," + k;
    o += "\n";
    for (const auto& d : t.devices) {
        char b[160];
        std::snprintf(b, sizeof b, ",%s,%llu,%lld,%lld,%.1f,%.1f", d.kind.c_str(), (unsigned long long)d.count, (long long)d.firstSeen, (long long)d.lastSeen, d.rssiDb, d.snrDb);
        o += q(d.model) + "," + q(d.id) + "," + q(d.channel) + b;
        for (const auto& k : keys) {
            o += ",";
            if (auto* f = findField(d.latest, k.c_str())) o += f->text;
        }
        o += "\n";
    }
    return o;
}

std::string sensorsJson(const SensorsTelemetry& t) {
    std::string o = "[";
    bool first = true;
    for (const auto& d : t.devices) {
        if (!first) o += ",\n";
        first = false;
        o += "{";
        o += "\"model\":"; jsonEscape(o, d.model.c_str());
        o += ",\"id\":"; jsonEscape(o, d.id.c_str());
        o += ",\"channel\":"; jsonEscape(o, d.channel.c_str());
        o += ",\"kind\":"; jsonEscape(o, d.kind.c_str());
        o += ",\"count\":" + std::to_string(d.count);
        o += ",\"first_seen\":" + std::to_string(d.firstSeen) + ",\"last_seen\":" + std::to_string(d.lastSeen);
        o += ",\"rssi_db\":" + num3(d.rssiDb) + ",\"snr_db\":" + num3(d.snrDb);
        o += ",\"values\":{";
        bool f1 = true;
        for (const auto& f : d.latest) {
            if (!f1) o += ",";
            f1 = false;
            jsonEscape(o, f.key.c_str());
            o += ":";
            if (f.numeric) o += numText(f.num); else jsonEscape(o, f.text.c_str());
        }
        o += "}}";
    }
    return o + "]\n";
}

std::string sensorsMqtt(const SensorsTelemetry& t, const std::string& prefix) {
    auto clean = [](std::string s) { for (auto& c : s) if (c == '/' || c == ' ' || c == '#' || c == '+') c = '_'; return s; };
    std::string o;
    for (const auto& d : t.devices) {
        std::string topic = prefix + "/devices/" + clean(d.model);
        if (!d.channel.empty()) topic += "/" + clean(d.channel);
        if (!d.id.empty()) topic += "/" + clean(d.id);
        for (const auto& f : d.latest) {
            if (f.key == "model" || f.key == "id" || f.key == "channel") continue;
            o += topic + "/" + clean(f.key) + " " + f.text + "\n";
        }
        o += topic + "/rssi " + num3(d.rssiDb) + "\n";
    }
    return o;
}

ModeTuning sensorsTuning() {
    ModeTuning t;
    t.stdMode = 243; t.id = "sensors"; t.name = "Sub-GHz sensors";
    t.minMhz = 300; t.maxMhz = 928; t.defMhz = 433.92;
    t.sampleRate = 250e3;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 200e3;
    t.tuneOffsetHz = 0;
    t.carrierAtCentre = true;   // OOK carriers can sit on the centre: the DC removal would take them with the spike
    return t;
}

} // namespace dect2
