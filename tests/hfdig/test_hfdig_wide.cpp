// HF digital receiver, whole span (hfdig_wide.h) on a 2.4 Msps capture:
//  1. FT8 on 14.074 and FT4 on 14.080 MHz at once (the radio is tuned to 14.077 MHz, between them): every decode of every slot, with the
//     slot's dial frequency; the WSPR slot (14.0956) and the slots outside the span are not made; with the switch off nothing is heard
//  2. RTTY: six pairs of tones over +-32 kHz (170 / 425 / 850 Hz shift, 45.45 / 50 / 75 baud, two of them 1.1 kHz apart): each decoded,
//     with the baud rate and shift found; the CPU time of the receiver
// The signals are made here at the sample rate (RTTY) or from audio at 48 kHz made by the FT8 / FT4 generator, turned into the analytic
// signal and interpolated up (FT8 / FT4).
#include "dect2/dsp/fftutil.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/hfdig/hfdig_wide.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

constexpr double kFs = 2400000, kOff = -20000, kT0 = 1767225600.0;
using dect2::math::kPi;

struct Rng {
    uint64_t s = 88172645463325252ull;
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; }
    double g() { return (u() + u() + u() + u() - 2.0) * 1.7320508; }   // about N(0,1)
};

// ---- FT8 / FT4: audio at 48 kHz -> analytic -> interpolated to kFs, placed at dial offset delta
struct Layer { std::vector<cf32> a; double delta = 0; std::complex<double> rot{1, 0}, step{1, 0}; };

static Layer ftxLayer(int mode, const std::vector<std::pair<std::string, double>>& st, double delta, double dBsnr, double secs) {
    const int R = 48000;
    const size_t N = (size_t)1 << 21;
    std::vector<float> au;
    au.assign((size_t)(secs * R), 0.f);
    const double P = ftxPeriod(mode), t0 = 0.5;
    const double sigma = 0.05, amp = std::sqrt(2 * std::pow(10.0, dBsnr / 10) * sigma * sigma * 2500.0 / 4000.0);
    for (int k = 0; (k + 1) * P <= secs; k++)
        for (const auto& s : st)
            if (!ftxAddTransmission(mode, s.first, s.second, amp, R, k * P + t0, false, au)) printf("cannot send %s\n", s.first.c_str());
    au.resize(std::max(au.size(), N));
    std::vector<cf32> sp(N);
    for (size_t i = 0; i < N; i++) sp[i] = cf32(i < au.size() ? au[i] : 0.f, 0.f);
    Fft f((int)N);
    f.forward(sp.data());
    for (size_t i = 1; i < N / 2; i++) sp[i] *= 2.f;
    for (size_t i = N / 2 + 1; i < N; i++) sp[i] = cf32(0, 0);
    f.inverse(sp.data());
    for (auto& v : sp) v /= (float)N;
    Layer L; L.a = std::move(sp); L.delta = delta;
    return L;
}

// ---- RTTY directly at kFs
const char kLet[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K', 'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
struct RttyLayer {
    double lowHz, shift, baud; std::string text; double amp = 0.2;
    std::vector<std::pair<bool, double>> segs;   // mark?, bits
    size_t idx = 0; double rem = 0; int cnt = 0;
    std::complex<double> rm{1, 0}, rs{1, 0}, sm, ss;
    void init() {
        segs.push_back({true, 30.0});
        auto put = [&](int code) { segs.push_back({false, 1.0}); for (int b = 0; b < 5; b++) segs.push_back({((code >> b) & 1) != 0, 1.0}); segs.push_back({true, 1.5}); };
        put(31);
        for (char ch : text) { int c = -1; for (int k = 0; k < 32; k++) if (kLet[k] == ch && ch) c = k; if (c >= 0) put(c); }
        segs.push_back({true, 40.0});
        rem = segs[0].second;
        sm = std::polar(1.0, 2 * kPi * (kOff + lowHz + shift) / kFs);   // mark = the lower tone (normal): lowHz; space the upper
        ss = std::polar(1.0, 2 * kPi * (kOff + lowHz + shift) / kFs);
        sm = std::polar(1.0, 2 * kPi * (kOff + lowHz) / kFs);
    }
    std::complex<double> next() {
        rem -= baud / kFs;
        while (rem <= 0) { idx = (idx + 1) % segs.size(); rem += segs[idx].second; }
        rm *= sm; rs *= ss;
        if (++cnt >= 4096) { cnt = 0; rm /= std::abs(rm); rs /= std::abs(rs); }
        return amp * (segs[idx].first ? rm : rs);
    }
};

int main() {
    // ---------------------------------------------------------------- 1. FT8 + FT4
    {
        const double secs = 34;
        Layer ft8 = ftxLayer(0, {{"CQ LY2ABC KO24", 600}, {"LY2ABC OH3XYZ -10", 1400}, {"CQ DL5QRP JO62", 2300}}, 14074000.0 - 14077000.0, 0, secs);
        Layer ft4 = ftxLayer(1, {{"CQ SM4DDD JO89", 900}, {"SM4DDD LY2ABC R-05", 1800}}, 14080000.0 - 14077000.0, 0, secs);
        for (int wide = 0; wide < 2; wide++) {
            for (Layer* L : {&ft8, &ft4}) { L->rot = {1, 0}; L->step = std::polar(1.0, 2 * kPi * (kOff + L->delta) / kFs); }
            HfdigReceiver r;
            r.setSilent(true);
            r.setDecoders(8);
            r.ftx().setStartTime(kT0);
            r.setDialHz(14077000.0);
            r.setWide(wide == 1);
            r.configure(kFs);
            r.setSignalOffset(kOff);
            Rng rng;
            std::vector<cf32> x(240000);
            HfdigTelemetry t; uint64_t seq = 0;
            double cpu = 0;
            size_t n = 0;
            const size_t total = (size_t)(secs * kFs);
            while (n < total) {
                for (size_t i = 0; i < x.size(); i++, n++) {
                    std::complex<double> v(0.7 * rng.g(), 0.7 * rng.g());
                    for (Layer* L : {&ft8, &ft4}) {
                        const double pos = (double)n / 50.0;
                        const size_t i0 = (size_t)pos;
                        const float fr = (float)(pos - (double)i0);
                        const cf32 a = i0 + 1 < L->a.size() ? L->a[i0] * (1 - fr) + L->a[i0 + 1] * fr : cf32(0, 0);
                        v += std::complex<double>(a.real(), a.imag()) * L->rot;
                        L->rot *= L->step;
                        if ((n & 1023) == 0) L->rot /= std::abs(L->rot);
                    }
                    x[i] = cf32((float)v.real(), (float)v.imag());
                }
                const double t0 = processCpuSecs();
                r.feed(x.data(), x.size());
                cpu += (processCpuSecs() - t0);
                HfdigTelemetry tt;
                if (r.telemetry(tt, seq)) { seq = tt.seq; t = std::move(tt); }
            }
            for (int w = 0; w < 80; w++) {   // the decoder threads of the slots
                HfdigFtxTelemetry ft;
                r.ftx().telemetry(ft);
                int a8 = 0, a4 = 0;
                for (const auto& d : ft.decodes) { if (d.mode == 0) a8++; else a4++; }
                if (wide == 0 ? w > 12 : (a8 >= 6 && a4 >= 8 && ft.pending == 0)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            HfdigFtxTelemetry ft;
            r.ftx().telemetry(ft);
            int n8 = 0, n4 = 0;
            for (const auto& d : ft.decodes) {
                if (d.mode == 0 && std::fabs(d.dialHz - 14074000.0) < 1 ) n8++;
                if (d.mode == 1 && std::fabs(d.dialHz - 14080000.0) < 1) n4++;
            }
            printf("wide=%d: decodes %zu, FT8 on 14.074: %d, FT4 on 14.080: %d, cpu %.2f s for %.0f s (%.0f %% of one core)\n", wide, ft.decodes.size(), n8, n4, cpu, secs, cpu / secs * 100);
            for (const auto& d : ft.decodes) printf("   %s %.3f MHz %+.0f dB %.0f Hz  %s\n", ftxModeName(d.mode), d.dialHz / 1e6, d.snrDb, d.hz, d.msg.c_str());
            if (wide == 1) {
                CHECK(n8 >= 4, "FT8 decodes of the 14.074 slot: %d (3 stations, 2 slots)", n8);
                CHECK(n4 >= 3, "FT4 decodes of the 14.080 slot: %d (2 stations, 4 slots)", n4);
            } else {
                CHECK(n8 == 0 && n4 == 0, "off: decodes with a slot dial frequency (%d, %d)", n8, n4);
            }
        }
    }
    // ---------------------------------------------------------------- 2. RTTY
    {
        const std::string txt = "RYRY CQ CQ DE LY2ABC LY2ABC THE QUICK BROWN FOX 123 K\n";
        struct P { double lo, shift, baud; } ps[] = {{1200, 170, 45.45}, {18400, 170, 45.45}, {-31200, 425, 50.0}, {30010, 850, 75.0}, {-26300, 170, 50.0}, {-25200, 425, 45.45}};
        for (int wide = 0; wide < 2; wide++) {
            std::vector<RttyLayer> L;
            for (const P& p : ps) { RttyLayer l; l.lowHz = p.lo; l.shift = p.shift; l.baud = p.baud; l.text = txt; l.init(); L.push_back(std::move(l)); }
            HfdigReceiver r;
            r.setSilent(true);
            r.setDecoders(1);
            r.setDialHz(7040000.0);
            r.setWide(wide == 1, 150000);
            r.configure(kFs);
            r.setSignalOffset(kOff);
            Rng rng;
            std::vector<cf32> x(240000);
            HfdigTelemetry t; uint64_t seq = 0;
            double cpu = 0;
            for (int blk = 0; blk < 300; blk++) {   // 30 s
                for (auto& v : x) {
                    std::complex<double> a(1.0 * rng.g(), 1.0 * rng.g());
                    for (auto& l : L) a += l.next();
                    v = cf32((float)a.real(), (float)a.imag());
                }
                const double t0 = processCpuSecs();
                r.feed(x.data(), x.size());
                cpu += (processCpuSecs() - t0);
                HfdigTelemetry tt;
                if (r.telemetry(tt, seq)) { seq = tt.seq; t = std::move(tt); }
            }
            int ok = 0;
            for (const P& p : ps) {
                const RttyWideSignal* b = nullptr;
                for (const auto& s : t.rttyWide) if (std::fabs(s.lowHz - p.lo) < 25 && s.shiftHz == p.shift && (!b || s.chars > b->chars)) b = &s;
                const bool good = b && b->text.find("QUICK BROWN FOX") != std::string::npos && std::fabs(b->baud - p.baud) < 0.1;
                if (good) ok++;
                if (wide == 1) CHECK(good, "RTTY %.0f Hz %g/%g: %s", p.lo, p.shift, p.baud, b ? b->text.c_str() : "(none)");
            }
            printf("wide=%d: RTTY signals listed %zu, decoded with the right speed %d/6, cpu %.2f s for 30 s (%.0f %% of one core)\n", wide, t.rttyWide.size(), ok, cpu, cpu / 30 * 100);
            for (const auto& s : t.rttyWide) printf("   %+.0f Hz shift %g baud %g %s chars %llu fe %llu\n", s.lowHz, s.shiftHz, s.baud, s.active ? "on" : "gone", (unsigned long long)s.chars, (unsigned long long)s.framingErrors);
            if (wide == 0) CHECK(t.rttyWide.empty(), "off: a wide list");
        }
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
