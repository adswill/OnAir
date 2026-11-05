#include "spec_ghost.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace sghost {
namespace fs = std::filesystem;

Snap make(const float* db, size_t n, double loMhz, double hiMhz, int64_t when, const std::string& label) {
    Snap s; s.loMhz = loMhz; s.hiMhz = hiMhz; s.when = when;
    for (char c : label) if (c != '\n' && c != '\r') s.label += c;
    if (n == 0) return s;
    const size_t m = std::min(n, kMaxBins);
    s.db.resize(m);
    for (size_t j = 0; j < m; j++) {
        const size_t a = j * n / m, b = std::max(a + 1, (j + 1) * n / m);
        float mx = db[a];
        for (size_t i = a + 1; i < b && i < n; i++) mx = std::max(mx, db[i]);
        s.db[j] = mx;
    }
    return s;
}

void columns(const float* db, size_t n, double loMhz, double hiMhz, double vLo, double vHi, int npx, std::vector<float>& out) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.assign((size_t)std::max(0, npx), nan);
    if (n == 0 || npx <= 0 || hiMhz <= loMhz || vHi <= vLo) return;
    const double bin = (hiMhz - loMhz) / (double)n, colW = (vHi - vLo) / npx;
    for (int c = 0; c < npx; c++) {
        const double f0 = vLo + c * colW, f1 = f0 + colW;
        if (f1 < loMhz || f0 > hiMhz) continue;
        long a = (long)std::floor((f0 - loMhz) / bin), b = (long)std::floor((f1 - loMhz) / bin);
        a = std::max(0L, a); b = std::min((long)n - 1, b);
        if (b < a) b = a;
        float mx = db[a];
        for (long i = a + 1; i <= b; i++) mx = std::max(mx, db[i]);
        out[(size_t)c] = mx;
    }
}

std::vector<Span> newSignals(const std::vector<float>& live, const std::vector<float>& ghost, double vLo, double vHi, float thrDb, int minCols) {
    std::vector<Span> r;
    const size_t n = std::min(live.size(), ghost.size());
    if (n == 0) return r;
    const double colW = (vHi - vLo) / (double)n;
    size_t i = 0;
    while (i < n) {
        auto up = [&](size_t k) { return std::isfinite(live[k]) && std::isfinite(ghost[k]) && live[k] - ghost[k] > thrDb; };
        if (!up(i)) { i++; continue; }
        size_t j = i; float mx = 0;
        while (j < n && up(j)) { mx = std::max(mx, live[j] - ghost[j]); j++; }
        if ((int)(j - i) >= minCols) r.push_back({vLo + (double)i * colW, vLo + (double)j * colW, mx});
        i = j;
    }
    return r;
}

std::string fileFor(const std::string& dir, int64_t when) { return (fs::path(dir) / ("ghost-" + std::to_string((long long)when) + ".oag")).string(); }

bool save(const std::string& dir, const Snap& s) {
    std::error_code ec; fs::create_directories(dir, ec);
    std::ofstream f(fileFor(dir, s.when), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.precision(9);
    f << "OAGHOST1 " << s.loMhz << ' ' << s.hiMhz << ' ' << (long long)s.when << ' ' << s.db.size() << ' ' << s.label << '\n';
    std::vector<unsigned char> b(s.db.size() * 2);
    for (size_t i = 0; i < s.db.size(); i++) {
        const long v = std::lround(std::max(-327.f, std::min(327.f, s.db[i])) * 100.f);
        const uint16_t u = (uint16_t)(int16_t)v;
        b[2 * i] = (unsigned char)(u & 0xff); b[2 * i + 1] = (unsigned char)(u >> 8);
    }
    f.write((const char*)b.data(), (std::streamsize)b.size());
    return (bool)f;
}

static bool header(std::istream& f, Meta& m, size_t& n) {
    std::string line;
    if (!std::getline(f, line)) return false;
    std::istringstream h(line);
    std::string magic; long long when = 0;
    if (!(h >> magic >> m.loMhz >> m.hiMhz >> when >> n) || magic != "OAGHOST1") return false;
    m.when = when;
    std::getline(h, m.label);
    if (!m.label.empty() && m.label[0] == ' ') m.label.erase(0, 1);
    return n <= kMaxBins * 4 && m.hiMhz > m.loMhz;
}

bool load(const std::string& path, Snap& out) {
    std::ifstream f(path, std::ios::binary);
    Meta m; size_t n = 0;
    if (!f || !header(f, m, n)) return false;
    std::vector<unsigned char> b(n * 2);
    f.read((char*)b.data(), (std::streamsize)b.size());
    if ((size_t)f.gcount() != b.size()) return false;
    out = Snap(); out.loMhz = m.loMhz; out.hiMhz = m.hiMhz; out.when = m.when; out.label = m.label; out.db.resize(n);
    for (size_t i = 0; i < n; i++) out.db[i] = (float)(int16_t)(uint16_t)(b[2 * i] | (b[2 * i + 1] << 8)) / 100.f;
    return true;
}

std::vector<Meta> list(const std::string& dir) {
    std::vector<Meta> v;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".oag") continue;
        std::ifstream f(it->path(), std::ios::binary);
        Meta m; size_t n = 0;
        if (f && header(f, m, n)) { m.path = it->path().string(); v.push_back(m); }
    }
    std::sort(v.begin(), v.end(), [](const Meta& a, const Meta& b) { return a.when > b.when; });
    return v;
}

int prune(const std::string& dir, int keep) {
    if (keep <= 0) return 0;
    const auto all = list(dir);
    int gone = 0;
    for (size_t i = (size_t)keep; i < all.size(); i++) { std::error_code ec; if (fs::remove(all[i].path, ec)) gone++; }
    return gone;
}

double coverage(const Meta& m, double lo, double hi) {
    if (hi <= lo) return 0;
    return std::max(0.0, std::min(hi, m.hiMhz) - std::max(lo, m.loMhz)) / (hi - lo);
}

int pick(const std::vector<Meta>& all, double lo, double hi, int64_t target, double minCover) {
    int best = -1; int64_t bd = 0;
    for (size_t i = 0; i < all.size(); i++) {
        if (coverage(all[i], lo, hi) < minCover) continue;
        const int64_t d = all[i].when > target ? all[i].when - target : target - all[i].when;
        if (best < 0 || d < bd) { best = (int)i; bd = d; }
    }
    return best;
}

bool autoDue(int64_t lastWhen, int64_t now, int64_t everySec) { return everySec > 0 && now - lastWhen >= everySec; }

}  // namespace sghost
