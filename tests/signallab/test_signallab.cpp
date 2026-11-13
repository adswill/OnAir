// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab tests: every stage alone (dsp, channel, classify, rate, recover, line codes, scramblers, checksums, frames, fields,
// exports) and the pipeline end to end on generated signals. "quick" is the ctest signallab; "eval N [seed]" runs the evaluation
// harness on N random signals and prints how often each stage recovers the truth; "gate" is the ctest signallab_eval (a fixed
// set with minimum success rates).
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace dect2::signallab;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Bits randBits(std::mt19937& r, size_t n) { Bits b(n); for (auto& v : b) v = (uint8_t)(r() & 1); return b; }

static void testDsp() {
    std::vector<std::complex<double>> a(64);
    for (size_t i = 0; i < 64; i++) a[i] = std::polar(1.0, 2 * M_PI * 5 * (double)i / 64);
    fft(a);
    size_t pk = 0;
    for (size_t i = 0; i < 64; i++) if (std::abs(a[i]) > std::abs(a[pk])) pk = i;
    CHECK(pk == 5, "fft peak %zu", pk);
    fft(a, true);
    CHECK(std::fabs(a[3].real() - std::cos(2 * M_PI * 15 / 64)) < 1e-9, "inverse fft");
    // a tone 30 kHz off in a 250 kHz capture, channelised to 10 kHz: lands at DC
    std::vector<cf> x(50000);
    for (size_t i = 0; i < x.size(); i++) x[i] = std::polar(1.f, (float)(2 * M_PI * 30000.0 * (double)i / 250000));
    double fo = 0;
    const auto y = channelise(x, 250000, 30000, 10000, fo);
    CHECK(fo >= 40000 && fo <= 62500, "channelised rate %f", fo);
    double ph = 0;
    for (size_t i = 200; i + 1 < y.size(); i++) ph += std::arg(y[i + 1] * std::conj(y[i]));
    CHECK(std::fabs(ph / (double)(y.size() - 201)) < 1e-3, "residual rotation %f", ph);
    const auto P = psd(x.data(), x.size(), 1024);
    size_t pp = 0;
    for (size_t i = 0; i < P.size(); i++) if (P[i] > P[pp]) pp = i;
    CHECK(std::abs((long)pp - (512 + 123)) <= 1, "psd peak %zu", pp);
}

static GenSpec spec(Mod m, int order, double fs, double baud) {
    GenSpec g;
    g.mod = m; g.order = order; g.fs = fs; g.baud = baud; g.snrDb = 25; g.offsetHz = 0;
    g.h = m == Mod::FSK ? (order == 2 ? 1.0 : 0.8) : 0.5;
    g.sync = stringToBits("0010110111010100");
    g.crcAlgo = 6;
    g.frames = 8; g.payloadBytes = 10; g.preambleBits = 32; g.gapSec = 0.01;
    return g;
}

static void testChannel() {
    GenSpec g = spec(Mod::FSK, 2, 250000, 9600);
    g.offsetHz = 40000;
    const auto x = generate(g);
    const ChannelInfo c = measureChannel(x, g.fs);
    CHECK(c.bursts.size() == 8, "bursts %zu", c.bursts.size());
    CHECK(std::fabs(c.offsetHz - 40000) < 3000, "offset %f", c.offsetHz);
    CHECK(c.bw99Hz > 9600 && c.bw99Hz < 40000, "bw99 %f", c.bw99Hz);
    CHECK(c.dutyCycle > 0.3 && c.dutyCycle < 0.95, "duty %f", c.dutyCycle);
    CHECK(c.snrDb > 10, "snr %f", c.snrDb);
    CHECK(!c.continuous, "not continuous");
    g.gapSec = 0;
    const ChannelInfo c2 = measureChannel(generate(g), g.fs);
    CHECK(c2.continuous, "continuous: %zu bursts duty %f", c2.bursts.size(), c2.dutyCycle);
}

static ModResult classifyOf(const std::vector<cf>& x0, double fs, double* fsOut = nullptr) {
    const ChannelInfo c0 = measureChannel(x0, fs);
    double f2 = fs;
    const auto x = channelise(x0, fs, c0.offsetHz, c0.bw99Hz * 1.25, f2);
    const ChannelInfo c = measureChannel(x, f2);
    if (fsOut) *fsOut = f2;
    return classify(x, f2, c.bw99Hz, &c.bursts);
}

static std::vector<cf> ofdm(double fs, size_t symbols, std::mt19937& r) {
    const size_t N = 256, cp = 32;
    std::vector<cf> out;
    std::normal_distribution<float> nd(0, 0.05f);
    for (size_t s = 0; s < symbols; s++) {
        std::vector<std::complex<double>> b(N, 0.0);
        for (size_t k = 8; k < N - 8; k++) if (k < N / 2 - 1 || k > N / 2 + 1) b[(k + N / 2) % N] = std::polar(1.0, M_PI / 4 + M_PI / 2 * (double)(r() % 4));
        fft(b, true);
        for (size_t i = N - cp; i < N; i++) out.push_back(cf((float)b[i].real(), (float)b[i].imag()) * 16.f);
        for (size_t i = 0; i < N; i++) out.push_back(cf((float)b[i].real(), (float)b[i].imag()) * 16.f);
    }
    for (auto& v : out) v += cf(nd(r), nd(r));
    (void)fs;
    return out;
}

static void testClassify() {
    struct C { Mod m; int order; Mod want; };
    const C cs[] = {{Mod::FSK, 2, Mod::FSK}, {Mod::FSK, 4, Mod::FSK}, {Mod::OOK, 2, Mod::OOK}, {Mod::PSK, 2, Mod::PSK}, {Mod::PSK, 4, Mod::PSK}, {Mod::PSK, 8, Mod::PSK}};
    for (const C& c : cs) {
        GenSpec g = spec(c.m, c.order, 250000, 12500);
        g.offsetHz = 20000;
        const ModResult r = classifyOf(generate(g), g.fs);
        const ModGuess b = r.best();
        CHECK(b.mod == c.want && (c.m == Mod::OOK || b.order == c.order), "%s-%d classified as %s-%d (%.2f) envCv %.2f lines %.0f/%.0f/%.0f/%.0f clusters %d sep %.1f", modName(c.m), c.order,
              modName(b.mod), b.order, b.confidence, r.f.envCv, r.f.line[1], r.f.line[2], r.f.line[4], r.f.line[8], r.f.freqClusters, r.f.freqSep);
        CHECK(b.confidence > 0.3, "%s-%d confidence %.2f", modName(c.m), c.order, b.confidence);
    }
    {
        GenSpec g = spec(Mod::FSK, 2, 250000, 10000);
        g.h = 1.0;
        const ModResult r = classifyOf(generate(g), g.fs);
        CHECK(std::fabs(r.toneSpacingHz - 10000) < 2500, "FSK tone spacing %.0f", r.toneSpacingHz);
    }
    std::mt19937 rng(3);
    const auto o = ofdm(1e6, 300, rng);
    const ModResult r = classify(o, 1e6, 0.9e6);
    CHECK(r.best().mod == Mod::OFDM, "OFDM classified as %s (kurt %.2f cp %.2f at %zu)", modName(r.best().mod), r.f.envKurt, r.f.cpCorr, r.f.cpLag);
    CHECK(r.f.cpLag == 256, "cp lag %zu", r.f.cpLag);
}

static void testRate() {
    for (Mod m : {Mod::FSK, Mod::OOK}) {
        GenSpec g = spec(m, 2, 250000, 9600);
        const auto x0 = generate(g);
        const ChannelInfo c0 = measureChannel(x0, g.fs);
        double fs = g.fs;
        const auto x = channelise(x0, g.fs, c0.offsetHz, c0.bw99Hz * 1.25, fs);
        const ChannelInfo c = measureChannel(x, fs);
        const int L = std::max(1, (int)std::lround(0.5 * fs / c.bw99Hz));
        std::vector<std::vector<float>> ss;
        for (const Burst& b : c.bursts) {
            const auto v = symbolSignal(std::vector<cf>(x.begin() + (long)b.start, x.begin() + (long)b.end), fs, m);
            std::vector<float> sm(v.size());
            double acc = 0;
            for (size_t i = 0; i < v.size(); i++) { acc += v[i]; if (i >= (size_t)L) acc -= v[i - L]; sm[i] = (float)(acc / std::min<double>((double)i + 1, L)); }
            ss.push_back(sm);
        }
        const RateEstimate z = rateZeroCrossing(ss, fs), s = rateSpectralLine(ss, fs), a = rateAutocorr(ss, fs);
        CHECK(std::fabs(z.baud / 9600 - 1) < 0.003, "%s zero crossing %.1f", modName(m), z.baud);
        CHECK(std::fabs(s.baud / 9600 - 1) < 0.01, "%s spectral line %.1f", modName(m), s.baud);
        CHECK(std::fabs(a.baud / 9600 - 1) < 0.02, "%s autocorrelation %.1f", modName(m), a.baud);
        const RateResult rr = combineRates({z, s, a});
        CHECK(std::fabs(rr.baud / 9600 - 1) < 0.003 && rr.confidence > 0.5, "%s combined %.1f conf %.2f", modName(m), rr.baud, rr.confidence);
    }
    GenSpec g = spec(Mod::PSK, 4, 250000, 20000);
    const auto x = generate(g);
    const ChannelInfo c = measureChannel(x, g.fs);
    std::vector<std::vector<cf>> cs;
    for (const Burst& b : c.bursts) cs.push_back(std::vector<cf>(x.begin() + (long)b.start, x.begin() + (long)b.end));
    const RateEstimate cy = rateCyclo(cs, g.fs);
    CHECK(std::fabs(cy.baud / 20000 - 1) < 0.01, "QPSK cyclostationary %.1f", cy.baud);
}

static void testRecover() {
    // BPSK and QPSK with a carrier offset: the loops lock and the EVM is small
    for (int order : {2, 4}) {
        GenSpec g = spec(Mod::PSK, order, 200000, 25000);
        g.offsetHz = 300; g.gapSec = 0; g.snrDb = 30;
        const auto x = generate(g);
        const Symbols s = recoverSymbols(x, g.fs, 25000, Mod::PSK, order);
        double e = 0; size_t n = 0;
        for (size_t i = 300; i < s.points.size(); i++) {
            const float a = std::arg(s.points[i]) / (float)(2 * M_PI / order) - (order == 4 ? 0.5f : 0.f);
            e += std::fabs(a - std::round(a)); n++;
        }
        CHECK(n > 300 && e / (double)n < 0.08, "PSK-%d phase error %.3f over %zu", order, n ? e / (double)n : 1.0, n);
        CHECK(s.evm < 0.35, "PSK-%d evm %.2f", order, s.evm);
    }
    // FSK: two well separated levels
    GenSpec g = spec(Mod::FSK, 2, 100000, 5000);
    g.gapSec = 0;
    const Symbols s = recoverSymbols(generate(g), g.fs, 5000, Mod::FSK, 2);
    CHECK(s.levels.size() == 2 && s.levels[1] - s.levels[0] > 3000, "FSK levels %.0f %.0f", s.levels[0], s.levels.size() > 1 ? s.levels[1] : 0);
    CHECK(s.evm < 0.4, "FSK evm %.2f", s.evm);
    const std::vector<float> km = kmeans1d({1, 1.1f, 0.9f, 5, 5.2f, 4.8f, 9, 9.1f}, 3);
    CHECK(std::fabs(km[0] - 1) < 0.01 && std::fabs(km[1] - 5) < 0.01 && std::fabs(km[2] - 9.05f) < 0.01, "kmeans %.2f %.2f %.2f", km[0], km[1], km[2]);
}

static void testLineCodes() {
    std::mt19937 r(5);
    const Bits d = randBits(r, 400);
    for (LineCode c : {LineCode::NRZ, LineCode::NRZI, LineCode::Manchester, LineCode::ManchesterInv, LineCode::DiffManchester, LineCode::Miller, LineCode::Differential}) {
        const Bits e = encodeLine(d, c);
        double v = 1;
        Bits back = decodeLine(e, c, 0, &v);
        // the differential codes lose (or guess) the first bit
        const size_t skip = (c == LineCode::NRZ || c == LineCode::Manchester || c == LineCode::ManchesterInv || c == LineCode::Miller) ? 0 : 1;
        size_t bad = 0;
        if (c == LineCode::NRZI || c == LineCode::Differential) { for (size_t i = 0; i < d.size() && i < back.size(); i++) bad += back[i] != d[i]; }
        else for (size_t i = skip; i < d.size() && i < back.size(); i++) bad += back[i] != d[i];
        CHECK(bad == 0 && back.size() >= d.size() - 1, "%s round trip: %zu errors, %zu bits", lineCodeName(c), bad, back.size());
        CHECK(v == 0, "%s violations %.3f", lineCodeName(c), v);
    }
    // Manchester on the wrong chip phase shows violations
    double v = 0;
    decodeLine(encodeLine(d, LineCode::Manchester), LineCode::Manchester, 1, &v);
    CHECK(v > 0.2, "Manchester wrong phase violations %.2f", v);
    for (const Scrambler& s : scramblers()) {
        const Bits sc = scramble(d, s);
        const Bits ds = descramble(sc, s);
        CHECK(ds == d, "%s round trip", s.name);
        if (!s.additive) {   // self-synchronising: a receiver starting late is right after degree bits
            const Bits late(sc.begin() + 50, sc.end());
            const Bits dl = descramble(late, s);
            size_t bad = 0;
            for (size_t i = (size_t)s.degree; i < dl.size(); i++) bad += dl[i] != d[50 + i];
            CHECK(bad == 0, "%s self-sync %zu errors", s.name, bad);
        }
    }
    // ranking: Manchester-coded frames come out as Manchester
    std::vector<Bits> st;
    const Bits sync = stringToBits("1110010110010011");
    for (int i = 0; i < 8; i++) {
        Bits f;
        for (int k = 0; k < 16; k++) f.push_back(0);
        f.insert(f.end(), sync.begin(), sync.end());
        const Bits p = randBits(r, 96);
        f.insert(f.end(), p.begin(), p.end());
        st.push_back(encodeLine(f, LineCode::Manchester));
    }
    const auto rk = rankLineCodes(st);
    CHECK(!rk.empty() && rk[0].code == LineCode::Manchester, "ranked %s first", rk.empty() ? "-" : lineCodeName(rk[0].code));
    CHECK(structureScore(Bits(512, 0)) > structureScore(randBits(r, 512)), "structure score");
}

static void testChecksums() {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    for (const CheckAlgo& a : checkAlgos()) {
        const uint32_t v = checksum(a, s, 9);
        CHECK(v == a.check, "%s check value %08X want %08X", a.name, v, a.check);
    }
    std::mt19937 r(9);
    const Bits sync = stringToBits("0010110111010100");
    for (int variant = 0; variant < 4; variant++) {
        const int algo = variant == 3 ? 18 : 6 + variant * 3;   // CCITT-FALSE, X-25, MODBUS, CRC-32
        const bool lsb = variant == 1, inv = variant == 2;
        std::vector<Bits> frames;
        for (int i = 0; i < 8; i++) {
            std::vector<uint8_t> by(12);
            for (auto& b : by) b = (uint8_t)r();
            const CheckAlgo& a = checkAlgos()[(size_t)algo];
            const uint32_t v = checksum(a, by.data() + 2, by.size() - 2);   // coverage starts after 2 bytes
            for (int k = 0; k < a.width / 8; k++) by.push_back(a.refout ? (uint8_t)(v >> (8 * k)) : (uint8_t)(v >> (a.width - 8 - 8 * k)));
            Bits f;
            for (uint8_t b : by) for (int k = 0; k < 8; k++) f.push_back(lsb ? (uint8_t)((b >> k) & 1) : (uint8_t)((b >> (7 - k)) & 1));
            if (inv) f = invert(f);
            frames.push_back(f);
        }
        const auto h = findChecksums(frames, sync);
        // X-25 (reflected) on LSB-first bytes is bit for bit GENIBUS (plain) on MSB-first bytes: either answer is right
        const bool twin = variant == 1 && !h.empty() && h[0].algo == 10 && !h[0].lsbFirst && h[0].coverStart == 16 && h[0].coverEnd == 96;
        CHECK(twin || (!h.empty() && h[0].algo == algo && h[0].coverStart == 16 && h[0].coverEnd == 96 && h[0].lsbFirst == lsb && h[0].inverted == inv),
              "variant %d: %s start %zu end %zu lsb %d inv %d", variant, h.empty() ? "none" : checkAlgos()[(size_t)h[0].algo].name, h.empty() ? 0 : h[0].coverStart,
              h.empty() ? 0 : h[0].coverEnd, h.empty() ? 0 : h[0].lsbFirst, h.empty() ? 0 : h[0].inverted);
    }
    // random frames: no checksum
    std::vector<Bits> rnd;
    for (int i = 0; i < 10; i++) rnd.push_back(randBits(r, 120));
    CHECK(findChecksums(rnd, sync).empty(), "false checksum on random frames");
    const auto pb = packBytes(stringToBits("1000000000000001"), 0, 16, true);
    CHECK(pb.size() == 2 && pb[0] == 1 && pb[1] == 0x80, "packBytes lsb");
}

static void testFrames() {
    std::mt19937 r(11);
    const Bits sync = stringToBits("0010110111010100");
    std::vector<Bits> st;
    for (int i = 0; i < 10; i++) {
        Bits f;
        for (int k = 0; k < 24 + (i % 3) * 2; k++) f.push_back((uint8_t)((k & 1) ^ 1));
        f.insert(f.end(), sync.begin(), sync.end());
        const Bits p = randBits(r, 80);
        f.insert(f.end(), p.begin(), p.end());
        for (int k = 0; k < (int)(r() % 4); k++) f.push_back((uint8_t)(r() & 1));   // ramp-down garbage
        st.push_back(f);
    }
    const FrameInfo fi = findFrames(st);
    CHECK(fi.sync.pattern.size() >= 12 && std::equal(fi.sync.pattern.begin(), fi.sync.pattern.end(), sync.end() - (long)fi.sync.pattern.size()),
          "sync %s", bitsToString(fi.sync.pattern).c_str());
    CHECK(fi.frameBits == 80, "frame bits %zu", fi.frameBits);
    CHECK(fi.frames.size() == 10, "frames %zu", fi.frames.size());
    CHECK(fi.preambleBits >= 24, "preamble %zu", fi.preambleBits);
    // continuous: the same sync every 120 bits
    Bits cont;
    for (int i = 0; i < 20; i++) { cont.insert(cont.end(), sync.begin(), sync.end()); const Bits p = randBits(r, 104); cont.insert(cont.end(), p.begin(), p.end()); }
    const FrameInfo fc = findFrames({cont});
    CHECK(fc.frameBits == 104 && fc.frames.size() >= 18, "continuous frames %zu of %zu bits, sync %s", fc.frames.size(), fc.frameBits, bitsToString(fc.sync.pattern).c_str());
    CHECK(!fc.autocorrPeaks.empty() && fc.autocorrPeaks[0] % 120 == 0, "autocorr peak %zu", fc.autocorrPeaks.empty() ? 0 : fc.autocorrPeaks[0]);
    CHECK(findPattern(stringToBits("0001011000"), stringToBits("1011"), 0).size() == 1, "findPattern");
    CHECK(preambleLength(stringToBits("1010101100"), 0) == 7, "preamble length");
}

static void testFields() {
    std::mt19937 r(13);
    std::vector<Bits> fr;
    for (int i = 0; i < 12; i++) {
        Bits f;
        auto put = [&](uint32_t v, int n) { for (int k = n - 1; k >= 0; k--) f.push_back((uint8_t)((v >> k) & 1)); };
        put(0xA5, 8);                 // constant
        put((uint32_t)(40 + i), 8);   // counter
        const char* txt = i % 2 ? "HELLO" : "WORLD";
        for (int k = 0; k < 5; k++) put((uint32_t)txt[k], 8);
        put(r() & 3, 8);              // enum
        put(r(), 32);                 // value
        fr.push_back(f);
    }
    const auto cs = columnStats(fr);
    CHECK(cs.size() == 96 && cs[0].entropy == 0 && cs[15].change > 0.9, "column stats %zu", cs.size());
    const auto fl = suggestFields(fr, {});
    auto typeAt = [&](size_t bit) { for (const Field& f : fl) if (bit >= f.start && bit < f.start + f.length) return f.type; return FieldType::Bytes; };
    CHECK(typeAt(0) == FieldType::Constant, "constant field");
    CHECK(typeAt(8) == FieldType::Counter, "counter field %s", fieldTypeName(typeAt(8)));
    CHECK(typeAt(24) == FieldType::Ascii, "ascii field %s", fieldTypeName(typeAt(24)));
    CHECK(typeAt(60) == FieldType::Enum || typeAt(60) == FieldType::Constant, "enum field %s", fieldTypeName(typeAt(60)));
    CHECK(interpretHex(fr[0], 0, 16) == "A5 28", "hex %s", interpretHex(fr[0], 0, 16).c_str());
    CHECK(interpretAscii(fr[0], 16, 40) == "WORLD", "ascii %s", interpretAscii(fr[0], 16, 40).c_str());
    CHECK(interpretDecimal(fr[0], 8, 8) == "40", "decimal");
    CHECK(interpretDecimal(stringToBits("1111"), 0, 4, false, true) == "-1", "signed");
    CHECK(interpretBits(stringToBits("10100011"), 0, 8, 4) == "1010 0011", "bits");
    CHECK(bitsValue(stringToBits("1000"), 0, 4, true) == 1, "lsb value");
    const Bits df = diffFrames(fr[0], fr[1]);
    CHECK(df.size() == 96 && df[0] == 0 && df[15] == 1, "diff");
}

static void testExport() {
    GenSpec g = spec(Mod::FSK, 2, 250000, 9600);
    Analysis a = analyse(generate(g), g.fs);
    a.notes = "line 1\n\"quoted\"";
    a.name = "test signal";
    Json j;
    std::string err;
    CHECK(parseJson(toModeJson(a), j, err), "mode json parses: %s", err.c_str());
    CHECK(j["format"].str() == "onair-signal-description" && j["version"].num() == 1, "format");
    CHECK(std::fabs(j["modulation"]["symbol_rate"].num() - 9600) < 50, "symbol rate %f", j["modulation"]["symbol_rate"].num());
    CHECK(j["sync_word"]["binary"].str().size() >= 8, "sync word");
    CHECK(j["checksum"]["algorithm"].str() == checkAlgos()[6].name, "checksum %s", j["checksum"]["algorithm"].str().c_str());
    CHECK(j["fields"].a.size() >= 2, "fields %zu", j["fields"].a.size());
    Analysis b;
    CHECK(loadAnalysisJson(saveAnalysisJson(a), b, err), "load: %s", err.c_str());
    CHECK(b.notes == a.notes && b.name == a.name, "notes round trip");
    CHECK(b.frame.frames.size() == a.frame.frames.size() && b.frame.frames == a.frame.frames, "frames round trip");
    CHECK(b.frame.sync.pattern == a.frame.sync.pattern && b.bits.size() == a.bits.size(), "sync / streams round trip");
    CHECK(!b.checks.empty(), "checksum found again after loading");
    const std::string csv = toCsv(a);
    CHECK(std::count(csv.begin(), csv.end(), '\n') == (long)a.frame.frames.size() + 1, "csv rows");
    CHECK(!toHexDump(a).empty(), "hex dump");
    CHECK(!parseJson("{\"a\": [1, 2,", j, err), "bad json rejected");
    CHECK(parseJson("{\"a\": \"\\u00e9\", \"b\": -1.5e3, \"c\": [true, null]}", j, err) && j["b"].num() == -1500 && j["a"].str() == "\xc3\xa9", "json values");
}

static void testPipeline(int only = -1) {
    struct C { Mod m; int order; LineCode lc; int scr; double off; double gap; };
    const C cs[] = {{Mod::FSK, 2, LineCode::NRZ, -1, 25000, 0.01}, {Mod::OOK, 2, LineCode::Manchester, -1, -30000, 0.02}, {Mod::PSK, 2, LineCode::NRZ, -1, 10000, 0.01},
                    {Mod::PSK, 4, LineCode::NRZ, -1, 5000, 0.01}, {Mod::FSK, 2, LineCode::NRZI, 0, 0, 0}, {Mod::GMSK, 2, LineCode::NRZ, 5, 15000, 0.01},
                    {Mod::FSK, 4, LineCode::NRZ, -1, -10000, 0.01}};
    for (int ci = 0; ci < (int)(sizeof cs / sizeof cs[0]); ci++) {
        if (only >= 0 && ci != only) continue;
        const C& c = cs[ci];
        GenSpec g = spec(c.m, c.order, 250000, 10000);
        g.line = c.lc; g.scrambler = c.scr; g.offsetHz = c.off; g.gapSec = c.gap;
        if (c.m == Mod::OOK) g.baud = 5000;
        GenTruth t;
        const Analysis a = analyse(generate(g, &t), g.fs);
        size_t found = 0;
        for (const Bits& p : t.payloads) for (const Bits& f : a.frame.frames) if (f.size() >= p.size() && std::equal(p.begin(), p.end(), f.begin())) { found++; break; }
        CHECK(found * 10 >= t.payloads.size() * 8, "%s-%d %s: %zu of %zu frames, used %s-%d %.1f Bd, line %s, sync %s", modName(c.m), c.order, lineCodeName(c.lc), found, t.payloads.size(),
              modName(a.usedMod), a.usedOrder, a.rate.baud, lineCodeName(a.line.code), bitsToString(a.frame.sync.pattern).c_str());
        CHECK(!a.checks.empty() && a.checks[0].algo == g.crcAlgo, "%s-%d: checksum %s", modName(c.m), c.order, a.checks.empty() ? "none" : checkAlgos()[(size_t)a.checks[0].algo].name);
        CHECK(a.frame.frameBits == t.frameBits, "%s-%d: frame bits %zu want %zu", modName(c.m), c.order, a.frame.frameBits, t.frameBits);
        if (only >= 0) for (const auto& l : a.log) printf("    %s\n", l.c_str());
    }
    // known bits only (stages 4-6)
    std::mt19937 r(21);
    std::vector<Bits> st;
    for (int i = 0; i < 6; i++) { Bits f(16, 0); for (size_t k = 0; k < 16; k++) f[k] = (uint8_t)((k & 1) ^ 1); const Bits s = stringToBits("1100101011110000"); f.insert(f.end(), s.begin(), s.end()); const Bits p = randBits(r, 64); f.insert(f.end(), p.begin(), p.end()); st.push_back(f); }
    const Analysis ab = analyseBits(st, 1200);
    CHECK(ab.frame.frames.size() == 6 && ab.frame.frameBits == 64, "analyseBits %zu frames of %zu", ab.frame.frames.size(), ab.frame.frameBits);
    Analysis rf = ab;
    reframe(rf, stringToBits("11110000"), 32);
    CHECK(rf.frame.frames.size() >= 6 && rf.frame.frameBits == 32, "reframe %zu of %zu", rf.frame.frames.size(), rf.frame.frameBits);
}

static int gate() {
    const EvalReport r = evaluate(36, 1000, false);
    printf("%s", r.text.c_str());
    // minimum success rates: what the pipeline reaches on this fixed set with some margin; a drop below is a regression
    // (reached on 2026-10-22: 97 97 92 92 92 94 78 75 83 72 87 100 %)
    const double need[] = {0.85, 0.85, 0.80, 0.80, 0.80, 0.85, 0.60, 0.60, 0.65, 0.55, 0.70, 0.60};
    int bad = 0;
    for (size_t i = 0; i < r.stages.size(); i++) {
        const auto& s = r.stages[i];
        if (s.tested && (double)s.ok / s.tested < need[i]) { printf("FAIL %s %.0f %% < %.0f %%\n", s.name, 100.0 * s.ok / s.tested, need[i] * 100); bad++; }
    }
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "quick";
    if (mode == "eval" || mode == "evaluate") {
        const int n = argc > 2 ? atoi(argv[2]) : 100;
        const uint32_t seed = argc > 3 ? (uint32_t)atoi(argv[3]) : 1;
        printf("%s", evaluate(n, seed, true).text.c_str());
        return 0;
    }
    if (mode == "gate") return gate();
    if (mode == "pipe") { testPipeline(argc > 2 ? atoi(argv[2]) : 0); printf("%d checks, %d failed\n", checks, fails); return fails ? 1 : 0; }
    if (mode == "case") {   // one random signal of the evaluation, with the pipeline's log
        const GenSpec g = randomSpec((uint32_t)(argc > 2 ? atoi(argv[2]) : 1));
        printf("truth: %s-%d %.0f Bd fs %.0f offset %.0f line %s scrambler %d crc %d frames %d gap %.1f ms snr %.1f h %.2f sync %s\n", modName(g.mod), g.order, g.baud, g.fs, g.offsetHz,
               lineCodeName(g.line), g.scrambler, g.crcAlgo, g.frames, g.gapSec * 1000, g.snrDb, g.h, bitsToString(g.sync).c_str());
        const Analysis a = analyse(generate(g), g.fs);
        for (const auto& l : a.log) printf("  %s\n", l.c_str());
        return 0;
    }
    testDsp();
    testChannel();
    testClassify();
    testRate();
    testRecover();
    testLineCodes();
    testChecksums();
    testFrames();
    testFields();
    testExport();
    testPipeline();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
