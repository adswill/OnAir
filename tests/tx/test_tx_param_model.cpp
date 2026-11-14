// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The transmit parameter table (tx_params.h) against the generators, for EVERY transmit mode (txModes()):
//   1. every row that is drawn and not marked "meta" changes the generated samples when its value changes (a control that does nothing fails);
//   2. no generator reads a key of TxRequest::opt that is not a row of its mode (or one of the radio's calibration / output keys);
//   3. the table itself is sane: unique keys, defaults inside the range, a choice default that exists, `when` conditions on rows of the same table.
// The modes whose signal depends on the clock (two runs with the same values differ) are listed and their rows are not compared.
// Everything is generated into memory: NOTHING is transmitted. Signals are the clean generator output (no noise); this checks wiring, not sensitivity.
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_params.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0, rowsChecked = 0, rowsMeta = 0, modesChecked = 0;
static std::vector<std::string> nondet, noGen;
#define FAIL(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } while (0)

static const char* sample(TxPField f, bool alt) {
    switch (f) {
    case TxPField::Callsign: return alt ? "SV1ABC" : "LY2XYZ";
    case TxPField::Grid: return alt ? "KN00" : "KO24";
    case TxPField::Text: return alt ? "TEST 456" : "HELLO 123";
    case TxPField::Dest: return alt ? "SV1ABC" : "A61AB";
    case TxPField::Path: return alt ? "WIDE2-2" : "WIDE1-1";
    case TxPField::Name: return alt ? "TESTNAME" : "ONAIR";
    case TxPField::Network: return alt ? "TESTNET" : "ONAIRNET";
    default: return "";
    }
}

struct Values { std::map<std::string, double> num; std::map<std::string, std::string> txt; };

// the values that show a row hidden by its `when` (the first value of "k=a|b", one above "k>v", one below "k<v", another for "k!=v")
static void reveal(const TxParamDef& d, const std::vector<TxParamDef>& rows, Values& v) {
    for (int guard = 0; guard < 4 && d.when && !txParamShown(d, rows, v.num); guard++) {
        const std::string w = d.when;
        const size_t op = w.find_first_of("=<>!");
        if (op == std::string::npos) return;
        const std::string key = w.substr(0, op);
        const double x = atof(w.c_str() + op + (w[op] == '!' ? 2 : 1));
        if (w[op] == '=') v.num[key] = x;
        else if (w[op] == '>') v.num[key] = x + 1;
        else if (w[op] == '<') v.num[key] = x - 1;
        else v.num[key] = x == 0 ? 1 : 0;
        if (const TxParamDef* k = txParamFind(rows, key)) reveal(*k, rows, v);   // a chain of conditions
    }
}

static double changedValue(const TxParamDef& d) {
    if (d.hasTestValue) return d.testValue;
    switch (d.type) {
    case TxPType::Bool: return d.def > 0.5 ? 0 : 1;
    case TxPType::Choice: return d.def < 0.5 ? 1 : 0;
    default: {
        const double far = std::fabs(d.hi - d.def) >= std::fabs(d.def - d.lo) ? d.hi : d.lo;
        double v = d.def + 0.5 * (far - d.def);
        if (d.type == TxPType::Int || d.type == TxPType::Hex) v = std::round(v);
        if (v == d.def) v = far;
        return v;
    }
    }
}

static double ph = 0, t = 0;
static int pullTone(float* st, int n) {   // the "microphone": a 700 Hz tone with a slow level change, 48 kHz stereo
    for (int i = 0; i < n; i++) {
        const float a = (float)(0.4 * (0.6 + 0.4 * std::sin(2 * M_PI * 3 * t)) * std::sin(ph));
        st[2 * i] = st[2 * i + 1] = a;
        ph += 2 * M_PI * 700 / 48000.0; t += 1 / 48000.0;
        if (ph > 2 * M_PI) ph -= 2 * M_PI;
    }
    return n;
}

// the samples the mode makes with these values; keys: every opt key the generator asked for
static bool make(const TxModeSpec& m, const TxParamCtx& c, const std::vector<TxParamDef>& rows, const Values& v, bool audio, double sec,
                 std::vector<cf32>& out, std::vector<std::string>& keys, std::string& err) {
    TxRequest r;
    r.modeId = m.id;
    r.analogVoice = c.analogVoice;
    if (std::string(m.id) == "qo100") r.variant = c.variant;
    r.audioSrc = 2;
    r.loop = true;
    txParamsApplyNumbers(rows, v.num, r);
    for (const auto& d : rows) if (d.type == TxPType::Text || d.type == TxPType::File) {
        const auto it = v.txt.find(d.key);
        if (it != v.txt.end()) txParamApply(d, 0, it->second, r);
    }
    txParamsSanitize(m, c, r);
    if (audio) { r.audioSrc = 0; r.pullAudio = pullTone; ph = 0; t = 0; }
    keys.clear();
    txTraceKeys(&keys);
    double rate = m.stdMode == 0 ? 0 : m.nominalRateHz;
    // a long window of a mode that can run slower: the rate is lowered so the window fits the sample cap below (NAVTEX, ALE need many seconds)
    if (rate > 0 && !m.fixedRate && sec * rate > 6e6) rate = std::max(m.minRateHz > 0 ? m.minRateHz : 48000.0, 6e6 / sec);
    auto g = makeTxGenerator(m, r, rate, err);
    if (!g) { txTraceKeys(nullptr); return false; }
    const double fs = g->sampleRate();
    const size_t n = (size_t)std::min(6e6, std::max(20000.0, sec * fs));
    out.assign(n, cf32(0, 0));
    for (size_t done = 0; done < n;) {
        const size_t k = std::min<size_t>(65536, n - done);
        g->generate(out.data() + done, k);
        txOutputStage(r, out.data() + done, k, done, fs);
        done += k;
        if (g->finished()) break;
    }
    txTraceKeys(nullptr);
    return true;
}

static bool differs(const std::vector<cf32>& a, const std::vector<cf32>& b) {
    if (a.size() != b.size()) return true;
    double e = 0, d = 0;
    for (size_t i = 0; i < a.size(); i++) { e += std::norm(a[i]); d += std::norm(a[i] - b[i]); }
    return d > 1e-9 * std::max(e, 1e-12) && d > 1e-12;
}

static void checkTable(const TxModeSpec& m, const TxParamCtx& c, const std::vector<TxParamDef>& rows) {
    std::set<std::string> seen;
    for (const auto& d : rows) {
        if (!seen.insert(d.key).second) FAIL("%s: the key %s is declared twice", m.id, d.key);
        if (!d.label || !*d.label) FAIL("%s: %s has no label", m.id, d.key);
        if (!d.custom && !d.hidden && (!d.help || !*d.help)) FAIL("%s: %s has no help line", m.id, d.key);
        if (d.type == TxPType::Choice) {
            if (d.choices.empty()) FAIL("%s: %s is a choice without names", m.id, d.key);
            if (!d.values.empty() && d.values.size() != d.choices.size()) FAIL("%s: %s has %zu names and %zu values", m.id, d.key, d.choices.size(), d.values.size());
            if (d.def < 0 || d.def >= (double)d.choices.size()) FAIL("%s: the default of %s is not one of its choices", m.id, d.key);
        } else if (d.type != TxPType::Text && d.type != TxPType::File && !d.custom && (d.def < d.lo - 1e-9 || d.def > d.hi + 1e-9))
            FAIL("%s: the default %g of %s is outside %g .. %g", m.id, d.def, d.key, d.lo, d.hi);
        if (d.when) {
            const std::string w = d.when;
            const std::string key = w.substr(0, w.find_first_of("=<>!"));
            if (!txParamFind(rows, key)) FAIL("%s: %s depends on %s, which is not in the table", m.id, d.key, key.c_str());
        }
    }
    (void)c;
}

static void checkMode(const TxModeSpec& m, const TxParamCtx& c) {
    const auto& rows = txParams(m, c);
    checkTable(m, c, rows);
    double probe = 0.25;
    for (const auto& d : rows) if (d.probeSec > 0) probe = std::max(probe, d.probeSec);
    Values base;
    for (const auto& d : rows) {
        base.num[d.key] = d.def;
        if (d.type == TxPType::Text) base.txt[d.key] = sample(d.field, false);
    }
    std::set<std::string> declared;
    for (const auto& d : rows) declared.insert(d.key);
    for (const auto& k : txParamRadioKeys()) declared.insert(k);
    std::vector<cf32> a, a2, b;
    std::vector<std::string> keys;
    std::string err;
    const std::string tag = std::string(m.id) + (c.analogVoice ? " (analog voice)" : "") + (c.variant == 1 ? " (DATV)" : "");
    if (!make(m, c, rows, base, false, probe, a, keys, err)) { noGen.push_back(tag + ": " + err); FAIL("%s: no generator with the defaults: %s", tag.c_str(), err.c_str()); return; }
    // a bursty generator may be silent at first: the window grows until it holds some signal (up to 8 s), and every row is compared over at least that
    auto energy = [](const std::vector<cf32>& x) { double e = 0; for (const auto& v : x) e += std::norm(v); return e; };
    while (energy(a) < 1e-9 && probe < 8) {
        probe = std::min(8.0, probe * 4);
        make(m, c, rows, base, false, probe, a, keys, err);
    }
    auto checkKeys = [&](const char* what) {
        for (const auto& k : keys) if (!declared.count(k)) { FAIL("%s: the generator reads %s, which is not a row of the table (%s)", tag.c_str(), k.c_str(), what); declared.insert(k); }
    };
    // a UTC-slot generator (FT8 family, JT65 with its 60 s slots) is silent until its next slot: still silent after 8 s means the slot lies
    // beyond the window at this time of the minute, which is the clock again, not a broken generator
    if (energy(a) < 1e-9 && txPayloadKind(m.id) == TxPayloadKind::Ftx) { nondet.push_back(tag + " (waits for its UTC slot)"); return; }
    checkKeys("defaults");
    make(m, c, rows, base, false, probe, a2, keys, err);
    modesChecked++;
    if (differs(a, a2)) { nondet.push_back(tag); return; }
    for (const auto& d : rows) {
        if (d.custom || d.hidden) continue;
        if (d.meta) { rowsMeta++; continue; }
        Values on = base, off = base;
        reveal(d, rows, on);
        reveal(d, rows, off);
        if (d.type == TxPType::Text) on.txt[d.key] = *d.testText ? d.testText : sample(d.field, true);
        else if (d.type == TxPType::File) continue;
        else on.num[d.key] = changedValue(d);
        const double sec = std::max(probe, d.probeSec);
        std::vector<cf32> x, y;
        if (!make(m, c, rows, off, d.needsAudio, sec, x, keys, err)) { FAIL("%s: no generator with %s revealed: %s", tag.c_str(), d.key, err.c_str()); continue; }
        if (!make(m, c, rows, on, d.needsAudio, sec, y, keys, err)) { FAIL("%s: no generator with %s changed: %s", tag.c_str(), d.key, err.c_str()); continue; }
        checkKeys(d.key);
        rowsChecked++;
        if (!differs(x, y)) FAIL("%s: \"%s\" (%s) changes nothing in the first %.2f s of the signal", tag.c_str(), d.label, d.key, sec);
    }
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;   // one mode id: only that one
    for (const auto& m : txModes()) {
        if (only && strcmp(only, m.id)) continue;
        std::vector<TxParamCtx> ctxs(1);
        const auto& rows = txParams(m, ctxs[0]);
        if (txParamFind(rows, "analog.voice")) { TxParamCtx a; a.analogVoice = true; ctxs.push_back(a); }
        if (txParamFind(rows, "qo100.path")) { TxParamCtx d; d.variant = 1; ctxs.push_back(d); }
        for (const auto& c : ctxs) checkMode(m, c);
        fflush(stdout);
    }
    // the sub-mode that has its own table
    if (!only || !strcmp(only, "marine")) { TxParamCtx w; w.subId = "wefax"; checkMode(*txModeById("marine"), w); }
    printf("modes and sub-modes checked: %d, rows that change the signal: %d, rows marked as not changing the samples: %d\n", modesChecked, rowsChecked, rowsMeta);
    if (!nondet.empty()) {
        printf("clock-dependent signals (rows not compared): %zu:", nondet.size());
        for (const auto& s : nondet) printf(" %s;", s.c_str());
        printf("\n");
    }
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL PASSED\n");
    return 0;
}
