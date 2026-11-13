// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Station health report: see core/include/dect2/sweep/station_health.h and docs/STATION_HEALTH.md.
#include "dect2/sweep/station_health.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace sweep {

static double db10(double x) { return 10.0 * std::log10(std::max(x, 1e-30)); }

std::vector<HealthBand> healthBands(double minHz, double maxHz) {
    std::vector<HealthBand> all = {
        {"fm", 87.5e6, 108e6, 25e3, HealthRef::Fm},
        {"air", 118e6, 137e6, 25e3, HealthRef::None},
        {"dab", 174e6, 240e6, 100e3, HealthRef::Dab},
        {"uhf", 470e6, 694e6, 100e3, HealthRef::DvbT},
        {"ism868", 863e6, 870e6, 25e3, HealthRef::None},
        {"adsb", 1080e6, 1100e6, 50e3, HealthRef::None},
    };
    std::vector<HealthBand> out;
    for (const auto& b : all) {
        if (minHz > 0 && b.startHz < minHz) continue;
        if (maxHz > 0 && b.stopHz > maxHz) continue;
        out.push_back(b);
    }
    return out;
}

double adcQuantFloorDbfs(int adcBits) { return -(6.02 * adcBits + 1.76); }

double adcMarginDb(double floorDbPerBin, double binHz, double rateHz, int adcBits) {
    // white noise of power P over the rate shows in one Hann-windowed bin as P * 1.5 / N, a bin being rate / N wide: P = bin power + 10 log10(rate / bin) - 1.76 dB
    const double noiseTotal = floorDbPerBin + db10(std::max(rateHz, binHz) / binHz) - 1.76;
    return noiseTotal - adcQuantFloorDbfs(adcBits);
}

AdcStats measureAdc(const cf32* x, size_t n, float clipLevel) {
    AdcStats s;
    s.n = n;
    if (!n) return s;
    double p = 0;
    size_t clip = 0;
    float pk = 0;
    for (size_t i = 0; i < n; i++) {
        const float re = std::fabs(x[i].real()), im = std::fabs(x[i].imag());
        p += (double)re * re + (double)im * im;
        const float m = std::max(re, im);
        pk = std::max(pk, m);
        if (m >= clipLevel) clip++;
    }
    s.rmsDbfs = db10(p / (double)n);
    s.peak = pk;
    s.clipFraction = (double)clip / (double)n;
    return s;
}

namespace {
struct RefSpec { double minBw, maxBw, minSnr, expect; };
RefSpec specOf(HealthRef r) {
    switch (r) {
    case HealthRef::Fm: return {60e3, 450e3, 10, 25};       // a broadcast carrier with its sidebands: about 150 to 250 kHz
    case HealthRef::Dab: return {1.0e6, 1.9e6, 8, 12};      // one 1.536 MHz ensemble
    case HealthRef::DvbT: return {5.5e6, 8.8e6, 8, 12};     // a 6, 7 or 8 MHz channel (flat top; the skirts fall under the threshold)
    default: return {0, 0, 0, 0};
    }
}
} // namespace

const char* refName(HealthRef r) {
    switch (r) {
    case HealthRef::Fm: return "FM";
    case HealthRef::Dab: return "DAB";
    case HealthRef::DvbT: return "DVB-T";
    default: return "";
    }
}

BandResult summariseBand(const HealthBand& b, const Sweep& s, const AdcStats& adc, int adcBits) {
    BandResult r;
    r.band = b;
    r.adc = adc;
    r.rateHz = s.rateHz;
    if (s.bins() == 0) return r;
    r.measured = true;
    r.floorDb = s.floorOverallDb;
    const double bin = s.binHz > 0 ? s.binHz : b.binHz;
    r.densityDbfsHz = r.floorDb - db10(bin);
    r.noiseTotalDbfs = r.floorDb + db10(std::max(s.rateHz, bin) / bin) - 1.76;
    r.adcMarginDb = adcMarginDb(r.floorDb, bin, s.rateHz, adcBits);
    size_t occ = 0;
    for (size_t i = 0; i < s.bins(); i++) {
        r.peakDb = std::max<double>(r.peakDb, s.db[i]);
        const double fl = i < s.floorDb.size() ? s.floorDb[i] : s.floorOverallDb;
        if (s.db[i] >= fl + 8) occ++;
    }
    r.peakSnrDb = r.peakDb - r.floorDb;
    r.occupancyPct = 100.0 * (double)occ / (double)s.bins();
    const std::vector<Detection> det = findSignals(s, 8);
    r.nSignals = (int)det.size();
    if (b.ref != HealthRef::None) {
        const RefSpec sp = specOf(b.ref);
        r.ref.kind = b.ref;
        r.ref.expectedMinDb = sp.expect;
        r.ref.level = RefLevel::Absent;
        double best = -1;
        for (const auto& d : det) {
            if (d.bwHz < sp.minBw || d.bwHz > sp.maxBw || d.snrDb < sp.minSnr) continue;
            r.ref.count++;
            if (d.snrDb > best) { best = d.snrDb; r.ref.strongestHz = d.freqHz; r.ref.strongestSnrDb = d.snrDb; }
        }
        if (r.ref.count > 0) r.ref.level = r.ref.strongestSnrDb >= sp.expect ? RefLevel::Ok : RefLevel::Weak;
    }
    return r;
}

PpmResult frequencyError(const Sweep& s, double expectedHz, double searchHz) {
    PpmResult r;
    r.expectedHz = expectedHz;
    const size_t n = s.bins();
    if (n < 5 || s.binHz <= 0 || expectedHz <= 0) return r;
    long best = -1;
    for (size_t i = 1; i + 1 < n; i++) {
        if (std::fabs(s.freqOf(i) - expectedHz) > searchHz) continue;
        if (best < 0 || s.db[i] > s.db[(size_t)best]) best = (long)i;
    }
    if (best < 0) return r;
    const size_t k = (size_t)best;
    const double fl = k < s.floorDb.size() ? s.floorDb[k] : s.floorOverallDb;
    if (s.db[k] < fl + 10) return r;   // no carrier there
    double f = s.freqOf(k);
    const double l = s.db[k - 1], m = s.db[k], rr = s.db[k + 1], den = l - 2 * m + rr;
    if (den < 0) f += std::max(-0.5, std::min(0.5, 0.5 * (l - rr) / den)) * s.binHz;
    r.valid = true;
    r.measuredHz = f;
    r.errorHz = f - expectedHz;
    r.ppm = r.errorHz / expectedHz * 1e6;
    return r;
}

RangeResult assessRange(const float* sec, int n, unsigned long long positions) {
    RangeResult r;
    if (!sec || n < 4 || positions < 50) return r;
    double mx = 0, sum = 0, mn = 1e30;
    for (int i = 0; i < n; i++) {
        if (sec[i] > 0) {
            r.sectorsWithData++;
            sum += sec[i];
            if (sec[i] > mx) { mx = sec[i]; r.bestSector = i; }
            if (sec[i] < mn) { mn = sec[i]; r.worstSector = i; }
        }
    }
    if (r.sectorsWithData < 4) return r;
    r.valid = true;
    r.maxNm = mx;
    r.meanSectorNm = sum / r.sectorsWithData;
    r.ratio = mn / mx;
    // blind: a run of empty sectors with a decent range on both sides
    int firstBlind = -1;
    for (int i = 0; i < n; i++) {
        if (sec[i] > 0 || sec[(i + n - 1) % n] <= 0) continue;   // not the start of a run
        int len = 0;
        while (len < n && sec[(i + len) % n] <= 0) len++;
        if (len >= n) break;
        if (sec[(i + n - 1) % n] > 0.4 * mx && sec[(i + len) % n] > 0.4 * mx) {
            r.blindSectors += len;
            if (firstBlind < 0) firstBlind = i;
        }
    }
    if (firstBlind >= 0) r.blindStart = firstBlind;
    return r;
}

namespace {
void add(std::vector<Finding>& v, FindCode c, Severity sv, const std::string& where, double a = 0, double b = 0) { v.push_back({c, sv, where, a, b}); }
} // namespace

HealthReport assessHealth(const HealthInput& in) {
    HealthReport rep;
    rep.in = in;
    std::vector<Finding>& f = rep.findings;
    std::vector<const BandResult*> ok;
    for (const auto& b : in.bands) if (b.measured) ok.push_back(&b);

    const bool gainAtMax = in.gainMaxDb > 0 && in.gainDb >= in.gainMaxDb - 1;
    bool anyAdcLimited = false;
    const BandResult* strongest = nullptr;
    for (const BandResult* b : ok) {
        const std::string id = b->band.id;
        const bool over = b->adc.clipFraction > 0.001 || b->adc.peak >= 0.99;
        if (over) {
            add(f, FindCode::Overload, Severity::Bad, id, b->adc.clipFraction * 100, b->adc.peak);
            if (!strongest || b->peakSnrDb > strongest->peakSnrDb) strongest = b;
        } else if (b->adc.n > 0 && b->adc.rmsDbfs > -12) {
            add(f, FindCode::HotHeadroom, Severity::Warn, id, b->adc.rmsDbfs);
        }
        if (!over && b->adcMarginDb < 6) {
            anyAdcLimited = true;
            add(f, gainAtMax ? FindCode::AdcLimitedMax : FindCode::AdcLimited, Severity::Warn, id, b->adcMarginDb);
        } else if (!over && b->adcMarginDb > 26 && b->adc.rmsDbfs > -30) {
            // the noise sits far above the ADC's own and the signal level is high already: the gain buys no sensitivity
            add(f, FindCode::WastedGain, Severity::Info, id, b->adcMarginDb - 20);
        }
    }
    // a band that overloads while another band is ADC-limited at the same gain: the strong band needs a filter, not less gain
    if (strongest && anyAdcLimited) {
        for (auto& x : f) if (x.code == FindCode::Overload && x.where == strongest->band.id) {
            x.code = FindCode::StrongOutOfBand;
            x.a = strongest->peakSnrDb;
            x.b = strongest->adc.clipFraction * 100;
        }
    }
    // excess noise against the quietest band, allowing for the galactic noise that rises towards low frequencies
    if (ok.size() >= 2) {
        auto allow = [](double hz) { return hz < 300e6 ? 15.0 * std::log10(300e6 / hz) : 0.0; };
        double quiet = 1e9;
        for (const BandResult* b : ok) quiet = std::min(quiet, b->densityDbfsHz - allow(0.5 * (b->band.startHz + b->band.stopHz)));
        for (const BandResult* b : ok) {
            const double ex = b->densityDbfsHz - allow(0.5 * (b->band.startHz + b->band.stopHz)) - quiet;
            if (ex > 12) add(f, FindCode::NoiseExcess, ex > 20 ? Severity::Bad : Severity::Warn, b->band.id, ex);
        }
    }
    for (const BandResult* b : ok) {
        if (b->ref.level == RefLevel::Absent) add(f, FindCode::RefAbsent, Severity::Warn, refName(b->ref.kind), b->ref.expectedMinDb);
        else if (b->ref.level == RefLevel::Weak) add(f, FindCode::RefWeak, Severity::Warn, refName(b->ref.kind), b->ref.strongestSnrDb, b->ref.expectedMinDb);
    }
    if (in.havePpm && in.ppm.valid)
        add(f, std::fabs(in.ppm.ppm) > in.ppmLimit ? FindCode::PpmHigh : FindCode::PpmOk, std::fabs(in.ppm.ppm) > in.ppmLimit ? Severity::Warn : Severity::Info, "", in.ppm.ppm);
    if (in.haveRange) {
        if (!in.range.valid) add(f, FindCode::RangeNone, Severity::Info, "");
        else {
            if (in.range.maxNm < 60) add(f, FindCode::RangeShort, in.range.maxNm < 25 ? Severity::Bad : Severity::Warn, "", in.range.maxNm, in.range.worstSector * 10);
            if (in.range.blindSectors > 0) add(f, FindCode::RangeBlind, Severity::Warn, "", in.range.blindSectors, in.range.blindStart * 10);
        }
    }
    bool problem = false;
    for (const auto& x : f) if (x.severity != Severity::Info) problem = true;
    if (!problem && !ok.empty()) add(f, FindCode::AllGood, Severity::Info, "");
    std::stable_sort(f.begin(), f.end(), [](const Finding& a, const Finding& b) { return (int)a.severity > (int)b.severity; });
    for (const auto& x : f) if ((int)x.severity > (int)rep.worst) rep.worst = x.severity;
    return rep;
}

const char* findCodeName(FindCode c) {
    switch (c) {
    case FindCode::Overload: return "overload";
    case FindCode::HotHeadroom: return "hot_headroom";
    case FindCode::AdcLimited: return "adc_limited";
    case FindCode::AdcLimitedMax: return "adc_limited_max_gain";
    case FindCode::WastedGain: return "wasted_gain";
    case FindCode::StrongOutOfBand: return "strong_out_of_band";
    case FindCode::NoiseExcess: return "noise_excess";
    case FindCode::RefAbsent: return "reference_absent";
    case FindCode::RefWeak: return "reference_weak";
    case FindCode::PpmHigh: return "ppm_high";
    case FindCode::PpmOk: return "ppm_ok";
    case FindCode::RangeShort: return "range_short";
    case FindCode::RangeBlind: return "range_blind";
    case FindCode::RangeNone: return "range_none";
    case FindCode::AllGood: return "all_good";
    }
    return "";
}

namespace {
std::string num(double v) { char b[40]; std::snprintf(b, sizeof b, "%.2f", std::isfinite(v) ? v : 0.0); return b; }
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
    return o + "\"";
}
} // namespace

std::string healthJson(const HealthReport& r) {
    std::string j = "{\"gain_db\":" + num(r.in.gainDb) + ",\"bands\":[";
    for (size_t i = 0; i < r.in.bands.size(); i++) {
        const BandResult& b = r.in.bands[i];
        if (i) j += ",";
        j += "{\"id\":" + jstr(b.band.id) + ",\"start_hz\":" + num(b.band.startHz) + ",\"stop_hz\":" + num(b.band.stopHz) + ",\"measured\":" + (b.measured ? "true" : "false");
        if (b.measured) {
            j += ",\"noise_floor_dbfs_per_bin\":" + num(b.floorDb) + ",\"noise_density_dbfs_hz\":" + num(b.densityDbfsHz) + ",\"adc_margin_db\":" + num(b.adcMarginDb) +
                 ",\"peak_snr_db\":" + num(b.peakSnrDb) + ",\"occupancy_pct\":" + num(b.occupancyPct) + ",\"adc_rms_dbfs\":" + num(b.adc.rmsDbfs) +
                 ",\"adc_peak\":" + num(b.adc.peak) + ",\"adc_clip_pct\":" + num(b.adc.clipFraction * 100);
            if (b.ref.kind != HealthRef::None)
                j += ",\"reference\":{\"service\":" + jstr(refName(b.ref.kind)) + ",\"found\":" + std::to_string(b.ref.count) + ",\"strongest_hz\":" + num(b.ref.strongestHz) +
                     ",\"strongest_snr_db\":" + num(b.ref.strongestSnrDb) + ",\"expected_min_snr_db\":" + num(b.ref.expectedMinDb) + "}";
        }
        j += "}";
    }
    j += "]";
    if (r.in.havePpm && r.in.ppm.valid) j += ",\"frequency_error\":{\"expected_hz\":" + num(r.in.ppm.expectedHz) + ",\"measured_hz\":" + num(r.in.ppm.measuredHz) + ",\"ppm\":" + num(r.in.ppm.ppm) + "}";
    if (r.in.haveRange && r.in.range.valid)
        j += ",\"adsb_range\":{\"max_nm\":" + num(r.in.range.maxNm) + ",\"sectors_with_data\":" + std::to_string(r.in.range.sectorsWithData) + ",\"blind_sectors\":" + std::to_string(r.in.range.blindSectors) + "}";
    j += ",\"findings\":[";
    for (size_t i = 0; i < r.findings.size(); i++) {
        const Finding& x = r.findings[i];
        if (i) j += ",";
        j += "{\"code\":" + jstr(findCodeName(x.code)) + ",\"severity\":" + std::to_string((int)x.severity) + ",\"where\":" + jstr(x.where) + ",\"a\":" + num(x.a) + ",\"b\":" + num(x.b) + "}";
    }
    return j + "]}";
}

bool runHealthCheck(SweepSource& src, const HealthConfig& c, std::vector<BandResult>& out, std::string& err,
                    const std::function<void(int, int, const std::string&)>& progress, const std::atomic<bool>* cancel) {
    out.clear();
    const int n = (int)c.bands.size();
    for (int i = 0; i < n; i++) {
        if (cancel && cancel->load()) { err = "cancelled"; return false; }
        const HealthBand& b = c.bands[(size_t)i];
        if (progress) progress(i, n, b.id);
        SweepConfig sc;
        sc.startHz = b.startHz; sc.stopHz = b.stopHz; sc.binHz = b.binHz; sc.dwellSec = c.dwellSec; sc.settleSec = c.settleSec;
        Sweep s;
        if (!runSweep(src, sc, s, err, cancel)) return false;
        AdcStats adc;
        const double mid = 0.5 * (b.startHz + b.stopHz);
        if (src.retune(mid, err)) {
            src.discard(c.settleSec);
            const size_t cnt = (size_t)std::min(2e6, std::max(1e4, c.adcSeconds * src.sampleRate()));
            std::vector<cf32> buf(cnt);
            const size_t got = src.capture(buf.data(), cnt);
            adc = measureAdc(buf.data(), got);
        } else err.clear();
        out.push_back(summariseBand(b, s, adc, c.adcBits));
    }
    return true;
}

} // namespace sweep
} // namespace dect2
