// Radio clips, pure half: histories, range maths, PNG / WAV / ZIP / HTML / Discord text. See clips.h.
#include "clips.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <unordered_set>

namespace clips {

// ================================================================ range maths
Range lastSeconds(double now, double sec, double earliest) {
    Range r{now - sec, now};
    if (earliest > 0 && r.t0 < earliest) r.t0 = earliest;
    if (r.t0 > r.t1) r.t0 = r.t1;
    return r;
}
Range clampRange(Range r, double earliest, double now, double minLen) {
    if (r.t0 > r.t1) std::swap(r.t0, r.t1);
    if (earliest > 0 && r.t0 < earliest) r.t0 = earliest;
    if (r.t1 > now) r.t1 = now;
    if (r.t1 - r.t0 < minLen) {
        r.t0 = r.t1 - minLen;
        if (earliest > 0 && r.t0 < earliest) { r.t0 = earliest; r.t1 = std::min(now, earliest + minLen); }
    }
    return r;
}
Range fromFraction(double earliest, double now, double f0, double f1) {
    f0 = std::min(1.0, std::max(0.0, f0)); f1 = std::min(1.0, std::max(0.0, f1));
    if (f0 > f1) std::swap(f0, f1);
    return {earliest + f0 * (now - earliest), earliest + f1 * (now - earliest)};
}
double fractionOf(double earliest, double now, double t) { return now > earliest ? std::min(1.0, std::max(0.0, (t - earliest) / (now - earliest))) : 0; }

// ================================================================ waterfall history
void WfHistory::configure(bool on, double maxSeconds, size_t maxBytes) {
    const size_t rowsByTime = (size_t)std::max(0.0, std::floor(maxSeconds * kMaxRate));
    const size_t rowsByMem = maxBytes / (W + sizeof(double));
    const size_t cap = on ? std::min(rowsByTime, rowsByMem) : 0;
    on_ = on && cap >= 10;
    if (!on_ || cap != cap_) {
        std::vector<uint8_t>().swap(buf_); std::vector<double>().swap(stamp_); std::vector<uint8_t>().swap(acc_);
        cap_ = 0; head_ = 0; count_ = 0; accHas_ = false;
        if (on_) { cap_ = cap; buf_.assign(cap_ * W, 0); stamp_.assign(cap_, 0); acc_.assign(W, 0); }
    }
}
void WfHistory::clear() { head_ = 0; count_ = 0; accHas_ = false; lastRow_ = 0; }
double WfHistory::oldest() const { return count_ ? stamp_[(head_ + cap_ - count_) % cap_] : 0; }
double WfHistory::newest() const { return count_ ? stamp_[(head_ + cap_ - 1) % cap_] : 0; }
void WfHistory::feed(const float* db, size_t n, double epoch) {
    if (!on_ || !n) return;
    if (lastRow_ == 0 || epoch < lastRow_ - 1) { lastRow_ = epoch; accHas_ = false; }   // first frame, or the clock moved back
    const float k = 255.f / (float)(kDbHi - kDbLo);
    for (int x = 0; x < W; x++) {
        const size_t a = (size_t)x * n / W, b = std::max(a + 1, (size_t)(x + 1) * n / W);
        float m = db[a];
        for (size_t i = a + 1; i < b && i < n; i++) m = std::max(m, db[i]);
        const float v = (m - (float)kDbLo) * k;
        const uint8_t q = v <= 0.f ? 0 : v >= 255.f ? 255 : (uint8_t)v;
        if (!accHas_ || q > acc_[x]) acc_[x] = q;
    }
    accHas_ = true;
    if (epoch < lastRow_) return;   // lastRow_ is the time the next row is due (a steady 10 a second, whatever the frame rate)
    lastRow_ = epoch - lastRow_ > 1.0 ? epoch + 1.0 / kMaxRate : lastRow_ + 1.0 / kMaxRate;
    memcpy(&buf_[head_ * W], acc_.data(), W);
    stamp_[head_] = epoch;
    head_ = (head_ + 1) % cap_;
    count_ = std::min(cap_, count_ + 1);
    accHas_ = false;
}
size_t WfHistory::extract(Range r, std::vector<uint8_t>& out, std::vector<double>& times) const {
    out.clear(); times.clear();
    for (size_t i = 0; i < count_; i++) {
        const size_t k = (head_ + cap_ - count_ + i) % cap_;
        if (stamp_[k] < r.t0 || stamp_[k] >= r.t1) continue;
        out.insert(out.end(), buf_.begin() + (long)(k * W), buf_.begin() + (long)((k + 1) * W));
        times.push_back(stamp_[k]);
    }
    return times.size();
}

Image renderSpectrogram(const std::vector<uint8_t>& rows, size_t nRows, const uint32_t* lut, float minDb, float maxDb, int outW, int maxH) {
    Image im;
    if (!nRows || rows.size() < nRows * WfHistory::W) return im;
    outW = std::max(16, std::min(outW, (int)WfHistory::W));
    maxH = std::max(16, maxH);
    int h = (int)std::min<size_t>(nRows, (size_t)maxH);
    const int minH = 96;
    const int outH = std::max(h, minH);
    im.w = outW; im.h = outH;
    im.rgba.assign((size_t)outW * outH * 4, 255);
    const float step = (float)(WfHistory::kDbHi - WfHistory::kDbLo) / 255.f;
    const float inv = 255.f / std::max(1.f, maxDb - minDb);
    uint8_t colAt[256];   // byte -> lut index
    for (int b = 0; b < 256; b++) {
        const float v = ((float)WfHistory::kDbLo + b * step - minDb) * inv;
        colAt[b] = (uint8_t)(v <= 0.f ? 0 : v >= 255.f ? 255 : (int)v);
    }
    std::vector<uint8_t> line((size_t)outW);
    for (int y = 0; y < outH; y++) {
        const int yy = y * h / outH;   // newest at the top (as the waterfall): row 0 of the image = the last row
        const size_t g0 = (size_t)yy * nRows / h, g1 = std::max(g0 + 1, (size_t)(yy + 1) * nRows / h);
        std::fill(line.begin(), line.end(), 0);
        for (size_t g = g0; g < g1 && g < nRows; g++) {
            const uint8_t* src = &rows[(nRows - 1 - g) * WfHistory::W];
            for (int x = 0; x < outW; x++) {
                const size_t a = (size_t)x * WfHistory::W / outW, b = std::max(a + 1, (size_t)(x + 1) * WfHistory::W / outW);
                uint8_t m = src[a];
                for (size_t i = a + 1; i < b; i++) m = std::max(m, src[i]);
                line[(size_t)x] = std::max(line[(size_t)x], m);
            }
        }
        uint8_t* o = &im.rgba[(size_t)y * outW * 4];
        for (int x = 0; x < outW; x++) {
            const uint32_t c = lut[colAt[line[(size_t)x]]];
            o[4 * x] = c & 255; o[4 * x + 1] = (c >> 8) & 255; o[4 * x + 2] = (c >> 16) & 255; o[4 * x + 3] = 255;
        }
    }
    return im;
}

// ================================================================ decoded rows history
static uint64_t fnv(const std::vector<std::string>& c) {
    uint64_t h = 1469598103934665603ull;
    for (const auto& s : c) { for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ull; } h ^= 0x1F; h *= 1099511628211ull; }
    return h;
}
static size_t rowBytes(const std::vector<std::string>& c) { size_t n = 64; for (const auto& s : c) n += s.size() + 24; return n; }

void TableLog::configure(bool on, double maxSeconds, size_t maxBytes) {
    on_ = on; maxSec_ = maxSeconds; maxBytes_ = maxBytes;
    if (!on) clear();
}
double TableLog::oldest() const { double o = 0; for (const auto& k : kinds_) if (!k.rows.empty() && (o == 0 || k.rows.front().t < o)) o = k.rows.front().t; return o; }
WfHistory& wfHistory() { static WfHistory h; return h; }
TableLog& tableLog() { static TableLog t; return t; }
void TableLog::clear() { kinds_.clear(); bytes_ = 0; }
bool TableLog::due(const std::string& kind, double now) const {
    if (!on_) return false;
    for (const auto& x : kinds_) if (x.name == kind) return now - x.lastObs >= 1.0;
    return true;
}
void TableLog::observe(const std::string& kind, const resexp::Table& t, double now) {
    if (!on_ || t.rows.empty()) return;
    Kind* k = nullptr;
    for (auto& x : kinds_) if (x.name == kind) { k = &x; break; }
    if (!k) { kinds_.push_back(Kind()); k = &kinds_.back(); k->name = kind; }
    if (now - k->lastObs < 1.0) return;
    k->lastObs = now;
    if (k->cols != t.cols) { for (auto& r : k->rows) bytes_ -= rowBytes(r.cells); k->rows.clear(); k->cols = t.cols; }
    std::unordered_set<uint64_t> have;
    have.reserve(k->rows.size() * 2 + 16);
    for (const auto& r : k->rows) have.insert(r.key);
    for (size_t i = 0; i < t.rows.size(); i++) {
        const uint64_t key = fnv(t.rows[i]);
        if (!have.insert(key).second) continue;
        const double e = i < t.epoch.size() && t.epoch[i] > 0 ? t.epoch[i] : now;
        k->rows.push_back({e, t.rows[i], key});
        bytes_ += rowBytes(t.rows[i]);
    }
    trim(now);
}
void TableLog::trim(double now) {
    for (auto& k : kinds_) {
        size_t cut = 0;
        while (cut < k.rows.size() && k.rows[cut].t < now - maxSec_) { bytes_ -= rowBytes(k.rows[cut].cells); cut++; }
        if (cut) k.rows.erase(k.rows.begin(), k.rows.begin() + (long)cut);
    }
    while (bytes_ > maxBytes_) {
        Kind* big = nullptr;
        for (auto& k : kinds_) if (!k.rows.empty() && (!big || k.rows.size() > big->rows.size())) big = &k;
        if (!big) { bytes_ = 0; break; }
        const size_t n = std::max<size_t>(1, big->rows.size() / 10);
        for (size_t i = 0; i < n; i++) bytes_ -= rowBytes(big->rows[i].cells);
        big->rows.erase(big->rows.begin(), big->rows.begin() + (long)n);
    }
}
resexp::Table TableLog::range(Range r, std::string* kindsOut) const {
    resexp::Table out;
    out.cols = {"time_utc", "table"};
    std::vector<std::string> uni;
    for (const auto& k : kinds_) {
        bool any = false;
        for (const auto& row : k.rows) if (row.t >= r.t0 && row.t < r.t1) { any = true; break; }
        if (!any) continue;
        for (const auto& c : k.cols) if (c != "time_utc" && c != "table" && std::find(uni.begin(), uni.end(), c) == uni.end()) uni.push_back(c);
        if (kindsOut) { if (!kindsOut->empty()) *kindsOut += ", "; *kindsOut += k.name; }
    }
    out.cols.insert(out.cols.end(), uni.begin(), uni.end());
    struct Item { double t; const Kind* k; const Row* r; };
    std::vector<Item> items;
    for (const auto& k : kinds_) for (const auto& row : k.rows) if (row.t >= r.t0 && row.t < r.t1) items.push_back({row.t, &k, &row});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t < b.t; });
    for (const auto& it : items) {
        std::vector<std::string> cells(out.cols.size());
        cells[0] = utcText(it.t, true);
        cells[1] = it.k->name;
        for (size_t c = 0; c < it.k->cols.size() && c < it.r->cells.size(); c++) {
            const auto p = std::find(out.cols.begin() + 2, out.cols.end(), it.k->cols[c]);
            if (p != out.cols.end()) cells[(size_t)(p - out.cols.begin())] = it.r->cells[c];
        }
        out.add(std::move(cells), it.t);
    }
    return out;
}

}  // namespace clips
