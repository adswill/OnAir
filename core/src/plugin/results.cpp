// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/plugin/results.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::plugin {

namespace {
std::string clip(const std::string& s, size_t n) {
    std::string o = s.size() > n ? s.substr(0, n) : s;
    // do not leave half a UTF-8 sequence at the cut
    while (!o.empty() && s.size() > n && ((unsigned char)o.back() & 0xC0) == 0x80) o.pop_back();
    if (s.size() > n && !o.empty() && (unsigned char)o.back() >= 0xC0) o.pop_back();
    for (char& c : o) if ((unsigned char)c < 32 && c != '\n' && c != '\t') c = ' ';
    return o;
}
bool goodId(const std::string& id) { return !id.empty() && id.size() <= kMaxIdLen; }
template <class V> auto* findId(V& v, const std::string& id) { for (auto& e : v) if (e.id == id) return &e; return (decltype(&v[0]))nullptr; }
} // namespace

int Results::defineTable(const std::string& id, const std::string& title, const std::vector<Col>& cols) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!goodId(id) || cols.empty()) return kBadArg;
    if (cols.size() > kMaxCols) return refuse();
    Table* t = findId(tables_, id);
    if (!t) { if (tables_.size() >= kMaxTables) return refuse(); tables_.emplace_back(); t = &tables_.back(); t->id = id; }
    t->title = clip(title, kMaxTitleLen);
    if (t->cols.size() != cols.size()) { t->rows.clear(); }
    t->cols.clear();
    for (const Col& c : cols) t->cols.push_back({clip(c.name, 40), c.type >= 0 && c.type <= 5 ? c.type : 0});
    seq_++;
    return kOk;
}

int Results::addRow(const std::string& id, const std::vector<Cell>& cells) {
    std::lock_guard<std::mutex> lk(mu_);
    Table* t = findId(tables_, id);
    if (!t || cells.empty()) return kBadArg;
    if (cells.size() > t->cols.size()) return refuse();
    std::vector<Cell> row;
    row.reserve(t->cols.size());
    for (size_t i = 0; i < t->cols.size(); i++) {
        Cell c = i < cells.size() ? cells[i] : Cell{};
        c.type = t->cols[i].type;
        c.s = clip(c.s, kMaxCellText);
        if (!std::isfinite(c.d)) c.d = 0;
        row.push_back(std::move(c));
    }
    t->rows.push_back(std::move(row));
    t->rowsTotal++;
    while (t->rows.size() > kMaxRows) t->rows.pop_front();
    seq_++;
    return kOk;
}

int Results::clearTable(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    Table* t = findId(tables_, id);
    if (!t) return kBadArg;
    t->rows.clear();
    seq_++;
    return kOk;
}

int Results::setStatus(const std::string& key, const std::string& text, int32_t lamp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!goodId(key)) return kBadArg;
    lamp = std::clamp(lamp, 0, 3);
    for (auto& s : status_) if (s.key == key) { s.text = clip(text, kMaxCellText); s.lamp = lamp; seq_++; return kOk; }
    if (status_.size() >= kMaxStatus) return refuse();
    status_.push_back({key, clip(text, kMaxCellText), lamp});
    seq_++;
    return kOk;
}

int Results::addLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    log_.push_back(clip(line, kMaxLogLine));
    while (log_.size() > kMaxLogLines) log_.pop_front();
    seq_++;
    return kOk;
}

int Results::setPlot(const std::string& id, const std::string& title, int32_t kind, const float* data, size_t n, double x0, double dx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!goodId(id) || (!data && n)) return kBadArg;
    Plot* p = findId(plots_, id);
    if (!p) { if (plots_.size() >= kMaxPlots) return refuse(); plots_.emplace_back(); p = &plots_.back(); p->id = id; }
    const size_t m = std::min(n, kMaxPlotFloats);
    if (m < n) refused_++;
    p->title = clip(title, kMaxTitleLen);
    p->kind = std::clamp(kind, 0, 2);
    p->data.assign(data, data + m);
    for (float& f : p->data) if (!std::isfinite(f)) f = 0;
    p->x0 = std::isfinite(x0) ? x0 : 0;
    p->dx = std::isfinite(dx) && dx != 0 ? dx : 1;
    seq_++;
    return kOk;
}

int Results::setImage(const std::string& id, int32_t format, uint32_t w, uint32_t h, const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!goodId(id) || !data || !size || (format != 0 && format != 1)) return kBadArg;
    if (size > kMaxImageBytes) return refuse();
    if (format == 0) {
        if (!w || !h || w > kMaxImageSide || h > kMaxImageSide || (uint64_t)w * h * 4 != size) return kBadArg;
    } else if (size < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) return kBadArg;
    Image* im = findId(images_, id);
    if (!im) { if (images_.size() >= kMaxImages) return refuse(); images_.emplace_back(); im = &images_.back(); im->id = id; }
    im->format = format; im->w = w; im->h = h;
    im->bytes.assign(data, data + size);
    seq_++;
    return kOk;
}

int Results::setPoint(const std::string& id, const std::string& label, double lat, double lon) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!goodId(id) || !std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return kBadArg;
    MapPoint* p = findId(points_, id);
    if (!p) { if (points_.size() >= kMaxPoints) return refuse(); points_.emplace_back(); p = &points_.back(); p->id = id; }
    p->label = clip(label, 80); p->lat = lat; p->lon = lon;
    seq_++;
    return kOk;
}

void Results::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    tables_.clear(); status_.clear(); log_.clear(); plots_.clear(); images_.clear(); points_.clear();
    seq_++;
}

uint64_t Results::seq() const { std::lock_guard<std::mutex> lk(mu_); return seq_; }
uint64_t Results::refused() const { std::lock_guard<std::mutex> lk(mu_); return refused_; }

Snapshot Results::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    Snapshot s;
    s.tables = tables_; s.status = status_; s.log.assign(log_.begin(), log_.end()); s.plots = plots_; s.images = images_; s.points = points_;
    s.seq = seq_; s.refused = refused_;
    return s;
}

std::vector<std::string> Results::logTail(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    const size_t k = std::min(n, log_.size());
    return std::vector<std::string>(log_.end() - k, log_.end());
}

size_t Results::approxBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& t : tables_) for (auto& r : t.rows) { n += 24 * r.size(); for (auto& c : r) n += c.s.size(); }
    for (auto& p : plots_) n += p.data.size() * 4;
    for (auto& i : images_) n += i.bytes.size();
    for (auto& l : log_) n += l.size();
    return n;
}

// ---- wire ----
void wirePut(std::vector<uint8_t>& out, Wire type, const std::vector<uint8_t>& body) {
    const uint32_t len = (uint32_t)body.size() + 1;
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)(len >> (8 * i)));
    out.push_back((uint8_t)type);
    out.insert(out.end(), body.begin(), body.end());
}
void WireWriter::f64(double v) { uint64_t u; memcpy(&u, &v, 8); u64(u); }
void WireWriter::str(const std::string& s, size_t maxLen) { const size_t n = std::min(s.size(), maxLen); u32((uint32_t)n); b.insert(b.end(), s.begin(), s.begin() + n); }
uint32_t WireReader::u8() { if (pos + 1 > n) { ok = false; return 0; } return p[pos++]; }
uint32_t WireReader::u32() { if (pos + 4 > n) { ok = false; return 0; } uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[pos + i] << (8 * i); pos += 4; return v; }
uint64_t WireReader::u64() { if (pos + 8 > n) { ok = false; return 0; } uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[pos + i] << (8 * i); pos += 8; return v; }
double WireReader::f64() { const uint64_t u = u64(); double d; memcpy(&d, &u, 8); return d; }
std::string WireReader::str(size_t maxLen) {
    const uint32_t len = u32();
    if (!ok || len > maxLen || pos + len > n) { ok = false; return {}; }
    std::string s((const char*)p + pos, len);
    pos += len;
    return s;
}
bool WireReader::bytes(std::vector<uint8_t>& out, size_t maxLen) {
    const uint32_t len = u32();
    if (!ok || len > maxLen || pos + len > n) { ok = false; return false; }
    out.assign(p + pos, p + pos + len);
    pos += len;
    return true;
}

std::vector<uint8_t> wireTableDefine(const std::string& id, const std::string& title, const std::vector<Col>& cols) {
    WireWriter w;
    w.str(id, kMaxIdLen); w.str(title, kMaxTitleLen); w.u32((uint32_t)std::min(cols.size(), kMaxCols));
    for (size_t i = 0; i < cols.size() && i < kMaxCols; i++) { w.str(cols[i].name, 40); w.u32((uint32_t)cols[i].type); }
    return w.b;
}
std::vector<uint8_t> wireTableRow(const std::string& id, const std::vector<Cell>& cells) {
    WireWriter w;
    w.str(id, kMaxIdLen); w.u32((uint32_t)std::min(cells.size(), kMaxCols));
    for (size_t i = 0; i < cells.size() && i < kMaxCols; i++) { w.u64((uint64_t)cells[i].i); w.f64(cells[i].d); w.str(cells[i].s, kMaxCellText); }
    return w.b;
}
std::vector<uint8_t> wirePlot(const std::string& id, const std::string& title, int32_t kind, const float* data, size_t n, double x0, double dx) {
    WireWriter w;
    n = std::min(n, kMaxPlotFloats);
    w.str(id, kMaxIdLen); w.str(title, kMaxTitleLen); w.u32((uint32_t)kind); w.f64(x0); w.f64(dx); w.u32((uint32_t)n);
    const size_t o = w.b.size();
    w.b.resize(o + n * 4);
    if (n) memcpy(w.b.data() + o, data, n * 4);
    return w.b;
}
std::vector<uint8_t> wireImage(const std::string& id, int32_t format, uint32_t width, uint32_t height, const uint8_t* data, size_t size) {
    WireWriter w;
    w.str(id, kMaxIdLen); w.u32((uint32_t)format); w.u32(width); w.u32(height);
    w.bytes(data, std::min(size, (size_t)kMaxImageBytes));
    return w.b;
}

bool wireApply(Results& r, Wire type, const uint8_t* body, size_t size) {
    WireReader rd(body, size);
    switch (type) {
    case Wire::TableDefine: {
        const std::string id = rd.str(kMaxIdLen), title = rd.str(kMaxTitleLen);
        const uint32_t nc = rd.u32();
        if (!rd.ok || nc > kMaxCols) return false;
        std::vector<Col> cols;
        for (uint32_t i = 0; i < nc; i++) { Col c; c.name = rd.str(40); c.type = (int32_t)rd.u32(); cols.push_back(c); }
        if (!rd.ok) return false;
        r.defineTable(id, title, cols);
        return true;
    }
    case Wire::TableRow: {
        const std::string id = rd.str(kMaxIdLen);
        const uint32_t nc = rd.u32();
        if (!rd.ok || nc > kMaxCols) return false;
        std::vector<Cell> cells;
        for (uint32_t i = 0; i < nc; i++) { Cell c; c.i = (int64_t)rd.u64(); c.d = rd.f64(); c.s = rd.str(kMaxCellText); cells.push_back(c); }
        if (!rd.ok) return false;
        r.addRow(id, cells);
        return true;
    }
    case Wire::TableClear: { const std::string id = rd.str(kMaxIdLen); if (!rd.ok) return false; r.clearTable(id); return true; }
    case Wire::Status: { const std::string k = rd.str(kMaxIdLen), t = rd.str(kMaxCellText); const uint32_t l = rd.u32(); if (!rd.ok) return false; r.setStatus(k, t, (int32_t)l); return true; }
    case Wire::ResultLog: { const std::string l = rd.str(kMaxLogLine); if (!rd.ok) return false; r.addLog(l); return true; }
    case Wire::Plot: {
        const std::string id = rd.str(kMaxIdLen), title = rd.str(kMaxTitleLen);
        const uint32_t kind = rd.u32(); const double x0 = rd.f64(), dx = rd.f64(); const uint32_t n = rd.u32();
        if (!rd.ok || n > kMaxPlotFloats || rd.pos + (size_t)n * 4 > size) return false;
        std::vector<float> d(n);
        if (n) memcpy(d.data(), body + rd.pos, (size_t)n * 4);
        r.setPlot(id, title, (int32_t)kind, d.data(), n, x0, dx);
        return true;
    }
    case Wire::Image: {
        const std::string id = rd.str(kMaxIdLen);
        const uint32_t fmt = rd.u32(), w = rd.u32(), h = rd.u32();
        std::vector<uint8_t> b;
        if (!rd.bytes(b, kMaxImageBytes)) return false;
        r.setImage(id, (int32_t)fmt, w, h, b.data(), b.size());
        return true;
    }
    case Wire::MapPoint: { const std::string id = rd.str(kMaxIdLen), l = rd.str(80); const double la = rd.f64(), lo = rd.f64(); if (!rd.ok) return false; r.setPoint(id, l, la, lo); return true; }
    default: return false;
    }
}

} // namespace dect2::plugin
