// LR-FHSS coding chain against Semtech's reference implementation, bit for bit: the physical frame (lr_fhss_build_frame), the bit and hop
// counts, the time on air and the carrier of every hop as the SX126x driver writes it into its hop table (PLL steps of 0.95367431640625 Hz
// from the centre). Then the decoders: header (tail-biting Viterbi, CRC-8) and payload (de-interleaver, de-puncturing, Viterbi, CRC-16) with
// noise and lost hops, and the LoRaWAN data rates of the plans.
//
// How the vectors were made: Semtech sx126x_driver v2.5.0 (https://github.com/Lora-net/sx126x_driver, src/lr_fhss_mac.c and
// src/sx126x_lr_fhss.c, unchanged) compiled with gcc together with a small harness that stubs the radio I/O (sx126x_write_register,
// sx126x_write_buffer, ...): it calls lr_fhss_build_frame for the frame bytes, lr_fhss_process_parameters and lr_fhss_get_time_on_air_in_ms,
// and sx126x_lr_fhss_build_frame plus sx126x_lr_fhss_handle_hop with center_freq_in_pll_steps = 2^30, recording the frequency written for
// every hop minus 2^30. Sync word 0x2C0F7995 (LoRaWAN), hopping on. Synthetic tests only; not checked against a real transmitter.
#include "dect2/lrfhss/lrfhss.h"
#include "dect2/lorawan/lorawan_plan.h"
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
static void fail(const char* fmt, ...) {
    std::string b(600, '\0');
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(&b[0], b.size(), fmt, ap);
    va_end(ap);
    printf("FAIL: %s\n", b.c_str());
    fails++;
}

struct Vec {
    const char* name;
    int cr, grid, bw, headers, devOffset, hopSeq;
    const char* payload;
    const char* frame;
    int bits, hops;
    unsigned toaMs;
    std::vector<long> hopPll;
};

static const Vec kVecs[] = {
    {"EU868 DR8", 3, 1, 2, 3, 0, 17, "402B1A0126806400011122334455667788D4C3B2A1",
     "2A70CE40E6CB03DE6567DC6F8D690A90339139A2C0F79958E39FE67A42871DECC668B03DE6561AF7F192D4B0CACB8AA3"
     "722AD1242B258B8B1A3263D0BB02D6FC0E603FE053E8DB2DD68604AA06E74FFD00D4DF67C1983054A40C0DF1FC654D3F"
     "C3C7453EA7DB3F0C6DC5C15445E0D131FB1A500156",
     936, 15, 1917, {-4096, 41216, 57344, 65536, -65536, 12288, -57344, -24576, -16384, -45056, 28672, 0, -32768, -49152, -40960}},
    {"EU868 DR9", 1, 1, 2, 2, 0, 300, "402B1A0126806400011122334455667788D4C3B2A1",
     "29C59CC36BCB03DE6561283CBBE10AFD2312FAF2C0F79958C24F0EC953DF238415739C9F81A20D83AA16019E14009845"
     "CB3671351112D14BF95A55E850D5EBB09D90",
     525, 8, 1076, {-48896, 36864, 49152, -28672, -61440, -45056, -36864, -32768}},
    {"EU868 DR10", 3, 1, 4, 3, -2, 5, "0B30557A9FC4E90E33587DA2",
     "12C8D173660B03DE6568A9C2084084BE345DD992C0F7995B3EF48730210C9C1FFE64B03DE656EDAD29C04C0DEEEA80A2"
     "AE165F369436C2034FE32C6C9251E09361EABCBC0F77A7EB82E3111544DB916EC95A1638719380E893",
     712, 11, 1459, {72704, -148224, -82944, -50176, -1024, -25600, -13312, 76800, 117760, -168960, 15360}},
    {"EU868 DR11", 1, 1, 4, 2, 3, 511, "0B30557A9FC4E90E33587DA2C7EC11365B80A5CAEF14395E83A8CDF2173C6186ABD0F51A3F6489AE",
     "030609BD278B03DE656E6899AE28804DC64D69E2C0F7995B12664BBB31D0A04E1D4C4C2769497D9A4A0940EF79444149"
     "83925056BCA279AE0D95C2F44DB699861C9C146D1D639447880CC97305F08325B4518993F89022B3285E3E30A181E7C0",
     763, 13, 1563, {54016, 115200, -85504, 49664, 168448, 512, -142848, 20992, 107008, -89600, -11776, 86528, 123392}},
    {"US915 DR5", 3, 0, 8, 3, 0, 100, "0B30557A9FC4E90E",
     "07BFDA22710B03DE65609F6BBD7841E3F6899C52C0F79959335EEA7E105BECAAEF14B03DE6566EC7B293C03FDD7DD708"
     "8F21EE1C353E5B8C6D2523601E236EC4BEEC98F0341B9EDD5BFF05E480",
     612, 9, 1254, {492544, 679168, -252928, 252928, -39936, 146432, 652288, 173056, 412672}},
    {"US915 DR6", 1, 0, 8, 2, -26, 383, "0B30557A9FC4E90E33587DA2C7EC11365B80A5CAEF14395E83A8CDF2173C6186AB",
     "1288A03B160B03DE656D68B8871AC42E6C2CE582C0F7995BD26E01F7A1415E8B8678E08F56E9DB420D24A976BE769888"
     "F102C4085D132B33895DCD2067E6498C44052004E0627ABC0B543C93601641BE32032FA6C0",
     675, 11, 1383, {133376, -106496, 718848, -186368, -745472, 399360, -346112, -772096, -559104, -452608, -399360}},
    {"CR 5/6", 0, 1, 2, 1, 1, 64, "0B30557A9FC4E90E33587DA2C7EC11365B80A5CA",
     "3844FC483B4B03DE65429063846284B6379EC28672165717A40C50A0B21B9E1DE728856E1CF482CCE36C54",
     343, 6, 703, {60928, -41472, -20992, 24064, -512, 19968}},
    {"CR 1/2", 2, 1, 6, 4, 0, 200, "0B",
     "1662268C180B03DE6577330E87C74514CD812602C0F7995D448381C0C14633600984B03DE6571401E1383059489A2061"
     "2C0F7995CD847A4D1D1CDA868FCFF7C53E",
     520, 6, 1065, {-290560, -4096, -139008, 249856, 159744, -327680}},
};

static std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> v;
    const std::string t(s);
    auto nib = [](char c) { return c >= 'A' ? c - 'A' + 10 : c - '0'; };
    for (size_t i = 0; i + 1 < t.size(); i += 2) v.push_back((uint8_t)(nib(t[i]) * 16 + nib(t[i + 1])));
    return v;
}

// soft values of bits through additive Gaussian noise (bit 1 = +1)
static std::vector<float> noisy(const std::vector<uint8_t>& bits, double sigma, std::mt19937& rng) {
    std::normal_distribution<double> n(0, sigma);
    std::vector<float> s(bits.size());
    for (size_t i = 0; i < bits.size(); i++) s[i] = (float)((bits[i] ? 1.0 : -1.0) + n(rng));
    return s;
}

int main() {
    using namespace lrfhss;
    // ---- known answers
    for (const Vec& v : kVecs) {
        Params p;
        p.cr = v.cr; p.grid = v.grid; p.bw = v.bw; p.headers = v.headers;
        const std::vector<uint8_t> pl = unhex(v.payload), want = unhex(v.frame);
        const std::vector<uint8_t> got = buildFrame(p, v.hopSeq, pl.data(), pl.size());
        if (got != want) fail("%s: the frame differs from Semtech's (%zu / %zu bytes)", v.name, got.size(), want.size());
        const Digest d = digest(p, pl.size());
        if (d.bits != v.bits || d.hops != v.hops || d.bytes != (int)want.size()) fail("%s: digest %d bits %d hops, want %d %d", v.name, d.bits, d.hops, v.bits, v.hops);
        if (airMs(p, pl.size()) != v.toaMs) fail("%s: time on air %u ms, want %u", v.name, airMs(p, pl.size()), v.toaMs);
        if (std::fabs(airSeconds(p, pl.size()) * 1000 - v.toaMs) > 1) fail("%s: airSeconds %.3f", v.name, airSeconds(p, pl.size()));
        const HopParams h = hopParams(p, v.hopSeq);
        const std::vector<int> seq = gridSequence(p, v.hopSeq, d.hops);
        if (!h.ok || seq.size() != v.hopPll.size()) { fail("%s: %zu hops, want %zu", v.name, seq.size(), v.hopPll.size()); continue; }
        for (size_t k = 0; k < seq.size(); k++) {
            const int s = (int)k < p.headers ? p.headers - 1 - (int)k : -1;
            const double hz = hopOffsetHz(p, h.nGrid, seq[k], v.devOffset, s);
            const long pll = std::lround(hz / 0.95367431640625);
            if (pll != v.hopPll[k]) fail("%s: hop %zu at %ld PLL steps, want %ld", v.name, k, pll, v.hopPll[k]);
            const int pos = s >= 0 ? gridPositionHeader(p, v.hopSeq, s) : gridPositionPayload(p, v.hopSeq, (int)k - p.headers);
            if (pos != seq[k]) fail("%s: hop %zu grid position %d by index, %d in sequence", v.name, k, pos, seq[k]);
        }
        // the bits round trip through the decoders
        const std::vector<uint8_t> bits = frameBits(p, v.hopSeq, pl.data(), pl.size());
        for (int i = 0; i < p.headers; i++) {
            std::vector<float> s80;
            const size_t o = (size_t)i * kHeaderBits + 2;
            for (int j = 0; j < 40; j++) s80.push_back(bits[o + (size_t)j] ? 1.f : -1.f);
            for (int j = 0; j < 40; j++) s80.push_back(bits[o + 72 + (size_t)j] ? 1.f : -1.f);
            const Header hd = decodeHeader(s80.data());
            if (!hd.ok || hd.len != (int)pl.size() || hd.cr != p.cr || hd.bw != p.bw || hd.grid != p.grid || hd.hopSeqId != v.hopSeq || hd.syncIndex != p.headers - 1 - i)
                fail("%s: header copy %d decoded as ok %d len %d cr %d bw %d seq %d index %d", v.name, i, hd.ok, hd.len, hd.cr, hd.bw, hd.hopSeqId, hd.syncIndex);
        }
        std::vector<float> soft;
        for (size_t k = (size_t)p.headers * kHeaderBits; k < bits.size(); k++) {
            const size_t off = (k - (size_t)p.headers * kHeaderBits) % kBlockBits;
            if (off >= 2) soft.push_back(bits[k] ? 1.f : -1.f);
        }
        std::vector<uint8_t> back;
        if ((int)soft.size() != d.codedBits || !decodePayload(p.cr, pl.size(), soft.data(), back) || back != pl) fail("%s: payload round trip", v.name);
    }
    // ---- interleaver: a permutation for every length the payloads give
    for (int len = 1; len <= 255; len++)
        for (int cr = 0; cr < 4; cr++) {
            Params p; p.cr = cr;
            const int n = digest(p, (size_t)len).codedBits;
            const std::vector<uint16_t> o = interleaverOrder(n);
            std::vector<uint8_t> seen((size_t)n, 0);
            bool ok = (int)o.size() == n;
            for (uint16_t x : o) { if (x >= n || seen[x]) ok = false; else seen[x] = 1; }
            if (!ok) fail("interleaver of %d bits (length %d, CR %s) is not a permutation", n, len, crName(cr));
        }
    // ---- decoders with noise and lost hops
    std::mt19937 rng(7);
    {
        Params p; p.cr = Cr1_3; p.headers = 3;
        int good = 0;
        for (int t = 0; t < 200; t++) {
            const auto raw = rawHeader(p, t % 384, 1 + t % 60, t % 3);
            const std::vector<float> s = noisy(headerAirBits(raw), 0.6, rng);
            const Header h = decodeHeader(s.data());
            good += h.ok && h.raw == raw;
        }
        if (good < 196) fail("header at sigma 0.6: %d of 200", good);
    }
    for (int cr : {(int)Cr1_3, (int)Cr2_3}) {
        Params p; p.cr = cr;
        const size_t len = 30;
        int good = 0;
        for (int t = 0; t < 50; t++) {
            std::vector<uint8_t> pl(len);
            for (auto& b : pl) b = (uint8_t)rng();
            const std::vector<uint8_t> bits = frameBits(p, 7, pl.data(), len);
            std::vector<uint8_t> coded;
            for (size_t k = (size_t)p.headers * kHeaderBits; k < bits.size(); k++) if ((k - (size_t)p.headers * kHeaderBits) % kBlockBits >= 2) coded.push_back(bits[k]);
            std::vector<float> s = noisy(coded, cr == Cr1_3 ? 0.5 : 0.4, rng);
            // lose hops: CR 1/3 one in three, CR 2/3 one in eight
            const int hops = ((int)s.size() + kFragBits - 1) / kFragBits;
            const int every = cr == Cr1_3 ? 3 : 8;
            for (int h = (t % every); h < hops; h += every)
                for (int j = h * kFragBits; j < std::min((int)s.size(), (h + 1) * kFragBits); j++) s[(size_t)j] = 0;
            std::vector<uint8_t> back;
            good += decodePayload(cr, len, s.data(), back) && back == pl;
        }
        if (good < 48) fail("payload CR %s with noise and lost hops: %d of 50", crName(cr), good);
        printf("  CR %s: %d of 50 payloads with noise (sigma %.1f) and lost hops\n", crName(cr), good, cr == Cr1_3 ? 0.5 : 0.4);
    }
    // a damaged payload fails its CRC
    {
        Params p;
        std::vector<uint8_t> pl(10, 0x55), back;
        const std::vector<uint8_t> bits = frameBits(p, 1, pl.data(), pl.size());
        std::vector<float> s;
        for (size_t k = (size_t)p.headers * kHeaderBits; k < bits.size(); k++) if ((k - (size_t)p.headers * kHeaderBits) % kBlockBits >= 2) s.push_back(bits[k] ? -1.f : 1.f);
        if (decodePayload(p.cr, pl.size(), s.data(), back)) fail("an inverted payload passed its CRC");
    }
    // ---- the LoRaWAN data rates (RP002)
    {
        using namespace lorawan;
        const auto eu = planLrfhssRates(EU868), us = planLrfhssRates(US915);
        if (eu.size() != 4 || us.size() != 2) fail("LR-FHSS data rates: EU868 %zu, US915 %zu", eu.size(), us.size());
        else {
            if (eu[0].dr != 8 || eu[0].cr != Cr1_3 || eu[0].bw != 2 || eu[0].grid != Grid3906 || eu[0].headers != 3) fail("EU868 DR8");
            if (eu[1].dr != 9 || eu[1].cr != Cr2_3 || eu[1].bw != 2 || eu[1].headers != 2) fail("EU868 DR9");
            if (eu[2].dr != 10 || eu[2].cr != Cr1_3 || eu[2].bw != 4) fail("EU868 DR10");
            if (eu[3].dr != 11 || eu[3].cr != Cr2_3 || eu[3].bw != 4) fail("EU868 DR11");
            if (us[0].dr != 5 || us[0].cr != Cr1_3 || us[0].bw != 8 || us[0].grid != Grid25391) fail("US915 DR5");
            if (us[1].dr != 6 || us[1].cr != Cr2_3 || us[1].bw != 8) fail("US915 DR6");
            if (std::fabs(occupiedHz(eu[0].bw) - 136719) > 1 || std::fabs(occupiedHz(eu[2].bw) - 335938) > 1 || std::fabs(occupiedHz(us[0].bw) - 1523438) > 1) fail("OCW widths");
        }
        if (lrfhssDataRate(EU868, Cr2_3, 4, Grid3906) != 11 || lrfhssDataRate(US915, Cr1_3, 8, Grid25391) != 5 || lrfhssDataRate(AS923_1, Cr1_3, 2, Grid3906) != -1)
            fail("lrfhssDataRate lookup");
        // EU868 DR8, 10 bytes: (12 * 8 + 6) * 3 = 306 coded bits, 7 hops of payload, 3 headers
        Params p;
        const Digest d = digest(p, 10);
        if (d.hops != 10 || d.bits != 3 * 114 + 6 * 50 + 18 + 2) fail("DR8 10 bytes: %d hops %d bits", d.hops, d.bits);
    }
    if (fails) printf("lrfhss coding: %d FAILED\n", fails); else printf("%s\n", "lrfhss coding: all passed");
    return fails ? 1 : 0;
}
