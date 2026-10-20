// LBJ link and message layer: codeword coding and correction, page extraction, field parsing, plausibility checks.
#include "dect2/lbj/lbj_gen.h"
#include "dect2/lbj/lbj_pocsag.h"
#include "dect2/lbj/lbj_proto.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static lbj::Burst burstOf(const std::vector<lbj::GenPage>& pages) {
    lbj::Burst b;
    b.words = lbj::burstWords(pages);
    return b;
}

int main() {
    // the known POCSAG constants: the idle word and the sync word are valid codewords
    CHECK(lbj::codewordValid(lbj::kIdleWord), "idle word not valid");
    CHECK(lbj::codewordValid(lbj::kSyncWord), "sync word not valid");
    // encoding then BCH check, and the correction of every single and double error
    for (uint32_t d : {0u, 1u, 0x155555u, 0x1FFFFFu, 0x0ABCDEu}) {
        const uint32_t w = lbj::encodeCodeword(d);
        CHECK(lbj::codewordValid(w), "encode %x invalid", d);
        for (int i = 0; i < 32; i++) {
            const lbj::CwResult r = lbj::correctCodeword(w ^ (1u << i));
            CHECK(r.ok && r.word == w && r.fixed == 1, "single error bit %d of %x", i, d);
            for (int j = i + 1; j < 32; j += 3) {
                const lbj::CwResult r2 = lbj::correctCodeword(w ^ (1u << i) ^ (1u << j));
                CHECK(r2.ok && r2.word == w && r2.fixed == 2, "double error %d,%d of %x", i, j, d);
            }
        }
    }
    // a real frame off the air (recording by the reporter of issue #153): address word 4b5149f0 of RIC 1234000, then the three
    // message words of a basic message with every field blank ("----- --- -----")
    {
        lbj::Burst b;
        b.words = {0x4b5149f0u, 0xdddddf97u, 0x9ddd9f29u, 0xdddddf97u};
        while (b.words.size() < 16) b.words.push_back(lbj::kIdleWord);
        const auto pages = lbj::pagesFromBurst(b);
        CHECK(pages.size() == 1 && pages[0].ric == 1234000 && pages[0].function == 1 && pages[0].digits == "----- --- -----", "real blank frame: %zu pages '%s'", pages.size(), pages.empty() ? "" : pages[0].digits.c_str());
        lbj::LbjSessionState st;
        LbjMessage m;
        CHECK(!lbj::parsePage(pages[0], 1.0, st, m) && st.emptyFrames == 1, "blank frame must not give a message");
    }
    // a basic message round trip through the codeword layer
    {
        const lbj::GenPage g = lbj::basicPage("19533", 67, 382, 1);
        CHECK(g.digits == "19533  67   382", "format '%s'", g.digits.c_str());
        const auto pages = lbj::pagesFromBurst(burstOf({g}));
        CHECK(pages.size() == 1 && pages[0].ric == 1234000 && pages[0].digits == g.digits, "page round trip");
        lbj::LbjSessionState st;
        LbjMessage m;
        CHECK(lbj::parsePage(pages[0], 2.0, st, m), "basic page rejected");
        CHECK(m.train == "19533" && m.speedKmh == "67" && m.kmMarker == "38.2" && m.direction == "down" && m.crcOk, "fields: %s %s %s %s", m.train.c_str(), m.speedKmh.c_str(), m.kmMarker.c_str(), m.direction.c_str());
        // Odd train numbers remain down even when the POCSAG function is 3
        const auto pu = lbj::pagesFromBurst(burstOf({lbj::basicPage("10481", 112, 12046, 3)}));
        CHECK(lbj::parsePage(pu[0], 3.0, st, m) && m.direction == "down" && m.kmMarker == "1204.6" && m.speedKmh == "112", "up message");
        // standing still: speed 0
        const auto p0 = lbj::pagesFromBurst(burstOf({lbj::basicPage("23017", 0, 50, 1)}));
        CHECK(lbj::parsePage(p0[0], 4.0, st, m) && m.speedKmh == "0" && m.kmMarker == "5.0", "speed 0");
    }
    // two bit errors in every codeword are corrected; a lost speed word leaves the train and kilometre post
    {
        lbj::Burst b = burstOf({lbj::basicPage("19533", 67, 382, 1)});
        for (size_t i = 0; i < 4; i++) b.words[i] ^= (1u << 3) | (1u << 20);
        const auto pages = lbj::pagesFromBurst(b);
        lbj::LbjSessionState st;
        LbjMessage m;
        CHECK(pages.size() == 1 && lbj::parsePage(pages[0], 5.0, st, m) && m.train == "19533" && m.speedKmh == "67" && m.kmMarker == "38.2", "double errors");
        b = burstOf({lbj::basicPage("19533", 67, 382, 1)});
        b.words[2] ^= 0x00080420u;     // three bit errors: detected, not correctable
        const auto pl = lbj::pagesFromBurst(b);
        lbj::LbjSessionState st2;
        CHECK(pl.size() == 1 && lbj::parsePage(pl[0], 6.0, st2, m) && m.train == "19533" && m.speedKmh.empty() && m.kmMarker == "38.2" && !m.crcOk, "lost speed word: train %s speed '%s' km %s", m.train.c_str(), m.speedKmh.c_str(), m.kmMarker.c_str());
        b = burstOf({lbj::basicPage("19533", 67, 382, 1)});
        b.words[1] ^= 0x00080420u;     // the train number lost: nothing to show
        const auto pn = lbj::pagesFromBurst(b);
        lbj::LbjSessionState st3;
        CHECK(pn.size() == 1 && !lbj::parsePage(pn[0], 7.0, st3, m), "message without a train number");
    }
    // plausibility: a speed over 400 km/h, letters in the train number and other addresses are not messages
    {
        lbj::LbjSessionState st;
        LbjMessage m;
        lbj::Page p;
        p.ric = lbj::kAddrBasic; p.function = 1; p.digits = "19533 999   382";
        CHECK(!lbj::parsePage(p, 1.0, st, m) && st.rejected == 1, "speed 999");
        p.digits = "1953U  67   382";
        CHECK(!lbj::parsePage(p, 1.0, st, m), "letter in train number");
        p.ric = 1234567; p.digits = "19533  67   382";
        CHECK(!lbj::parsePage(p, 1.0, st, m), "foreign address");
    }
    // pages of different frames in one burst and the idle fill between them
    {
        lbj::GenPage other;
        other.ric = 2000001; other.function = 0; other.digits = "12345";
        const auto pages = lbj::pagesFromBurst(burstOf({lbj::basicPage("19533", 67, 382, 1), other}));
        CHECK(pages.size() == 2 && pages[1].ric == 2000001 && pages[1].digits == "12345", "two pages: %zu", pages.size());
    }
    return testSummary("test_lbj_proto");
}
