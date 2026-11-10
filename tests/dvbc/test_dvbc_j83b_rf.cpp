// Annex B through the whole DVB-C demodulator: the test transmitter's pulse shaping, carrier offset and clock error into DvbcReceiver, packets out. Simulated.
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbc/dvbc.h"
#include "dect2/dvbc/j83b.h"
#include <memory>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    struct C { int qam; double cfo, ppm, snr; };
    for (const C c : std::initializer_list<C>{{2, 0, 0, 32}, {2, 2500, 20, 28}, {4, 0, 0, 36}, {4, -1800, -15, 33}}) {
        const double fs = 12e6;
        j83b::EncConfig ec; ec.qam256 = c.qam == 4; ec.control = 0;
        auto enc = std::make_shared<j83b::J83bEncoder>(ec);
        auto n = std::make_shared<uint64_t>(0);
        dvbc::SignalConfig sc; sc.qam = c.qam; sc.sampleRate = fs; sc.symbolRate = c.qam == 4 ? j83b::kSymRate256 : j83b::kSymRate64; sc.rollOff = c.qam == 4 ? j83b::kRollOff256 : j83b::kRollOff64;
        sc.snrDb = c.snr; sc.cfoHz = c.cfo; sc.clockPpm = c.ppm;
        sc.symbols = [enc, n](std::vector<cf32>& out) {
            const size_t was = out.size(); for (int i = 0; i < 2000 && out.size() == was; i++) { uint8_t p[188]; memset(p, 0x5A, 188); const int pid = 0x100 + ((*n) & 3); p[0] = 0x47; p[1] = pid >> 8; p[2] = pid; p[3] = 0x10 | ((*n / 4) & 15); (*n)++; enc->push(p); enc->take(out); }
        };
        dvbc::Signal sig(sc);
        DvbcReceiver rx; rx.setLogCallback([](const std::string& m) { printf("  %s\n", m.c_str()); }); rx.configure(fs); rx.setAnnexB(true); rx.setBlocking(true);
        std::vector<uint8_t> got;
        rx.setPacketCallback([&](const uint8_t* p, size_t k, double) { got.insert(got.end(), p, p + k * 188); });
        std::vector<cf32> buf(1 << 18);
        DvbcTelemetry t; uint64_t q = 0;
        for (int i = 0; i < 60; i++) { sig.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); if (rx.telemetry(t, q)) q = t.seq; }
        rx.flush(); rx.telemetry(t, 0);
        int payload = 0;
        for (size_t i = 0; i + 188 <= got.size(); i += 188) if (got[i + 100] == 0x5A && got[i + 150] == 0x5A) payload++;
        printf("qam %d cfo %+.0f %+.0f ppm snr %.0f: packets %zu, payload ok %d, RS clean %llu failed %llu\n", c.qam, c.cfo, c.ppm, c.snr, got.size() / 188, payload, (unsigned long long)t.rsClean, (unsigned long long)t.rsFailed);
        CHECK(payload > 5000, "only %d good packets", payload);
    }
    printf(fails ? "j83b rf FAILED\n" : "j83b rf passed\n");
    return fails ? 1 : 0;
}
