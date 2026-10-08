// AIS physical layer, independent of the generator: real sentences from gpsd's sample.aivdm are framed (flags, stuffing, FCS by a table-driven CRC),
// NRZI coded and modulated here by a plain textbook GMSK modulator (Gaussian filtered NRZ, BT 0.4, 64 samples per bit, phase by summing), put on
// the channel they were seen on and fed to the receiver, which must return the very same sentences. Different rates, carrier phases and bit timing.
#include "dect2/ais_testutil.h"
#include "dect2/gen_util.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::ais;
using namespace dect2::aistest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;

// ---- framing written a second way: bytes, a CRC table, a stuffing loop
static uint16_t crcTable[256];
static void initCrc() {
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
        crcTable[i] = c;
    }
}
static std::vector<int> frameBits(const Bits& payload) {
    std::vector<int> d;
    // FCS over bytes whose bits are taken in transmission order, least significant bit first
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i + 8 <= payload.size(); i += 8) {
        int byte = 0;
        for (int k = 0; k < 8; k++) byte |= payload[i + k] << k;
        crc = (uint16_t)((crc >> 8) ^ crcTable[(crc ^ byte) & 0xFF]);
    }
    crc = (uint16_t)~crc;
    std::vector<int> body(payload.begin(), payload.end());
    for (int k = 0; k < 16; k++) body.push_back((crc >> k) & 1);
    for (int v : {0, 1, 1, 1, 1, 1, 1, 0}) d.push_back(v);                // flag
    int ones = 0;
    for (int v : body) { d.push_back(v); if (v) { if (++ones == 5) { d.push_back(0); ones = 0; } } else ones = 0; }
    for (int v : {0, 1, 1, 1, 1, 1, 1, 0}) d.push_back(v);
    for (int i = 0; i < 8; i++) d.push_back(0);
    std::vector<int> line;
    for (int i = 0; i < 24; i++) line.push_back(i % 2);                    // training sequence: alternates on the line, ends on 1
    int lv = 1;                                                            // NRZI for the rest: a 0 changes the level
    for (int v : d) { if (!v) lv ^= 1; line.push_back(lv); }
    return line;
}

// ---- textbook GMSK
static const int kOv = 64;
static std::vector<double> gmskPhase(const std::vector<int>& line, int padBits) {
    const double B = 0.4;                                                  // B T with T = 1
    const int span = 4;
    std::vector<double> g;
    double sum = 0;
    for (int i = -span * kOv; i <= span * kOv; i++) {
        const double t = (double)i / kOv;
        const double v = std::exp(-2 * kPi * kPi * B * B * t * t / std::log(2.0));
        g.push_back(v); sum += v;
    }
    for (double& v : g) v /= sum;
    std::vector<double> nrz((line.size() + 2 * padBits) * kOv, 0.0);
    for (size_t k = 0; k < line.size(); k++)
        for (int j = 0; j < kOv; j++) nrz[(k + padBits) * kOv + j] = line[k] ? 1.0 : -1.0;
    std::vector<double> ph(nrz.size(), 0.0);
    double acc = 0;
    const int half = (int)g.size() / 2;
    for (size_t n = 0; n < nrz.size(); n++) {
        double f = 0;
        for (int i = 0; i < (int)g.size(); i++) {
            const long idx = (long)n + half - i;
            if (idx >= 0 && idx < (long)nrz.size()) f += g[i] * nrz[idx];
        }
        acc += f * (kPi / 2) / kOv;                                        // one bit at full frequency moves the phase by pi/2
        ph[n] = acc;
    }
    return ph;
}

static std::vector<cf32> signalFor(const std::vector<std::pair<Bits, char>>& msgs, double rate, double startFrac, double snrDb, double cfo, uint32_t seed) {
    const double gap = 0.06;
    const size_t n = (size_t)((0.3 + gap * msgs.size() + 0.3) * rate);
    std::vector<cf32> x(n, cf32(0, 0));
    for (size_t m = 0; m < msgs.size(); m++) {
        const std::vector<int> line = frameBits(msgs[m].first);
        const int pad = 4;
        const std::vector<double> ph = gmskPhase(line, pad);
        const double t0 = 0.3 + gap * m + startFrac / 9600.0;
        const double fc = (msgs[m].second == 'B' ? 25000.0 : -25000.0) + cfo;
        const double len = (double)line.size() / 9600.0;
        const size_t i0 = (size_t)((t0 - 0.001) * rate), i1 = std::min(n, (size_t)((t0 + len + 0.001) * rate));
        for (size_t i = i0; i < i1; i++) {
            const double t = (double)i / rate - t0;
            const double u = t * 9600.0;                                    // bits since the burst start
            if (u < -pad + 1e-6 || u > (double)line.size() + pad - 1e-6) continue;
            const double pos = (u + pad) * kOv;
            const size_t k = (size_t)pos;
            if (k + 1 >= ph.size()) continue;
            const double phi = ph[k] + (pos - (double)k) * (ph[k + 1] - ph[k]);
            double amp = 1.0;
            if (u < 0) amp = std::max(0.0, 1 + u / 3); else if (u > (double)line.size()) amp = std::max(0.0, 1 - (u - (double)line.size()) / 3);
            const double ang = 2 * kPi * fc * t + phi + 0.7 * (double)m;
            x[i] += cf32((float)(0.3 * amp * std::cos(ang)), (float)(0.3 * amp * std::sin(ang)));
        }
    }
    if (snrDb > -100) {
        genutil::NoiseSource ns(seed);
        const double pn = 0.09 * (rate / 48000.0) / std::pow(10.0, snrDb / 10.0);
        ns.add(x.data(), n, (float)std::sqrt(pn / 2));
    }
    return x;
}

struct Known { const char* sentence; };

int main() {
    initCrc();
    // single sentence messages from sample.aivdm (the channel letter is part of the sentence and of its checksum)
    const char* sentences[] = {
        "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A",
        "!AIVDM,1,1,,A,16SteH0P00Jt63hHaa6SagvJ087r,0*42",
        "!AIVDM,1,1,,B,25Cjtd0Oj;Jp7ilG7=UkKBoB0<06,0*60",
        "!AIVDM,1,1,,A,38Id705000rRVJhE7cl9n;160000,0*40",
        "!AIVDM,1,1,,A,403OviQuMGCqWrRO9>E6fE700@GO,0*4D",
        "!AIVDM,1,1,,A,91b77=h3h00nHt0Q3r@@07000<0b,0*69",
        "!AIVDM,1,1,,A,B52K>;h00Fc>jpUlNV@ikwpUoP06,0*4C",
        "!AIVDM,1,1,,B,C5N3SRgPEnJGEBT>NhWAwwo862PaLELTBJ:V00000000S0D:R220,0*0B",
        "!AIVDM,1,1,,A,H42O55i18tMET00000000000000,2*6D",
        "!AIVDM,1,1,,A,H42O55lti4hhhilD3nink000?050,0*40",
        "!AIVDM,1,1,,A,>5?Per18=HB1U:1@E=B0m<L,2*51",
    };
    std::vector<std::pair<Bits, char>> msgs;
    std::vector<std::string> want;
    for (const char* s : sentences) {
        std::string chars; int fill, cnt, no; char ch;
        if (!parseNmea(s, chars, fill, cnt, no, &ch)) { printf("FAIL: bad test sentence %s\n", s); return 1; }
        Bits b = unarmour(chars, fill);
        msgs.push_back({b, ch});
        want.push_back(s);
    }
    // the 424 bit type 5 message of the same file, in two sentences
    {
        std::string c1, c2; int f1, f2, n1, n2; char ch;
        parseNmea("!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C", c1, f1, n1, n2, &ch);
        parseNmea("!AIVDM,2,2,1,A,88888888880,2*25", c2, f2, n1, n2);
        msgs.push_back({unarmour(c1 + c2, f2), 'A'});
    }
    struct Case { double rate; double frac; double cfo; double snr; };
    const Case cases[] = {{2e6, 0.0, 0, 40}, {2e6, 0.37, 1500, 25}, {2.4e6, 0.71, -2000, 25}, {8e6, 0.5, 700, 25}, {1e6, 0.23, -300, 30}};
    for (const Case& cs : cases) {
        const std::vector<cf32> x = signalFor(msgs, cs.rate, cs.frac, cs.snr, cs.cfo, 3);
        AisReceiver rx;
        rx.configure(cs.rate);
        Collector col;
        const AisTelemetry t = runReceiver(rx, x, 16384, &col);
        // the sentences come back unchanged (the type 5 sequence id of the second one may differ, so only its payload is compared)
        std::set<std::string> have(t.nmea.begin(), t.nmea.end());
        int found = 0;
        for (const auto& w : want) if (have.count(w)) found++;
        CHECK(found == (int)want.size(), "rate %.1f MS/s, offset %.0f Hz: %d of %zu sentences came back unchanged", cs.rate / 1e6, cs.cfo, found, want.size());
        bool t5 = false;
        for (const auto& g : col.got) if (g.bits == msgs.back().first) t5 = true;
        CHECK(t5, "type 5 (2 sentences) decoded at %.1f MS/s", cs.rate / 1e6);
        CHECK(t.blocksOk == msgs.size(), "message count %llu of %zu", (unsigned long long)t.blocksOk, msgs.size());
        // the fields of the first message from the values printed in sample.aivdm
        bool seen = false;
        for (const AisVessel& v : t.vessels) if (v.mmsi == 371798000) {
            seen = true;
            CHECK(std::fabs(v.sog - 12.3) < 1e-3 && std::fabs(v.lat - 48.3816333) < 1e-5 && std::fabs(v.lon + 123.3953833) < 1e-5 && v.heading == 215 && std::fabs(v.cog - 224) < 1e-3, "vessel 371798000 fields");
        }
        CHECK(seen, "vessel 371798000 in the table");
        bool ever = false;
        for (const AisVessel& v : t.vessels) if (v.mmsi == 351759000) {
            ever = true;
            CHECK(v.name == "EVER DIADEM" && v.callsign == "3FOF8" && v.destination == "NEW YORK" && v.imo == 9134270 && v.shipType == 70 && v.dimA == 225 && v.dimB == 70, "EVER DIADEM voyage data");
        }
        CHECK(ever, "vessel 351759000 in the table");
        CHECK(std::fabs(t.cfoHz - cs.cfo) < 150, "carrier offset %.0f reported as %.0f", cs.cfo, t.cfoHz);
        printf("rate %.1f MS/s offset %5.0f Hz start %.2f bit: %llu messages, carrier %.0f Hz, snr %.1f dB\n", cs.rate / 1e6, cs.cfo, cs.frac, (unsigned long long)t.blocksOk, t.cfoHz, t.snrDb);
    }
    if (fails) { printf("%d checks failed\n", fails); return 1; }
    printf("ais_phy: ok\n");
    return 0;
}
