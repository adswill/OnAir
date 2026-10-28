// Auto-labelled spectrum, the pure part: the labels (name, frequency, source), a store sorted by frequency, and the layout that places the names
// of the frequencies in view on the plot without overlap: highest priority first, stacked in rows, thinned out when the view is wide, and spelled
// out when there is room. No ImGui and no App in here, so tests/app/test_autolabel.cpp drives it directly. The App side is autolabel.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace autolabel {

enum Src { Band = 0, FreqDb, Fm, Dab, Dvb, Memories, NumSrc };   // where a name comes from: the band plan, the frequency database, RDS names, DAB ensembles, DVB networks and services, the user's memories
constexpr unsigned kAllSources = (1u << NumSrc) - 1;

// decoded names beat the memories, the memories beat the database, the database beats the plain channel raster
inline float srcPriority(int s) {
    static const float p[NumSrc] = {1.f, 2.f, 4.f, 4.f, 4.f, 3.f};
    return s >= 0 && s < NumSrc ? p[s] : 0.f;
}

struct Label {
    double mhz = 0;        // where the name sits on the frequency axis
    std::string text;      // the name
    std::string detail;    // shown when there is room (and in the tooltip): services of a mux, the radio text of a station ...
    std::string mode;      // the mode a click opens ("" = only tune); a Memory / Scanner mode key
    double bwMhz = 0;      // the nominal bandwidth around mhz (0 = not known): a name whose band is in view stays, a bar shows the band
    bool allocation = false;   // a broad frequency allocation: its extent keeps the name in view, but only the allocation strip shows its range
    int src = Band;
    float prio = 0;        // within a source: higher wins; the source's priority is added
};

// the labels, sorted by frequency
class Store {
public:
    void clear() { v_.clear(); maxBw_ = 0; }
    double maxBw() const { return maxBw_; }
    void add(Label l) { l.prio += srcPriority(l.src); maxBw_ = std::max(maxBw_, l.bwMhz); v_.push_back(std::move(l)); }
    void finish() { std::stable_sort(v_.begin(), v_.end(), [](const Label& a, const Label& b) { return a.mhz < b.mhz; }); }
    size_t size() const { return v_.size(); }
    const Label& at(size_t i) const { return v_[i]; }
    // indexes of the labels with loMhz <= mhz <= hiMhz whose source is on in mask
    std::vector<size_t> inRange(double loMhz, double hiMhz, unsigned mask) const {
        std::vector<size_t> o;
        auto it = std::lower_bound(v_.begin(), v_.end(), loMhz, [](const Label& l, double x) { return l.mhz < x; });
        for (; it != v_.end() && it->mhz <= hiMhz; ++it) if (mask & (1u << it->src)) o.push_back((size_t)(it - v_.begin()));
        return o;
    }
private:
    std::vector<Label> v_;
    double maxBw_ = 0;
};

// x0..x1: the box of the name; cx: the true x of the frequency (may be outside the plot); centreIn: cx is inside the plot (the pointer is drawn
// there); otherwise side says which edge the band overlaps (-1 left, +1 right) and only a stub arrow is drawn. bar0..bar1: the nominal band
// directly under this name, clipped to its box (bar1 <= bar0: no band). A faded end means the band continues beyond the name.
struct Placed { size_t idx = 0; float x0 = 0, x1 = 0, cx = 0; int row = 0; bool centreIn = true; int side = 0; float bar0 = 0, bar1 = 0; bool fadeLeft = false, fadeRight = false; };

struct View {
    double loMhz = 0, hiMhz = 0;   // the frequency range of the plot
    float px0 = 0, px1 = 0;        // the part of it the names may use, in pixels (the plot minus the corner buttons, say)
    float ax0 = 0, ax1 = 0;        // the pixels the span loMhz..hiMhz is drawn over: the whole plot rectangle (0,0: the same as px0..px1)
    int maxRows = 3;
    float gap = 4.f;               // the smallest space between two names in a row
    float padX = 4.f;              // space inside a name's box, each side
    unsigned mask = kAllSources;
};

// the pixel of a frequency: the same for every pane that shows the span over the same pixels
inline float xOf(const View& v, double mhz) {
    const float a0 = v.ax1 > v.ax0 ? v.ax0 : v.px0, a1 = v.ax1 > v.ax0 ? v.ax1 : v.px1;
    return (float)(a0 + (mhz - v.loMhz) / (v.hiMhz - v.loMhz) * (double)(a1 - a0));
}

// the labels to place: the frequency in view, or (nominal band known) the band overlapping the view
inline std::vector<size_t> inView(const Store& s, const View& v) {
    std::vector<size_t> o;
    const double m = s.maxBw() * 0.5;
    for (size_t i : s.inRange(v.loMhz - m, v.hiMhz + m, v.mask)) {
        const Label& l = s.at(i);
        const bool in = l.mhz >= v.loMhz && l.mhz <= v.hiMhz;
        const bool band = l.bwMhz > 0 && l.mhz - l.bwMhz * 0.5 <= v.hiMhz && l.mhz + l.bwMhz * 0.5 >= v.loMhz;
        if (in || band) o.push_back(i);
    }
    return o;
}

struct Result {
    std::vector<Placed> placed;    // sorted by x0
    size_t candidates = 0, dropped = 0;
    bool detailed = false;         // every name is shown with its detail (there was room for all)
};

// width of the box of a label (text and padding) in pixels; detailed: with its detail
using WidthFn = std::function<float(const Label&, bool detailed)>;

// Names for the same station and mode (within tolMhz) merge to the strongest source. Distinct database names and band-plan channels stay separate.
inline std::vector<size_t> mergeSame(const Store& s, std::vector<size_t> idx, double tolMhz) {
    std::vector<char> gone(idx.size(), 0);
    for (size_t i = 0; i < idx.size(); i++) {
        if (gone[i] || s.at(idx[i]).src == Band) continue;
        for (size_t j = i + 1; j < idx.size() && s.at(idx[j]).mhz - s.at(idx[i]).mhz <= tolMhz; j++) {
            if (gone[j] || s.at(idx[j]).src == Band) continue;
            const Label& a = s.at(idx[i]); const Label& b = s.at(idx[j]);
            if (a.mode != b.mode || (a.src == FreqDb && b.src == FreqDb && a.text != b.text)) continue;
            if (b.prio > a.prio) { gone[i] = 1; break; }
            gone[j] = 1;
        }
    }
    std::vector<size_t> o;
    for (size_t i = 0; i < idx.size(); i++) if (!gone[i]) o.push_back(idx[i]);
    return o;
}

namespace detail {
inline Result place(const Store& s, const std::vector<size_t>& order, const View& v, const WidthFn& width, bool detailed) {
    Result r;
    r.candidates = order.size();
    r.detailed = detailed;
    std::vector<std::vector<std::pair<float, float>>> rows((size_t)std::max(1, v.maxRows));
    for (size_t k : order) {
        const Label& l = s.at(k);
        const float w = width(l, detailed);
        const float cx = xOf(v, l.mhz);
        const bool in = l.mhz >= v.loMhz && l.mhz <= v.hiMhz;
        float x0 = cx - w * 0.5f;
        if (!in) x0 = std::max(v.px0, std::min(x0, v.px1 - w));   // only a name kept for its band is moved inside; one with its frequency in view is clipped at the edge, never dragged along
        const float x1 = x0 + w;
        int row = -1;
        for (size_t ri = 0; ri < rows.size() && row < 0; ri++) {
            bool free = true;
            for (const auto& q : rows[ri]) if (x0 < q.second + v.gap && x1 + v.gap > q.first) { free = false; break; }
            if (free) row = (int)ri;
        }
        if (row < 0) { r.dropped++; continue; }
        rows[(size_t)row].push_back({x0, x1});
        Placed p{k, x0, x1, cx, row};
        p.centreIn = in;
        p.side = in ? 0 : (l.mhz < v.loMhz ? -1 : 1);
        if (l.bwMhz > 0 && !l.allocation) {
            const float b0 = xOf(v, l.mhz - l.bwMhz * 0.5), b1 = xOf(v, l.mhz + l.bwMhz * 0.5);
            p.bar0 = std::max(x0, b0); p.bar1 = std::min(x1, b1);
            p.fadeLeft = b0 < x0; p.fadeRight = b1 > x1;
        }
        r.placed.push_back(p);
    }
    std::sort(r.placed.begin(), r.placed.end(), [](const Placed& a, const Placed& b) { return a.x0 < b.x0; });
    return r;
}
} // namespace detail

// Places the labels in view. The strongest are placed first; a name that finds no free spot in any row is left out (dropped counts them), so
// zooming in brings the rest. When everything fits in less than a third of the rows' length, the names are laid out again with their detail.
inline Result layout(const Store& s, const View& v, const WidthFn& width) {
    Result empty;
    if (!(v.hiMhz > v.loMhz) || !(v.px1 > v.px0) || v.maxRows < 1) return empty;
    const double perPx = (v.hiMhz - v.loMhz) / (double)(v.px1 - v.px0);
    std::vector<size_t> idx = mergeSame(s, inView(s, v), std::max(0.002, perPx));
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return s.at(a).prio > s.at(b).prio; });
    constexpr size_t kCap = 800;   // beyond what could ever fit: the strongest only (a raster of thousands of channels in a wide view)
    if (idx.size() > kCap) idx.resize(kCap);
    Result r = detail::place(s, idx, v, width, false);
    if (r.dropped == 0 && !r.placed.empty()) {
        float used = 0;
        for (const Placed& p : r.placed) used += p.x1 - p.x0;
        bool anyDetail = false;
        for (const Placed& p : r.placed) anyDetail = anyDetail || !s.at(p.idx).detail.empty();
        if (anyDetail && used < (v.px1 - v.px0) * (float)v.maxRows / 3.f) {
            Result d = detail::place(s, idx, v, width, true);
            if (d.dropped == 0) return d;
        }
    }
    return r;
}

// After the names have their final positions, let their bandwidth lines use the gaps in each row. Name placement never changes.
inline void layoutBandwidth(const Store& s, const View& v, Result& r) {
    const float axis0 = v.ax1 > v.ax0 ? v.ax0 : v.px0, axis1 = v.ax1 > v.ax0 ? v.ax1 : v.px1;
    if (!(axis1 > axis0)) return;
    struct Room { float lo = 0, hi = 0, band0 = 0, band1 = 0; bool band = false; };
    std::vector<Room> room(r.placed.size());
    std::vector<std::vector<size_t>> rows((size_t)std::max(1, v.maxRows));
    for (size_t i = 0; i < r.placed.size(); i++) {
        Placed& p = r.placed[i];
        const Label& l = s.at(p.idx);
        Room& a = room[i];
        a.lo = std::clamp(p.x0, axis0, axis1); a.hi = std::clamp(p.x1, axis0, axis1);
        a.band = l.bwMhz > 0 && !l.allocation;
        if (a.band) { a.band0 = xOf(v, l.mhz - l.bwMhz * 0.5); a.band1 = xOf(v, l.mhz + l.bwMhz * 0.5); }
        p.bar0 = p.bar1 = 0; p.fadeLeft = p.fadeRight = false;
        rows[(size_t)p.row].push_back(i);
    }
    for (auto& row : rows) {
        if (row.empty()) continue;
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return r.placed[a].x0 < r.placed[b].x0; });
        room[row.front()].lo = axis0; room[row.back()].hi = axis1;
        for (size_t k = 1; k < row.size(); k++) {
            Room& a = room[row[k - 1]]; Room& b = room[row[k]];
            if (a.band && b.band && a.band1 <= b.band0 + 0.01f) {   // text can overhang a shared border without the bandwidth lines overlapping
                a.hi = std::max(a.hi, a.band1); b.lo = std::min(b.lo, b.band0);
                continue;
            }
            const float gap = std::max(0.f, b.lo - a.hi);
            const float needA = a.band ? std::max(0.f, a.band1 - a.hi) : 0.f;
            const float needB = b.band ? std::max(0.f, b.lo - b.band0) : 0.f;
            // Preserve true endpoints when both bands fit; reserve separation only for overlapping requests.
            const float space = needA + needB <= gap + 0.01f ? gap : std::max(0.f, gap - std::max(0.f, v.gap));
            // Equal shares when both need them; a smaller request returns its unused share to the other side.
            const float takeA = std::min(needA, std::max(space * 0.5f, space - needB));
            const float takeB = std::min(needB, std::max(space * 0.5f, space - needA));
            a.hi += takeA; b.lo -= takeB;
        }
    }
    for (size_t i = 0; i < r.placed.size(); i++) {
        const Room& a = room[i];
        if (!a.band) continue;
        Placed& p = r.placed[i];
        p.bar0 = std::max(a.lo, a.band0); p.bar1 = std::min(a.hi, a.band1);
        // Only neighbouring names cause fades. Keep true endpoints outside the plot so clipping cannot create false end ticks.
        p.fadeLeft = a.lo > axis0 && a.band0 < p.bar0 - 0.01f; p.fadeRight = a.hi < axis1 && a.band1 > p.bar1 + 0.01f;
        if (!p.fadeLeft) p.bar0 = a.band0;
        if (!p.fadeRight) p.bar1 = a.band1;
    }
}

// the label under a point (the rows are rowH high and start at y0, row 0 on top); -1: none
inline int hit(const Result& r, float x, float y, float y0, float rowH, float rowGap) {
    if (y < y0) return -1;
    const int row = (int)((y - y0) / (rowH + rowGap));
    if (y - y0 - row * (rowH + rowGap) > rowH) return -1;
    for (size_t i = 0; i < r.placed.size(); i++) if (r.placed[i].row == row && x >= r.placed[i].x0 && x <= r.placed[i].x1) return (int)i;
    return -1;
}

// the trimmed name for a radio (RDS names come padded to 8 characters)
inline std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && s[b] == ' ') b++;
    return s.substr(b);
}

// a cheap fingerprint of the sources, to know when the store must be rebuilt
inline uint64_t mix(uint64_t h, uint64_t x) { h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h; }
inline uint64_t mix(uint64_t h, const std::string& s) { for (unsigned char c : s) h = (h ^ c) * 1099511628211ull; return h; }

} // namespace autolabel
