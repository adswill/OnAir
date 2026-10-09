// HD Radio channel coding against small known cases of the NRSC-5 documents (1011s, 1012s) and round trips: the scrambler polynomial,
// the impulse responses of the convolutional codes, the soft Viterbi decoder on noisy tail-biting words, the Reed-Solomon header code,
// the interleaver tables (every position used once; the AM block layout of 1012s figure 10-2), and the Layer 2 pieces (SIS, PDU, HDLC,
// ID3, SIG, LOT) through the decoder. Quick and silent.
#include "dect2/hdr_fec.h"
#include "dect2/hdr_gen.h"
#include "dect2/hdr_l2.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::hdr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void testScrambler() {
    // 1011s section 8.2: P(x) = 1 + x^9 + x^11, maximal length, reset to 0111 1111 111 for every transfer frame
    std::vector<uint8_t> z(4200, 0);
    scramble(z.data(), z.size());
    bool rec = true;
    for (size_t m = 11; m < z.size(); m++) if (z[m] != (z[m - 2] ^ z[m - 11])) rec = false;
    CHECK(rec, "scrambler sequence does not follow its polynomial");
    int period = 0;
    for (int p = 1; p <= 2047 && !period; p++) {
        bool same = true;
        for (int i = 0; i < 2048 && same; i++) same = z[(size_t)i] == z[(size_t)(i + p)];
        if (same) period = p;
    }
    CHECK(period == 2047, "scrambler period %d (2047 expected)", period);
    std::vector<uint8_t> a(100, 1), b = a;
    scramble(a.data(), a.size()); scramble(a.data(), a.size());
    CHECK(a == b, "scrambling twice is not the identity");
}

static void testConv() {
    // the impulse response of each output is its generator polynomial, highest power first (1011s table 9-2, 1012s section 9.1.4)
    const ConvCode* codes[3] = {&kCodeFm, &kCodeE1, &kCodeE2};
    for (const ConvCode* c : codes) {
        std::vector<uint8_t> in(40, 0), out(120);
        in[10] = 1;
        convEncode(*c, in.data(), in.size(), out.data());
        bool ok = true;
        for (int t = 0; t < c->k; t++)
            for (int m = 0; m < 3; m++) if (out[(size_t)(3 * (10 + t) + m)] != ((c->gen[m] >> (c->k - 1 - t)) & 1)) ok = false;
        for (size_t i = 0; i < 30; i++) if (out[i]) ok = false;
        CHECK(ok, "K=%d impulse response", c->k);
    }
    // tail biting: a word that wraps round gives the same output as its rotation
    std::mt19937 rng(5);
    std::vector<uint8_t> w(64), r(64), o1(192), o2(192);
    for (auto& x : w) x = rng() & 1;
    for (int i = 0; i < 64; i++) r[(size_t)i] = w[(size_t)((i + 7) % 64)];
    convEncode(kCodeFm, w.data(), 64, o1.data());
    convEncode(kCodeFm, r.data(), 64, o2.data());
    bool rot = true;
    for (int i = 0; i < 192; i++) if (o2[(size_t)i] != o1[(size_t)((i + 21) % 192)]) rot = false;
    CHECK(rot, "tail-biting encoder");
    // Viterbi on noisy, punctured words: FM rate 2/5 (K=7), AM rate 5/12 (E1) and 2/3 (E2)
    struct Case { const ConvCode* c; const uint8_t* pat; int plen; int n; float snr; };
    const Case cases[] = {{&kCodeFm, kPunct25, 6, 3000, 3.5f}, {&kCodeFm, kPunct25, 6, 80, 4.f}, {&kCodeE1, kPunctE1, 15, 3750, 3.5f}, {&kCodeE2, kPunctE2, 6, 2400, 7.f}};
    for (const Case& cs : cases) {
        std::vector<uint8_t> d((size_t)cs.n), e((size_t)cs.n * 3), dec((size_t)cs.n);
        for (auto& x : d) x = rng() & 1;
        convEncode(*cs.c, d.data(), d.size(), e.data());
        std::normal_distribution<float> nd(0.f, std::pow(10.f, -cs.snr / 20.f));
        std::vector<float> soft(e.size());
        for (size_t i = 0; i < e.size(); i++) soft[i] = cs.pat[i % (size_t)cs.plen] ? (e[i] ? 1.f : -1.f) + nd(rng) : 0.f;
        Viterbi v;
        v.decode(*cs.c, soft.data(), d.size(), dec.data());
        int errs = 0;
        for (size_t i = 0; i < d.size(); i++) errs += d[i] != dec[i];
        int cnt = 0;
        const int ch = reencodeErrors(*cs.c, soft.data(), dec.data(), d.size(), &cnt);
        CHECK(errs == 0, "K=%d n=%d at %.1f dB: %d bit errors after decoding (channel %d of %d)", cs.c->k, cs.n, cs.snr, errs, ch, cnt);
    }
}

static void testRs() {
    std::mt19937 rng(9);
    for (int t = 0; t < 200; t++) {
        uint8_t w[96];
        for (int i = 8; i < 96; i++) w[i] = (uint8_t)rng();
        rsEncodeHeader(w);
        uint8_t c[96];
        memcpy(c, w, 96);
        const int ne = t % 5;
        for (int k = 0; k < ne; k++) c[rng() % 96] ^= (uint8_t)(1 + rng() % 255);
        int fixed = 0;
        const bool ok = rsDecodeHeader(c, &fixed);
        CHECK(ok && !memcmp(c, w, 96), "RS: %d errors not corrected", ne);
    }
    int caught = 0;
    for (int t = 0; t < 100; t++) {
        uint8_t w[96];
        for (int i = 8; i < 96; i++) w[i] = (uint8_t)rng();
        rsEncodeHeader(w);
        uint8_t c[96];
        memcpy(c, w, 96);
        for (int k = 0; k < 6; k++) c[(k * 17 + t) % 96] ^= (uint8_t)(1 + k);
        if (!rsDecodeHeader(c, nullptr) || memcmp(c, w, 96)) caught++;
    }
    CHECK(caught == 100, "RS: %d of 100 words with 6 errors came out wrong without notice", 100 - caught);
}

static void testInterleavers() {
    // FM MP1: the P1 and PIDS bits fill the 16 x 32 x 720 primary main matrix exactly once
    std::vector<int> use((size_t)16 * kPmBlock, 0);
    for (int p : fmP1Pos()) use[(size_t)p]++;
    for (int b = 0; b < 16; b++) for (int p : fmPidsPos()) use[(size_t)b * kPmBlock + (size_t)p]++;
    int bad = 0;
    for (int u : use) bad += u != 1;
    CHECK(bad == 0, "FM matrix: %d positions not used exactly once", bad);
    // AM MA1: every bit of every word once, the training symbols on the rows of nrsc5 and of 1012s figure 10-2
    const AmMaps& M = amMaps();
    auto cover = [&](const std::vector<const std::vector<AmBit>*>& lists, int bitsPer, const char* what) {
        std::vector<int> u((size_t)8 * 32 * kAmCols * (size_t)bitsPer, 0);
        for (const auto* l : lists) for (const AmBit& b : *l) u[(size_t)b.elem * (size_t)bitsPer + (size_t)b.bit]++;
        int miss = 0, dbl = 0;
        for (size_t e = 0; e < (size_t)8 * 32 * kAmCols; e++) {
            const int col = (int)(e % kAmCols), row = (int)((e / kAmCols) % 32);
            const bool train = row == M.trainRow[col][0] || row == M.trainRow[col][1];
            for (int b = 0; b < bitsPer; b++) {
                const int v = u[e * (size_t)bitsPer + (size_t)b];
                if (train ? v != 0 : v != 1) { if (v == 0) miss++; else dbl++; }
            }
        }
        CHECK(miss == 0 && dbl == 0, "AM %s: %d missing, %d doubled", what, miss, dbl);
    };
    cover({&M.bl, &M.ml}, 6, "PL");
    cover({&M.bu, &M.mu}, 6, "PU");
    cover({&M.eu}, 4, "S");
    cover({&M.el}, 2, "T");
    bool tr = true;
    for (int c = 0; c < kAmCols; c++) {
        const int a = (5 + 11 * c) % 32, b = (21 + 11 * c) % 32;
        if (M.trainRow[c].size() != 2 || !((M.trainRow[c][0] == a && M.trainRow[c][1] == b) || (M.trainRow[c][0] == b && M.trainRow[c][1] == a))) tr = false;
    }
    CHECK(tr, "AM training rows");
    // 1012s figure 10-2, row 28: the k of every column (-1: training)
    const int row28[25] = {225, 189, 128, 92, 31, -1, 709, 673, 612, 551, 515, 454, 418, 357, 321, 260, 224, 163, 102, 66, 5, -1, 733, 697, 636};
    bool fig = true;
    for (int c = 0; c < 25; c++) {
        int found = -1;
        for (int k = 0; k < 750; k++) {
            const int col = (9 * k) % 25, row = (11 * col + 16 * (k / 25) + 11 * (k / 50)) % 32;
            if (col == c && row == 28) found = k;
        }
        if (found != row28[c]) fig = false;
    }
    CHECK(fig, "AM block layout differs from 1012s figure 10-2");
    // the PIDS rows avoid the training rows 8 and 24, and every (row, bit) is used once per column
    int pu[32][4] = {}, pl[32][4] = {};
    for (int n = 0; n < 120; n++) { pl[M.pidsRowL[(size_t)n]][M.pidsBitL[(size_t)n]]++; pu[M.pidsRowU[(size_t)n]][M.pidsBitU[(size_t)n]]++; }
    int pbad = 0;
    for (int r = 0; r < 32; r++) for (int b = 0; b < 4; b++) { const int want = (r == 8 || r == 24) ? 0 : 1; pbad += (pl[r][b] != want) + (pu[r][b] != want); }
    CHECK(pbad == 0, "AM PIDS matrix: %d wrong entries", pbad);
}

// MP3's convolutional interleaver: the transmit side's placement read back by the receiver's interleaver (nrsc5 interleaver_iv), which may
// start at any pair of blocks
static void testPx() {
    std::mt19937 rng(11);
    const int L = 2 * kP3LenMp3;
    for (int startCall : {0, 5}) {
        PxInterleaver tx;
        tx.reset(kP3LenMp3);
        PxDeinterleaver rx;
        rx.reset(kP3LenMp3);
        std::vector<std::vector<uint8_t>> frames;
        std::vector<uint8_t> call((size_t)L);
        std::vector<float> in((size_t)L), out;
        int checked = 0, wrong = 0;
        for (int c = 0; c < 40; c++) {
            while (tx.wantsFrame()) {
                std::vector<uint8_t> f((size_t)L);
                for (auto& x : f) x = rng() & 1;
                tx.addFrame(f.data());
                frames.push_back(f);
            }
            tx.call(call.data());
            if (c < startCall) continue;
            for (int i = 0; i < L; i++) in[(size_t)i] = call[(size_t)i] ? 1.f : -1.f;
            if (!rx.push(in.data(), out)) continue;
            size_t k = 0;
            for (size_t i = 0; i < out.size(); i++) {
                if (!kPunctP3[i % 6]) { if (out[i] != 0.f) wrong++; continue; }
                if ((out[i] > 0) != (frames[(size_t)c][k] != 0)) wrong++;
                k++;
            }
            checked++;
        }
        CHECK(checked >= 8 && wrong == 0, "P3 interleaver from call %d: %d code words checked, %d bits wrong", startCall, checked, wrong);
    }
}

static void testL2() {
    // SIS through the decoder
    const HdrTestContent& tc = hdrTestContent();
    SisConfig sc;
    sc.callSign = tc.callSign; sc.name = tc.name; sc.longName = tc.longName; sc.slogan = tc.slogan; sc.message = tc.message;
    sc.country = tc.country; sc.facilityId = tc.facilityId; sc.location = true; sc.lat = tc.lat; sc.lon = tc.lon; sc.altM = tc.altM;
    sc.audio.push_back({0, 5, 0, 0});
    sc.audio.push_back({1, 14, 0, 0});
    SisEncoder enc(sc);
    L2Decoder dec;
    for (int i = 0; i < 200; i++) {
        uint8_t f[80], l1[80];
        enc.next(f);
        for (int j = 0; j < 80; j++) l1[((j >> 3) << 3) + 7 - (j & 7)] = f[j];
        dec.pushPids(l1);
    }
    HdrTelemetry t;
    dec.fill(t);
    CHECK(t.callSign == tc.callSign && t.stationName == tc.name && t.longName == tc.longName && t.slogan == tc.slogan && t.message == tc.message,
          "SIS: '%s' '%s' '%s' '%s' '%s'", t.callSign.c_str(), t.stationName.c_str(), t.longName.c_str(), t.slogan.c_str(), t.message.c_str());
    CHECK(t.countryCode == "US" && t.facilityId == tc.facilityId && t.haveLocation && std::fabs(t.latitude - tc.lat) < 1e-3 && std::fabs(t.longitude - tc.lon) < 1e-3 && t.altitudeM == tc.altM,
          "SIS station id / location");
    CHECK(t.programs.size() == 2 && t.programs[0].type == 5 && t.programs[1].type == 14, "SIS programs");
    CHECK(t.pidsOk == 200 && t.pidsBad == 0, "PIDS frames %llu ok %llu bad", (unsigned long long)t.pidsOk, (unsigned long long)t.pidsBad);
    // a P1 transfer frame: two programs with PSD, the guide and a picture in the fixed data
    std::vector<uint8_t> psd0 = {0x7E}, psd1 = {0x7E};
    hdlcAppend(psd0, aasFrame(0x5100, 0, id3Tag("Title A", "Artist A", "Album A", "Rock", 7, kMimePrimaryImage)));
    hdlcAppend(psd1, aasFrame(0x5201, 0, id3Tag("Title B", "Artist B", "", "", -1, 0)));
    std::vector<SigService> sv(1);
    sv[0].number = 1; sv[0].name = "Main";
    sv[0].comps.push_back({true, 0, 0, 0, kMimeHdc, -1});
    sv[0].comps.push_back({false, 1, 0x1234, 3, kMimePrimaryImage, 0});
    LotFile lf{7, "x.png", kMimePng, hdrTestLogoPng()};
    for (int frame = 0; frame < 4; frame++) {
        // the decoder needs the fixed data width twice before it reads the subchannel: every frame carries the guide and the picture
        std::vector<uint8_t> hdlc = {0x7E}, sub;
        hdlcAppend(hdlc, aasFrame(0x20, 0, sigTable(sv)));
        for (const auto& fr : lotFragments(lf, 0)) hdlcAppend(hdlc, aasFrame(0x1234, 0, fr));
        while (hdlc.size()) fixedBlock(hdlc, sub);
        AudioPduSpec a;
        a.codecMode = 0; a.program = 0; a.progType = 5; a.packetBytes.assign(32, 100); a.psdRoom = 256 - audioPduHeaderBytes(a);
        AudioPduSpec b = a;
        b.program = 1; b.progType = 14; b.packetBytes.assign(32, 40); b.psdRoom = 256 - audioPduHeaderBytes(b);
        std::vector<uint8_t> pdu = audioPdu(a, psd0);
        const std::vector<uint8_t> p2 = audioPdu(b, psd1);
        pdu.insert(pdu.end(), p2.begin(), p2.end());
        FixedSpec fs;
        fs.subLen = 7 * 259;
        std::vector<uint8_t> tail;
        fixedRegion(fs, sub, tail);
        pdu.resize((size_t)pduBytes(kP1LenFm) - tail.size(), 0);
        pdu.insert(pdu.end(), tail.begin(), tail.end());
        std::vector<uint8_t> bits((size_t)kP1LenFm);
        packTransfer(pdu, kPciAudioFixed, kP1LenFm, bits.data());
        dec.pushTransfer(bits.data(), kP1LenFm, 0);
    }
    dec.fill(t);
    CHECK(dec.pciOk() == 4 && dec.pciBad() == 0, "PCI %llu ok %llu bad", (unsigned long long)dec.pciOk(), (unsigned long long)dec.pciBad());
    const HdrProgram* p0 = nullptr;
    const HdrProgram* p1 = nullptr;
    for (const auto& p : t.programs) { if (p.number == 0) p0 = &p; if (p.number == 1) p1 = &p; }
    CHECK(p0 && p0->title == "Title A" && p0->artist == "Artist A" && p0->album == "Album A" && p0->genre == "Rock" && p0->xhdrLot == 7, "HD1 PSD");
    CHECK(p1 && p1->title == "Title B" && p1->artist == "Artist B", "HD2 PSD");
    CHECK(p0 && p0->packetsOk == 128 && p0->packetsBad == 0 && p0->codecMode == 0 && p0->name == "Main" && p0->artPort == 0x1234, "HD1 audio packets %llu / %llu",
          p0 ? (unsigned long long)p0->packetsOk : 0ull, p0 ? (unsigned long long)p0->packetsBad : 0ull);
    std::vector<uint8_t> img;
    CHECK(dec.lotBytes(0x1234, 7, img) && img == hdrTestLogoPng(), "LOT object (%zu bytes)", img.size());
    printf("hdr fec: %s\n", fails ? "FAILED" : "ok");
}

int main() {
    testScrambler();
    testConv();
    testRs();
    testInterleavers();
    testPx();
    testL2();
    return fails ? 1 : 0;
}
