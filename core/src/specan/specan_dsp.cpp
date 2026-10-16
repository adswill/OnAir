// Spectrum analyser maths: see specan_dsp.h
#include "dect2/specan_dsp.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

SaGrid saMakeGrid(double loHz, double hiHz, double binHz, size_t maxBins) {
    SaGrid g;
    if (hiHz < loHz) std::swap(loHz, hiHz);
    if (hiHz - loHz < 1) hiHz = loHz + 1;
    binHz = std::max(binHz, 1.0);
    if ((hiHz - loHz) / binHz > (double)maxBins) binHz = (hiHz - loHz) / (double)maxBins;
    g.loHz = loHz;
    g.binHz = binHz;
    g.bins = (size_t)std::ceil((hiHz - loHz) / binHz - 1e-9);
    return g;
}

void SaStitcher::setGrid(const SaGrid& g) {
    g_ = g;
    acc_.assign(g.bins, 0.0);
    cnt_.assign(g.bins, 0);
    los_.clear();
}

void SaStitcher::beginSweep() {
    std::fill(acc_.begin(), acc_.end(), 0.0);
    std::fill(cnt_.begin(), cnt_.end(), 0u);
    los_.clear();
}

void SaStitcher::add(double lo, double rate, const float* pow, size_t n, double usableHz, double dcGapHz) {
    if (!n || rate <= 0 || !g_.bins) return;
    los_.push_back(lo);
    const double fb = rate / (double)n;                 // FFT bin width
    const double f0 = lo - rate / 2;                    // left edge of FFT bin 0
    usableHz = std::min(usableHz, rate / 2);
    const long k0 = std::max(0L, g_.index(lo - usableHz));
    const long k1 = std::min((long)g_.bins - 1, g_.index(lo + usableHz));
    for (long k = k0; k <= k1; k++) {
        const double a = g_.loHz + k * g_.binHz, b = a + g_.binHz, c = (a + b) / 2;
        if (std::fabs(c - lo) > usableHz || std::fabs(c - lo) < dcGapHz) continue;
        // FFT bins whose centre falls inside the grid bin; none (the FFT is coarser): the nearest one
        long i0 = (long)std::ceil((a - f0) / fb - 0.5), i1 = (long)std::floor((b - f0) / fb - 0.5 - 1e-9);
        if (i1 < i0) i0 = i1 = (long)std::floor((c - f0) / fb);
        i0 = std::max(0L, i0); i1 = std::min((long)n - 1, i1);
        if (i1 < i0) continue;
        double s = 0;
        for (long i = i0; i <= i1; i++) s += pow[i];
        acc_[(size_t)k] += s / (double)(i1 - i0 + 1);
        cnt_[(size_t)k]++;
    }
}

std::vector<float> SaStitcher::finishDb() const {
    std::vector<float> o(g_.bins, -200.f);
    for (size_t k = 0; k < g_.bins; k++)
        if (cnt_[k]) o[k] = (float)(10 * std::log10(std::max(acc_[k] / cnt_[k], 1e-20)));
    return o;
}

size_t SaStitcher::covered() const {
    size_t c = 0;
    for (uint32_t v : cnt_) c += v != 0;
    return c;
}

int saRemoveSpurs(std::vector<float>& db, const SaGrid& g, const std::vector<double>& los, double widthHz, double thr) {
    int changed = 0;
    const long w = std::max(1L, (long)std::ceil(widthHz / g.binHz));
    const long n = (long)db.size();
    std::vector<float> nb;
    for (double lo : los) {
        const long c = g.index(lo);
        if (c < -w || c >= n + w) continue;
        nb.clear();
        for (long j = c - 2 * w - 2; j <= c + 2 * w + 2; j++)
            if (j >= 0 && j < n && std::labs(j - c) > w && db[(size_t)j] > -199) nb.push_back(db[(size_t)j]);
        if (nb.size() < 2) continue;
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
        const float med = nb[nb.size() / 2];
        for (long j = std::max(0L, c - w); j <= std::min(n - 1, c + w); j++)
            if (db[(size_t)j] > med + thr) { db[(size_t)j] = med; changed++; }
    }
    return changed;
}

void SaTraces::update(const std::vector<float>& db) {
    if (db.size() != live.size()) { reset(); peak = db; avg = db; avgPow_.resize(db.size()); for (size_t i = 0; i < db.size(); i++) avgPow_[i] = std::pow(10.0, db[i] / 10.0); }
    live = db;
    sweeps++;
    const double k = 1.0 / (double)std::min<uint64_t>(sweeps, (uint64_t)std::max(1, avgN));
    for (size_t i = 0; i < db.size(); i++) {
        peak[i] = std::max(peak[i], db[i]);
        if (sweeps > 1) avgPow_[i] += (std::pow(10.0, db[i] / 10.0) - avgPow_[i]) * k;
        avg[i] = (float)(10 * std::log10(std::max(avgPow_[i], 1e-20)));
    }
}

size_t saCsvStream(const SaGrid& g, const std::vector<float>& live, const std::vector<float>& peak, const std::vector<float>& avg,
                   const std::function<bool(const char*, size_t)>& sink) {
    std::string s = "frequency_hz,live_dbfs,peak_dbfs,average_dbfs\n";
    char b[128];
    auto col = [&](const std::vector<float>& v, size_t k) {
        if (k < v.size() && v[k] > -199) { snprintf(b, sizeof b, ",%.2f", v[k]); s += b; } else s += ",";
    };
    size_t k = 0;
    for (; k < g.bins; k++) {
        snprintf(b, sizeof b, "%.0f", g.centre(k));
        s += b;
        col(live, k); col(peak, k); col(avg, k);
        s += "\n";
        if (s.size() > (1u << 16)) { if (!sink(s.data(), s.size())) return k; s.clear(); }
    }
    if (!s.empty() && !sink(s.data(), s.size())) return k > 0 ? k - 1 : 0;
    return k;
}

std::string saCsv(const SaGrid& g, const std::vector<float>& live, const std::vector<float>& peak, const std::vector<float>& avg) {
    std::string out;
    out.reserve(40 + g.bins * 36);
    saCsvStream(g, live, peak, avg, [&](const char* p, size_t n) { out.append(p, n); return true; });
    return out;
}

bool saCsvWriteFile(const std::string& path, const SaGrid& g, const std::vector<float>& live, const std::vector<float>& peak, const std::vector<float>& avg) {
    FILE* f = u8fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    saCsvStream(g, live, peak, avg, [&](const char* p, size_t n) { if (fwrite(p, 1, n, f) != n) ok = false; return ok; });
    if (fclose(f) != 0) ok = false;
    return ok;
}

size_t saCsvEstimateBytes(size_t bins) { return 40 + bins * 36; }

const SaBand* saAllocations(int& count) {
    static const SaBand kB[] = {
        {0.1485, 0.2835, "LW", "long-wave broadcasting"},
        {0.5265, 1.6065, "MW", "medium-wave broadcasting"},
        {1.81, 2.0, "160 m", "amateur"},
        {3.5, 3.8, "80 m", "amateur"},
        {5.9, 6.2, "49 m", "short-wave broadcasting"},
        {7.0, 7.2, "40 m", "amateur"},
        {9.4, 9.9, "31 m", "short-wave broadcasting"},
        {14.0, 14.35, "20 m", "amateur"},
        {26.965, 27.405, "CB", "citizens band"},
        {28.0, 29.7, "10 m", "amateur"},
        {50.0, 52.0, "6 m", "amateur"},
        {87.5, 108.0, "FM", "FM broadcasting"},
        {108.0, 117.975, "VOR/ILS", "aeronautical radionavigation"},
        {118.0, 137.0, "Airband", "aeronautical voice (AM)"},
        {137.0, 138.0, "WX sat", "weather satellites (NOAA, Meteor)"},
        {144.0, 146.0, "2 m", "amateur"},
        {146.0, 174.0, "VHF mobile", "land mobile, pagers"},
        {156.0, 162.05, "Marine", "maritime VHF, AIS"},
        {174.0, 230.0, "DAB", "DAB+ (band III)"},
        {225.0, 400.0, "Mil air", "military aviation, satellites"},
        {380.0, 400.0, "TETRA", "emergency services"},
        {430.0, 440.0, "70 cm", "amateur"},
        {433.05, 434.79, "ISM 433", "short-range devices"},
        {446.0, 446.2, "PMR446", "licence-free radios"},
        {470.0, 694.0, "DVB-T", "digital terrestrial TV (UHF)"},
        {694.0, 790.0, "LTE 700", "mobile (band 28 / n28)"},
        {791.0, 862.0, "LTE 800", "mobile (band 20)"},
        {863.0, 870.0, "SRD 868", "short-range devices, LoRa"},
        {876.0, 880.0, "GSM-R up", "railway GSM uplink"},
        {880.0, 915.0, "GSM 900 up", "mobile uplink (band 8)"},
        {921.0, 925.0, "GSM-R", "railway GSM downlink"},
        {925.0, 960.0, "GSM 900", "mobile downlink (band 8)"},
        {960.0, 1215.0, "DME", "aeronautical radionavigation"},
        {1089.0, 1091.0, "ADS-B", "1090 MHz Mode S"},
        {1164.0, 1300.0, "GNSS L5/L2", "satellite navigation"},
        {1240.0, 1300.0, "23 cm", "amateur"},
        {1452.0, 1492.0, "L-band SDL", "mobile downlink (band 32)"},
        {1525.0, 1559.0, "Inmarsat", "mobile satellite downlink"},
        {1559.0, 1610.0, "GNSS L1", "GPS, Galileo, GLONASS, BeiDou"},
        {1616.0, 1626.5, "Iridium", "mobile satellite"},
        {1710.0, 1785.0, "LTE 1800 up", "mobile uplink (band 3)"},
        {1805.0, 1880.0, "LTE 1800", "mobile downlink (band 3)"},
        {1920.0, 1980.0, "UMTS up", "mobile uplink (band 1)"},
        {2110.0, 2170.0, "UMTS 2100", "mobile downlink (band 1)"},
        {2400.0, 2483.5, "ISM 2.4", "Wi-Fi, Bluetooth, microwave ovens"},
        {2500.0, 2690.0, "LTE 2600", "mobile (band 7)"},
        {3400.0, 3800.0, "5G n78", "mobile"},
        {5150.0, 5350.0, "Wi-Fi 5 GHz", "RLAN (UNII-1/2)"},
        {5470.0, 5725.0, "Wi-Fi 5.5", "RLAN (radar DFS)"},
        {5725.0, 5875.0, "ISM 5.8", "Wi-Fi, FPV video, SRD"},
    };
    count = (int)(sizeof kB / sizeof *kB);
    return kB;
}

const SaBand* saAllocationAt(double mhz) {
    int n = 0;
    const SaBand* b = saAllocations(n);
    const SaBand* best = nullptr;
    for (int i = 0; i < n; i++)
        if (mhz >= b[i].loMhz && mhz < b[i].hiMhz && (!best || b[i].hiMhz - b[i].loMhz < best->hiMhz - best->loMhz)) best = &b[i];
    return best;
}

const SaPreset* saPresets(int& count) {
    static const SaPreset kP[] = {
        {"Full 1 - 6000 MHz", 1, 6000}, {"FM broadcast", 87.5, 108}, {"Airband", 108, 137}, {"2 m", 144, 146}, {"70 cm", 430, 440},
        {"ISM 433", 433, 435}, {"ISM 868", 863, 870}, {"Mobile 700 - 2700", 700, 2700}, {"L-band", 1000, 2000},
        {"2.4 GHz", 2400, 2500}, {"5.8 GHz", 5650, 5925}, {"Satellite IF", 950, 2150}, {"Ku LNB IF 739-2150", 739, 2150},
    };
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

double saTickStep(double lo, double hi, int maxTicks) {
    const double span = std::fabs(hi - lo);
    if (span <= 0 || maxTicks < 1) return 1;
    const double raw = span / maxTicks;
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) if (m * p >= raw) return m * p;
    return 10 * p;
}

SaSweepTap& SaSweepTap::get() { static SaSweepTap t; return t; }
void SaSweepTap::setActive(bool on, double rate, double off) { std::lock_guard<std::mutex> lk(mu_); on_ = on; rate_ = rate; off_ = off; q_.clear(); }
bool SaSweepTap::active() const { std::lock_guard<std::mutex> lk(mu_); return on_; }
double SaSweepTap::offsetHz() const { std::lock_guard<std::mutex> lk(mu_); return off_; }
void SaSweepTap::post(double tag, const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on_ || q_.size() > 4096) return;   // nobody reads: drop
    SaBlock b;
    b.loHz = tag + off_;
    b.rateHz = rate_;
    b.iq.assign(x, x + n);
    q_.push_back(std::move(b));
}
bool SaSweepTap::take(std::vector<SaBlock>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    for (auto& b : q_) out.push_back(std::move(b));
    q_.clear();
    return true;
}

} // namespace dect2
