// The landing prediction on three real RS41 flights from SondeHub (tests/data/sonde/predict/sh_*.csv, SondeHub open data, CC BY-SA 2.0):
// sondes that kept sending on the ground, so the landing point and the ground height (in the sonde's own altitude reference) are known.
// Errors of the old (3.9) and the new predictor when the sonde was at each height above that ground and at the last frame before it
// stopped moving. Offline only (the forecast of those days is not stored): the ascent winds, the descent fit and the drift.
#include "dect2/sonde/sonde_predict.h"
#include "dect2/sonde/sonde_land.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

namespace {
std::string dataDir() {
#ifdef DECT2_SOURCE_DIR
    return std::string(DECT2_SOURCE_DIR) + "/tests/data/sonde/predict/";
#else
    return "tests/data/sonde/predict/";
#endif
}
struct Row { double t, lat, lon, alt; };
const double kD = 3.14159265358979323846 / 180, kR = 6371000.0;
double errM(double la, double lo, double tla, double tlo) { return std::hypot((lo - tlo) * kD * kR * std::cos(tla * kD), (la - tla) * kD * kR); }
}  // namespace

int main() {
    const char* files[] = {"sh_Y3432290.csv", "sh_Y2212795.csv", "sh_X2652450.csv"};
    const double hts[] = {10000, 5000, 2000, 1000, 500, 200};
    printf("real flights: error in metres, old (3.9, true ground) / new offline\n%-10s", "serial");
    for (double h : hts) printf("| %-13.0f", h);
    printf("| last frame (m above ground)\n");
    double sumOld = 0, sumNew = 0; int n = 0;
    for (const char* fn : files) {
        std::ifstream in(dataDir() + fn);
        CHECK((bool)in, "open %s", fn);
        if (!in) continue;
        std::string line; double lla = 0, llo = 0, lal = 0;
        std::vector<Row> rows;
        while (std::getline(in, line)) {
            if (line.rfind("# landed", 0) == 0) { std::sscanf(line.c_str(), "# landed %lf %lf %lf", &lla, &llo, &lal); continue; }
            if (line.empty() || line[0] == '#') continue;
            Row r; if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf", &r.t, &r.lat, &r.lon, &r.alt) == 4) rows.push_back(r);
        }
        CHECK(rows.size() > 1000 && lla != 0, "%s: %zu rows", fn, rows.size());
        SondeLandTracker tr;
        std::vector<SondeTrackPoint> thin; double step = 1, lastT = -1e9;
        size_t top = 0; for (size_t i = 0; i < rows.size(); i++) if (rows[i].alt > rows[top].alt) top = i;
        printf("%-10.8s", fn + 3);
        int next = 0;
        auto groundFn = [lal](double, double, double& h) { h = lal; return true; };
        SondePredictEnv env; env.ground = groundFn;
        for (size_t i = 0; i < rows.size(); i++) {
            const Row& r = rows[i];
            tr.addFix(1.7e9 + r.t, r.lat, r.lon, r.alt);
            if (r.t - lastT >= 0.8 * step) {
                SondeTrackPoint p; p.lat = (float)r.lat; p.lon = (float)r.lon; p.altM = (float)r.alt; p.unixT = (uint32_t)(1.7e9 + r.t);
                thin.push_back(p); lastT = r.t;
                if (thin.size() > 600) { std::vector<SondeTrackPoint> th; for (size_t k = 0; k < thin.size(); k += 2) th.push_back(thin[k]); thin.swap(th); step *= 2; }
            }
            if (i <= top + 30) continue;
            const double agl = r.alt - lal;
            const bool last = i + 1 == rows.size();
            int slot = -1;
            while (next < 6 && agl < hts[next]) { slot = next; next++; }
            if (slot < 0 && !last) continue;
            // the old predictor's velocity: from the frames of the last 4 s
            size_t a = i; while (a > 0 && r.t - rows[a - 1].t <= 4) a--;
            const double dt = std::max(1.0, r.t - rows[a].t);
            const double ve = (r.lon - rows[a].lon) * kD * kR * std::cos(r.lat * kD) / dt, vn = (r.lat - rows[a].lat) * kD * kR / dt, vz = (r.alt - rows[a].alt) / dt;
            double hd = std::atan2(ve, vn) / kD; if (hd < 0) hd += 360;
            const SondeLanding o = predictSondeLandingTrack(thin, r.lat, r.lon, r.alt, vz, std::hypot(ve, vn), hd, SondeLandOpts(), lal);
            SondePredictOpts po; po.members = 100;
            const SondePrediction p = tr.predict(po, env);
            const double eo = o.valid ? errM(o.lat, o.lon, lla, llo) : NAN, en = p.valid ? errM(p.lat, p.lon, lla, llo) : NAN;
            if (slot >= 0) printf("| %5.0f / %-5.0f", eo, en);
            if (last) {
                for (int k = next; k < 6; k++) printf("| %-13s", "-");
                printf("| %.0f / %.0f (%.0f m; r50 %.0f, r95 %.0f)\n", eo, en, agl, p.r50M, p.r95M);
                CHECK(p.valid, "%s: a prediction at the last frame", fn);
                if (o.valid && p.valid) { sumOld += eo; sumNew += en; n++; }
            }
        }
    }
    if (n) printf("mean at the last frame: old %.0f m, new %.0f m\n", sumOld / n, sumNew / n);
    CHECK(n == 3, "three flights");
    return testSummary("sonde_predict_real");
}
