// Multi-channel TV on a 60 Msps capture (a LimeSDR at 61.44 Msps; USRP, BladeRF, PlutoSDR and others at 40 - 56 Msps): the slots of the span, the
// cost of the channeliser at that rate, the rate ladder the UI offers for a radio that runs that fast, MultiTv with several DVB-T2 channels from
// generated signals, and the same through the engine from a recorded 8-bit capture. Nothing here needs hardware; what a real LimeSDR's USB link
// sustains at 61.44 Msps is not tested.
#include "dect2/engine/channelizer.h"
#include "../common/cpu_report.h"
#include "../common/ci_slack.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/multitv.h"
#include "dect2/engine/rate_choice.h"
#include "multitv_testkit.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
using namespace dect2;
using namespace multitvtest;
#include "../common/test_util.h"

static bool has(const std::vector<RateEntry>& v, double hz) { for (auto& e : v) if (std::fabs(e.getHz - hz) < 1) return true; return false; }
static bool hasName(const std::vector<MultiService>& v, const char* name, int sign) {
    for (auto& s : v) if (s.svc.name == name && (sign == 0 || (s.offsetHz > 0) == (sign > 0))) return true;
    return false;
}

static void testSlotsAndRates() {
    // how many channels a wide capture holds (the raster on a channel, between two channels, the better of both)
    CHECK(Engine::slotsInSpan(61.44e6, 8, 1) >= 6, "61.44 Msps, 8 MHz: %d", Engine::slotsInSpan(61.44e6, 8, 1));
    CHECK(Engine::slotsInSpan(61.44e6, 7, 0) >= 7, "61.44 Msps, 7 MHz: %d", Engine::slotsInSpan(61.44e6, 7, 0));
    CHECK(Engine::slotsInSpan(61.44e6, 6, 0) >= 8, "61.44 Msps, 6 MHz: %d", Engine::slotsInSpan(61.44e6, 6, 0));
    CHECK(Engine::slotsInSpan(40e6, 8, 0) >= 4, "40 Msps, 8 MHz: %d", Engine::slotsInSpan(40e6, 8, 0));
    printf("  slots: 61.44 Msps holds %d x 8 MHz, %d x 7 MHz, %d x 6 MHz; 40 Msps %d x 8 MHz\n", Engine::slotsInSpan(61.44e6, 8, 0), Engine::slotsInSpan(61.44e6, 7, 0),
           Engine::slotsInSpan(61.44e6, 6, 0), Engine::slotsInSpan(40e6, 8, 0));
    // the sample-rate list of the toolbar for a LimeSDR (native, 0.1 - 61.44 Msps): the wide rates are in it, nothing above the radio's maximum
    DeviceInfo lime; lime.kind = DeviceInfo::Native; lime.board = "lime"; lime.minRateHz = 0.1e6; lime.maxRateHz = 61.44e6;
    auto e = rateEntries(rateLimitsOf(lime), 0);
    CHECK(has(e, 30.72e6) && has(e, 40e6) && has(e, 61.44e6) && has(e, 20e6), "LimeSDR rate list lacks the wide rates (%zu entries)", e.size());
    CHECK(checkManualRate(rateLimitsOf(lime), 0, 61.44e6).why.empty() && checkManualRate(rateLimitsOf(lime), 0, 62e6).ok && !checkManualRate(rateLimitsOf(lime), 0, 62e6).why.empty(), "manual rate: a warning above the radio's list, never a refusal");
    DeviceInfo mini = lime; mini.maxRateHz = 30.72e6;
    e = rateEntries(rateLimitsOf(mini), 0);
    CHECK(has(e, 30.72e6) && !has(e, 40e6) && !has(e, 61.44e6), "LimeSDR Mini list stops at 30.72");
    DeviceInfo hack; hack.kind = DeviceInfo::HackRF;
    e = rateEntries(rateLimitsOf(hack), 0);
    CHECK(!has(e, 25e6) && has(e, 20e6), "HackRF list unchanged");
    DeviceInfo unknown; unknown.kind = DeviceInfo::Soapy;
    CHECK(rateEntries(rateLimitsOf(unknown), 0).size() == 17, "an unknown radio keeps the 17-step ladder");
}

static void testFilterCost() {
    for (double off : {0.0, -24e6, 24e6}) {
        ChannelFilter g;
        g.configure(61.44e6, off, 10e6);
        std::vector<cf32> in(1 << 20, cf32(0.1f, 0.05f)), o;
        const auto t0 = std::chrono::steady_clock::now();
        size_t done = 0;
        while (done < 61440000) { o.clear(); g.process(in.data(), in.size(), o); done += in.size(); }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 61.44e6 / (double)done;
        printf("  channeliser at %+.0f MHz: %.2f s of one core per second of a 61.44 Msps capture\n", off / 1e6, s);
        cpuCal("multitv_wide", "channeliser at " + std::to_string((long long)std::lround(off / 1e6)) + " MHz", s, "cores", '<', 0.9 * ciSlack());
        CHECK(s < 0.9 * ciSlack(), "one channel needs %.2f cores: it must stay below one", s);
    }
    // the tone test at the wide rate: the wanted tone passes, a neighbour 8 MHz away does not
    ChannelFilter f;
    f.configure(61.44e6, 24e6, 10e6);
    const size_t n = 1 << 20;
    std::vector<cf32> x(n), y;
    for (size_t i = 0; i < n; i++) {
        const double a = 2 * M_PI * 24.5e6 * (double)i / 61.44e6, b = 2 * M_PI * 16e6 * (double)i / 61.44e6;
        x[i] = cf32((float)std::cos(a), (float)std::sin(a)) + cf32((float)std::cos(b), (float)std::sin(b));
    }
    f.process(x.data(), n, y);
    auto corr = [&](double hz) {
        std::complex<double> s = 0;
        for (size_t i = 2000; i < y.size(); i++) { const double a = -2 * M_PI * hz * (double)i / 10e6; s += std::complex<double>(y[i]) * std::complex<double>(std::cos(a), std::sin(a)); }
        return std::abs(s) / (double)(y.size() - 2000);
    };
    const double want = corr(0.5e6), alias = corr(-8e6 + 10e6);
    CHECK(want > 0.97 && want < 1.03 && alias < 0.006, "tone %.3f, neighbour %.5f", want, alias);
}

struct Spec { std::vector<ChanSpec> ch; double rate; };

static void testMultiTv() {
    const double rate = 61.44e6;
    // DVB-T2 channels at -24, -8, 0 and +24 MHz (the raster on a channel): four service lists, the gaps get no receiver
    const std::vector<ChanSpec> ch = {{false, -24e6, 8, 11, "Wide One", 0, 0, 0x1001, 1}, {false, -8e6, 8, 22, "Wide Two", -2, 2, 0x1002, 2},
                                      {false, 0, 8, 33, "Wide Three", -1, 5, 0x1003, 3}, {false, 24e6, 8, 44, "Wide Four", -3, 7, 0x1004, 4}};
    Capture cap(ch, rate, 28);
    MultiTv mt;
    MultiTvOptions o; o.spanRate = rate; o.widthMhz = 8; o.maxChannels = 4; o.autoGrid = false; o.gridOffsetHz = 0; o.blocking = true;
    CHECK(mt.configure(o), "configure");
    printf("four DVB-T2 channels in a 61.44 Msps capture\n");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<cf32> blk;
    const size_t total = (size_t)(rate * 4.0), step = 1 << 18;
    RxTelemetry t;
    for (size_t done = 0; done < total; done += step) { cap.pull(step, blk); mt.feed(blk.data(), blk.size()); mt.poll(t); }
    mt.wait();
    std::vector<MultiService> svc;
    for (int i = 0; i < 600; i++) {
        mt.poll(t);
        svc = mt.services();
        if (hasName(svc, "Wide One", -1) && hasName(svc, "Wide Two", -1) && hasName(svc, "Wide Three", 0) && hasName(svc, "Wide Four", +1)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto rows = mt.channels();
    int running = 0, locked = 0, occupied = 0;
    for (const auto& r : rows) {
        running += r.running; locked += r.locked; occupied += r.occupied;
        printf("  slot %+d (%+5.1f MHz): %s%s +%.1f dB, state %d, %d services, receiver thread %.0f %%, dropped %llu\n", r.slot, r.offsetHz / 1e6, r.occupied ? "occupied" : "empty", r.running ? ", running" : "",
               r.aboveDb, r.state, r.services, r.cpuPct, (unsigned long long)r.dropped);
        CHECK(r.dropped == 0, "slot %d dropped samples in blocking mode", r.slot);
    }
    printf("  wall %.1f s for 4 s of signal\n", wall);
    CHECK(rows.size() >= 6, "the 61.44 Msps span lists %zu slots", rows.size());
    CHECK(occupied == 4 && running == 4, "%d occupied, %d running (the empty slots must get no receiver)", occupied, running);
    CHECK(locked == 4, "%d channels locked", locked);
    CHECK(hasName(svc, "Wide One", -1) && hasName(svc, "Wide Two", -1) && hasName(svc, "Wide Three", 0) && hasName(svc, "Wide Four", +1), "four service lists (%zu services)", svc.size());
    mt.reset();
}

// writes secs of the capture as 8-bit samples
static std::string writeCapture(const char* name, const std::vector<ChanSpec>& ch, double rate, double secs) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    Capture cap(ch, rate, 30);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    const size_t blk = 1 << 18;
    std::vector<cf32> v;
    std::vector<int8_t> q(2 * blk);
    for (size_t done = 0; done < (size_t)(rate * secs); done += blk) {
        cap.pull(blk, v);
        for (size_t i = 0; i < blk; i++) {
            q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[i].real() * 128 * 3.f)));
            q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[i].imag() * 128 * 3.f)));
        }
        fwrite(q.data(), 1, q.size(), f);
    }
    fclose(f);
    return path;
}

static void testEngine() {
    const double rate = 61.44e6;
    const std::string path = writeCapture("onair_mtv_wide_61Msps.cs8", {{false, -16e6, 8, 11, "Lime A", 0, 0, 0x1001, 1}, {false, 8e6, 8, 22, "Lime B", -2, 2, 0x1002, 2}, {false, 24e6, 8, 33, "Lime C", -1, 5, 0x1003, 3}}, rate, 3.0);
    CHECK(!path.empty(), "capture written");
    if (path.empty()) return;
    printf("engine: three DVB-T2 channels from a 61.44 Msps recording (grid found out)\n");
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = rate; fo.loop = true;
    TuneSettings t; t.centerHz = 522e6; t.sampleRate = rate; t.bandwidthMhz = 8;
    e.setStandard(1);
    e.setMultiChannel(true, 4, 0);
    CHECK(e.start(dev, t, fo), "engine start");
    const auto t0 = std::chrono::steady_clock::now();
    bool all = false;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 120) {
        auto v = e.multiServices();
        if (e.multiChannelActive() && hasName(v, "Lime A", -1) && hasName(v, "Lime B", +1) && hasName(v, "Lime C", +1)) { all = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));   // the load figures need about a second of signal
    printf("  active %d, wall %.1f s, note '%s'\n", (int)e.multiChannelActive(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), e.multiChannelNote().c_str());
    for (const auto& r : e.tvChannels())
        printf("  slot %+d (%+5.1f MHz): %s%s state %d, %d services, receiver thread %.0f %%, dropped %llu\n", r.slot, r.offsetHz / 1e6, r.occupied ? "occupied" : "empty", r.running ? ", running" : "",
               r.state, r.services, r.cpuPct, (unsigned long long)r.dropped);
    CHECK(all, "service lists of the three channels");
    int running = 0;
    for (const auto& r : e.tvChannels()) running += r.running;
    CHECK(running == 3, "%d receivers", running);
    if (getenv("MTV_LOG")) { size_t tot = 0; for (auto& l : e.logSnapshot(tot)) printf("    log: %s\n", l.c_str()); }
    e.stop();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

int main() {
    testSlotsAndRates();
    testFilterCost();
    testMultiTv();
    testEngine();
    printf(fails ? "multitv wide: %d FAILED\n" : "multitv wide: all passed\n", fails);
    return fails != 0;
}
