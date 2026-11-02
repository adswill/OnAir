// MPT 1327 against the 1997 issue of the standard (section 5 figures): every address codeword of the control channel is assembled here field by field from the bit
// widths printed in the figures, independently of the generator in mpt1327_gen.cpp, then decoded; and the engine receiver is swept over carrier offset, radio
// centre offset, sample rate, deviation, bit clock error, tone polarity and level.
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_gen.h"
#include "dect2/mpt1327/mpt1327_proto.h"
#include "dect2/mpt1327/mpt1327_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::mpt1327;
#include "../common/test_util.h"

namespace {

struct Fld { int bits; uint64_t v; };

// 48 information bits from fields given first bit first; the check bits come from encode()
uint64_t build(std::initializer_list<Fld> f) {
    uint64_t info = 0;
    int n = 0;
    for (const Fld& x : f) { info = (info << x.bits) | (x.v & ((1ull << x.bits) - 1)); n += x.bits; }
    if (n != 48) { std::printf("builder: %d bits\n", n); return 0; }
    return encode(info);
}

Message dec(uint64_t cw) {
    Message m;
    CHECK(valid(cw), "check bits of a built codeword");
    CHECK(decodeAddress(cw, m), "not an address codeword");
    return m;
}

}  // namespace

int main() {
    // ---- the figures
    {   // 5.4 GTC: 1 PFIX 7, IDENT1 13, 0, D, CHAN 10, IDENT2 13, 2 bits
        Message m = dec(build({{1, 1}, {7, 5}, {13, 1234}, {1, 0}, {1, 1}, {10, 801}, {13, 77}, {2, 0}}));
        CHECK(m.type == "GTC" && m.called.pfix == 5 && m.called.ident == 1234 && m.channel == 801 && m.caller.ident == 77, "GTC");
        CHECK(m.detail.find("data") != std::string::npos, "GTC D=1 is a data call: %s", m.detail.c_str());
    }
    {   // 5.5.1 ALH: 1 PFIX 7, IDENT1 13, 1, CAT 000, TYPE 00, FUNC 3, CHAN4 4, WT 3, RSVD 2, M 5, N 4
        const char* names[8] = {"ALH", "ALHS", "ALHD", "ALHE", "ALHR", "ALHX", "ALHF", "reserved"};
        for (int fn = 0; fn < 8; fn++) {
            Message m = dec(build({{1, 1}, {7, 9}, {13, 4000}, {1, 1}, {3, 0}, {2, 0}, {3, (uint64_t)fn}, {4, 11}, {3, 5}, {2, 0}, {5, 17}, {4, 9}}));
            CHECK(m.type == names[fn] && m.called.pfix == 9 && m.called.ident == 4000, "ALH func %d: %s", fn, m.type.c_str());
            CHECK(m.detail.find("(low 4 bits) 11") != std::string::npos && m.detail.find("wait 5") != std::string::npos && m.detail.find("address bits 17") != std::string::npos &&
                  m.detail.find("aloha number 9") != std::string::npos, "ALH fields: %s", m.detail.c_str());
        }
    }
    {   // 5.5.2 ACK: ... TYPE 01, FUNC 3, IDENT2 13, QUAL 1, N 4
        const char* names[8] = {"ACK", "ACKI", "ACKQ", "ACKX", "ACKV", "ACKE", "ACKT", "ACKB"};
        for (int fn = 0; fn < 8; fn++) {
            Message m = dec(build({{1, 1}, {7, 100}, {13, 8100}, {1, 1}, {3, 0}, {2, 1}, {3, (uint64_t)fn}, {13, 4321}, {1, 1}, {4, 3}}));
            CHECK(m.type == names[fn] && m.called.pfix == 100 && m.called.ident == 8100 && m.caller.ident == 4321 && m.detail.find("qualifier 1") != std::string::npos, "ACK func %d: %s %s", fn, m.type.c_str(), m.detail.c_str());
        }
    }
    {   // 5.5.3.2 ahoys: TYPE 10, FUNC 000 AHY, 010 AHYX, 101 AHYP, 110 AHYQ, 111 AHYC
        Message m = dec(build({{1, 1}, {7, 3}, {13, 55}, {1, 1}, {3, 0}, {2, 2}, {3, 0}, {13, 8189}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}));
        CHECK(m.type == "AHY" && m.caller.ident == 8189 && addressText(m.caller) == "IPFIXI" && m.detail.find("calling") != std::string::npos && m.detail.find("data codeword follows") != std::string::npos &&
              m.detail.find("emergency") != std::string::npos, "AHY: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {7, 3}, {13, 55}, {1, 1}, {3, 0}, {2, 2}, {3, 2}, {13, 66}, {5, 0}}));
        CHECK(m.type == "AHYX" && m.caller.ident == 66, "AHYX");
        m = dec(build({{1, 1}, {7, 3}, {13, 55}, {1, 1}, {3, 0}, {2, 2}, {3, 5}, {13, 8190}, {5, 0}}));
        CHECK(m.type == "AHYP" && addressText(m.caller) == "TSCI", "AHYP");
        m = dec(build({{1, 1}, {7, 3}, {13, 55}, {1, 1}, {3, 0}, {2, 2}, {3, 6}, {13, 8190}, {5, 21}}));
        CHECK(m.type == "AHYQ" && m.detail.find("status message 21") != std::string::npos, "AHYQ: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {7, 3}, {13, 55}, {1, 1}, {3, 0}, {2, 2}, {3, 7}, {13, 8188}, {2, 2}, {3, 0}}));
        CHECK(m.type == "AHYC" && addressText(m.caller) == "SDMI" && m.detail.find("descriptor 0") != std::string::npos && m.detail.find("2 slots") != std::string::npos, "AHYC: %s", m.detail.c_str());
    }
    {   // 5.5.4 miscellaneous: TYPE 11
        Message m = dec(build({{1, 1}, {4, 0xA}, {1, 0}, {15, 31000}, {1, 1}, {3, 0}, {2, 3}, {3, 0}, {18, 0x2AAAA}}));
        CHECK(m.type == "MARK" && m.sys == 31000 && m.detail.find("(low 4 bits) 10") != std::string::npos, "MARK: %s sys %d", m.detail.c_str(), m.sys);
        m = dec(build({{1, 1}, {7, 8}, {13, 200}, {1, 1}, {3, 0}, {2, 3}, {3, 1}, {10, 77}, {3, 3}, {5, 0}}));
        CHECK(m.type == "MAINT" && m.channel == 77 && m.called.ident == 200 && m.detail.find("disconnect") != std::string::npos, "MAINT: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {10, 500}, {10, 3}, {1, 1}, {3, 0}, {2, 3}, {3, 2}, {4, 0}, {2, 0}, {12, 0xAAA}}));
        CHECK(m.type == "CLEAR" && m.channel == 500 && m.detail.find("control channel 3") != std::string::npos && m.called.ident < 0, "CLEAR: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {7, 8}, {13, 200}, {1, 1}, {3, 0}, {2, 3}, {3, 3}, {10, 640}, {5, 19}, {2, 0}, {1, 0}}));
        CHECK(m.type == "MOVE" && m.channel == 640 && m.called.pfix == 8 && m.detail.find("qualifier 19") != std::string::npos, "MOVE: %s", m.detail.c_str());
        // BCAST (5.5.4.5): SYSDEF 5, SYS 15, 1, CAT, TYPE 11, FUNC 100, then the parameters
        m = dec(build({{1, 1}, {5, 0}, {15, 20001}, {1, 1}, {3, 0}, {2, 3}, {3, 4}, {10, 311}, {2, 0}, {6, 0}}));
        CHECK(m.type == "BCAST" && m.sys == 20001 && m.channel == 311 && m.detail.find("announced") != std::string::npos, "BCAST announce: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {5, 1}, {15, 20001}, {1, 1}, {3, 0}, {2, 3}, {3, 4}, {10, 311}, {2, 0}, {6, 0}}));
        CHECK(m.channel == 311 && m.detail.find("withdrawn") != std::string::npos, "BCAST withdraw: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {5, 2}, {15, 20001}, {1, 1}, {3, 0}, {2, 3}, {3, 4}, {1, 0}, {5, 12}, {1, 0}, {1, 1}, {2, 0}, {8, 0}}));
        CHECK(m.detail.find("every 12 s") != std::string::npos && m.detail.find("pressel on sent") != std::string::npos && m.detail.find("individual") != std::string::npos, "BCAST maintenance: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {5, 4}, {15, 20001}, {1, 1}, {3, 0}, {2, 3}, {3, 4}, {10, 99}, {2, 0}, {2, 0}, {4, 6}}));
        CHECK(m.channel == 99 && m.detail.find("adjacent") != std::string::npos && m.detail.find("site 6") != std::string::npos, "BCAST adjacent: %s", m.detail.c_str());
    }
    {   // 5.6: HEAD and SAM
        Message m = dec(build({{1, 1}, {7, 4}, {13, 8190}, {1, 1}, {3, 1}, {1, 1}, {2, 2}, {7, 6}, {13, 1500}}));
        CHECK(m.type == "HEAD" && addressText(m.called) == "TSCI" && m.caller.pfix == 6 && m.caller.ident == 1500 && m.detail.find("3 data codewords") != std::string::npos, "HEAD: %s", m.detail.c_str());
        m = dec(build({{1, 1}, {7, 4}, {13, 700}, {1, 1}, {3, 1}, {1, 0}, {22, 0}}));
        CHECK(m.type == "SAM", "SAM");
    }
    {   // special idents (Table 4-1)
        CHECK(addressText({-1, 8191}) == "ALLI" && addressText({-1, 8190}) == "TSCI" && addressText({-1, 8186}) == "INCI" && addressText({-1, 8101}) == "PSTNGI" &&
              addressText({3, 100}) == "3/100" && addressText({-1, 0}) == "DUMMYI" && addressText({-1, 8125}) == "short-form ident 5", "special idents");
    }
    {   // Appendix 2: any single error is repaired, any two or three are detected (never turned into another codeword)
        std::mt19937 g(11);
        int fixed = 0, lost = 0, wrong = 0;
        for (int i = 0; i < 400; i++) {
            const uint64_t cw = build({{1, 1}, {7, (uint64_t)(g() % 128)}, {13, (uint64_t)(g() % 8192)}, {1, 1}, {3, 0}, {2, (uint64_t)(g() % 4)}, {3, (uint64_t)(g() % 8)}, {13, (uint64_t)(g() % 8192)}, {5, (uint64_t)(g() % 32)}});
            uint64_t one = cw ^ (1ull << (g() % 64));
            if (correct(one) && one == cw) fixed++;
            uint64_t two = cw;
            const int a = (int)(g() % 64);
            int b = (int)(g() % 64);
            if (b == a) b = (b + 1) % 64;
            two ^= (1ull << a) | (1ull << b);
            const uint64_t orig = two;
            if (correct(two)) wrong++; else if (two == orig) lost++;
        }
        CHECK(fixed == 400 && wrong == 0 && lost == 400, "single / double error handling: fixed %d, miscorrected %d, detected %d (of 400)", fixed, wrong, lost);
    }
    // ---- the engine receiver on a whole control channel, swept
    {
        const std::vector<uint64_t> msgs = {makeGtc(5, 1234, 0, 17, 2003), makeAloha(5, 0, 0, 3, 2, 8, 1), makeBcastChannel(0, 20001, 3), makeAck(5, 77, 0, 4321, 0, 0),
                                            makeGtc(7, 88, 1, 900, 9), makeBcastChannel(4, 20001, 9, 2)};
        const std::vector<uint8_t> bits = controlChannelBits(20001, msgs);
        int cases = 0, bad = 0, wrongCases = 0;
        for (double fs : {48000.0, 120000.0, 240000.0, 960000.0})
            for (double offs : {-90000.0, -50000.0, 0.0, 40000.0})
                for (double dev : {3000.0, 4500.0, 6000.0})   // tone deviation = dev x 0.45: about 1.3 to 2.7 kHz
                    for (int inv = 0; inv < 2; inv++)
                        for (double ce : {-0.001, 0.0, 0.0005}) {
                            if (fs < 2 * (std::fabs(offs) + 6000.0) && offs != 0.0) continue;   // the channel must fit in the capture
                            const std::vector<float> aud = ffskAudio(bits, fs, inv != 0, 0.45f, 1.0 + ce);
                            std::vector<float> pad((size_t)(fs * 0.5), 0.f);
                            std::vector<float> all = pad; all.insert(all.end(), aud.begin(), aud.end()); all.insert(all.end(), pad.begin(), pad.end());
                            const double cfo = (cases % 3 - 1) * 700.0;   // the carrier is a little off the tuned channel
                            std::vector<std::complex<float>> iq = fmModulate(all, fs, dev, offs + cfo);
                            std::mt19937 g(5 + (unsigned)cases);
                            std::normal_distribution<float> nd(0.f, 0.05f);
                            for (auto& v : iq) v += std::complex<float>(nd(g), nd(g));
                            MptReceiver rx;
                            rx.configure(fs);
                            rx.setSignalOffset(offs);
                            rx.setCenterHz(170e6);
                            for (size_t i = 0; i < iq.size(); i += 4096) rx.feed(iq.data() + i, std::min<size_t>(4096, iq.size() - i));
                            MptTelemetry t;
                            const bool have = rx.telemetry(t, 0);
                            cases++;
                            bool spurious = false;   // a row that was not sent would be a wrong message
                            if (have) for (const MptMessageRow& r : t.rows) spurious |= r.type != "GTC" && r.type != "ALH" && r.type != "BCAST" && r.type != "ACK";
                            if (!have || t.messages > msgs.size() || spurious || (t.sysId != 20001 && t.sysId != -1)) { wrongCases++; std::printf("sweep WRONG fs %.0f off %.0f dev %.0f inv %d clk %+.4f\n", fs, offs, dev, inv, ce); }
                            else if (t.messages != msgs.size() || t.sysId != 20001) {
                                bad++;
                                std::printf("sweep miss fs %.0f off %.0f dev %.0f inv %d clk %+.4f: sys %d messages %llu of %zu\n", fs, offs, dev, inv, ce, t.sysId, (unsigned long long)t.messages, msgs.size());
                            }
                        }
        // no wrong message or system identity is ever reported; the noise (sigma 0.05, no soft decisions in the decoder) may cost one message in about a hundred runs
        CHECK(wrongCases == 0, "engine sweep: %d wrong results", wrongCases);
        CHECK(bad * 100 <= cases * 2, "engine sweep: %d of %d cases lost a message or the system id", bad, cases);
        std::printf("engine sweep: %d cases, %d incomplete, %d wrong\n", cases, bad, wrongCases);
    }
    return testSummary("test_mpt1327_std");
}
