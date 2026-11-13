// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS: the receiver against a transmitter written separately from OnAir's own generator (issue #87).
// Nothing here uses ais_gen / ais_testutil / the proto layer's stuff(), nrziEncode(), appendFcs() or burstLineBits(): the frame is built from the
// gpsd AIVDM document's sentences exactly as ITU-R M.1371-5 annex 2 describes it:
//   6 bit armour -> message bits (field MSB first) -> octets (bit 0 of the tables = MSB of the octet) -> sent LSB first -> FCS = CRC-16/X-25
//   over the octets (poly 0x1021 reflected = 0x8408, init 0xFFFF, xorout 0xFFFF, sent low octet first, each octet LSB first) -> zero insertion after five 1s
//   -> flags 0x7E -> NRZI (a 0 is a change of level) -> GMSK, BT 0.4, modulation index 0.5, 9600 bd at 96 kHz -> placed at +-25 kHz at 2.4 Msps.
// The result goes into AisReceiver and the stations, positions and sentences it reports are compared with what the sentences say.
#include "dect2/ais/ais_rx.h"
#include <cstdlib>
#include "dect2/common/stable_random.h"
#include "dect2/ais/ais_proto.h"
#include <cmath>
#include "dect2/common/crc.h"
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef std::vector<int> V;

// ---- the transmitter
static V armourToBits(const std::string& chars, int fill) {
    V b;
    for (char c : chars) {
        int v = c - 48;
        if (v > 40) v -= 8;
        for (int k = 5; k >= 0; k--) b.push_back((v >> k) & 1);
    }
    b.resize(b.size() - (size_t)fill);
    return b;
}
static std::string bitsToArmour(const V& bits) {      // the other way, for a message built from fields below
    std::string s;
    for (size_t i = 0; i < bits.size(); i += 6) {
        int v = 0;
        for (size_t k = 0; k < 6; k++) v = v * 2 + (i + k < bits.size() ? bits[i + k] : 0);
        s.push_back((char)(v < 40 ? v + 48 : v + 56));
    }
    return s;
}
static void field(V& b, size_t pos, size_t len, long long v) {
    if (b.size() < pos + len) b.resize(pos + len, 0);
    for (size_t i = 0; i < len; i++) b[pos + i] = (int)((((unsigned long long)v) >> (len - 1 - i)) & 1);
}
static long long fieldU(const V& b, size_t pos, size_t len) { long long v = 0; for (size_t i = 0; i < len; i++) v = v * 2 + b[pos + i]; return v; }
static long long fieldS(const V& b, size_t pos, size_t len) { long long v = fieldU(b, pos, len); return (v >> (len - 1)) ? v - (1LL << len) : v; }

static uint16_t crc16x25(const std::vector<uint8_t>& bytes) { return dect2::crc::crc16X25(bytes.data(), bytes.size()); }

// the bits on the air for one packet: training sequence, flag, stuffed message + FCS, flag, buffer (all after NRZI)
static V airBits(const V& msg) {
    V m = msg;
    while (m.size() % 8) m.push_back(0);                      // whole octets
    std::vector<uint8_t> oct(m.size() / 8, 0);
    for (size_t i = 0; i < m.size(); i++) oct[i / 8] |= (uint8_t)(m[i] << (7 - i % 8));   // the message tables number the bits of an octet from the MSB
    const uint16_t fcs = crc16x25(oct);
    V frame;                                                   // HDLC sends every octet LSB first: the order on the air is NOT the order of the tables
    for (uint8_t by : oct) for (int k = 0; k < 8; k++) frame.push_back((by >> k) & 1);
    for (int i = 0; i < 16; i++) frame.push_back((fcs >> i) & 1);      // FCS: low octet first, LSB first
    V st;
    int ones = 0;
    for (int b : frame) {
        st.push_back(b);
        ones = b ? ones + 1 : 0;
        if (ones == 5) { st.push_back(0); ones = 0; }
    }
    V nrz;
    for (int i = 0; i < 24; i++) nrz.push_back(0);             // training: zeros turn into 0101... after NRZI
    const int flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    for (int b : flag) nrz.push_back(b);
    for (int b : st) nrz.push_back(b);
    for (int b : flag) nrz.push_back(b);
    for (int i = 0; i < 4; i++) nrz.push_back(1);              // buffer
    V line;
    int lv = 1;
    for (int b : nrz) { if (b == 0) lv ^= 1; line.push_back(lv); }
    return line;
}

// GMSK at 96 kHz, then the frequency pulse is interpolated to 2.4 Msps and integrated there; carrier at 'fc' Hz
static void gmsk(const V& line, double fc, double amp, double phase0, std::vector<cf32>& out) {
    const int up = 10;                                          // 96 kHz / 9600
    const double bt = 0.4, sigma = std::sqrt(std::log(2.0)) / (2 * M_PI * bt) * up;   // Gaussian filter in samples
    std::vector<double> nrz;
    for (int i = 0; i < 3 * up; i++) nrz.push_back(0);
    for (int b : line) for (int k = 0; k < up; k++) nrz.push_back(b ? 1.0 : -1.0);
    for (int i = 0; i < 3 * up; i++) nrz.push_back(0);
    const int half = 4 * up;
    std::vector<double> g(2 * half + 1);
    double sum = 0;
    for (int i = -half; i <= half; i++) { g[(size_t)(i + half)] = std::exp(-0.5 * i * i / (sigma * sigma)); sum += g[(size_t)(i + half)]; }
    for (double& v : g) v /= sum;
    std::vector<double> f(nrz.size(), 0.0);
    for (size_t i = 0; i < nrz.size(); i++) {
        double a = 0;
        for (int k = -half; k <= half; k++) { const long j = (long)i + k; if (j >= 0 && j < (long)nrz.size()) a += g[(size_t)(k + half)] * nrz[(size_t)j]; }
        f[i] = a;
    }
    const double fs = 2.4e6, dev = 0.5 * 9600.0 / 2;            // h = 0.5: +-2400 Hz
    const size_t n = (size_t)((double)(f.size() - 1) * 25.0);
    double ph = phase0;
    out.resize(n);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / 25.0;
        const size_t i0 = (size_t)t;
        const double fr = t - (double)i0;
        const double fm = f[i0] * (1 - fr) + f[std::min(i0 + 1, f.size() - 1)] * fr;
        ph += 2 * M_PI * (fc + dev * fm) / fs;
        out[i] = cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
    }
}

struct Tx { std::string chars; int fill; char chan; double cfo; };

static std::string payloadOf(const AisTelemetry& t, const std::string& want) {   // does the receiver's sentence list contain this payload?
    for (const std::string& s : t.nmea) if (s.find("," + want + ",") != std::string::npos) return s;
    return "";
}

static void runCase(const char* name, const std::vector<Tx>& txs, double snrDb, uint32_t seed) {
    const double fs = 2.4e6;
    std::mt19937 rng(seed);
    dect2::normal_distribution<double> nd(0.0, 1.0);
    const double amp = 0.3, nsig = amp / std::pow(10.0, snrDb / 20.0) / std::sqrt(2.0) * std::sqrt(fs / 48000.0 / 1.0);   // noise per sample for snr in 48 kHz
    std::vector<cf32> x;
    auto gap = [&](double sec) { const size_t n = (size_t)(sec * fs); for (size_t i = 0; i < n; i++) x.push_back(cf32(0, 0)); };
    gap(0.6);
    std::vector<V> msgs;
    for (const Tx& t : txs) {
        const V msg = armourToBits(t.chars, t.fill);
        msgs.push_back(msg);
        std::vector<cf32> b;
        gmsk(airBits(msg), (t.chan == 'A' ? -25000.0 : 25000.0) + t.cfo, amp, 0.7, b);
        for (const cf32& v : b) x.push_back(v);
        gap(0.4);
    }
    for (cf32& v : x) v += cf32{(float)(nd(rng) * nsig), (float)(nd(rng) * nsig)};
    if (const char* d = getenv("AIS_WIRE_DUMP")) {   // measurement only: the signal as cf32 for other decoders (2.4 Msps)
        if (FILE* f = fopen((std::string(d) + "_" + std::to_string(seed) + ".cf32").c_str(), "wb")) { fwrite(x.data(), sizeof(cf32), x.size(), f); fclose(f); }
    }
    AisReceiver rx;
    rx.configure(fs);
    std::vector<std::vector<uint8_t>> got;
    rx.setFrameCallback([&](char, const ais::Bits& b, double, double, double) { got.push_back(b); });
    AisTelemetry t, last;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 16384) {
        rx.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    rx.feed(std::vector<cf32>(20000, cf32(0, 0)).data(), 20000);
    while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    printf("%s: frames %zu ok %llu bad %llu vessels %zu\n", name, got.size(), (unsigned long long)last.blocksOk, (unsigned long long)last.blocksBad, last.vessels.size());
    for (size_t k = 0; k < txs.size(); k++) {
        bool found = false;
        for (const auto& g : got) {
            if (g.size() != msgs[k].size()) continue;
            bool same = true;
            for (size_t i = 0; i < g.size() && same; i++) same = g[i] == msgs[k][i];
            found = found || same;
        }
        CHECK(found, "%s: message %zu (%s) not received bit for bit", name, k, txs[k].chars.substr(0, 12).c_str());
        CHECK(!payloadOf(last, txs[k].chars).empty() || msgs[k].size() > 360, "%s: sentence %zu missing from the list", name, k);
        const long long mmsi = fieldU(msgs[k], 8, 30);
        const AisVessel* v = nullptr;
        for (const auto& s : last.vessels) if (s.mmsi == (uint32_t)mmsi) v = &s;
        CHECK(v != nullptr, "%s: station %lld not in the table", name, mmsi);
        if (!v) continue;
        const int type = (int)fieldU(msgs[k], 0, 6);
        if (type == 1 || type == 18) {
            const double lon = type == 1 ? fieldS(msgs[k], 61, 28) / 600000.0 : fieldS(msgs[k], 57, 28) / 600000.0;
            const double lat = type == 1 ? fieldS(msgs[k], 89, 27) / 600000.0 : fieldS(msgs[k], 85, 27) / 600000.0;
            CHECK(v->hasPos && std::fabs(v->lat - lat) < 1e-5 && std::fabs(v->lon - lon) < 1e-5, "%s: %lld position %.5f %.5f, want %.5f %.5f", name, mmsi, v->lat, v->lon, lat, lon);
        }
    }
    CHECK(last.vessels.size() == txs.size() || txs.size() > 1, "%s: %zu stations for %zu transmitters", name, last.vessels.size(), txs.size());
}

// Three messages from a real recording (sigidwiki.com "AIS IQ", 2016): the 168 bits of the frame as they came off the air with a good FCS, and what
// the ships are. Read in the order of the air they look like type 8 with MMSI 6857735; with every octet reversed they are class A position reports.
static void realFrames() {
    const struct { const char* air; unsigned mmsi; } f[] = {
        {"001000000000000110100010100100000001110100000000000000000000100010110100101001011100101000000101111001010111010011100100000011011010011001010000000000010010101000011001", 538002030u},
        {"001000000010110010110010001001110100110100000100000000000000000010110100010100101100110000000100100001011111100000001010000000001111111100111001010000000000001111100000", 219380012u},
        {"001000000010110011011111011110101001110000000111000000000000000010110100010100100011110000000101100001010000010011011010110001101111111101110101000000001100000110011010", 222222222u}};
    for (const auto& e : f) {
        ais::Bits b;
        for (const char* c = e.air; *c; c++) b.push_back((uint8_t)(*c - '0'));
        ais::reverseOctets(b);
        ais::AisMsg m;
        const bool ok = ais::decodeMessage(b, m);
        CHECK(ok && m.type == 1 && m.mmsi == e.mmsi, "real frame: type %d mmsi %u, want 1 / %u", m.type, m.mmsi, e.mmsi);
    }
}

int main() {
    realFrames();
    // gpsd AIVDM document: type 1 (MMSI 477553000, 47.5828 N 122.3458 W), another type 1, a two part type 5
    const std::string t1 = "177KQJ5000G?tO`K>RA1wUbN0TKH", t1b = "15N4cJ`005Jrek0H@9n`DW5608EP";
    const std::string t5 = "55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53" "1@0000000000000";
    {   // the expected values straight from the documented sentence: the independent bit reader and the document agree
        const V m = armourToBits(t1, 0);
        CHECK(m.size() == 168 && fieldU(m, 8, 30) == 477553000, "gpsd type 1 mmsi %lld", fieldU(m, 8, 30));
        CHECK(std::fabs(fieldS(m, 89, 27) / 600000.0 - 47.5828) < 1e-3 && std::fabs(fieldS(m, 61, 28) / 600000.0 + 122.3458) < 1e-3, "gpsd type 1 position");
    }
    // a Dutch class B position report (type 18) written from fields
    V b18;
    field(b18, 0, 6, 18); field(b18, 8, 30, 244670316); field(b18, 46, 10, 52);
    field(b18, 57, 28, std::llround(4.8952 * 600000)); field(b18, 85, 27, std::llround(52.3702 * 600000));
    field(b18, 112, 12, 1234); field(b18, 124, 9, 511); field(b18, 133, 6, 20); b18.resize(168, 0);
    const std::string t18 = bitsToArmour(b18);
    // a Dutch class A (type 1) in Rotterdam
    V b1;
    field(b1, 0, 6, 1); field(b1, 8, 30, 244820123); field(b1, 38, 4, 0); field(b1, 42, 8, -128); field(b1, 50, 10, 83);
    field(b1, 61, 28, std::llround(4.1234 * 600000)); field(b1, 89, 27, std::llround(51.9876 * 600000));
    field(b1, 116, 12, 2210); field(b1, 128, 9, 221); field(b1, 137, 6, 33); b1.resize(168, 0);
    const std::string t1nl = bitsToArmour(b1);

    runCase("clean, channel A", {{t1, 0, 'A', 0}}, 40, 1);
    runCase("clean, channel B", {{t1, 0, 'B', 0}}, 40, 2);
    runCase("gpsd messages, both channels, offsets", {{t1, 0, 'A', 300}, {t1b, 0, 'B', -700}, {t5, 2, 'A', 150}, {t18, 0, 'B', 0}, {t1nl, 0, 'A', -400}}, 28, 3);
    runCase("noisy", {{t1nl, 0, 'A', 200}, {t18, 0, 'B', -300}}, 17, 4);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
