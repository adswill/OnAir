// DRM best-hour book (see drm_besthour.h).
#include "dect2/smart5/drm_besthour.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace smart5 {

static int utcHour(double utc) { return (int)(std::fmod(std::floor(utc / 3600.0), 24.0) + 24) % 24; }

void DrmBestHour::add(const std::string& st, int khz, double utc, double dt, bool ok) {
    if (dt <= 0) return;
    auto& h = book_[{st, khz}][utcHour(utc)];
    h.listenS += dt;
    if (ok) h.goodS += dt;
}

const std::array<DrmHourStat, 24>* DrmBestHour::hours(const std::string& st, int khz) const {
    auto it = book_.find({st, khz});
    return it == book_.end() ? nullptr : &it->second;
}

std::vector<int> DrmBestHour::bestHours(const std::string& st, int khz, double minListenS) const {
    std::vector<int> v;
    auto h = hours(st, khz);
    if (!h) return v;
    for (int i = 0; i < 24; i++) if ((*h)[i].listenS >= minListenS) v.push_back(i);
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return (*h)[a].rate() > (*h)[b].rate(); });
    return v;
}

double DrmBestHour::nextGood(const std::string& st, int khz, double utc, double minRate, double minListenS) const {
    auto h = hours(st, khz);
    if (!h) return -1;
    const double hourStart = std::floor(utc / 3600.0) * 3600.0;
    for (int k = 1; k <= 24; k++) {
        const double t = hourStart + k * 3600.0;
        const auto& s = (*h)[utcHour(t)];
        if (s.listenS >= minListenS && s.rate() >= minRate) return t;
    }
    return -1;
}

std::string DrmBestHour::save() const {
    std::ostringstream o;
    for (auto& kv : book_) {
        o << kv.first.second;
        for (auto& s : kv.second) o << '\t' << (long long)std::llround(s.listenS) << ':' << (long long)std::llround(s.goodS);
        o << '\t' << kv.first.first << '\n';
    }
    return o.str();
}

void DrmBestHour::load(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string f;
        if (!std::getline(l, f, '\t')) continue;
        const int khz = atoi(f.c_str());
        std::array<DrmHourStat, 24> a{};
        bool ok = true;
        for (int i = 0; i < 24 && ok; i++) {
            if (!std::getline(l, f, '\t')) { ok = false; break; }
            long long x = 0, y = 0;
            if (sscanf(f.c_str(), "%lld:%lld", &x, &y) != 2) ok = false;
            a[i].listenS = (double)x; a[i].goodS = (double)y;
        }
        std::string name;
        if (!ok || !std::getline(l, name)) continue;
        auto& dst = book_[{name, khz}];
        for (int i = 0; i < 24; i++) { dst[i].listenS += a[i].listenS; dst[i].goodS += a[i].goodS; }
    }
}

} // namespace smart5
} // namespace dect2
