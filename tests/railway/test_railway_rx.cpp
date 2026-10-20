#include "dect2/railway/railway_modem.h"
#include "dect2/railway/railway_gen.h"
#include "dect2/railway/railway_rx.h"
#include "../common/test_util.h"
#include <algorithm>
#include <cmath>
using namespace dect2;
using namespace dect2::railway;
static std::vector<float> audio(const std::vector<uint8_t>& bits, double baud = 1200) {
    std::vector<float> samples; double phase = 0;
    const size_t n = size_t(std::ceil(bits.size() * 48000 / baud));
    for (size_t i = 0; i < n; i++) {
        const size_t at = std::min(bits.size() - 1, size_t(i * baud / 48000));
        phase += 2 * 3.14159265358979323846 * (bits[at] ? 1200 : 1800) / 48000;
        samples.push_back(float(std::sin(phase) + 0.8)); // discriminator frequency offset becomes DC
    }
    samples.resize(samples.size() + 4800, 0); return samples;
}
int main() {
    for (auto protocol : {Protocol::Klw, Protocol::Tdcs}) for (bool inverted : {false, true}) {
        const auto expected = demoFrame(protocol); const auto samples = audio(airBits(expected, inverted));
        for (size_t chunk : {size_t(1), size_t(37), size_t(4096), samples.size()}) {
            AudioDecoder decoder; std::vector<Message> found;
            for (size_t i = 0; i < samples.size(); i += chunk) decoder.feed(samples.data() + i, std::min(chunk, samples.size() - i), found);
            CHECK(found.size() == 1 && found[0].raw == expected, "audio protocol %d inverted %d chunk %zu: %zu frames", int(protocol), inverted, chunk, found.size());
        }
        // The same wire frame is a new reception after the short phase-dedup window.
        AudioDecoder repeated; std::vector<Message> found;
        repeated.feed(samples.data(), samples.size(), found); repeated.feed(samples.data(), samples.size(), found);
        CHECK(found.size() == 2, "real repeats survive phase deduplication");
    }
    for (double skew : {0.997, 1.003}) {
        const auto samples = audio(airBits(demoFrame(Protocol::Tdcs)), 1200 * skew);
        AudioDecoder decoder; std::vector<Message> found;
        for (size_t i = 0; i < samples.size(); i += 997) decoder.feed(samples.data() + i, std::min(size_t(997), samples.size() - i), found);
        CHECK(found.size() == 1 && found[0].train == "T112", "symbol clock drift %.3f", skew);
    }
    for (auto protocol : {Protocol::Klw, Protocol::Tdcs}) for (double rate : {240000.0, 960000.0}) for (int variant = 0; variant < 5; variant++) {
        const double offsets[] = {0, 1800, -2200, 8000, -8000};
        SynthConfig cfg; cfg.snrDb = variant >= 3 ? 15 : 25; cfg.cfoHz = offsets[variant]; cfg.modeOpt[1] = variant == 2;
        auto synth = makeRailwaySynth(cfg, rate, protocol); RailwayReceiver rx(protocol); rx.configure(rate); rx.setSignalOffset(-50000);
        std::vector<cf32> samples(size_t(rate * 2.2)); synth->generate(samples.data(), samples.size());
        const size_t chunk = variant == 1 ? 37 : 32768;
        for (size_t i = 0; i < samples.size(); i += chunk) rx.feed(samples.data() + i, std::min(chunk, samples.size() - i));
        RailwayTelemetry t; rx.telemetry(t, 0);
        CHECK(t.messages.size() >= 2 && t.messages[0].raw == demoFrame(protocol), "IQ protocol %d rate %.0f variant %d: %zu frames", int(protocol), rate, variant, t.messages.size());
        const auto saved = rx.saveResults(); RailwayReceiver restored(protocol);
        CHECK(restored.loadResults(saved), "saved frame CRC round trip");
        RailwayTelemetry loaded; restored.telemetry(loaded, 0);
        CHECK(loaded.messages.size() == t.messages.size(), "results rows");
        RailwayReceiver other(protocol == Protocol::Klw ? Protocol::Tdcs : Protocol::Klw);
        CHECK(!other.loadResults(saved), "reject another protocol's results");
        rx.clearMessages(); rx.telemetry(t, 0); CHECK(t.messages.empty() && !t.dataValid, "clear");
        rx.reset(); rx.telemetry(t, 0); CHECK(t.blocksOk == 0, "reset counters");
    }
    // Noise must not produce a decoded application frame.
    AudioDecoder decoder; std::vector<float> noise(48000 * 2); unsigned seed = 77;
    for (auto& s : noise) { seed = seed * 1664525u + 1013904223u; s = float(int(seed >> 16) - 32768) / 32768; }
    std::vector<Message> found; decoder.feed(noise.data(), noise.size(), found); CHECK(found.empty(), "audio noise");
    return testSummary("test_railway_rx");
}
