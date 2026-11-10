#include "dect2/sweep/survey.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace sweep {

void Survey::clear() { times_.clear(); floor_.clear(); cells_.clear(); bins_ = 0; startHz_ = binHz_ = 0; }

bool Survey::add(const Sweep& s) {
    if (s.bins() == 0 || s.binHz <= 0) return false;
    if (bins_ == 0) { startHz_ = s.startHz; binHz_ = s.binHz; bins_ = s.bins(); }
    std::vector<uint8_t> row(bins_, 0);
    bool any = false;
    // each target bin takes the maximum of the source bins whose centres fall into it (nearest bin when the source is coarser)
    std::vector<float> best(bins_, -1e9f);
    for (size_t j = 0; j < s.bins(); j++) {
        const double f = s.freqOf(j);
        const long i = std::lround((f - startHz_) / binHz_);
        if (i < 0 || i >= (long)bins_) continue;
        best[i] = std::max(best[i], s.db[j] - s.floorDb[j]);
        any = true;
    }
    {   // coarser source: fill the holes from the nearest source bin
        for (size_t i = 0; i < bins_; i++) {
            if (best[i] > -1e8f) continue;
            const double f = startHz_ + (double)i * binHz_;
            const long j = std::lround((f - s.startHz) / s.binHz);
            if (j >= 0 && j < (long)s.bins() && std::fabs(s.freqOf((size_t)j) - f) <= s.binHz) { best[i] = s.db[j] - s.floorDb[j]; any = true; }
        }
    }
    if (!any) return false;
    for (size_t i = 0; i < bins_; i++) row[i] = best[i] <= 0 ? 0 : (uint8_t)std::min(255.f, std::round(best[i] * 2.f));
    times_.push_back(s.timeSec);
    floor_.push_back(s.floorOverallDb);
    cells_.insert(cells_.end(), row.begin(), row.end());
    if (times_.size() > cfg_.maxRows) merge();
    return true;
}

// the older half of the rows is merged pairwise (maximum of the levels, the earlier time, the mean floor)
void Survey::merge() {
    const size_t n = times_.size(), half = n / 2 & ~size_t(1);
    std::vector<double> t; std::vector<float> f; std::vector<uint8_t> c;
    for (size_t r = 0; r + 1 < half + 1 && r < half; r += 2) {
        t.push_back(times_[r]); f.push_back(0.5f * (floor_[r] + floor_[r + 1]));
        for (size_t i = 0; i < bins_; i++) c.push_back(std::max(cells_[r * bins_ + i], cells_[(r + 1) * bins_ + i]));
    }
    for (size_t r = half; r < n; r++) { t.push_back(times_[r]); f.push_back(floor_[r]); c.insert(c.end(), cells_.begin() + r * bins_, cells_.begin() + (r + 1) * bins_); }
    times_ = std::move(t); floor_ = std::move(f); cells_ = std::move(c);
}

std::vector<float> Survey::occupancy() const {
    std::vector<float> o(bins_, 0.f);
    if (times_.empty()) return o;
    const uint8_t thr = (uint8_t)std::min(255.0, std::ceil(cfg_.thresholdDb * 2));
    for (size_t r = 0; r < times_.size(); r++) for (size_t i = 0; i < bins_; i++) if (cells_[r * bins_ + i] >= thr) o[i] += 1.f;
    for (auto& v : o) v /= (float)times_.size();
    return o;
}
std::vector<float> Survey::maxLevel() const {
    std::vector<float> m(bins_, 0.f);
    for (size_t r = 0; r < times_.size(); r++) for (size_t i = 0; i < bins_; i++) m[i] = std::max(m[i], level(r, i));
    return m;
}
std::vector<float> Survey::meanLevel() const {
    std::vector<float> s(bins_, 0.f); std::vector<int> n(bins_, 0);
    const uint8_t thr = (uint8_t)std::min(255.0, std::ceil(cfg_.thresholdDb * 2));
    for (size_t r = 0; r < times_.size(); r++) for (size_t i = 0; i < bins_; i++) { const uint8_t v = cells_[r * bins_ + i]; if (v >= thr) { s[i] += v * 0.5f; n[i]++; } }
    for (size_t i = 0; i < bins_; i++) s[i] = n[i] ? s[i] / (float)n[i] : 0.f;
    return s;
}
float Survey::meanFloorDb() const {
    if (floor_.empty()) return -150.f;
    double a = 0; for (float f : floor_) a += f;
    return (float)(a / (double)floor_.size());
}

std::vector<RecurringSignal> Survey::recurring(int minStarts, double maxJitter) const {
    std::vector<RecurringSignal> out;
    const size_t R = rows();
    if (R < (size_t)minStarts * 2 || !bins_) return out;
    const std::vector<float> occ = occupancy();
    // adjacent bins that were ever on and not always on, one signal per run; the row is "on" when any bin of the run is above the threshold
    const uint8_t on = (uint8_t)std::min(255.0, cfg_.thresholdDb * 2.0);
    for (size_t b = 0; b < bins_;) {
        if (occ[b] <= 0 || occ[b] > 0.9f) { b++; continue; }
        size_t e = b;
        while (e + 1 < bins_ && occ[e + 1] > 0 && occ[e + 1] <= 0.9f) e++;
        std::vector<double> st;
        bool was = false;
        float occSum = 0;
        for (size_t r = 0; r < R; r++) {
            bool now = false;
            for (size_t i = b; i <= e && !now; i++) now = cells_[r * bins_ + i] >= on;
            if (now && !was) st.push_back(times_[r]);
            if (now) occSum++;
            was = now;
        }
        if ((int)st.size() >= minStarts) {
            std::vector<double> d;
            for (size_t i = 1; i < st.size(); i++) d.push_back(st[i] - st[i - 1]);
            std::vector<double> sorted = d; std::sort(sorted.begin(), sorted.end());
            const double med = sorted[sorted.size() / 2];
            double dev = 0; for (double x : d) dev += std::fabs(x - med);
            dev /= d.size();
            if (med > 0 && dev <= maxJitter * med + 0.51 * (R > 1 ? (times_[R - 1] - times_[0]) / (R - 1) : 0)) {
                RecurringSignal q;
                q.freqHz = freqOf((b + e) / 2); q.periodSec = med; q.starts = (int)st.size(); q.jitter = (float)(dev / med); q.occupancy = occSum / R;
                q.nextSec = st.back() + med;
                while (q.nextSec < times_.back()) q.nextSec += med;
                out.push_back(q);
            }
        }
        b = e + 1;
    }
    std::sort(out.begin(), out.end(), [](const RecurringSignal& a, const RecurringSignal& b) { return a.starts > b.starts; });
    return out;
}

std::vector<PersistentSignal> Survey::persistent(double minOcc, int bridge) const {
    std::vector<PersistentSignal> out;
    const auto occ = occupancy(), mean = meanLevel(), mx = maxLevel();
    size_t i = 0;
    while (i < bins_) {
        if (occ[i] < minOcc) { i++; continue; }
        size_t e = i + 1, gap = 0;
        while (e < bins_ && (occ[e] >= minOcc || gap < (size_t)bridge)) { gap = occ[e] >= minOcc ? 0 : gap + 1; e++; }
        e -= gap;
        // the run's strongest bin is the signal's frequency
        size_t pk = i; double oc = 0, mn = 0; float m = 0;
        for (size_t k = i; k < e; k++) { if (mean[k] > mean[pk]) pk = k; oc = std::max(oc, (double)occ[k]); mn = std::max(mn, (double)mean[k]); m = std::max(m, mx[k]); }
        PersistentSignal p;
        p.freqHz = freqOf(pk); p.bwHz = (double)(e - i) * binHz_; p.occupancy = (float)oc; p.meanDb = (float)mn; p.maxDb = m;
        out.push_back(p);
        i = e + gap;
    }
    std::stable_sort(out.begin(), out.end(), [](const PersistentSignal& a, const PersistentSignal& b) { return a.occupancy > b.occupancy; });
    return out;
}

std::vector<QuietSlot> Survey::quiet(double widthHz, int n) const {
    std::vector<QuietSlot> out;
    if (bins_ == 0 || times_.empty()) return out;
    const size_t w = std::max<size_t>(1, (size_t)std::llround(widthHz / binHz_));
    if (w > bins_) return out;
    const auto occ = occupancy(), mx = maxLevel();
    std::vector<double> po(bins_ + 1, 0.0);
    for (size_t i = 0; i < bins_; i++) po[i + 1] = po[i] + occ[i];
    struct C { size_t i; double o; float m; };
    std::vector<C> c;
    for (size_t i = 0; i + w <= bins_; i++) {
        float m = 0; for (size_t k = i; k < i + w; k++) m = std::max(m, mx[k]);
        c.push_back({i, (po[i + w] - po[i]) / (double)w, m});
    }
    std::stable_sort(c.begin(), c.end(), [](const C& a, const C& b) { return a.o != b.o ? a.o < b.o : a.m < b.m; });
    std::vector<char> used(bins_, 0);
    for (const auto& x : c) {
        if ((int)out.size() >= n) break;
        bool ok = true; for (size_t k = x.i; k < x.i + w && ok; k++) if (used[k]) ok = false;
        if (!ok) continue;
        for (size_t k = x.i; k < x.i + w; k++) used[k] = 1;
        QuietSlot q; q.startHz = freqOf(x.i) - binHz_ / 2; q.widthHz = (double)w * binHz_; q.occupancy = (float)x.o; q.maxDb = x.m;
        out.push_back(q);
    }
    return out;
}

std::vector<uint8_t> Survey::heatmap(int W, int H, double full) const {
    std::vector<uint8_t> img((size_t)W * H * 4, 0);
    if (W <= 0 || H <= 0 || times_.empty() || bins_ == 0) return img;
    for (int y = 0; y < H; y++) {
        const size_t r0 = (size_t)y * rows() / H, r1 = std::max(r0 + 1, (size_t)(y + 1) * rows() / H);
        for (int x = 0; x < W; x++) {
            const size_t b0 = (size_t)x * bins_ / W, b1 = std::max(b0 + 1, (size_t)(x + 1) * bins_ / W);
            uint8_t m = 0;
            for (size_t r = r0; r < std::min(r1, rows()); r++) for (size_t b = b0; b < std::min(b1, bins_); b++) m = std::max(m, cells_[r * bins_ + b]);
            const double t = std::min(1.0, m * 0.5 / full);
            // black - blue - cyan - yellow - red
            double R, G, B;
            if (t < 0.25) { R = 0; G = 0; B = t / 0.25; }
            else if (t < 0.5) { R = 0; G = (t - .25) / .25; B = 1; }
            else if (t < 0.75) { R = (t - .5) / .25; G = 1; B = 1 - (t - .5) / .25; }
            else { R = 1; G = 1 - (t - .75) / .25; B = 0; }
            uint8_t* p = &img[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)(R * 255); p[1] = (uint8_t)(G * 255); p[2] = (uint8_t)(B * 255); p[3] = 255;
        }
    }
    return img;
}

namespace {
const char kMagic[8] = {'O', 'A', 'S', 'U', 'R', 'V', '0', '1'};
}

bool Survey::save(const std::string& path, std::string& err) const {
    FILE* f = u8fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    const uint64_t nr = times_.size(), nb = bins_;
    bool ok = fwrite(kMagic, 1, 8, f) == 8;
    ok = ok && fwrite(&startHz_, 8, 1, f) == 1 && fwrite(&binHz_, 8, 1, f) == 1 && fwrite(&nb, 8, 1, f) == 1 && fwrite(&nr, 8, 1, f) == 1
        && fwrite(&cfg_.thresholdDb, 8, 1, f) == 1;
    if (nr) ok = ok && fwrite(times_.data(), 8, nr, f) == nr && fwrite(floor_.data(), 4, nr, f) == nr && fwrite(cells_.data(), 1, cells_.size(), f) == cells_.size();
    fclose(f);
    if (!ok) err = "write failed: " + path;
    return ok;
}

bool Survey::load(const std::string& path, std::string& err) {
    FILE* f = u8fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    char mg[8]; uint64_t nr = 0, nb = 0; double st = 0, bh = 0, thr = 0;
    bool ok = fread(mg, 1, 8, f) == 8 && !memcmp(mg, kMagic, 8) && fread(&st, 8, 1, f) == 1 && fread(&bh, 8, 1, f) == 1 && fread(&nb, 8, 1, f) == 1
        && fread(&nr, 8, 1, f) == 1 && fread(&thr, 8, 1, f) == 1;
    ok = ok && bh > 0 && nb > 0 && nb < (1u << 24) && nr < (1u << 24) && nb * nr < (1ull << 31);
    std::vector<double> t(ok ? nr : 0); std::vector<float> fl(ok ? nr : 0); std::vector<uint8_t> c(ok ? nr * nb : 0);
    if (ok && nr) ok = fread(t.data(), 8, nr, f) == nr && fread(fl.data(), 4, nr, f) == nr && fread(c.data(), 1, c.size(), f) == c.size();
    fclose(f);
    if (!ok) { err = "not a band survey file: " + path; return false; }
    startHz_ = st; binHz_ = bh; bins_ = nb; cfg_.thresholdDb = thr; times_ = std::move(t); floor_ = std::move(fl); cells_ = std::move(c);
    return true;
}

bool Survey::exportOccupancyCsv(const std::string& path, std::string& err) const {
    FILE* f = u8fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    const auto o = occupancy(), m = meanLevel(), x = maxLevel();
    fprintf(f, "freq_hz,occupancy,mean_db_above_floor,max_db_above_floor\n");
    for (size_t i = 0; i < bins_; i++) fprintf(f, "%.0f,%.4f,%.1f,%.1f\n", freqOf(i), o[i], m[i], x[i]);
    fclose(f);
    return true;
}

bool Survey::exportSignalsCsv(const std::string& path, double minOcc, std::string& err) const {
    FILE* f = u8fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    fprintf(f, "freq_hz,bandwidth_hz,occupancy,mean_db_above_floor,max_db_above_floor\n");
    for (const auto& p : persistent(minOcc)) fprintf(f, "%.0f,%.0f,%.4f,%.1f,%.1f\n", p.freqHz, p.bwHz, p.occupancy, p.meanDb, p.maxDb);
    fclose(f);
    return true;
}

SurveyDiff compareSurveys(const Survey& a, const Survey& b, double minOcc) {
    SurveyDiff d;
    d.floorChangeDb = b.meanFloorDb() - a.meanFloorDb();
    auto mean = [](const std::vector<float>& v) { double s = 0; for (float x : v) s += x; return v.empty() ? 0.0 : s / (double)v.size(); };
    d.occupancyChange = (float)(mean(b.occupancy()) - mean(a.occupancy()));
    const auto pa = a.persistent(minOcc), pb = b.persistent(minOcc);
    const double tol = std::max(a.binHz(), b.binHz()) * 1.5;
    auto has = [&](const std::vector<PersistentSignal>& v, const PersistentSignal& p) {
        for (const auto& q : v) if (std::fabs(q.freqHz - p.freqHz) <= tol + 0.5 * std::min(q.bwHz, p.bwHz)) return true;
        return false;
    };
    for (const auto& p : pb) if (!has(pa, p)) d.appeared.push_back(p);
    for (const auto& p : pa) if (!has(pb, p)) d.vanished.push_back(p);
    return d;
}

} // namespace sweep
} // namespace dect2
