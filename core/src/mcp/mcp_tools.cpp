// The MCP tools. Each one is a thin layer over the remote-control host (RemoteApiHost) and the engine's signal identifier; no tool touches the
// file system, a shell, keys, credentials or settings, and the transmit tools exist only when Options::allowTx is on.
#include "dect2/bandplan/bandplan.h"
#include "dect2/channels/freq_db.h"
#include "dect2/codec/codec_paths.h"
#include "dect2/codec/speech_codec.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/sig_ident.h"
#include "mcp_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace dect2 {
namespace mcp {

namespace {

using Clock = std::chrono::steady_clock;
double nowS() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---------------------------------------------------------------- schema helpers
Json prop(const char* type, const char* desc) {
    Json p = Json::object();
    p.setStr("type", type);
    p.setStr("description", desc);
    return p;
}
Json enumProp(const char* desc, std::initializer_list<const char*> vals) {
    Json p = prop("string", desc);
    Json e = Json::array();
    for (const char* v : vals) e.push(Json::string(v));
    p.set("enum", e);
    return p;
}
struct Schema {
    Json props = Json::object();
    Json req = Json::array();
    Schema& add(const char* name, Json p, bool required = false) {
        props.set(name, std::move(p));
        if (required) req.push(Json::string(name));
        return *this;
    }
    Json done() const {
        Json s = Json::object();
        s.setStr("type", "object");
        s.set("properties", props);
        if (!req.arr.empty()) s.set("required", req);
        s.setBool("additionalProperties", false);
        return s;
    }
};

// ---------------------------------------------------------------- argument helpers
double needNum(const Json& a, const char* k) {
    const Json* j = a.get(k);
    if (!j) throw ToolError(std::string("missing argument '") + k + "'");
    const double v = a.numOr(k, NAN);
    if (!std::isfinite(v)) throw ToolError(std::string("argument '") + k + "' must be a number");
    return v;
}
// A frequency given as frequency_mhz (or frequency_hz / khz). required: throw when none.
bool freqArg(const Json& a, double& hz, const char* mhzKey = "frequency_mhz", const char* hzKey = "frequency_hz", bool required = true) {
    if (a.has(mhzKey)) { hz = a.numOr(mhzKey, NAN) * 1e6; }
    else if (a.has(hzKey)) { hz = a.numOr(hzKey, NAN); }
    else if (a.has("frequency_khz")) { hz = a.numOr("frequency_khz", NAN) * 1e3; }
    else { if (required) throw ToolError(std::string("give ") + mhzKey + " (MHz)"); return false; }
    if (!std::isfinite(hz) || hz < 0 || hz > 1e12) throw ToolError("the frequency is not a valid number");
    return true;
}

// ---------------------------------------------------------------- host access
Json status(ToolCtx& c) { return hostJson(c.host.statusJson()); }
bool isRunning(ToolCtx& c) { return status(c).boolOr("running", false); }

struct SpecInfo {
    bool ok = false;
    double centreHz = 0, rateHz = 0, rms = 0, clip = 0;
    std::vector<float> db;
    uint64_t seq = 0;
};
SpecInfo spectrum(ToolCtx& c, int bins) {
    SpecInfo s;
    uint64_t seq = 0;
    const Json j = hostJson(c.host.spectrumJson(bins, seq));
    const Json* d = j.get("dbfs");
    if (!d || !d->isArr() || d->arr.empty()) return s;
    s.ok = true;
    s.seq = seq;
    s.centreHz = j.numOr("centerHz", 0);
    s.rateHz = j.numOr("sampleRateHz", 0);
    s.rms = j.numOr("rmsDbfs", -200);
    s.clip = j.numOr("clipFraction", 0);
    for (const Json& v : d->arr) s.db.push_back((float)v.num);
    return s;
}

void needSpectrumWait(ToolCtx& c, SpecInfo& s, int bins, double sec) {
    const double t0 = nowS();
    for (;;) {
        s = spectrum(c, bins);
        if (s.ok && s.rateHz > 0) return;
        if (nowS() - t0 > sec) return;
        sleepMs(150);
    }
}

void ensureRunning(ToolCtx& c) {
    if (isRunning(c)) return;
    std::string err;
    if (!c.host.start(-1, err)) throw ToolError("could not start the receiver: " + (err.empty() ? std::string("unknown error") : err));
    SpecInfo s;
    needSpectrumWait(c, s, 64, 6);
}

Json statusSummary(ToolCtx& c) {
    Json s = status(c);
    Json o = Json::object();
    for (const char* k : {"running", "device", "deviceKind", "family", "mode", "modeId", "sub", "frequencyHz", "sampleRateHz", "gainDb", "gainMinDb", "gainMaxDb", "agc", "error", "loadPct", "droppedSamples"})
        if (const Json* v = s.get(k)) o.set(k, *v);
    if (s.has("frequencyHz")) o.setNum("frequencyMhz", s.numOr("frequencyHz", 0) / 1e6);
    return o;
}

// ---------------------------------------------------------------- modes
// The host's list of modes, with number, id, band and typical frequencies filled in from the tables.
struct ModeRow {
    int family = 0;
    std::string id, name, sub;
    double defMhz = 0, minMhz = 0, maxMhz = 0;
};
std::vector<ModeRow> modeRows(ToolCtx& c) {
    std::vector<ModeRow> rows;
    const Json j = hostJson(c.host.modesJson());
    // name -> tuning
    struct T { std::string id; double mn, mx, df; };
    std::vector<std::pair<std::string, T>> byName;
    for (int s = 8; s < 400; s++) {
        const ModeTuning* mt = modeTuning(s);
        if (mt && *mt->id) byName.push_back({lower(mt->name), {mt->id, mt->minMhz, mt->maxMhz, mt->defMhz}});
    }
    int n = 0;
    const SubMode* sm = subModes(n);
    for (int i = 0; i < n; i++) {
        const ModeTuning* mt = modeTuning(sm[i].stdMode);
        byName.push_back({lower(sm[i].name), {sm[i].id, mt ? mt->minMhz : 0, mt ? mt->maxMhz : 0, sm[i].defMhz}});
    }
    static const struct { int f; const char* id; const char* name; double mhz; } builtin[] = {
        {0, "dvb", "DVB-T / T2 (automatic)", 522}, {1, "t2", "DVB-T2", 522}, {2, "t", "DVB-T", 522}, {3, "atsc", "ATSC", 575}, {4, "dab", "DAB / DAB+", 225.648},
        {5, "atsc3", "ATSC 3.0", 575}, {6, "isdbt", "ISDB-T", 527}, {7, "fm", "FM broadcast", 100}};
    if (j.isArr()) {
        for (const Json& e : j.arr) {
            ModeRow r;
            r.family = (int)e.numOr("family", 0);
            r.name = e.strOr("name");
            r.id = e.strOr("id");
            const Json* sv = e.get("sub");
            if (sv && sv->isStr()) r.sub = sv->str;
            r.defMhz = e.numOr("defaultMhz", 0);
            const std::string ln = lower(r.name);
            for (const auto& b : byName) if (b.first == ln) {
                if (r.id.empty()) r.id = r.sub.empty() ? b.second.id : r.sub;
                if (r.defMhz <= 0) r.defMhz = b.second.df;
                r.minMhz = b.second.mn; r.maxMhz = b.second.mx;
                break;
            }
            if (r.id.empty()) for (const auto& b : builtin) if (b.f == r.family && lower(b.name) == ln) { r.id = b.id; if (r.defMhz <= 0) r.defMhz = b.mhz; }
            if (r.id.empty()) r.id = r.sub;
            rows.push_back(r);
        }
    }
    return rows;
}

Json modeRowJson(const ModeRow& r) {
    Json o = Json::object();
    o.setNum("number", r.family);
    o.setStr("id", r.id);
    o.setStr("name", r.name);
    if (!r.sub.empty()) o.setStr("sub", r.sub);
    if (r.defMhz > 0) o.setNum("typical_mhz", r.defMhz);
    if (r.maxMhz > r.minMhz && r.maxMhz < 100000) { o.setNum("band_min_mhz", r.minMhz); o.setNum("band_max_mhz", r.maxMhz); }
    // typical frequencies from the frequency database
    Json fr = Json::array();
    const std::string mid = r.sub.empty() ? r.id : r.sub;
    if (!mid.empty()) {
        const auto& all = builtinFrequencies();
        int k = 0;
        for (const FreqEntry& e : all) {
            if (e.mode != mid && e.mode != r.id) continue;
            Json f = Json::object();
            f.setNum("mhz", (double)e.hz / 1e6);
            f.setStr("name", e.name);
            fr.push(f);
            if (++k >= 6) break;
        }
    }
    if (!fr.arr.empty()) o.set("known_frequencies", fr);
    return o;
}

// Finds a mode by number, id, sub id or name. Returns nullptr when not found; `ambiguous` lists the candidates when several fit.
const ModeRow* findRow(const std::vector<ModeRow>& rows, const std::string& q0, std::vector<std::string>* ambiguous = nullptr) {
    const std::string q = lower(q0);
    if (q.empty()) return nullptr;
    char* e = nullptr;
    const long v = strtol(q.c_str(), &e, 10);
    if (e && *e == 0 && e != q.c_str()) for (const ModeRow& r : rows) if (r.family == v && r.sub.empty()) return &r;
    for (const ModeRow& r : rows) if (lower(r.id) == q || (!r.sub.empty() && lower(r.sub) == q)) return &r;
    for (const ModeRow& r : rows) if (lower(r.name) == q) return &r;
    std::vector<const ModeRow*> hits;
    for (const ModeRow& r : rows) if (lower(r.name).compare(0, q.size(), q) == 0 || lower(r.id).compare(0, q.size(), q) == 0) hits.push_back(&r);
    if (hits.empty()) for (const ModeRow& r : rows) if (lower(r.name).find(q) != std::string::npos || lower(r.id).find(q) != std::string::npos) hits.push_back(&r);
    if (hits.size() == 1) return hits[0];
    if (hits.size() > 1 && ambiguous) for (size_t i = 0; i < hits.size() && i < 8; i++) ambiguous->push_back(hits[i]->name + " (" + hits[i]->id + ")");
    return nullptr;
}

const ModeRow& needRow(const std::vector<ModeRow>& rows, const std::string& q) {
    std::vector<std::string> amb;
    const ModeRow* r = findRow(rows, q, &amb);
    if (r) return *r;
    if (!amb.empty()) {
        std::string m = "several modes match '" + q + "': ";
        for (size_t i = 0; i < amb.size(); i++) m += (i ? "; " : "") + amb[i];
        throw ToolError(m + ". Use the id from list_modes.");
    }
    throw ToolError("no mode '" + q + "' (call list_modes to see the ids)");
}

// Switch the receiver to a mode. The receiver restarts when it was running.
void applyMode(ToolCtx& c, const ModeRow& r) {
    const bool was = isRunning(c);
    std::string err;
    if (was && !c.host.stop(err)) throw ToolError("could not stop the receiver: " + err);
    err.clear();
    if (!c.host.setMode(r.family, r.sub, err)) throw ToolError("could not set the mode: " + err);
    (void)was;
}

// ---------------------------------------------------------------- identification
Json candidateJson(const sigid::Candidate& cd) {
    Json o = Json::object();
    o.setStr("name", cd.name);
    o.setStr("mode_id", cd.subId.empty() ? cd.modeId : cd.subId);
    o.setStr("family", cd.family);
    o.setNum("confidence", std::round(cd.confidence * 100) / 100);
    o.setNum("score", std::round(cd.score));
    if (!cd.why.empty()) o.setStr("why", cd.why);
    if (cd.verify == sigid::VerifyConfirmed) o.setStr("verify", "confirmed");
    else if (cd.verify == sigid::VerifyFailed) o.setStr("verify", "failed");
    return o;
}

const char* verdictName(int v) { return v == sigid::VerdictLikely ? "likely" : v == sigid::VerdictAmbiguous ? "ambiguous" : "unknown"; }

Json resultJson(const sigid::Result& r, size_t topN, bool evidence) {
    Json o = Json::object();
    o.setStr("verdict", verdictName(r.verdict));
    o.setStr("headline", r.headline);
    o.setStr("summary", r.summary);
    Json cs = Json::array();
    for (size_t i = 0; i < r.cands.size() && i < topN; i++) {
        Json cj = candidateJson(r.cands[i]);
        if (evidence && !r.cands[i].evidence.empty()) {
            Json ev = Json::array();
            for (size_t k = 0; k < r.cands[i].evidence.size() && k < 6; k++) ev.push(Json::string(r.cands[i].evidence[k]));
            cj.set("evidence", ev);
        }
        cs.push(cj);
    }
    o.set("candidates", cs);
    if (r.f.valid) {
        Json f = Json::object();
        f.setNum("centre_mhz", r.f.rfHz / 1e6);
        f.setNum("occupied_bandwidth_khz", std::round(r.f.obwHz) / 1e3);
        f.setNum("bandwidth_3db_khz", std::round(r.f.bw3Hz) / 1e3);
        f.setNum("snr_db", std::round(r.f.snrDb * 10) / 10);
        f.setStr("modulation", sigid::modName(r.f.mod));
        if (r.f.symRate > 0) f.setNum("symbol_rate_baud", std::round(r.f.symRate));
        f.setBool("continuous", r.f.duty > 0.9);
        o.set("measured", f);
    } else if (!r.f.why.empty()) o.setStr("not_measured", r.f.why);
    if (!r.priors.empty()) {
        Json p = Json::array();
        for (const std::string& s : r.priors) p.push(Json::string(s));
        o.set("frequency_database", p);
    }
    return o;
}

struct SigCtx {
    double rc = 0, rate = 0;   // RF centre of the samples, sample rate
};
SigCtx sigContext(ToolCtx& c) {
    if (!c.eng) throw ToolError("signal identification is not available in this build / host");
    ensureRunning(c);
    SpecInfo s;
    needSpectrumWait(c, s, 64, 6);
    SigCtx x;
    x.rate = c.eng->sampleRate() > 0 ? c.eng->sampleRate() : s.rateHz;
    x.rc = s.ok && s.centreHz > 0 ? s.centreHz : status(c).numOr("frequencyHz", 0);
    if (x.rate <= 0) throw ToolError("the receiver is not delivering samples yet; check get_receiver_health");
    return x;
}

struct Found { double rfHz = 0, bwHz = 0, snr = 0, power = 0; sigid::Result res; };

// "Identify all signals" in the current capture window, at most maxN
std::vector<Found> identifyAll(ToolCtx& c, const SigCtx& x, size_t maxN) {
    sigid::Capture& cap = c.eng->sigid();
    std::vector<sigid::Capture::Found> f;
    uint64_t seq = 0;
    cap.resultAll(f, seq);   // the number of the last result, so only a newer one counts
    const double sec = sigid::captureSeconds(x.rate) * c.opt.scanDwellScale;
    cap.startAll(x.rate, x.rc, sec, maxN);
    const double t0 = nowS();
    for (;;) {
        if (cap.resultAll(f, seq)) break;
        if (nowS() - t0 > sec + 20) { cap.cancel(); throw ToolError("the identifier did not finish in time (is the receiver delivering samples?)"); }
        sleepMs(100);
    }
    std::vector<Found> out;
    for (auto& e : f) {
        Found g;
        g.rfHz = x.rc + e.region.centreHz;
        g.bwHz = e.region.bwHz;
        g.snr = e.region.snrDb;
        g.power = e.region.powerDb;
        g.res = std::move(e.res);
        out.push_back(std::move(g));
    }
    return out;
}

Json foundJson(const Found& g, size_t topN = 3) {
    Json o = Json::object();
    o.setNum("centre_mhz", std::round(g.rfHz) / 1e6);
    o.setNum("bandwidth_khz", std::round(g.bwHz / 100) / 10);
    o.setNum("snr_db", std::round(g.snr * 10) / 10);
    o.setNum("power_db", std::round(g.power * 10) / 10);
    o.setStr("verdict", verdictName(g.res.verdict));
    if (!g.res.cands.empty()) {
        o.set("best_guess", candidateJson(g.res.cands[0]));
        if (g.res.cands.size() > 1) {
            Json alt = Json::array();
            for (size_t i = 1; i < g.res.cands.size() && i < topN; i++) alt.push(candidateJson(g.res.cands[i]));
            o.set("alternatives", alt);
        }
    }
    if (!g.res.headline.empty()) o.setStr("headline", g.res.headline);
    if (!g.res.summary.empty()) o.setStr("summary", g.res.summary);
    return o;
}

// Scans [startHz, stopHz]: hops across it at the radio's current capture width, identifies every signal in each hop.
std::vector<Found> scanRange(ToolCtx& c, double startHz, double stopHz, size_t maxSignals, int& hopsDone, std::string& note) {
    if (!(stopHz > startHz)) throw ToolError("stop must be above start");
    SigCtx x = sigContext(c);
    const double usable = x.rate * 0.8;
    const double span = stopHz - startHz;
    std::vector<double> centres;
    if (span <= usable) centres.push_back((startHz + stopHz) / 2);
    else {
        const int need = (int)std::ceil(span / usable);
        if (need > 40) throw ToolError("that range needs " + std::to_string(need) + " hops at the current sample rate (" + std::to_string((long long)(x.rate / 1e6 * 100) / 100.0) +
                                       " Msps); narrow it or raise the sample rate with set_radio");
        for (int i = 0; i < need; i++) centres.push_back(startHz + usable / 2 + i * usable);
    }
    std::vector<Found> all;
    const double t0 = nowS();
    hopsDone = 0;
    for (double ctr : centres) {
        if (nowS() - t0 > 240) { note = "stopped after " + std::to_string(hopsDone) + " of " + std::to_string(centres.size()) + " hops (time limit)"; break; }
        std::string err;
        if (std::fabs(x.rc - ctr) > 1.0 || centres.size() > 1) {
            if (!c.host.setFrequencyHz(ctr, err)) throw ToolError("could not tune to " + std::to_string(ctr / 1e6) + " MHz: " + err);
            sleepMs((int)(500 * std::max(0.2, c.opt.scanDwellScale)));
            SpecInfo s;
            needSpectrumWait(c, s, 64, 4);
            if (s.ok && s.centreHz > 0) x.rc = s.centreHz; else x.rc = ctr;
            if (c.eng->sampleRate() > 0) x.rate = c.eng->sampleRate();
        }
        std::vector<Found> f = identifyAll(c, x, 16);
        hopsDone++;
        for (Found& g : f) {
            if (g.rfHz < startHz - g.bwHz / 2 || g.rfHz > stopHz + g.bwHz / 2) continue;
            bool dup = false;
            for (Found& h : all)
                if (std::fabs(h.rfHz - g.rfHz) < std::max(5e3, 0.5 * std::max(h.bwHz, g.bwHz))) { if (g.snr > h.snr) h = g; dup = true; break; }
            if (!dup) all.push_back(std::move(g));
        }
    }
    std::sort(all.begin(), all.end(), [](const Found& a, const Found& b) { return a.snr > b.snr; });
    if (all.size() > maxSignals) all.resize(maxSignals);
    return all;
}

// ---------------------------------------------------------------- diagnostics
Json diagnose(ToolCtx& c, const Json& st, const Json& tel, const SpecInfo& sp) {
    Json hints = Json::array();
    auto add = [&](const std::string& s) { hints.push(Json::string(s)); };
    if (!st.boolOr("running", false)) { add("The receiver is stopped: call start_receiver (or tune / auto_decode, which start it)."); return hints; }
    const double rate = st.numOr("sampleRateHz", 0), f = st.numOr("frequencyHz", 0);
    if (st.has("error") && !st.strOr("error").empty()) add("The host reports an error: " + st.strOr("error"));
    if (st.numOr("droppedSamples", 0) > 0 && st.numOr("loadPct", 0) > 90) add("The computer is overloaded (load " + std::to_string((int)st.numOr("loadPct", 0)) + " %) and samples are dropped: use a lower sample rate or close other programs.");
    const ModeTuning* mt = nullptr;
    const std::vector<ModeRow> rows = modeRows(c);
    for (const ModeRow& r : rows) if (r.family == (int)st.numOr("family", -1) && (r.sub.empty() || r.sub == st.strOr("sub"))) { if (r.maxMhz > r.minMhz) { if (f / 1e6 < r.minMhz - 1e-6 || f / 1e6 > r.maxMhz + 1e-6) add("The frequency " + std::to_string(f / 1e6) + " MHz is outside the usual band of " + r.name + " (" + std::to_string(r.minMhz) + " - " + std::to_string(r.maxMhz) + " MHz): check the frequency."); } break; }
    (void)mt;
    if (sp.ok) {
        if (sp.clip > 0.01) add("The input is clipping (" + std::to_string((int)(sp.clip * 100)) + " % of samples at full scale): lower the gain, or an attenuator may be needed.");
        std::vector<float> s = sp.db;
        std::sort(s.begin(), s.end());
        const float floor = s[s.size() / 2], top = s.back();
        if (top - floor < 6) add("There is no signal standing out of the noise in the capture window (strongest peak only " + std::to_string((int)(top - floor)) + " dB over the floor): wrong frequency, an antenna problem, or the gain is too low.");
        else if (sp.rms > -12) add("The input level is very high (" + std::to_string((int)sp.rms) + " dBFS): lower the gain to avoid overload.");
        else if (sp.rms < -70) add("The input level is very low (" + std::to_string((int)sp.rms) + " dBFS): raise the gain.");
    } else add("No spectrum yet: the radio is not delivering samples.");
    if (tel.isObj()) {
        const Json* q = tel.get("quality");
        if (q && q->isObj() && q->has("marginDb") && q->numOr("marginDb", 0) < 0)
            add("The SNR (" + std::to_string((int)q->numOr("snrDb", 0)) + " dB) is below what this mode needs (" + std::to_string((int)q->numOr("requiredDb", 0)) + " dB): a better antenna, a filter or a different gain is needed.");
        if (!tel.boolOr("locked", false) && rate > 0) {
            for (const ModeRow& r : rows) if (r.family == (int)st.numOr("family", -1)) {
                const ModeTuning* m = modeTuning(r.family);
                if (m && m->minSampleRate > rate + 1) add("The sample rate (" + std::to_string(rate / 1e6) + " Msps) is below the " + std::to_string(m->minSampleRate / 1e6) + " Msps this mode needs: raise it with set_radio.");
                break;
            }
        }
        for (const char* k : {"hint", "why", "note", "advice", "message"}) {
            const Json* v = tel.get(k);
            if (v && v->isStr() && !v->str.empty()) add(v->str);
        }
    }
    if (hints.arr.empty()) add("No problem found by the checks: the signal may just be weak or intermittent; give it more time or try identify_signal at this frequency.");
    return hints;
}

Json trimmedTelemetry(const Json& t) {
    // The telemetry can be large (tables); keep scalars and small objects, and say what was left out.
    if (!t.isObj()) return t;
    Json o = Json::object();
    Json left = Json::array();
    for (const auto& kv : t.obj) {
        const std::string d = kv.second.dump();
        if (d.size() <= 4000) o.set(kv.first, kv.second);
        else left.push(Json::string(kv.first));
    }
    if (!left.arr.empty()) o.set("omitted_large_fields", left);
    return o;
}

// ---------------------------------------------------------------- tools
std::vector<Tool> makeTools() {
    std::vector<Tool> T;
    auto add = [&](Tool t) { T.push_back(std::move(t)); };

    {
        Tool t;
        t.name = "list_radios"; t.title = "List radios";
        t.description = "List the radios and sources OnAir knows (SDRs, the built-in test signal, recordings): index, name, kind, tuning and gain range. Use the index with set_radio / start_receiver.";
        t.schema = Schema().done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) {
            Json o = Json::object();
            o.set("radios", hostJson(c.host.devicesJson()));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_status"; t.title = "Receiver status";
        t.description = "The receiver's state: running or not, radio, mode, frequency, sample rate, gain, and any error.";
        t.schema = Schema().done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) { return statusSummary(c); };
        add(t);
    }
    {
        Tool t;
        t.name = "set_radio"; t.title = "Configure the radio";
        t.description = "Change the radio: pick one by index (this restarts the receiver on it), set the sample rate, gain (also turns the AGC off) or auto gain, bandwidth, and the frequency. "
                        "Give only what you want to change. A wider sample rate lets scan_band cover more spectrum at once (HackRF: 10 Msps; RTL-SDR: 2.4).";
        t.schema = Schema()
                       .add("device", prop("integer", "Radio index from list_radios. Stops the receiver and starts it on that radio."))
                       .add("sample_rate_msps", prop("number", "Sample rate in megasamples per second; 0 = automatic."))
                       .add("gain_db", prop("number", "Total gain in dB (turns the AGC off)."))
                       .add("auto_gain", prop("boolean", "Turn automatic gain on or off."))
                       .add("bandwidth_khz", prop("number", "Channel bandwidth in kHz (where the mode supports it)."))
                       .add("frequency_mhz", prop("number", "Centre frequency in MHz."))
                       .done();
        t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            std::string err;
            Json done = Json::array();
            if (a.has("device")) {
                if (isRunning(c) && !c.host.stop(err)) throw ToolError("could not stop the receiver: " + err);
                err.clear();
                if (!c.host.start((int)needNum(a, "device"), err)) throw ToolError("could not start that radio: " + err);
                done.push(Json::string("device"));
            }
            if (a.has("sample_rate_msps")) {
                const double r = needNum(a, "sample_rate_msps");
                if (r < 0) throw ToolError("sample_rate_msps must be 0 or more");
                if (!c.host.setSampleRateHz(r * 1e6, err)) throw ToolError("sample rate: " + err);
                done.push(Json::string("sample_rate"));
            }
            if (a.has("auto_gain")) {
                if (!c.host.setAutoGain(a.boolOr("auto_gain", false), err)) throw ToolError("auto gain: " + err);
                done.push(Json::string("auto_gain"));
            }
            if (a.has("gain_db")) {
                if (!c.host.setGainDb(needNum(a, "gain_db"), err)) throw ToolError("gain: " + err);
                done.push(Json::string("gain"));
            }
            if (a.has("bandwidth_khz")) {
                if (!c.host.setBandwidthHz(needNum(a, "bandwidth_khz") * 1e3, err)) throw ToolError("bandwidth: " + err);
                done.push(Json::string("bandwidth"));
            }
            double hz = 0;
            if (freqArg(a, hz, "frequency_mhz", "frequency_hz", false)) {
                if (!c.host.setFrequencyHz(hz, err)) throw ToolError("frequency: " + err);
                done.push(Json::string("frequency"));
            }
            Json o = Json::object();
            o.set("changed", done);
            o.set("status", statusSummary(c));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "tune"; t.title = "Tune";
        t.description = "Tune the radio to a frequency (MHz). Starts the receiver if it is stopped. Returns the status.";
        t.schema = Schema().add("frequency_mhz", prop("number", "Frequency in MHz, e.g. 15.5 for 15.5 MHz or 162.55 for a marine weather channel."), true).done();
        t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            double hz = 0;
            freqArg(a, hz);
            std::string err;
            if (!c.host.setFrequencyHz(hz, err)) throw ToolError("could not tune: " + err);
            if (!isRunning(c)) ensureRunning(c);
            return statusSummary(c);
        };
        add(t);
    }
    {
        Tool t;
        t.name = "start_receiver"; t.title = "Start the receiver";
        t.description = "Start the receiver on the selected radio (or the radio with the given index) in the current mode and frequency.";
        t.schema = Schema().add("device", prop("integer", "Radio index from list_radios (optional).")).done();
        t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            std::string err;
            if (isRunning(c)) { if (!a.has("device")) return statusSummary(c); c.host.stop(err); err.clear(); }
            if (!c.host.start(a.has("device") ? (int)needNum(a, "device") : -1, err)) throw ToolError("could not start: " + err);
            return statusSummary(c);
        };
        add(t);
    }
    {
        Tool t;
        t.name = "stop_receiver"; t.title = "Stop the receiver";
        t.description = "Stop the receiver (the radio is released).";
        t.schema = Schema().done();
        t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) {
            std::string err;
            if (isRunning(c) && !c.host.stop(err)) throw ToolError("could not stop: " + err);
            return statusSummary(c);
        };
        add(t);
    }
    {
        Tool t;
        t.name = "list_modes"; t.title = "List decoder modes";
        t.description = "The decoder modes OnAir has (several hundred): number, id, name, usual band, and well-known frequencies. Use query to narrow it (e.g. 'drm', 'ais', 'weather', 'pager'). Use the id with set_mode.";
        t.schema = Schema().add("query", prop("string", "Words to look for in the mode name or id.")).add("limit", prop("integer", "At most this many (default 40, at most 200).")).done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            const std::vector<ModeRow> rows = modeRows(c);
            const std::string q = lower(a.strOr("query"));
            const size_t lim = (size_t)std::max(1.0, std::min(200.0, a.numOr("limit", 40)));
            Json arr = Json::array();
            size_t total = 0;
            for (const ModeRow& r : rows) {
                if (!q.empty()) {
                    bool all = true;
                    const std::string hay = lower(r.name + " " + r.id + " " + r.sub);
                    size_t p = 0;
                    while (p < q.size()) {
                        size_t e = q.find(' ', p);
                        if (e == std::string::npos) e = q.size();
                        if (e > p && hay.find(q.substr(p, e - p)) == std::string::npos) all = false;
                        p = e + 1;
                    }
                    if (!all) continue;
                }
                total++;
                if (arr.arr.size() < lim) arr.push(modeRowJson(r));
            }
            Json o = Json::object();
            o.setNum("total_matching", (double)total);
            o.setNum("returned", (double)arr.arr.size());
            o.set("modes", arr);
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "set_mode"; t.title = "Set the decoder mode";
        t.description = "Choose the decoder mode (by id, name or number from list_modes). The receiver restarts if it was running. Optionally tune to a frequency in the same step.";
        t.schema = Schema()
                       .add("mode", prop("string", "Mode id, name or number, e.g. 'drm', 'adsb', 'fm', 'dmr'."), true)
                       .add("frequency_mhz", prop("number", "Frequency to tune to afterwards (MHz)."))
                       .add("start", prop("boolean", "Start the receiver afterwards (default true)."))
                       .done();
        t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            const std::vector<ModeRow> rows = modeRows(c);
            const ModeRow& r = needRow(rows, a.strOr("mode"));
            applyMode(c, r);
            std::string err;
            double hz = 0;
            if (freqArg(a, hz, "frequency_mhz", "frequency_hz", false) && !c.host.setFrequencyHz(hz, err)) throw ToolError("could not tune: " + err);
            if (a.boolOr("start", true)) { err.clear(); if (!c.host.start(-1, err)) throw ToolError("mode set, but the receiver did not start: " + err); }
            Json o = Json::object();
            o.set("mode", modeRowJson(r));
            o.set("status", statusSummary(c));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "scan_band"; t.title = "Scan a band for signals";
        t.description = "Look at a frequency range and list the signals in it: centre frequency, bandwidth, SNR, and a guess at what each is (the signal identifier's best candidates with confidence). "
                        "Ranges wider than the radio's capture window are covered in hops (each takes a few seconds). Use a wide sample rate (set_radio) to cover more per hop. The receiver must be able to run; the radio is left tuned at the last hop.";
        t.schema = Schema()
                       .add("start_mhz", prop("number", "Lower edge in MHz."), true)
                       .add("stop_mhz", prop("number", "Upper edge in MHz."), true)
                       .add("max_signals", prop("integer", "At most this many signals, strongest first (default 20)."))
                       .done();
        t.fn = [](ToolCtx& c, const Json& a) {
            const double lo = needNum(a, "start_mhz") * 1e6, hi = needNum(a, "stop_mhz") * 1e6;
            const size_t mx = (size_t)std::max(1.0, std::min(60.0, a.numOr("max_signals", 20)));
            int hops = 0;
            std::string note;
            const std::vector<Found> f = scanRange(c, lo, hi, mx, hops, note);
            Json o = Json::object();
            o.setNum("start_mhz", lo / 1e6);
            o.setNum("stop_mhz", hi / 1e6);
            o.setNum("hops", hops);
            Json arr = Json::array();
            for (const Found& g : f) arr.push(foundJson(g));
            o.setNum("signals_found", (double)f.size());
            o.set("signals", arr);
            if (f.empty()) o.setStr("note", "No signal above the noise floor in this range. Check the antenna and gain with get_receiver_health, or try another range.");
            if (!note.empty()) o.setStr("warning", note);
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "identify_signal"; t.title = "Identify a signal";
        t.description = "Work out what the signal at a frequency is (modulation, bandwidth, symbol rate, ranked candidate modes with confidence and the evidence). Without bandwidth_khz it looks for the signal nearest to the frequency in the current capture window.";
        t.schema = Schema()
                       .add("frequency_mhz", prop("number", "Frequency of the signal in MHz."), true)
                       .add("bandwidth_khz", prop("number", "Width of the signal in kHz if you know it (e.g. 10 for DRM, 8000 for DVB-T); omit to let OnAir find it."))
                       .done();
        t.readOnly = false;
        t.fn = [](ToolCtx& c, const Json& a) {
            double hz = 0;
            freqArg(a, hz);
            SigCtx x = sigContext(c);
            std::string err;
            if (std::fabs(hz - x.rc) > x.rate * 0.4) {   // outside the window: retune onto it
                if (!c.host.setFrequencyHz(hz, err)) throw ToolError("could not tune: " + err);
                sleepMs((int)(600 * std::max(0.2, c.opt.scanDwellScale)));
                x = sigContext(c);
            }
            if (a.has("bandwidth_khz")) {
                const double bw = std::max(500.0, needNum(a, "bandwidth_khz") * 1e3);
                sigid::Capture& cap = c.eng->sigid();
                sigid::Result r;
                uint64_t seq = 0;
                cap.result(r, seq);
                const double sec = sigid::captureSeconds(x.rate) * c.opt.scanDwellScale;
                cap.start(x.rate, hz - x.rc, bw, x.rc, sec);
                const double t0 = nowS();
                while (!cap.result(r, seq)) {
                    if (nowS() - t0 > sec + 20) { cap.cancel(); throw ToolError("the identifier did not finish in time"); }
                    sleepMs(100);
                }
                Json o = resultJson(r, 5, true);
                o.setNum("frequency_mhz", hz / 1e6);
                return o;
            }
            const std::vector<Found> f = identifyAll(c, x, 16);
            const Found* best = nullptr;
            double bd = 1e18;
            for (const Found& g : f) {
                const double d = std::fabs(g.rfHz - hz);
                if (d < bd) { bd = d; best = &g; }
            }
            if (!best || bd > std::max(50e3, best->bwHz)) {
                Json o = Json::object();
                o.setStr("verdict", "no_signal");
                o.setStr("note", "No signal found near " + std::to_string(hz / 1e6) + " MHz in the current capture window. Try scan_band over a wider range, or check the gain.");
                o.setNum("signals_in_window", (double)f.size());
                return o;
            }
            Json o = resultJson(best->res, 5, true);
            o.setNum("frequency_mhz", std::round(best->rfHz) / 1e6);
            o.setNum("distance_from_requested_khz", std::round(bd / 100) / 10);
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "auto_decode"; t.title = "Find a signal and decode it";
        t.description = "Find a signal and start decoding it: scans around the given frequency (or range), identifies what is there, picks the mode that fits (optionally the one you name), switches the decoder to it, tunes onto the signal and waits for the decoder to lock. "
                        "Returns what locked (mode, exact frequency), the decoder's state, and hints when it did not lock. Example: 'there is DRM around 15 MHz' -> frequency_mhz 15, mode 'drm'.";
        t.schema = Schema()
                       .add("frequency_mhz", prop("number", "Where to look, in MHz (the centre of the search)."))
                       .add("span_khz", prop("number", "Width of the search around frequency_mhz in kHz (default 1000)."))
                       .add("start_mhz", prop("number", "Or: lower edge of a range to search."))
                       .add("stop_mhz", prop("number", "Or: upper edge of a range to search."))
                       .add("mode", prop("string", "What you expect (id, name or number from list_modes, e.g. 'drm'). Prefers signals identified as that; if none is found it tries that mode blind on the frequency."))
                       .add("timeout_s", prop("integer", "How long to wait for the decoder to lock (default 30, at most 120)."))
                       .done();
        t.fn = [](ToolCtx& c, const Json& a) {
            double lo = 0, hi = 0, f0 = 0;
            if (a.has("start_mhz") && a.has("stop_mhz")) { lo = needNum(a, "start_mhz") * 1e6; hi = needNum(a, "stop_mhz") * 1e6; f0 = (lo + hi) / 2; }
            else if (freqArg(a, f0, "frequency_mhz", "frequency_hz", false)) { const double sp = std::max(10.0, std::min(40000.0, a.numOr("span_khz", 1000))) * 1e3; lo = f0 - sp / 2; hi = f0 + sp / 2; }
            else if (a.has("mode")) { f0 = 0; }
            else throw ToolError("give frequency_mhz (or start_mhz and stop_mhz), and/or a mode");
            const double waitSec = std::max(3.0, std::min(120.0, a.numOr("timeout_s", c.opt.decodeWaitSec)));
            const std::vector<ModeRow> rows = modeRows(c);
            const ModeRow* hint = nullptr;
            if (a.has("mode")) hint = &needRow(rows, a.strOr("mode"));
            Json o = Json::object();
            Json steps = Json::array();
            const ModeRow* pick = nullptr;
            double pickHz = f0;
            Json pickJson;
            if (f0 > 0) {
                int hops = 0;
                std::string note;
                std::vector<Found> found;
                try { found = scanRange(c, lo, hi, 20, hops, note); } catch (const ToolError& e) { steps.push(Json::string(std::string("scan failed: ") + e.what())); }
                steps.push(Json::string("scanned " + std::to_string(lo / 1e6) + " - " + std::to_string(hi / 1e6) + " MHz: " + std::to_string(found.size()) + " signal(s)"));
                double bestScore = -1;
                for (const Found& g : found) {
                    for (size_t i = 0; i < g.res.cands.size() && i < 3; i++) {
                        const sigid::Candidate& cd = g.res.cands[i];
                        const ModeRow* r = nullptr;
                        for (const ModeRow& m : rows) {
                            const std::string want = lower(cd.subId.empty() ? cd.modeId : cd.subId);
                            if (!want.empty() && (lower(m.id) == want || (!m.sub.empty() && lower(m.sub) == want))) { r = &m; break; }
                        }
                        if (!r && !cd.modeId.empty()) for (const ModeRow& m : rows) if (lower(m.id) == lower(cd.modeId) && m.sub.empty()) { r = &m; break; }
                        if (!r) continue;
                        double sc = cd.confidence * 100 + (cd.verify == sigid::VerifyConfirmed ? 15 : 0) + std::min(20.0, g.snr) * 0.5 - (double)i * 5;
                        if (hint) { if (r == hint || (r->family == hint->family && (hint->sub.empty() || r->sub == hint->sub))) sc += 200; else continue; }
                        if (sc > bestScore) { bestScore = sc; pick = r; pickHz = g.rfHz; pickJson = foundJson(g); }
                        break;
                    }
                }
                if (!pick && hint) { pick = hint; pickHz = f0; steps.push(Json::string("no signal identified as " + hint->name + "; trying it blind on " + std::to_string(f0 / 1e6) + " MHz")); }
            } else if (hint) { pick = hint; pickHz = hint->defMhz * 1e6; steps.push(Json::string("no frequency given: using the mode's usual frequency")); }
            if (!pick) {
                o.setBool("locked", false);
                o.setStr("result", "no_decodable_signal");
                o.set("steps", steps);
                o.setStr("note", "No signal was found that OnAir can decode in this range. Try a wider range, another time of day (HF propagation changes), or check the antenna and gain (get_receiver_health).");
                return o;
            }
            steps.push(Json::string("mode " + pick->name + " at " + std::to_string(pickHz / 1e6) + " MHz"));
            std::string err;
            if (isRunning(c)) c.host.stop(err);
            err.clear();
            if (!c.host.setMode(pick->family, pick->sub, err)) throw ToolError("could not set the mode " + pick->name + ": " + err);
            if (!c.host.setFrequencyHz(pickHz, err)) throw ToolError("could not tune: " + err);
            if (!c.host.start(-1, err)) throw ToolError("could not start the receiver: " + err);
            // wait for a lock
            const double t0 = nowS();
            Json tel;
            bool locked = false;
            while (nowS() - t0 < waitSec) {
                sleepMs(500);
                uint64_t seq = 0;
                tel = hostJson(c.host.telemetryJson(seq));
                if (tel.boolOr("locked", false)) { locked = true; break; }
                if (!isRunning(c)) break;
            }
            o.setBool("locked", locked);
            o.setStr("result", locked ? "locked" : "no_lock_yet");
            o.set("mode", modeRowJson(*pick));
            o.setNum("frequency_mhz", std::round(pickHz) / 1e6);
            if (!pickJson.isNull()) o.set("identified_as", pickJson);
            o.setNum("waited_s", std::round((nowS() - t0) * 10) / 10);
            o.set("steps", steps);
            o.set("decoder", trimmedTelemetry(tel));
            if (!locked) {
                const SpecInfo sp = spectrum(c, 256);
                o.set("hints", diagnose(c, status(c), tel, sp));
            }
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_decoder_state"; t.title = "Decoder state";
        t.description = "What the running decoder reports: lock, SNR, quality and the mode's own live data (services, counters, latest decoded items). Large tables are left out and named; use get_messages for decoded text.";
        t.schema = Schema().done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) {
            uint64_t seq = 0;
            const Json tel = hostJson(c.host.telemetryJson(seq));
            Json o = Json::object();
            o.set("status", statusSummary(c));
            o.set("decoder", trimmedTelemetry(tel));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_messages"; t.title = "Decoded messages";
        t.description = "Decoded messages and items since a cursor: pager text, ADS-B aircraft, AIS vessels, ACARS, APRS, text from broadcasts, ... each with type, time, mode, frequency and fields. Pass the 'next' value of the previous call as 'since' to get only new ones. Not every host keeps a message stream: if not, use get_decoder_state.";
        t.schema = Schema()
                       .add("since", prop("integer", "Cursor from the previous call (omit: the most recent messages)."))
                       .add("max", prop("integer", "At most this many (default 50, at most 200)."))
                       .done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            uint64_t cur = a.has("since") ? (uint64_t)std::max(0.0, a.numOr("since", 0)) : 0;
            const size_t mx = (size_t)std::max(1.0, std::min(200.0, a.numOr("max", 50)));
            std::vector<std::string> lines;
            bool gap = false;
            if (!c.host.eventsSince(cur, a.has("since") ? mx : 512, lines, gap)) {
                uint64_t seq = 0;
                Json o = Json::object();
                o.setBool("message_stream", false);
                o.setStr("note", "This host keeps no message stream; the decoder's live data is in 'decoder'.");
                o.set("decoder", trimmedTelemetry(hostJson(c.host.telemetryJson(seq))));
                return o;
            }
            Json arr = Json::array();
            size_t from = lines.size() > mx ? lines.size() - mx : 0;   // no cursor given: the newest ones
            for (size_t i = from; i < lines.size(); i++) { Json e = hostJson(lines[i]); if (!e.isNull()) arr.push(e); else arr.push(Json::string(lines[i])); }
            Json o = Json::object();
            o.setBool("message_stream", true);
            o.setNum("next", (double)cur);
            o.setBool("gap", gap);
            o.setNum("count", (double)arr.arr.size());
            o.set("messages", arr);
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_spectrum"; t.title = "Spectrum";
        t.description = "The current spectrum of the capture window: centre, span, noise floor, the strongest peaks (frequency, dB over the floor, rough width) and optionally the bins (dBFS) downsampled to at most 512 values.";
        t.schema = Schema()
                       .add("bins", prop("integer", "Resolution used to look for peaks (default 512, 16 to 4096)."))
                       .add("include_bins", prop("boolean", "Also return the downsampled bins (at most 256 values, default false)."))
                       .add("max_peaks", prop("integer", "How many peaks to list (default 12)."))
                       .done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            const int bins = (int)std::max(16.0, std::min(4096.0, a.numOr("bins", 512)));
            SpecInfo s = spectrum(c, bins);
            if (!s.ok) throw ToolError("no spectrum: the receiver is not running or has not delivered samples (call start_receiver)");
            std::vector<float> sorted = s.db;
            std::sort(sorted.begin(), sorted.end());
            const float floor = sorted[sorted.size() / 2];
            const size_t n = s.db.size();
            const double bin = s.rateHz / (double)n, f0 = s.centreHz - s.rateHz / 2;
            Json o = Json::object();
            o.setNum("centre_mhz", s.centreHz / 1e6);
            o.setNum("span_mhz", s.rateHz / 1e6);
            o.setNum("noise_floor_dbfs", std::round(floor * 10) / 10);
            o.setNum("rms_dbfs", std::round(s.rms * 10) / 10);
            o.setNum("clip_fraction", s.clip);
            // peaks: local maxima 8 dB over the floor; width where it falls 6 dB
            std::vector<std::pair<float, size_t>> pk;
            for (size_t i = 1; i + 1 < n; i++) if (s.db[i] >= floor + 8 && s.db[i] >= s.db[i - 1] && s.db[i] > s.db[i + 1]) pk.push_back({s.db[i], i});
            std::sort(pk.begin(), pk.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            std::vector<size_t> taken;
            Json arr = Json::array();
            const size_t mp = (size_t)std::max(1.0, std::min(40.0, a.numOr("max_peaks", 12)));
            for (const auto& p : pk) {
                bool near = false;
                for (size_t t2 : taken) if ((p.second > t2 ? p.second - t2 : t2 - p.second) < 3) near = true;
                if (near) continue;
                taken.push_back(p.second);
                size_t l = p.second, r = p.second;
                while (l > 0 && s.db[l] > p.first - 6) l--;
                while (r + 1 < n && s.db[r] > p.first - 6) r++;
                Json j = Json::object();
                j.setNum("frequency_mhz", std::round(f0 + ((double)p.second + 0.5) * bin) / 1e6);
                j.setNum("over_floor_db", std::round((p.first - floor) * 10) / 10);
                j.setNum("width_khz", std::round((double)(r - l) * bin / 100) / 10);
                arr.push(j);
                if (arr.arr.size() >= mp) break;
            }
            o.set("peaks", arr);
            if (a.boolOr("include_bins", false)) {
                const size_t out = std::min<size_t>(n, 256), grp = (n + out - 1) / out;
                Json b = Json::array();
                for (size_t i = 0; i < n; i += grp) { float m = s.db[i]; for (size_t k = i + 1; k < std::min(n, i + grp); k++) m = std::max(m, s.db[k]); b.push(Json::number(std::round(m * 10) / 10)); }
                o.set("bins_dbfs", b);
                o.setNum("bin_width_khz", std::round((double)grp * bin / 100) / 10);
                o.setNum("first_bin_mhz", f0 / 1e6);
            }
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_receiver_health"; t.title = "Receiver health (why it is not decoding)";
        t.description = "SNR, lock, signal level, clipping, load, and a list of plain-language hints about why the decoder is not decoding (wrong frequency, too much or too little gain, sample rate too low, no signal, overload ...).";
        t.schema = Schema().done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) {
            uint64_t seq = 0;
            const Json st = status(c), tel = hostJson(c.host.telemetryJson(seq));
            const SpecInfo sp = spectrum(c, 256);
            Json o = Json::object();
            o.set("status", statusSummary(c));
            o.setBool("locked", tel.boolOr("locked", false));
            if (const Json* v = tel.get("snrDb")) o.set("snr_db", *v);
            if (const Json* v = tel.get("quality")) o.set("quality", *v);
            if (sp.ok) { o.setNum("input_level_dbfs", std::round(sp.rms * 10) / 10); o.setNum("clip_fraction", sp.clip); }
            o.set("hints", diagnose(c, st, tel, sp));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "start_recording"; t.title = "Start recording";
        t.description = "Start recording the raw IQ samples or the demodulated audio. The file is kept by the program (you get its name, not a path).";
        t.schema = Schema().add("kind", enumProp("What to record.", {"iq", "audio"}), true).done();
        t.fn = [](ToolCtx& c, const Json& a) {
            const std::string k = a.strOr("kind");
            if (k != "iq" && k != "audio") throw ToolError("kind must be 'iq' or 'audio'");
            std::string err;
            if (!c.host.recordStart(k, err)) throw ToolError("recording: " + err);
            Json o = Json::object();
            o.set("recording", hostJson(c.host.recordingJson()));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "stop_recording"; t.title = "Stop recording";
        t.description = "Stop the recording that start_recording began.";
        t.schema = Schema().done();
        t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) {
            std::string err;
            if (!c.host.recordStop(err)) throw ToolError("recording: " + err);
            Json o = Json::object();
            o.set("recording", hostJson(c.host.recordingJson()));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_recording_status"; t.title = "Recording status";
        t.description = "Is a recording running, which file, how long.";
        t.schema = Schema().done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx& c, const Json&) {
            const std::string j = c.host.recordingJson();
            if (j.empty()) throw ToolError("this host has no recorder");
            Json o = Json::object();
            o.set("recording", hostJson(j));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "search_frequencies"; t.title = "Search the frequency database";
        t.description = "Search OnAir's built-in database of well-known frequencies (broadcast, aviation, marine, ham, satellites, beacons, utility ...). Query words match name, notes, mode, region and category; a number matches a frequency in MHz ('15', '162.4').";
        t.schema = Schema()
                       .add("query", prop("string", "Words or a frequency in MHz."))
                       .add("mode", prop("string", "Limit to one mode id (see list_modes)."))
                       .add("region", prop("string", "Limit to a region (EU, US, UK, DE, GLOBAL ...)."))
                       .add("limit", prop("integer", "At most this many (default 25, at most 100)."))
                       .done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx&, const Json& a) {
            FreqQuery q;
            q.text = a.strOr("query");
            q.mode = a.strOr("mode");
            q.region = a.strOr("region");
            const auto& all = builtinFrequencies();
            const std::vector<size_t> idx = freqSearch(all, q);
            const size_t lim = (size_t)std::max(1.0, std::min(100.0, a.numOr("limit", 25)));
            Json arr = Json::array();
            for (size_t i = 0; i < idx.size() && i < lim; i++) {
                const FreqEntry& e = all[idx[i]];
                Json j = Json::object();
                j.setNum("mhz", (double)e.hz / 1e6);
                j.setStr("name", e.name);
                j.setStr("mode", e.mode);
                if (!e.region.empty()) j.setStr("region", e.region);
                if (!e.category.empty()) j.setStr("category", e.category);
                if (e.bwHz > 0) j.setNum("bandwidth_khz", e.bwHz / 1e3);
                if (!e.notes.empty()) j.setStr("notes", e.notes.size() > 160 ? e.notes.substr(0, 160) + "..." : e.notes);
                arr.push(j);
            }
            Json o = Json::object();
            o.setNum("total_matching", (double)idx.size());
            o.set("entries", arr);
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_band_plan"; t.title = "Band plan at a frequency";
        t.description = "What the built-in band plans say about a frequency: the band (broadcast, ham, airband, marine ...), its service, channel raster and suggested modes.";
        t.schema = Schema().add("frequency_mhz", prop("number", "Frequency in MHz."), true).done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx&, const Json& a) {
            double hz = 0;
            freqArg(a, hz);
            Json o = Json::object();
            o.setNum("frequency_mhz", hz / 1e6);
            o.set("bands", bandPlanFor(hz));
            return o;
        };
        add(t);
    }
    {
        Tool t;
        t.name = "get_codec_status"; t.title = "Voice codec status";
        t.description = "Which user-installed voice codecs are present (OnAir ships none; digital voice modes need the user to install a codec plug-in, see docs/CODECS.md). Shows installed / not installed per codec and which modes need it. Does not load or run anything.";
        t.schema = Schema().done();
        t.readOnly = true; t.idempotent = true;
        t.fn = [](ToolCtx&, const Json&) {
            struct C { const char* id; const char* modes; };
            static const C codecs[] = {{"ambe2", "DMR, NXDN, dPMR"}, {"ambe", "D-STAR"}, {"aeroambe", "Inmarsat Aero C channel voice"}, {"iridium", "Iridium voice"},
                                       {"imbe", "P25 phase 1"}, {"tetra", "TETRA (unencrypted)"}, {"tetrapol", "TETRAPOL (clear)"}, {"ysf", "Yaesu System Fusion"}};
            const std::vector<codec::CodecDir> dirs = codec::systemCodecDirs();
            Json arr = Json::array();
            for (const C& cd : codecs) {
                const std::string file = codec::speechCodecFileName(cd.id);
                std::string where;
                for (const codec::CodecDir& d : dirs) {
                    std::error_code ec;
                    if (std::filesystem::is_regular_file(codec::joinPath(d.path, file), ec)) { where = d.source; break; }
                }
                Json j = Json::object();
                j.setStr("codec", cd.id);
                j.setStr("modes", cd.modes);
                j.setBool("installed", !where.empty());
                if (!where.empty()) j.setStr("found_in", where);
                arr.push(j);
            }
            Json o = Json::object();
            o.set("codecs", arr);
            o.setStr("note", "OnAir never ships patented speech codecs; the user installs them (docs/CODECS.md). The AI cannot install one.");
            return o;
        };
        add(t);
    }
    // ---- transmit: only when the user turned the separate TX switch on
    {
        Tool t;
        t.name = "tx_status"; t.title = "Transmitter status";
        t.description = "The transmitter's state. (Exists only because the user enabled AI transmit control.)";
        t.schema = Schema().done();
        t.readOnly = true; t.needsTx = true;
        t.fn = [](ToolCtx& c, const Json&) {
            const std::string j = c.host.txStatusJson();
            if (j.empty()) throw ToolError("this program has no transmitter");
            return hostJson(j);
        };
        add(t);
    }
    {
        Tool t;
        t.name = "tx_start"; t.title = "Start transmitting";
        t.description = "Start a transmission (the user enabled AI transmit control). The user is responsible for licence and hardware; the answer carries warnings. Parameters as the remote API's /tx/start: mode, mhz, gain_db, seconds, test (dry run), text, callsign, grid ...";
        t.schema = Schema()
                       .add("mode", prop("string", "Transmit mode id."), true)
                       .add("mhz", prop("number", "Frequency in MHz."), true)
                       .add("gain_db", prop("number", "TX gain."))
                       .add("seconds", prop("number", "Duration."))
                       .add("test", prop("boolean", "Dry run: check and warn, do not transmit."))
                       .add("text", prop("string", "Message text where the mode has one."))
                       .add("callsign", prop("string", "Callsign where the mode has one."))
                       .add("grid", prop("string", "Locator where the mode has one."))
                       .done();
        t.destructive = true; t.needsTx = true;
        t.fn = [](ToolCtx& c, const Json& a) {
            std::map<std::string, JsonVal> body;
            for (const auto& kv : a.obj) {
                JsonVal v;
                if (kv.second.isNum()) { v.kind = JsonVal::Num; v.num = kv.second.num; }
                else if (kv.second.isStr()) { v.kind = JsonVal::Str; v.str = kv.second.str; }
                else if (kv.second.isBool()) { v.kind = JsonVal::Bool; v.b = kv.second.b; }
                else continue;
                body[kv.first] = v;
            }
            std::string out, err;
            if (!c.host.txStart(body, out, err)) throw ToolError("transmit: " + err);
            return hostJson(out);
        };
        add(t);
    }
    {
        Tool t;
        t.name = "tx_stop"; t.title = "Stop transmitting";
        t.description = "Stop any transmission.";
        t.schema = Schema().done();
        t.idempotent = true; t.needsTx = true;
        t.fn = [](ToolCtx& c, const Json&) {
            std::string out, err;
            if (!c.host.txStop(out, err)) throw ToolError("transmit: " + err);
            return hostJson(out);
        };
        add(t);
    }
    return T;
}

} // namespace

const std::vector<Tool>& allTools() {
    static const std::vector<Tool> t = makeTools();
    return t;
}

Json hostJson(const std::string& text) {
    Json j;
    std::string err;
    if (text.empty() || !parseJson(text, j, err)) return Json();
    return j;
}

Json modesTable(ToolCtx& c) {
    Json arr = Json::array();
    for (const ModeRow& r : modeRows(c)) arr.push(modeRowJson(r));
    return arr;
}

Json resourceModeInfo(ToolCtx& c, const std::string& q, bool& found) {
    const std::vector<ModeRow> rows = modeRows(c);
    const ModeRow* r = findRow(rows, q);
    found = r != nullptr;
    return r ? modeRowJson(*r) : Json();
}

Json bandPlanFor(double hz) {
    Json arr = Json::array();
    for (const bandplan::Plan& p : bandplan::builtinPlans()) {
        for (const bandplan::Band& b : p.bands) {
            if (!b.contains(hz)) continue;
            Json j = Json::object();
            j.setStr("plan", p.name);
            if (!p.region.empty()) j.setStr("region", p.region);
            j.setStr("band", b.name);
            j.setStr("service", b.service);
            j.setNum("start_mhz", b.startHz / 1e6);
            j.setNum("end_mhz", b.endHz / 1e6);
            if (b.stepHz > 0) j.setNum("channel_step_khz", b.stepHz / 1e3);
            if (b.bandwidthHz > 0) j.setNum("channel_width_khz", b.bandwidthHz / 1e3);
            bandplan::Channel ch;
            if (b.channelAt(hz, ch)) { j.setStr("nearest_channel", ch.label.empty() ? std::to_string(ch.number) : ch.label); j.setNum("channel_centre_mhz", ch.centreHz / 1e6); }
            if (!b.modes.empty()) { Json m = Json::array(); for (const std::string& s : b.modes) m.push(Json::string(s)); j.set("suggested_modes", m); }
            if (!b.note.empty()) j.setStr("note", b.note.size() > 200 ? b.note.substr(0, 200) + "..." : b.note);
            arr.push(j);
            if (arr.arr.size() >= 12) return arr;
        }
    }
    return arr;
}

std::string guideText() {
    return
        "OnAir MCP guide for AI assistants\n"
        "\n"
        "OnAir is a software-defined-radio receiver with several hundred decoder modes. You drive it with tools.\n"
        "\n"
        "Typical flow for 'find X around F MHz and decode it':\n"
        "1. get_status / list_radios: is a radio there and running? (A radio index 'Test signal' is a built-in synthetic signal, not real RF.)\n"
        "2. Optional: set_radio with a wide sample_rate_msps so one look covers more spectrum.\n"
        "3. auto_decode {frequency_mhz: F, mode: 'X'} does scan + identify + set mode + tune + wait for lock in one call. Or do the steps yourself:\n"
        "   scan_band -> identify_signal -> set_mode (id from list_modes) -> tune -> get_decoder_state / get_messages.\n"
        "4. If it does not lock: get_receiver_health gives plain-language hints (gain, clipping, wrong frequency, sample rate).\n"
        "\n"
        "Notes:\n"
        "- Every tool call is shown to the user in OnAir's AI activity log.\n"
        "- Frequencies are in MHz. HF (3-30 MHz) needs a radio that covers it (HackRF, RSP, RX-888 ... not a plain RTL-SDR without an upconverter).\n"
        "- Digital voice modes need a codec the user installs themselves (get_codec_status). Encrypted traffic is never decoded.\n"
        "- There are no transmit tools unless the user switched on 'Allow transmit' for the assistant.\n"
        "- The signal identifier gives a confidence; 'ambiguous' or 'unknown' means say so rather than guess.\n";
}

} // namespace mcp
} // namespace dect2
