// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ionosphere from my own receptions: bins, heard grid, MUF-style estimate, predicted vs observed, terminator, history cap and CSV.
#include "dect2/iono/iono_obs.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dect2::ionoobs;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static Obs mk(int64_t utc, double fMHz, double txLat, double txLon, float snr, const char* id, Source s = Source::Ft8) {
    Obs o;
    o.utc = utc; o.fHz = fMHz * 1e6; o.src = s; o.snrDb = snr;
    o.rxLat = 54.7; o.rxLon = 25.3; o.txLat = txLat; o.txLon = txLon; o.id = id;
    return o;
}

int main() {
    // geometry and bins
    CHECK(std::fabs(distanceKm(0, 0, 0, 90) - 10007.5) < 30);
    CHECK(std::fabs(bearingDeg(0, 0, 10, 0) - 0) < 1e-6);
    CHECK(std::fabs(bearingDeg(0, 0, 0, 10) - 90) < 1e-6);
    CHECK(std::fabs(bearingDeg(0, 0, 0, -10) - 270) < 1e-6);
    CHECK(distBin(0) == 0 && distBin(499.9) == 0 && distBin(500) == 1 && distBin(2999) == 3 && distBin(3000) == 4);
    CHECK(distBin(12000) == kDistBins - 1 && distBin(25000) == kDistBins - 1 && distBin(-1) == -1 && distBin(std::nan("")) == -1);
    CHECK(bearingBin(0) == 0 && bearingBin(359) == 0 && bearingBin(22.4) == 0 && bearingBin(22.6) == 1 && bearingBin(180) == 4 && bearingBin(-90) == 6);
    CHECK(bandOf(14.074e6) == 5 && bandOf(7.04e6) == 3 && bandOf(1.8366e6) == 0 && bandOf(28.124e6) == 9 && bandOf(13e6) == -1);

    // windows
    std::vector<Obs> v;
    const int64_t now = 1800000000;
    v.push_back(mk(now - 3600, 14.074, 40.7, -74.0, -10, "A"));
    v.push_back(mk(now - 3 * 86400, 14.074, 40.7, -74.0, -12, "B"));
    v.push_back(mk(now - 20 * 86400, 14.074, 40.7, -74.0, -12, "C"));
    CHECK(inWindow(v, now, Window::H24).size() == 1 && inWindow(v, now, Window::D7).size() == 2 && inWindow(v, now, Window::D30).size() == 3);

    // heard grid
    std::vector<Obs> g;
    const int64_t midnight = (now / 86400) * 86400;
    g.push_back(mk(midnight + 3 * 3600 + 5, 14.074, 40.7, -74.0, -15, "A"));
    g.push_back(mk(midnight + 3 * 3600 + 900, 14.074, 35.0, 139.0, -5, "B"));
    g.push_back(mk(midnight + 23 * 3600 + 59 * 60, 7.074, 52.5, 13.4, 3, "C"));
    g.push_back(mk(midnight + 100, 13.0, 52.5, 13.4, 3, "D"));   // not in a band
    HeardGrid hg = heardGrid(g);
    CHECK(hg.count[5][3] == 2 && hg.bestSnr[5][3] == -5 && hg.count[3][23] == 1 && hg.total() == 3 && hg.bestSnr[0][0] < -900);
    CHECK(utcHourOf(-1) == 23 && utcHourOf(0) == 0);

    // MUF-style estimate: the highest band with decodes in the distance bin
    std::vector<Obs> m;
    m.push_back(mk(now, 7.074, 52.5, 13.4, 0, "near40"));     // ~ 800 km
    m.push_back(mk(now, 14.074, 52.5, 13.4, 0, "near20"));
    m.push_back(mk(now, 21.074, 40.7, -74.0, 0, "ny15"));     // ~ 7000 km
    m.push_back(mk(now, 28.074, 35.0, 139.0, 0, "jp10"));     // ~ 8500 km
    m.push_back(mk(now, 14.074, 35.0, 139.0, 0, "jp20"));
    MufRow mr = mufEstimate(m);
    const int dNear = distBin(pathKm(m[0])), dNy = distBin(pathKm(m[2]));
    CHECK(mr.mufMHz[dNear] == bandCentreMHz(5));
    CHECK(mr.mufMHz[dNy] == bandCentreMHz(9) || mr.mufMHz[dNy] == bandCentreMHz(7));
    CHECK(mr.mufMHz[0] < 0);                                   // nothing that close
    CHECK(mufEstimate(m, 2).mufMHz[dNear] < 0);                // needs two decodes in that bin

    // predicted vs observed
    auto muf = [](double km, int) { return km < 2000 ? 12.0 : 25.0; };
    std::vector<Obs> c;
    for (int i = 0; i < 4; ++i) c.push_back(mk(now + i * 60, 14.074, 52.5, 13.4, -10, (std::string("x") + char('a' + i)).c_str()));   // 20 m, 800 km
    for (int i = 0; i < 4; ++i) c.push_back(mk(now + i * 60, 7.074, 35.0, 139.0, -10, (std::string("y") + char('a' + i)).c_str()));   // 40 m, Japan
    PredObs po = compare(c, muf);
    CHECK(po.v[5][dNear] == Verdict::ObservedOnly);           // 20 m at 800 km: MUF 12 predicts closed, but heard
    CHECK(po.v[3][dNear] == Verdict::PredictedOnly);          // 40 m at 800 km: predicted open, listened, not heard
    const int dJp = distBin(pathKm(c[4]));
    CHECK(po.v[3][dJp] == Verdict::BothOpen);                 // 40 m to Japan: 7 < 25, heard
    CHECK(po.v[9][dJp] == Verdict::NoData);                   // 10 m never listened to (no decodes at all): unknown, not closed
    CHECK(po.v[5][dJp] == Verdict::PredictedOnly);            // 20 m listened, Japan not heard
    CHECK(po.agree >= 1 && po.disagree >= 1);
    PredObs po2 = compare(c, muf, 13);                        // an hour with no data at all
    CHECK(po2.agree == 0 && po2.disagree == 0);
    CHECK(compare(c, nullptr).v[5][dNear] == Verdict::ObservedOnly);

    // sun and terminator: the March equinox 2026 (20 March 14:46 UTC)
    double sl, so;
    subSolarPoint(1773931560, sl, so);
    CHECK(std::fabs(sl) < 1.0);
    CHECK(std::fabs(so - (-(14.77 - 12) * 15)) < 3.0);        // ~ -41.5
    CHECK(!isNight(0, so, 1773931560) && isNight(0, so + 180, 1773931560));
    double tl[2];
    CHECK(terminatorLons(0, 1773931560, tl) == 2);
    CHECK(std::fabs(std::fabs(tl[1] - so) - 90) < 2.0);
    subSolarPoint(1782000000, sl, so);                         // June: north has polar day
    CHECK(sl > 20 && sl < 24 && terminatorLons(88, 1782000000, tl) == 0);
    CHECK(terminatorLine(1773931560).size() > 80);

    // colours
    CHECK(snrColor(-30) != snrColor(0) && snrColor(-30) == snrColor(-99) && (snrColor(5) >> 24) == 0xFF);

    // history: lines, dedupe, retention, cap, file round trip, CSV
    Obs o = mk(now, 14.074, 40.7, -74.0, -7.5f, "W1AW\t,x");
    Obs back;
    CHECK(fromLine(toLine(o), back) && back.utc == o.utc && back.fHz == o.fHz && back.src == o.src && std::fabs(back.snrDb + 7.5f) < 0.01);
    CHECK(back.id.find('\t') == std::string::npos);
    CHECK(!fromLine("junk", back) && !fromLine("1\t14000000\tNOPE\t0\t0\t0\t0\t0\tx", back) && !fromLine("1\t14000000\tFT8\t0\t95\t0\t0\t0\tx", back));
    std::vector<Obs> h;
    CHECK(addObs(h, o));
    Obs dup = o; dup.utc += 15;
    CHECK(!addObs(h, dup));
    dup.utc += 60;
    CHECK(addObs(h, dup));
    Obs other = o; other.id = "K1JT";
    CHECK(addObs(h, other) && h.size() == 3);

    std::vector<Obs> big;
    for (int i = 0; i < 1000; ++i) big.push_back(mk(now - 100 - i, 14.074, 40.7, -74.0, -10, "S"));   // newest first
    big.push_back(mk(now - 31 * 86400, 14.074, 40.7, -74.0, -10, "OLD"));
    trim(big, now, 1u << 20, 300);
    CHECK(big.size() == 300 && big.front().utc < big.back().utc && big.back().utc == now - 100);
    trim(big, now, 3000, 100000);
    size_t bytes = 0;
    for (const Obs& x : big) bytes += toLine(x).size() + 1;
    CHECK(bytes <= 3000 && big.size() > 20 && big.back().utc == now - 100);
    std::vector<Obs> old{mk(now - 40 * 86400, 7.074, 0, 0, 0, "o")};
    trim(old, now);
    CHECK(old.empty());

    const char* path = "iono_obs_test.tsv";
    CHECK(saveFile(path, h));
    std::vector<Obs> rd;
    CHECK(loadFile(path, rd) && rd.size() == h.size() && rd[2].id == h[2].id);
    std::remove(path);
    CHECK(!loadFile("no_such_dir/none.tsv", rd));
    const std::string csv = exportCsv(h);
    CHECK(csv.rfind("utc,freq_hz,band,source,snr_db", 0) == 0);
    size_t lines = 0;
    for (char ch : csv) lines += ch == '\n';
    CHECK(lines == h.size() + 1 && csv.find(",20m,FT8,") != std::string::npos);

    std::printf(fails ? "iono_obs: %d FAILED\n" : "iono_obs: all passed\n", fails);
    return fails ? 1 : 0;
}
