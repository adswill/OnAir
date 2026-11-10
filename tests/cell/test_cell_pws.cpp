// Cell towers: SIB9, SIB10, SIB11, SIB12 and SIB16, then CB Data text in GSM 7-bit and UCS2, with segmentation in any order, through the UPER coder and
// the full LTE receiver with noise and frequency offset. Simulated signals only: no real cell warning recording exists.
#include "dect2/cell/cell_asn.h"
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "dect2/cell/cell_pws.h"
#include "dect2/common/stable_random.h"
#include "../common/impair.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;
using namespace dect2::cell;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> pack7(const std::string& s) {
    std::vector<uint8_t> o;
    int bits = 0, n = 0; uint32_t acc = 0;
    for (char ch : s) { acc |= (uint32_t)(uint8_t)ch << bits; bits += 7; while (bits >= 8) { o.push_back((uint8_t)acc); acc >>= 8; bits -= 8; } n++; }
    if (bits) o.push_back((uint8_t)acc);
    return o;
}

static std::vector<uint8_t> cbData(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> c;
    int pages = (int)((payload.size() + 81) / 82);
    if (pages < 1) pages = 1;
    c.push_back((uint8_t)pages);
    for (int p = 0; p < pages; p++) {
        size_t off = (size_t)p * 82, len = std::min<size_t>(82, payload.size() > off ? payload.size() - off : 0);
        for (size_t i = 0; i < 82; i++) c.push_back(i < len ? payload[off + i] : 0);
        c.push_back((uint8_t)len);
    }
    return c;
}

static std::vector<LteWarnSeg> segment(int sib, uint32_t id, uint32_t serial, const std::vector<uint8_t>& content, int dcs, size_t seg) {
    std::vector<LteWarnSeg> v;
    for (size_t off = 0, k = 0; off < content.size(); off += seg, k++) {
        LteWarnSeg s; s.sib = sib; s.messageId = id; s.serial = serial; s.segment = (int)k;
        const size_t e = std::min(content.size(), off + seg);
        s.data.assign(content.begin() + (long)off, content.begin() + (long)e);
        s.last = e == content.size();
        if (k == 0) s.dcs = dcs;
        v.push_back(s);
    }
    return v;
}

static void asnRoundTrip() {
    LteWarnSeg a; a.sib = 10; a.messageId = 4353; a.serial = 0x3005; a.warningType = (1u << 9) | 0x0100;
    LteWarnSeg b; b.sib = 11; b.messageId = 4352; b.serial = 0x1234; b.last = false; b.segment = 3; b.data = {1, 2, 3, 4, 5}; b.dcs = 0x0F;
    LteWarnSeg c; c.sib = 12; c.messageId = 4370; c.serial = 0xABCD; c.last = true; c.segment = 7; c.data.assign(200, 0x5A); c.dcs = 0x11;
    LteSib16 t; t.hasTime = true; t.timeUtc = 390000000000ULL; t.dst = 1; t.leapSeconds = 18; t.localOffsetQh = 8;
    const Bits m = lteEncodeSiWarn({a, b, c}, &t, "Home-NB");
    LteBcch o;
    CHECK(lteParseBcch(m, o));
    CHECK(o.has9 && o.hnbName == "Home-NB");
    CHECK(o.warn.size() == 3);
    if (o.warn.size() == 3) {
        CHECK(o.warn[0].sib == 10 && o.warn[0].messageId == 4353 && o.warn[0].serial == 0x3005 && o.warn[0].warningType == a.warningType);
        CHECK(o.warn[1].sib == 11 && !o.warn[1].last && o.warn[1].segment == 3 && o.warn[1].data == b.data && o.warn[1].dcs == 0x0F);
        CHECK(o.warn[2].sib == 12 && o.warn[2].last && o.warn[2].segment == 7 && o.warn[2].data == c.data && o.warn[2].dcs == 0x11);
    }
    CHECK(o.has16 && o.sib16.timeUtc == t.timeUtc && o.sib16.dst == 1 && o.sib16.leapSeconds == 18 && o.sib16.localOffsetQh == 8);
    // a SIB after the marker that is not read must not lose what follows it: SIB14 then SIB12, written by hand
    BitWriter w;
    w.bit(0); w.bit(0); w.bit(0); w.bit(0);
    w.range(2, 1, 32);
    w.bit(1); w.bit(0); w.u(2, 6); w.u(3, 8); w.u(0xAAAAAA, 24);
    LteBcch c2;
    const Bits just = lteEncodeSiWarn({c});
    for (size_t i = 9; i < just.size(); i++) w.bit(just[i] != 0);
    CHECK(lteParseBcch(w.bits(), c2));
    CHECK(c2.warn.size() == 1 && c2.warn[0].messageId == 4370);
    // truncation at every length must not read past the end or invent a warning
    for (size_t len = 0; len < m.size(); len++) {
        Bits cut(m.begin(), m.begin() + (long)len);
        LteBcch tr;
        (void)lteParseBcch(cut, tr);
        for (const auto& s : tr.warn) CHECK(s.messageId == 4353 || s.messageId == 4352 || s.messageId == 4370);
    }
    // ids
    bool etws = false;
    CHECK(pwsCategory(4352, &etws) == "ETWS earthquake" && etws);
    CHECK(pwsCategory(4379, &etws).find("AMBER") != std::string::npos && !etws);
    CHECK(pwsCategory(4380).find("monthly") != std::string::npos);
    CHECK(pwsCategory(1234).empty());
    CHECK(std::string(etwsTypeName(4u << 9)) == "other");
}

static void textCases() {
    std::string out, lang;
    // 7-bit, one page, language from the DCS
    const std::string en = "Severe flood warning, move to higher ground now.";
    auto c1 = cbData(pack7(en));
    CHECK(pwsDecodeText(c1, 0x01, out, lang) && out == en && lang == "English");
    // UCS2, two pages
    std::string longTxt; for (int i = 0; i < 50; i++) longTxt += "\xD0\x96";   // U+0416 repeated, 2 octets each in UCS2
    std::vector<uint8_t> ucs; for (int i = 0; i < 50; i++) { ucs.push_back(0x04); ucs.push_back(0x16); }
    auto c2 = cbData(ucs);
    CHECK(c2.size() == 1 + 2 * 83);
    CHECK(pwsDecodeText(c2, 0x48, out, lang) && out == longTxt);
    // 7-bit with extension and language prefix
    const std::string body = "en\rPrice \x1B\x65 5";
    auto c3 = cbData(pack7(body));
    CHECK(pwsDecodeText(c3, 0x10, out, lang) && out == "Price \xE2\x82\xAC 5" && lang == "en");
    // not a CB Data block: treated as one block of text
    auto raw = pack7("Short test alert");
    CHECK(pwsDecodeText(raw, 0x0F, out, lang) && out == "Short test alert");
    // reserved DCS means the 7-bit default
    CHECK(pwsDecodeText(c1, 0xD3, out, lang) && out == en);
}

static void assembleOrders() {
    const std::string txt = "CMAS test message, segmented for every order, this is only an exercise for the assembler.";
    auto content = cbData(pack7(txt));
    for (size_t seg : {7u, 40u, 83u, 84u}) {
        auto v = segment(12, 4381, 0x0042, content, 0x01, seg);
        for (int order = 0; order < 3; order++) {
            auto p = v;
            if (order == 1) std::reverse(p.begin(), p.end());
            if (order == 2) { std::mt19937 g(13); std::shuffle(p.begin(), p.end(), g); }
            PwsAssembler as;
            PwsMessage m; int got = 0;
            for (const auto& s : p) if (as.add(s, m)) got++;
            CHECK(got == 1);
            CHECK(m.complete && m.text == txt && m.segments == (int)v.size());
            CHECK(m.messageCode == 4 && m.updateNumber == 2 && m.geoScope == 0);
            // repeats do not rebuild it
            for (const auto& s : p) CHECK(!as.add(s, m));
        }
    }
    // missing middle segment: nothing is published
    { auto v = segment(12, 4381, 0x0043, content, 0x01, 20); v.erase(v.begin() + 2); PwsAssembler as; PwsMessage m; for (const auto& s : v) CHECK(!as.add(s, m)); }
    // SIB10
    { LteWarnSeg s; s.sib = 10; s.messageId = 4352; s.serial = 1; s.warningType = (2u << 9) | 0x0180; PwsAssembler as; PwsMessage m; CHECK(as.add(s, m)); CHECK(m.etws && m.text.find("earthquake and tsunami") != std::string::npos && m.text.find("popup") != std::string::npos); CHECK(!as.add(s, m)); }
}

// through the whole receiver: the generator's PDSCH on SI-RNTI, noise and offsets
static void endToEnd() {
    const std::string txt = "EU-Alert test over LTE";
    auto content = cbData(pack7(txt));
    for (double snr : {25.0, 12.0, 6.0}) for (double cfo : {0.0, 7300.0, -12100.0}) {
        CellTestTruth tr;
        LteGenCfg g = cellTestCell(0, &tr);
        g.sfn0 = 60;
        LteSib w; w.bits = lteEncodeSiWarn(segment(12, 4370, 0x0011, content, 0x01, 150)); w.period = 8; w.sfnMod = 1; w.subframe = 7;
        g.sibs.push_back(w);
        std::vector<cf32> x = lteGenerate(g, 16);
        const double fs = lteRate(lteNfft(g.cell.nRb));
        impair::shift(x, cfo, fs);
        impair::noise(x, snr, 91);
        const auto res = lteSearchDecode(x.data(), x.size(), fs, 0, LteSearchOpts());
        PwsAssembler as; PwsMessage m; bool done = false;
        for (const auto& r : res) for (const auto& si : r.si) { LteBcch b; if (!lteParseBcch(si.bits, b)) continue; for (const auto& s : b.warn) if (as.add(s, m)) done = true; }
        printf("  snr %.0f cfo %+.0f: %s\n", snr, cfo, done ? "ok" : "miss");
        CHECK(done);
        if (done) CHECK(m.text == txt);
    }
}

int main() {
    asnRoundTrip();
    textCases();
    assembleOrders();
    endToEnd();
    return testSummary("cell_pws");
}
