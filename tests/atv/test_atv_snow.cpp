// Analogue TV: the untuned picture (snow) for hunting weak signals (#239), and a channel tuned at its vision carrier (carrier at the centre of the input).
#include "dect2/atv/atv_testkit.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atvkit;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static void greyStats(const AtvFrame& f, double& mean, double& sd) {
    double s = 0, s2 = 0;
    const size_t n = (size_t)f.width * f.height;
    for (size_t i = 0; i < n; i++) { const double v = f.rgba[4 * i]; s += v; s2 += v * v; }
    mean = s / n; sd = std::sqrt(std::max(0.0, s2 / n - mean * mean));
}

int main() {
    // ---- noise only: nothing without the option, snow with it
    for (int on = 0; on < 2; on++) {
        AtvReceiver rx;
        rx.setSilent(true);
        rx.configure(10e6);
        rx.setShowSnow(on != 0);
        std::mt19937_64 g(5);
        std::normal_distribution<float> d(0.f, 0.05f);
        std::vector<cf32> buf(65536);
        uint64_t seq = 0, last = 0;
        int frames = 0;
        bool mono = true, untuned = true, size = true;
        std::vector<std::shared_ptr<const AtvFrame>> keep;
        for (int c = 0; c < 80; c++) {   // 0.5 s
            for (auto& v : buf) v = cf32(d(g), d(g));
            rx.feed(buf.data(), buf.size());
            while (auto f = rx.frame(seq)) {
                frames++;
                mono = mono && f->seq > last; last = f->seq;
                untuned = untuned && f->untuned;
                size = size && f->width == 768 && f->height == 576 && f->rgba.size() == 768u * 576u * 4u;
                keep.push_back(f);
            }
        }
        jprintf("noise only, snow %s: %d pictures\n", on ? "on" : "off", frames);
        if (!on) CHECK(frames == 0, "pictures without the option: %d", frames);
        else {
            CHECK(frames >= 8, "snow pictures %d in half a second", frames);
            CHECK(mono && untuned && size, "snow picture numbers/flag/size (%d %d %d)", (int)mono, (int)untuned, (int)size);
            double m, s;
            greyStats(*keep.back(), m, s);
            jprintf("  snow grey mean %.1f spread %.1f\n", m, s);
            CHECK(m > 100 && m < 160 && s > 20, "snow looks wrong: mean %.1f spread %.1f", m, s);
        }
    }

    // ---- snow, then a real signal: the pictures of the signal take over, numbers keep growing
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = 10e6; c.sound = 4;
        AtvGenerator gen(c);
        AtvReceiver rx;
        rx.setSilent(true);
        rx.configure(c.rate);
        rx.setShowSnow(true);
        std::mt19937_64 g(9);
        std::normal_distribution<float> d(0.f, 0.05f);
        std::vector<cf32> buf(65536);
        uint64_t seq = 0, last = 0;
        int snowFrames = 0, picFrames = 0, snowAfter = 0;
        bool mono = true;
        double lockedAt = -1;
        for (int k = 0; k < 400; k++) {
            const bool sig = k >= 100;                 // 0.65 s of noise, then the signal
            if (sig) gen.generate(buf.data(), buf.size());
            else for (auto& v : buf) v = cf32(d(g), d(g));
            rx.feed(buf.data(), buf.size());
            while (auto f = rx.frame(seq)) {
                mono = mono && f->seq > last; last = f->seq;
                if (f->untuned) { snowFrames++; if (lockedAt >= 0 && (double)k * 65536 / c.rate > lockedAt + 0.4) snowAfter++; }
                else { picFrames++; }
            }
            AtvTelemetry t;
            if (lockedAt < 0 && rx.telemetry(t, 0) && t.state == 2) lockedAt = (double)k * 65536 / c.rate;
        }
        jprintf("snow then signal: snow pictures %d, signal pictures %d, snow after the lock %d, locked at %.2f s\n", snowFrames, picFrames, snowAfter, lockedAt);
        CHECK(snowFrames >= 10 && picFrames >= 20 && mono, "snow %d signal %d mono %d", snowFrames, picFrames, (int)mono);
        CHECK(lockedAt > 0 && snowAfter == 0, "snow kept coming after the lock (%d)", snowAfter);
    }

    // ---- the vision carrier at the centre (the user tuned the vision frequency, not the channel centre)
    const struct { const char* name; double rate; int sys; int col; bool mirror; } vc[] = {
        {"B/G 16 Msps", 16e6, kAtvG, kAtvPal, false},
        {"B/G 20 Msps", 20e6, kAtvG, kAtvPal, false},
        {"B/G 20 Msps mirrored", 20e6, kAtvG, kAtvPal, true},
        {"I 20 Msps", 20e6, kAtvI, kAtvPal, false},
        {"M 16 Msps", 16e6, kAtvM, kAtvNtsc, false},
        {"B/G 10 Msps", 10e6, kAtvG, kAtvPal, false},
    };
    for (const auto& v : vc) {
        AtvGenConfig c; c.sys = v.sys; c.colour = v.col; c.cnrDb = 40; c.rate = v.rate; c.sound = 4;
        AtvFormat f; atvMakeFormat(v.sys, v.col, f);
        c.cfoHz = -atvVisionOffsetHz(f);                  // the vision carrier at 0 Hz
        Options o;
        if (v.mirror) o.impair = [](cf32* x, size_t n, uint64_t) { for (size_t i = 0; i < n; i++) x[i] = std::conj(x[i]); };
        const Run r = run(c, 5.0, o);
        jprintf("vision carrier at the centre, %-22s state %d %-18s carrier %+.0f Hz fields ok %llu\n", v.name, r.tel.state, r.tel.system.empty() ? "-" : r.tel.system.c_str(), r.tel.visionHz,
                (unsigned long long)r.tel.blocksOk);
        CHECK(r.tel.state == 2 && std::fabs(r.tel.visionHz) < 2e3, "%s: state %d carrier %.0f Hz", v.name, r.tel.state, r.tel.visionHz);
        if (r.tel.state != 2) for (const auto& l : r.log) jprintf("    log: %s\n", l.c_str());
    }
    return fails ? 1 : 0;
}
