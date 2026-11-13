// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tx/tx_studio.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <thread>

namespace dect2 {

const char* studioFormatName(StudioFormat f) {
    switch (f) {
    case StudioFormat::Cu8: return "cu8 (unsigned 8-bit I/Q)";
    case StudioFormat::Cs8: return "cs8 (signed 8-bit I/Q)";
    case StudioFormat::Cs16: return "cs16 (signed 16-bit I/Q)";
    case StudioFormat::Cf32: return "cf32 (32-bit float I/Q)";
    case StudioFormat::SigMF: return "SigMF (cf32 + .sigmf-meta)";
    case StudioFormat::WavAudio: return "WAV audio (mono, real part)";
    }
    return "";
}

const char* studioFormatExt(StudioFormat f) {
    switch (f) {
    case StudioFormat::Cu8: return ".cu8";
    case StudioFormat::Cs8: return ".cs8";
    case StudioFormat::Cs16: return ".cs16";
    case StudioFormat::Cf32: return ".cf32";
    case StudioFormat::SigMF: return ".sigmf-data";
    case StudioFormat::WavAudio: return ".wav";
    }
    return "";
}

uint64_t studioFileBytes(StudioFormat f, uint64_t n) {
    switch (f) {
    case StudioFormat::Cu8:
    case StudioFormat::Cs8: return n * 2;
    case StudioFormat::Cs16: return n * 4;
    case StudioFormat::Cf32:
    case StudioFormat::SigMF: return n * 8;
    case StudioFormat::WavAudio: return 44 + n * 2;
    }
    return 0;
}

bool studioAudioFormatOk(double rateHz) { return rateHz > 0 && rateHz <= 100000.0; }

bool studioPlayFormat(StudioFormat f, FileFormat& out) {
    switch (f) {
    case StudioFormat::Cu8: out = FileFormat::CU8; return true;
    case StudioFormat::Cs8: out = FileFormat::CS8; return true;
    case StudioFormat::Cs16: out = FileFormat::CI16; return true;
    case StudioFormat::Cf32:
    case StudioFormat::SigMF: out = FileFormat::CF32; return true;
    case StudioFormat::WavAudio: return false;
    }
    return false;
}

bool studioGenerate(const TxModeSpec& mode, const TxRequest& rin, double rateHz, double seconds, StudioSignal& out, std::string& err) {
    out = StudioSignal();
    TxRequest r = rin;
    r.modeId = mode.id;
    r.carrierTune = false;
    if (r.audioSrc == 0) r.audioSrc = 2;          // never the microphone
    r.videoSrc = 0;
    r.pullAudio = nullptr;
    r.pullVideo = nullptr;
    r.loop = false;
    if (rateHz <= 0) rateHz = txRequestRate(mode, r, TxRateLimits{});
    if (rateHz <= 0) { err = "no sample rate"; return false; }
    r.tune.sampleRate = rateHz;
    std::unique_ptr<TxGenerator> g;
    try {
        g = makeTxGenerator(mode, r, rateHz, err);
    } catch (const std::exception& e) { err = std::string("the generator failed: ") + e.what(); return false; }
    if (!g) { if (err.empty()) err = "this mode cannot make a signal with these settings"; return false; }
    const double rate = g->sampleRate() > 0 ? g->sampleRate() : rateHz;
    const double want = std::max(0.001, seconds) * rate;
    size_t total = (size_t)std::min<double>(want, (double)kStudioMaxSamples);
    out.capped = want > (double)kStudioMaxSamples;
    out.iq.resize(total);
    size_t made = 0;
    try {
        while (made < total) {
            const size_t n = std::min<size_t>(32768, total - made);
            g->generate(out.iq.data() + made, n);
            txOutputStage(r, out.iq.data() + made, n, made, rate);
            txSoftLimit(out.iq.data() + made, n, nullptr);
            made += n;
            if (g->finished()) { out.finite = true; break; }
        }
    } catch (const std::exception& e) { err = std::string("the generator failed: ") + e.what(); out = StudioSignal(); return false; }
    out.iq.resize(made);
    out.rateHz = rate;
    out.seconds = (double)made / rate;
    out.note = r.fileNote;
    if (made == 0) { err = "the generator made no samples"; return false; }
    return true;
}

StudioMeasure studioMeasure(const StudioSignal& s, const char* modeId) {
    StudioMeasure m;
    if (s.iq.size() < 64 || s.rateHz <= 0) return m;
    m.valid = true;
    m.rateHz = s.rateHz;
    m.samples = s.iq.size();
    m.seconds = s.seconds;
    const auto psd = txm::welchPsd(s.iq.data(), s.iq.size(), s.rateHz, 4096, 64);
    m.obw99Hz = txm::obwHz(psd, 0.99);
    m.obw26Hz = txm::obwBelowPeakHz(psd, 26.0);
    m.centroidHz = txm::centroidHz(psd);
    const auto p = txm::papr(s.iq.data(), s.iq.size());
    m.peakDb = p.peakDb; m.rmsDb = p.rmsDb; m.paprDb = p.paprDb;
    m.papr999Db = txm::paprAtProbabilityDb(s.iq.data(), s.iq.size(), 0.001);
    const double chan = std::max(m.obw99Hz, psd.binHz * 2);
    m.acpr = txm::acpr(psd, chan, chan, m.centroidHz);
    m.mask = txm::maskForMode(modeId, chan);
    m.maskResult = txm::checkMask(psd, m.mask, chan, m.centroidHz);
    return m;
}

namespace {

std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string num(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.10g", std::isfinite(v) ? v : 0.0);
    return b;
}

std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

// the fields shared by the SigMF global block and the .meta.json: what was made and how
std::string onairFields(const StudioWriteInfo& in, const StudioSignal& s, StudioFormat f, const char* indent) {
    std::string o;
    auto kv = [&](const std::string& k, const std::string& v, bool last = false) { o += std::string(indent) + "\"" + k + "\": " + v + (last ? "\n" : ",\n"); };
    auto str = [&](const std::string& v) { return "\"" + jesc(v) + "\""; };
    kv("onair:tool", str("OnAir TX studio"));
    kv("onair:created_utc", str(utcNow()));
    if (in.mode) { kv("onair:mode", str(in.mode->id)); kv("onair:mode_name", str(in.mode->name)); kv("onair:generator", str(in.mode->generator)); }
    kv("onair:format", str(studioFormatName(f)));
    kv("onair:samples", num((double)s.iq.size()));
    kv("onair:duration_s", num(s.seconds));
    kv("onair:transmitted", "false");
    if (in.req) {
        const TxRequest& r = *in.req;
        std::string p = "{";
        auto add = [&](const char* k, const std::string& v) { if (!v.empty()) p += std::string(p.size() > 1 ? ", " : "") + "\"" + k + "\": " + str(v); };
        add("callsign", r.callsign); add("grid", r.grid); add("text", r.text); add("dest", r.dest); add("path", r.path);
        add("file", r.file); add("name", r.name); add("provider", r.provider); add("network", r.network);
        p += "}";
        kv("onair:payload", p);
        std::string q = "{";
        auto addn = [&](const char* k, double v) { q += std::string(q.size() > 1 ? ", " : "") + "\"" + k + "\": " + num(v); };
        addn("kind", r.kind); addn("baudIdx", r.baudIdx); addn("shiftIdx", r.shiftIdx); addn("variant", r.variant); addn("wpm", r.wpm);
        addn("dbm", r.dbm); addn("function", r.function); addn("address", (double)r.address); addn("toneHz", r.toneHz);
        addn("bandwidthMhz", r.bandwidthMhz); addn("deviationHz", r.deviationHz);
        for (const auto& kvp : r.opt) addn(("opt." + kvp.first).c_str(), kvp.second);
        for (size_t i = 0; i < 8; i++) addn(("synth.modeOpt" + std::to_string(i)).c_str(), r.synth.modeOpt[i]);
        q += "}";
        kv("onair:parameters", q);
    }
    if (in.sweep) kv("onair:sweep", "{\"key\": " + str(in.sweepKey) + ", \"value\": " + num(in.sweepValue) + "}");
    if (in.impair && in.impair->any())
        kv("onair:impairment", "{\"snr_db\": " + num(in.impair->snrDb) + ", \"cfo_hz\": " + num(in.impair->cfoHz) + ", \"fading\": " + num(in.impair->fading) +
                                   ", \"doppler_hz\": " + num(in.impair->dopplerHz) + ", \"delay_us\": " + num(in.impair->delayUs) + ", \"seed\": " + num(in.impair->seed) + "}");
    if (in.measure && in.measure->valid) {
        const auto& m = *in.measure;
        kv("onair:measure", "{\"obw99_hz\": " + num(m.obw99Hz) + ", \"obw26_hz\": " + num(m.obw26Hz) + ", \"papr_db\": " + num(m.paprDb) + ", \"peak_dbfs\": " + num(m.peakDb) +
                                ", \"rms_dbfs\": " + num(m.rmsDb) + ", \"acpr_lower_db\": " + (m.acpr.lowerValid ? num(m.acpr.lowerDb) : "null") +
                                ", \"acpr_upper_db\": " + (m.acpr.upperValid ? num(m.acpr.upperDb) : "null") + "}", true);
    } else {
        o.erase(o.size() - 2, 2);   // drop the trailing ",\n"
        o += "\n";
    }
    return o;
}

bool put(FILE* f, const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }

void le16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void le32(uint8_t* p, uint32_t v) { le16(p, v & 0xffff); le16(p + 2, v >> 16); }

} // namespace

bool studioWrite(const std::string& base, const StudioSignal& s, StudioFormat f, const StudioWriteInfo& info, std::string& dataPath, std::string& metaPath, std::string& err) {
    namespace fs = std::filesystem;
    dataPath = base + studioFormatExt(f);
    metaPath = f == StudioFormat::SigMF ? base + ".sigmf-meta" : base + ".meta.json";
    if (s.iq.empty()) { err = "no signal to write"; return false; }
    if (f == StudioFormat::WavAudio && !studioAudioFormatOk(s.rateHz)) { err = "WAV audio needs a sample rate of 100 kHz or less"; return false; }
    std::error_code ec;
    const fs::path parent = fs::path(dataPath).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    FILE* fp = fopen(dataPath.c_str(), "wb");
    if (!fp) { err = "cannot write " + dataPath + ": " + strerror(errno); return false; }
    bool ok = true;
    std::vector<uint8_t> buf;
    const size_t n = s.iq.size(), chunk = 1 << 16;
    auto clampi = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };
    if (f == StudioFormat::WavAudio) {
        double pk = 0;
        for (const auto& v : s.iq) pk = std::max<double>(pk, std::fabs(v.real()));
        const double sc = 32767.0 * (pk > 1.0 ? 1.0 / pk : 1.0);
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
        const uint32_t rate = (uint32_t)std::lround(s.rateHz);
        le32(h + 4, (uint32_t)(36 + n * 2)); le32(h + 24, rate); le32(h + 28, rate * 2); le16(h + 32, 2); le16(h + 34, 16);
        memcpy(h + 36, "data", 4); le32(h + 40, (uint32_t)(n * 2));
        ok = put(fp, h, 44);
        for (size_t i = 0; ok && i < n; i += chunk) {
            const size_t m = std::min(chunk, n - i);
            buf.resize(m * 2);
            for (size_t k = 0; k < m; k++) le16(&buf[k * 2], (uint16_t)(int16_t)std::lround(clampi(s.iq[i + k].real() * sc, -32768, 32767)));
            ok = put(fp, buf.data(), buf.size());
        }
    } else {
        for (size_t i = 0; ok && i < n; i += chunk) {
            const size_t m = std::min(chunk, n - i);
            const cf32* x = s.iq.data() + i;
            switch (f) {
            case StudioFormat::Cu8:
                buf.resize(m * 2);
                for (size_t k = 0; k < m; k++) {
                    buf[2 * k] = (uint8_t)std::lround(clampi(127.5 + 127.5 * x[k].real(), 0, 255));
                    buf[2 * k + 1] = (uint8_t)std::lround(clampi(127.5 + 127.5 * x[k].imag(), 0, 255));
                }
                break;
            case StudioFormat::Cs8:
                buf.resize(m * 2);
                for (size_t k = 0; k < m; k++) {
                    buf[2 * k] = (uint8_t)(int8_t)std::lround(clampi(127.0 * x[k].real(), -127, 127));
                    buf[2 * k + 1] = (uint8_t)(int8_t)std::lround(clampi(127.0 * x[k].imag(), -127, 127));
                }
                break;
            case StudioFormat::Cs16:
                buf.resize(m * 4);
                for (size_t k = 0; k < m; k++) {
                    le16(&buf[4 * k], (uint16_t)(int16_t)std::lround(clampi(32767.0 * x[k].real(), -32767, 32767)));
                    le16(&buf[4 * k + 2], (uint16_t)(int16_t)std::lround(clampi(32767.0 * x[k].imag(), -32767, 32767)));
                }
                break;
            default:   // cf32, SigMF cf32_le (little-endian hosts: a copy)
                buf.resize(m * 8);
                for (size_t k = 0; k < m; k++) {
                    const float re = x[k].real(), im = x[k].imag();
                    uint32_t a, b;
                    memcpy(&a, &re, 4); memcpy(&b, &im, 4);
                    le32(&buf[8 * k], a); le32(&buf[8 * k + 4], b);
                }
            }
            ok = put(fp, buf.data(), buf.size());
        }
    }
    if (fclose(fp) != 0) ok = false;
    if (!ok) { err = "write failed: " + dataPath; return false; }

    FILE* mp = fopen(metaPath.c_str(), "wb");
    if (!mp) { err = "cannot write " + metaPath; return false; }
    std::string j;
    const std::string fields = onairFields(info, s, f, f == StudioFormat::SigMF ? "    " : "  ");
    if (f == StudioFormat::SigMF) {
        j = "{\n  \"global\": {\n    \"core:datatype\": \"cf32_le\",\n    \"core:sample_rate\": " + num(s.rateHz) + ",\n    \"core:version\": \"1.0.0\",\n" +
            "    \"core:description\": \"" + jesc(std::string("OnAir TX studio: ") + (info.mode ? info.mode->id : "")) + "\",\n    \"core:recorder\": \"OnAir TX studio\",\n" + fields +
            "  },\n  \"captures\": [{\"core:sample_start\": 0" + (info.centerHz > 0 ? ", \"core:frequency\": " + num(info.centerHz) : std::string()) + ", \"core:datetime\": \"" + utcNow() + "\"}],\n  \"annotations\": []\n}\n";
    } else {
        const char* dt = f == StudioFormat::Cu8 ? "cu8" : f == StudioFormat::Cs8 ? "cs8" : f == StudioFormat::Cs16 ? "cs16" : f == StudioFormat::Cf32 ? "cf32" : "wav-mono-16";
        j = "{\n  \"data_file\": \"" + jesc(fs::path(dataPath).filename().string()) + "\",\n  \"datatype\": \"" + dt + "\",\n  \"sample_rate\": " + num(s.rateHz) + ",\n  \"center_frequency_hz\": " +
            num(info.centerHz) + ",\n" + fields + "}\n";
    }
    ok = put(mp, j.data(), j.size());
    fclose(mp);
    if (!ok) { err = "write failed: " + metaPath; return false; }
    return true;
}

StudioLoopback studioLoopback(const TxModeSpec& m, const StudioSignal& s, double timeoutSec, const std::atomic<bool>* cancel) {
    namespace fs = std::filesystem;
    StudioLoopback d;
    if (s.iq.empty() || s.rateHz <= 0) { d.summary = "no signal"; return d; }
    // a temporary cf32 file the engine plays from
    std::error_code ec;
    char nm[64];
    snprintf(nm, sizeof nm, "onair_txstudio_%llu.cf32", (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string path = (fs::temp_directory_path(ec) / nm).string();
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { d.summary = "cannot write the temporary file"; return d; }
    const bool wrote = fwrite(s.iq.data(), sizeof(cf32), s.iq.size(), fp) == s.iq.size();
    fclose(fp);
    if (!wrote) { fs::remove(path, ec); d.summary = "cannot write the temporary file"; return d; }
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "TX studio";
    const ModeTuning* mt = modeTuning(m.stdMode);
    TuneSettings t;
    t.sampleRate = s.rateHz;
    t.centerHz = 100e6;
    t.bandwidthMhz = m.stdMode == 3 || m.stdMode == 6 ? 6 : m.stdMode == 4 ? 1.536 : m.stdMode == 0 ? 8 : mt ? mt->bandwidthMhz : 8;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = s.rateHz; fo.loop = true;
    e.setStandard(m.stdMode == 0 ? m.variant : m.stdMode);
    if (!e.start(dev, t, fo)) { fs::remove(path, ec); d.summary = "the receiver did not start"; return d; }
    d.ran = true;
    const bool needServices = m.stdMode == 0 || m.stdMode == 3 || m.stdMode == 6 || m.stdMode == 8 || m.stdMode == 9;   // TV modes carry a transport stream
    RxTelemetry rx;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < timeoutSec && !(cancel && cancel->load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            if (rx.state == 2 && rx.dataValid) d.locked = true;
            d.snrDb = rx.dataSnrDb;
            d.blocks = rx.blocksOk;
        }
        if (needServices) {
            const auto ts = e.tsSnapshot();
            for (const auto& sv : ts.services) {
                bool v = false, a = false;
                for (const auto& x : sv.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
                d.services |= (v && a) || (m.stdMode == 0 && m.variant == 1);
            }
        }
        if (d.locked && (d.services || !needServices)) break;
    }
    e.stop();
    fs::remove(path, ec);
    char b[160];
    const bool cut = cancel && cancel->load();
    snprintf(b, sizeof b, "%s: lock %s, SNR %.1f dB, %llu blocks%s", cut ? "cancelled" : d.locked ? "decoded" : "NOT decoded", d.locked ? "yes" : "no", d.snrDb,
             (unsigned long long)d.blocks, needServices ? (d.services ? ", programmes found" : ", no programmes") : "");
    d.summary = b;
    return d;
}

void studioSetParam(TxRequest& r, const std::string& k, double v) {
    if (k == "wpm") r.wpm = (int)v;
    else if (k == "toneHz") r.toneHz = v;
    else if (k == "baudIdx") r.baudIdx = (int)v;
    else if (k == "shiftIdx") r.shiftIdx = (int)v;
    else if (k == "kind") r.kind = (int)v;
    else if (k == "variant") r.variant = (int)v;
    else if (k == "dbm") r.dbm = (int)v;
    else if (k == "function") r.function = (int)v;
    else if (k == "slot") r.slot = (int)v;
    else if (k == "colorCode") r.colorCode = (int)v;
    else if (k == "talkgroup") r.talkgroup = (int)v;
    else if (k == "address") r.address = (uint32_t)v;
    else if (k == "serviceId") r.serviceId = (int)v;
    else if (k == "bandwidthMhz") r.bandwidthMhz = v;
    else if (k == "deviationHz") r.deviationHz = v;
    else if (k == "preemphUs") r.preemphUs = v;
    else if (k == "lat") r.lat = v;
    else if (k == "lon") r.lon = v;
    else r.opt[k] = v;
}

std::vector<double> studioParseList(const std::string& s) {
    std::vector<double> v;
    auto trim = [](std::string t) {
        size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    auto parse = [](const std::string& t, double& out) {
        char* end = nullptr;
        out = strtod(t.c_str(), &end);
        return !t.empty() && end && *end == 0 && std::isfinite(out);
    };
    if (s.find(':') != std::string::npos && s.find(',') == std::string::npos) {   // first:last:step
        std::vector<double> p;
        size_t pos = 0;
        while (true) {
            const size_t q = s.find(':', pos);
            double x;
            if (!parse(trim(s.substr(pos, q == std::string::npos ? q : q - pos)), x)) return {};
            p.push_back(x);
            if (q == std::string::npos) break;
            pos = q + 1;
        }
        if (p.size() != 3 || p[2] == 0 || (p[1] - p[0]) / p[2] < 0) return {};
        const size_t n = (size_t)std::floor((p[1] - p[0]) / p[2] + 1e-9) + 1;
        if (n > 200) return {};
        for (size_t i = 0; i < n; i++) v.push_back(p[0] + (double)i * p[2]);
        return v;
    }
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t q = s.find(',', pos);
        const std::string t = trim(s.substr(pos, q == std::string::npos ? q : q - pos));
        if (!t.empty()) {
            double x;
            if (!parse(t, x)) return {};
            v.push_back(x);
        }
        if (q == std::string::npos) break;
        pos = q + 1;
    }
    if (v.size() > 200) v.clear();
    return v;
}

std::vector<StudioBatchItem> studioBatch(const TxModeSpec& mode, const TxRequest& base, const std::string& key, const std::vector<double>& values, double rateHz, double seconds,
                                         const txm::Impair& impair, StudioFormat f, const std::string& folder, const std::string& fileBase, double centerHz,
                                         const std::function<void(size_t, size_t)>& progress, const std::atomic<bool>* cancel) {
    std::vector<StudioBatchItem> out;
    for (size_t i = 0; i < values.size(); i++) {
        if (cancel && cancel->load()) break;
        if (progress) progress(i, values.size());
        StudioBatchItem it;
        it.value = values[i];
        TxRequest r = base;
        txm::Impair im = impair;
        if (key == "impair.snr") im.snrDb = values[i];
        else if (key == "impair.cfo") im.cfoHz = values[i];
        else studioSetParam(r, key, values[i]);
        StudioSignal sig;
        if (!studioGenerate(mode, r, rateHz, seconds, sig, it.err)) { out.push_back(it); continue; }
        if (im.any()) txm::applyImpairments(sig.iq, sig.rateHz, im);
        const StudioMeasure ms = studioMeasure(sig, mode.id);
        char vb[48];
        snprintf(vb, sizeof vb, "%g", values[i]);
        std::string tag = key + "_" + vb;
        for (auto& c : tag) if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') c = '_';
        std::string dir = folder;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        StudioWriteInfo wi;
        wi.mode = &mode; wi.req = &r; wi.measure = &ms; wi.impair = &im; wi.centerHz = centerHz;
        wi.sweep = true; wi.sweepKey = key; wi.sweepValue = values[i];
        it.seconds = sig.seconds;
        it.ok = studioWrite(dir + fileBase + "_" + tag, sig, f, wi, it.dataPath, it.metaPath, it.err);
        out.push_back(it);
    }
    if (progress) progress(values.size(), values.size());
    return out;
}

} // namespace dect2
