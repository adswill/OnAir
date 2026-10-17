// Opt-in hardware acceptance tool. Uses OnAir's actual source, not a separate SDRplay implementation.
// sdrplay_probe [--port 0|1|2] [--rate Hz] [--freq Hz] [--gain dB] [--secs N] [--matrix] [--capture file.cf32]
// Antenna power stays OFF. No RF transmission. Run with SDRconnect/SDRuno and other receivers stopped.
#include "../core/src/native_common.h"
#include <fstream>
#include <iostream>
#include <iomanip>
using namespace dect2;

static bool receive(IqSource& source, IqRing& ring, double seconds, std::ofstream* capture) {
    std::vector<cf32> b(65536);
    ring.clear(); ring.resetDropped();
    uint64_t total = 0, bad = 0;
    double energy = 0, peak = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        const size_t n = ring.read(b.data(), b.size());
        for (size_t i = 0; i < n; ++i) {
            const double p = std::norm(b[i]);
            if (!std::isfinite(p)) ++bad;
            else { energy += p; peak = std::max(peak, p); }
        }
        total += n;
        if (capture && n) capture->write(reinterpret_cast<const char*>(b.data()), n * sizeof(cf32));
        if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double measured = total / elapsed, expected = source.sampleRate();
    const bool ok = total > 0 && !bad && energy > 0 && ring.dropped() == 0 && measured > expected * 0.90 && measured < expected * 1.10 && (!capture || capture->good());
    std::cout << std::fixed << std::setprecision(3) << (ok ? "PASS" : "FAIL") << " rate=" << expected
              << " measured=" << measured << " samples=" << total << " dropped=" << ring.dropped()
              << " bad=" << bad << " rms_dbfs=" << (total && energy > 0 ? 10 * std::log10(energy / total) : -999)
              << " peak=" << std::sqrt(peak) << '\n';
    return ok;
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    int port = 0; double seconds = 3; bool matrix = false;
    TuneSettings t; t.centerHz = 100e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.2; t.gainDb = 50;
    std::string capturePath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--matrix") { matrix = true; continue; }
        if (i + 1 == argc) { std::cerr << "missing value for " << a << '\n'; return 2; }
        const std::string v = argv[++i];
        if (a == "--port") port = std::stoi(v);
        else if (a == "--rate") t.sampleRate = std::stod(v);
        else if (a == "--freq") t.centerHz = std::stod(v);
        else if (a == "--gain") t.gainDb = std::stod(v);
        else if (a == "--secs") seconds = std::stod(v);
        else if (a == "--capture") capturePath = v;
        else { std::cerr << "unknown option " << a << '\n'; return 2; }
    }
    if (port < 0 || port > 2 || seconds < 1 || seconds > 3600 || (matrix && !capturePath.empty())) return 2;
    std::vector<DeviceInfo> devices; native::listSdrplay(devices);
    for (size_t i = 0; i < devices.size(); ++i) std::cout << i << ": " << devices[i].name << '\n';
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const DeviceInfo& d) {
        return d.name.find("RSPduo") != std::string::npos && d.nativeArgs == d.serial + "#" + std::to_string(port);
    });
    if (it == devices.end()) { std::cerr << "RSPduo input unavailable. Install Hardware API and stop other receivers.\n"; return 1; }
    std::ofstream capture;
    if (!capturePath.empty()) { capture.open(capturePath, std::ios::binary); if (!capture) return 2; }
    auto source = native::makeSdrplay(*it);
    IqRing ring(1 << 22); std::string err; int failures = 0;
    if (!source->start(t, ring, err)) { std::cerr << err << '\n'; return 1; }
    if (!err.empty()) std::cerr << err << '\n';
    if (!receive(*source, ring, seconds, capture.is_open() ? &capture : nullptr)) ++failures;
    if (matrix) {
        for (double rate : {62500.0, 250000.0, 1000000.0, 2000000.0, 4000000.0, 8000000.0, 10000000.0, 10660000.0}) {
            t.sampleRate = rate; err.clear();
            if (!source->retune(t, err)) { std::cerr << "rate change: " << err << '\n'; return 1; }
            if (!receive(*source, ring, seconds, nullptr)) ++failures;
        }
        t.sampleRate = 2e6;
        for (double hz : {1000.0, 1e6, 7.1e6, 30e6, 59.9e6, 60e6, 100e6, 419.9e6, 420e6, 999.9e6, 1e9, 2e9}) {
            if (hz > it->maxFreqHz) continue;
            t.centerHz = hz;
            for (double gain : {0.0, it->gainMaxDb * 0.5, it->gainMaxDb}) {
                t.gainDb = gain; err.clear();
                std::cout << "tune=" << hz << " gain=" << gain << '\n';
                if (!source->retune(t, err)) { std::cerr << err << '\n'; ++failures; continue; }
                if (!receive(*source, ring, 1, nullptr)) ++failures;
            }
        }
        for (const char* key : {"rfnotch", "dabnotch", "ppm"}) {
            t.radio[key] = "1";
            if (!source->retune(t, err) || !receive(*source, ring, 1, nullptr)) ++failures;
            t.radio.clear();
            if (!source->retune(t, err)) ++failures;
        }
    }
    source->stop();
    // A fresh selection proves stop released the vendor handle.
    for (int i = 0; i < 3; ++i) {
        if (!source->start(t, ring, err)) { std::cerr << "restart: " << err << '\n'; ++failures; break; }
        if (!receive(*source, ring, 1, nullptr)) ++failures;
        source->stop();
    }
    std::cout << "hardware probe: " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
