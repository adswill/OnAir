// MPT 1327 control channel: codeword check bits against the standard's own constraints, address codeword field round trips, the decoder on
// generated FFSK audio (clean, shifted, other sample rates, inverted tones, noise, bit errors) and the engine receiver on a generated FM signal.
#include "dect2/engine/mode_tuning.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_gen.h"
#include "dect2/mpt1327/mpt1327_proto.h"
#include "dect2/mpt1327/mpt1327_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::mpt1327;
#include "../common/test_util.h"

namespace {

std::vector<uint64_t> sampleMessages() {
    return {makeGtc(5, 1234, 0, 17, 2003),    makeAloha(5, 0, 0, 3, 2, 8, 1),     makeAck(5, 77, 0, 4321, 0, 0),
            makeAhy(9, 100, 0, 0),            makeBcastChannel(0, 20001, 3),       makeBcastChannel(4, 20001, 9, 2),
            makeGtc(100, 8000, 1, 1023, 8100), makeAck(127, 8191, 3, 1, 1, 0)};
}

std::vector<float> addNoise(std::vector<float> a, double sigma, unsigned seed) {
    std::mt19937 g(seed);
    std::normal_distribution<float> nd(0.f, (float)sigma);
    for (float& v : a) v += nd(g);
    return a;
}

// Decode audio and return the messages as codewords
std::vector<uint64_t> decode(const std::vector<float>& a, double fs, Stats* out = nullptr) {
    Decoder d;
    const size_t chunk = 1000;
    for (size_t i = 0; i < a.size(); i += chunk) d.process(a.data() + i, std::min(chunk, a.size() - i), fs);
    std::vector<uint64_t> r;
    for (const Event& e : d.takeEvents()) r.push_back(e.msg.cw);
    if (out) *out = d.stats();
    return r;
}

bool sameSeq(const std::vector<uint64_t>& got, const std::vector<uint64_t>& want) { return got == want; }

}  // namespace

int main() {
    // --- the sync words and the check code
    CHECK(kSync == 0xC4D7 && kSynt == 0x3B28, "sync words");
    {
        std::mt19937_64 g(1);
        int badRoundTrip = 0, undetected1 = 0, undetected2 = 0;
        for (int n = 0; n < 200; n++) {
            const uint64_t info = g() & 0xFFFFFFFFFFFFull;
            const uint64_t cw = encode(info);
            if (!valid(cw) || (cw >> 16) != info) badRoundTrip++;
            for (int b = 0; b < 64; b++) if (valid(cw ^ (1ull << b))) undetected1++;
            for (int b = 0; b < 64; b++) for (int c = b + 1; c < 64; c++) if (valid(cw ^ (1ull << b) ^ (1ull << c))) undetected2++;
        }
        CHECK(badRoundTrip == 0, "encode/valid round trip failed %d times", badRoundTrip);
        CHECK(undetected1 == 0, "%d single bit errors not detected", undetected1);
        CHECK(undetected2 == 0, "%d double bit errors not detected", undetected2);
    }
    // the check bits of a CCSC are SYNC, for any SYS, and the CCS agrees with the standard's Appendix 3 algorithm
    for (int sys : {0, 1, 100, 12345, 20001, 32767}) {
        const uint64_t c = makeCcsc(sys);
        int s = -1;
        CHECK(valid(c), "CCSC %d: check bits fail", sys);
        CHECK(decodeCcsc(c, s) && s == sys, "CCSC %d: decoded %d", sys, s);
        CHECK((uint16_t)(c & 0xFFFF) == kSync, "CCSC %d: check bits %04X are not SYNC", sys, (unsigned)(c & 0xFFFF));
        CHECK(field(c, 33, 16) == 0xAAAA, "CCSC %d: preamble field %04X", sys, (unsigned)field(c, 33, 16));
        // Appendix 3: info = preamble, '1100010011010100', SYS (bits 1..47), bit 48 = 0, then 1 when the parity bit came out 0
        uint64_t info = ((uint64_t)0xAAAA << 32) | ((uint64_t)0xC4D4 << 16) | ((uint64_t)sys << 1);
        uint16_t chk = checkBits(info);
        if ((chk & 1) == 0) { info |= 1; chk = checkBits(info); }
        const uint32_t ccs = (uint32_t)((info & 1) << 15) | (((uint32_t)(chk >> 1)) ^ 1u);   // bits 48 .. 63 with bit 63 inverted
        CHECK(field(c, 17, 16) == ccs, "CCSC %d: CCS %04X, Appendix 3 gives %04X", sys, (unsigned)field(c, 17, 16), (unsigned)ccs);
    }

    // --- address codeword fields
    {
        Message m;
        CHECK(decodeAddress(makeGtc(5, 1234, 1, 17, 2003, 2), m) && m.type == "GTC" && m.called.pfix == 5 && m.called.ident == 1234 && m.channel == 17 &&
              m.caller.ident == 2003, "GTC fields: %s %d/%d chan %d caller %d", m.type.c_str(), m.called.pfix, m.called.ident, m.channel, m.caller.ident);
        CHECK(decodeAddress(makeGtc(5, 1234, 0, 1023, 8191), m) && m.channel == 1023 && m.caller.ident == 8191, "GTC max values");
        CHECK(decodeAddress(makeAloha(127, 8191, 3, 15, 7, 31, 15), m) && m.type == "ALHE" && m.called.pfix == 127 && m.called.ident == 8191, "ALHE: %s", m.type.c_str());
        CHECK(decodeAddress(makeAck(9, 33, 6, 77, 1, 3), m) && m.type == "ACKT" && m.called.ident == 33 && m.caller.ident == 77, "ACKT: %s", m.type.c_str());
        CHECK(decodeAddress(makeAhy(9, 100, 7, 5), m) && m.type == "AHYC", "AHYC: %s", m.type.c_str());
        CHECK(decodeAddress(makeBcastChannel(0, 20001, 513), m) && m.type == "BCAST" && m.sys == 20001 && m.channel == 513, "BCAST announce");
        CHECK(decodeAddress(makeBcastChannel(1, 20001, 12), m) && m.channel == 12 && m.detail.find("withdrawn") != std::string::npos, "BCAST withdraw");
        CHECK(decodeAddress(makeBcastChannel(4, 777, 40, 5), m) && m.channel == 40 && m.detail.find("site 5") != std::string::npos, "BCAST adjacent: %s", m.detail.c_str());
        CHECK(!decodeAddress(makeData(0x123456789ABull), m), "a data codeword is not an address codeword");
        CHECK(addressText(Address{5, 1234}) == "5/1234" && addressText(Address{-1, 7}) == "ident 7" && addressText(Address{}) == "-", "addressText");
    }

    const std::vector<uint64_t> msgs = sampleMessages();
    const std::vector<uint8_t> bits = controlChannelBits(20001, msgs);

    // --- French-style framing: the sync word is not the standard one
    {
        // the 16 bit SYNC inside every control channel system codeword is replaced by another word (and the check bits re-made are not needed:
        // the CCSC check bits cover the info bits only, the replaced bits are the check bits' neighbours so the codeword is rebuilt with encode)
        std::vector<uint8_t> b = bits;
        const uint16_t alt = 0x35A9;
        // locate every 16 bit SYNC and rewrite it
        int n = 0;
        for (size_t i = 0; i + 16 <= b.size(); i++) {
            uint32_t w = 0;
            for (int k = 0; k < 16; k++) w = (w << 1) | b[i + k];
            if (w == kSync) { for (int k = 0; k < 16; k++) b[i + k] = (alt >> (15 - k)) & 1; n++; i += 15; }
        }
        CHECK(n >= 2, "sync words rewritten: %d", n);
        for (bool inv : {false, true}) {
        std::vector<float> a = ffskAudio(b, 24000, inv);
        a.insert(a.end(), 24000 * 2, 0.f);
        for (SyncMode m : {SyncMode::Standard, SyncMode::Tolerant, SyncMode::Auto}) {
            Decoder d;
            d.setSyncMode(m);
            std::vector<float> in(24000 * 10, 0.f);   // 10 s of silence first: lets Auto time out
            in.insert(in.end(), a.begin(), a.end());
            for (size_t i = 0; i < in.size(); i += 1000) d.process(in.data() + i, std::min<size_t>(1000, in.size() - i), 24000);
            std::vector<uint64_t> got;
            for (const Event& e : d.takeEvents()) got.push_back(e.msg.cw);
            if (m == SyncMode::Standard) CHECK(got.empty(), "standard framing finds nothing behind a foreign sync word: %zu", got.size());
            else CHECK(got.size() >= msgs.size() - 1, "mode %d inv %d: %zu of %zu messages", (int)m, (int)inv, got.size(), msgs.size());
        }
        }
    }

    // --- decoder on FFSK audio
    for (double fs : {24000.0, 48000.0, 44100.0, 12000.0}) {
        for (bool inv : {false, true}) {
            for (int lead : {0, 3, 7, 13}) {
                std::vector<float> a(lead * (size_t)(fs / 24000.0 + 0.5), 0.f);
                const std::vector<float> s = ffskAudio(bits, fs, inv);
                a.insert(a.end(), s.begin(), s.end());
                Stats st;
                const std::vector<uint64_t> got = decode(a, fs, &st);
                CHECK(sameSeq(got, msgs), "clean fs %.0f inv %d lead %d: %zu of %zu messages", fs, (int)inv, lead, got.size(), msgs.size());
                CHECK(st.bad == 0 && st.sys == 20001 && st.ccsc >= 1, "clean fs %.0f inv %d lead %d: bad %llu sys %d ccsc %llu", fs, (int)inv, lead,
                      (unsigned long long)st.bad, st.sys, (unsigned long long)st.ccsc);
            }
        }
    }
    // bit rate error of 0.1 %, a DC offset and a scaled level
    {
        std::vector<float> a = ffskAudio(bits, 24000, false, 0.2f, 1.001);
        for (float& v : a) v += 0.05f;
        CHECK(sameSeq(decode(a, 24000), msgs), "0.1 %% bit rate error and DC offset");
    }
    // the same message repeated: every one is reported
    {
        std::vector<uint64_t> rep;
        for (int i = 0; i < 5; i++) rep.push_back(makeGtc(5, 1234 + i, 0, 10 + i, 2003));
        const std::vector<float> a = ffskAudio(controlChannelBits(300, rep), 24000);
        CHECK(sameSeq(decode(a, 24000), rep), "five GTC messages");
    }
    // --- noise
    {
        const std::vector<float> clean = ffskAudio(bits, 24000, false, 0.5f);
        std::set<uint64_t> allowed(msgs.begin(), msgs.end());
        int fewerAt10 = 0;
        for (double sigma : {0.05, 0.1, 0.15, 0.25, 0.5, 1.0}) {
            size_t good = 0, falsePos = 0;
            for (unsigned seed = 1; seed <= 6; seed++) {
                Stats st;
                for (uint64_t cw : decode(addNoise(clean, sigma, seed), 24000, &st)) (allowed.count(cw) ? good : falsePos)++;
            }
            std::printf("  noise sigma %.2f (tone amplitude 0.5): %zu of %zu messages, %zu false\n", sigma, good, msgs.size() * 6, falsePos);
            CHECK(falsePos == 0, "noise sigma %.2f: %zu messages that were not sent", sigma, falsePos);
            if (sigma <= 0.1) CHECK(good == msgs.size() * 6, "noise sigma %.2f: only %zu of %zu messages", sigma, good, msgs.size() * 6);
            if (sigma >= 0.5) fewerAt10 += (good < msgs.size() * 6);
        }
        CHECK(fewerAt10 > 0, "heavy noise did not cost any message (the test is not exercising errors)");
        // pure noise for 60 s: nothing is decoded, nothing is counted bad
        std::vector<float> n(24000 * 60, 0.f);
        Stats st;
        const std::vector<uint64_t> got = decode(addNoise(n, 0.3, 99), 24000, &st);
        CHECK(got.empty() && st.good == 0 && st.bad == 0 && !st.locked, "pure noise: %zu messages, good %llu bad %llu", got.size(), (unsigned long long)st.good, (unsigned long long)st.bad);
        // a tone and silence
        std::vector<float> tone(24000 * 5);
        for (size_t i = 0; i < tone.size(); i++) tone[i] = 0.5f * (float)std::sin(2 * 3.14159265358979 * 1200.0 * (double)i / 24000.0);
        CHECK(decode(tone, 24000).empty() && decode(std::vector<float>(24000 * 5, 0.f), 24000).empty(), "tone or silence gave a message");
    }
    // --- bit errors: a flipped bit in one address codeword loses that message and is counted bad; no wrong message is reported
    {
        for (size_t which = 0; which < msgs.size(); which++) {
            std::vector<uint8_t> b = bits;
            // position of the address codeword 'which' in the stream: preamble 16 + SYNC 16, then 64 (address), then 64 + 64 for each further
            const size_t pos = 32 + which * 128 + 10;   // a bit inside the information field
            b[pos] ^= 1;
            Stats st;
            const std::vector<uint64_t> got = decode(ffskAudio(b, 24000), 24000, &st);
            std::vector<uint64_t> want = msgs;
            want.erase(want.begin() + (long)which);
            CHECK(sameSeq(got, want), "bit error in message %zu: %zu messages decoded (wanted %zu)", which, got.size(), want.size());
            CHECK(st.bad >= 1, "bit error in message %zu: bad count %llu", which, (unsigned long long)st.bad);
        }
        // two errors in the same codeword
        std::vector<uint8_t> b = bits;
        b[32 + 128 + 5] ^= 1; b[32 + 128 + 40] ^= 1;
        std::vector<uint64_t> want = msgs;
        want.erase(want.begin() + 1);
        CHECK(sameSeq(decode(ffskAudio(b, 24000), 24000), want), "two bit errors in message 1");
    }

    // --- registered tuning
    {
#ifndef MPT_STANDALONE   // quick compile of the mode sources alone (no engine registry)
        const ModeTuning* reg = modeTuning(129);
        CHECK(reg != nullptr && std::string(reg->name) == "MPT1327 trunking (WIP)" && std::string(reg->id) == "mpt1327", "modeTuning(129)");
#endif
        CHECK(mpt1327Tuning().stdMode == 129, "stdMode");
    }

    // --- the engine receiver: FM at 240 kHz, channel 50 kHz below the centre, with a carrier error and noise
    for (double cfo : {0.0, 600.0, -900.0}) {
        for (double sigma : {0.0, 0.3}) {
            const double fs = 240000;
            const std::vector<float> aud = ffskAudio(bits, fs, false, 0.5f);
            std::vector<float> pad(24000, 0.f);
            std::vector<float> all = pad;
            all.insert(all.end(), aud.begin(), aud.end());
            all.insert(all.end(), pad.begin(), pad.end());
            // audio 1.0 = 3 kHz deviation, so the 0.5 tone amplitude is 1.5 kHz
            std::vector<std::complex<float>> iq = fmModulate(all, fs, 3000.0, -50000.0 + cfo);
            std::mt19937 g(5);
            std::normal_distribution<float> nd(0.f, (float)sigma);
            for (auto& v : iq) v += std::complex<float>(nd(g), nd(g));
            MptReceiver rx;
            rx.configure(fs);
            rx.setSignalOffset(-50000.0);
            rx.setCenterHz(170e6);
            CHECK(rx.ready(), "receiver not ready at 240 kHz");
            for (size_t i = 0; i < iq.size(); i += 4096) rx.feed(iq.data() + i, std::min<size_t>(4096, iq.size() - i));
            MptTelemetry t;
            const bool have = rx.telemetry(t, 0);
            CHECK(have, "no telemetry (cfo %.0f sigma %.1f)", cfo, sigma);
            if (!have) continue;
            std::printf("  engine cfo %+.0f noise %.1f: %llu messages, good %llu bad %llu, sys %d\n", cfo, sigma, (unsigned long long)t.messages, (unsigned long long)t.good, (unsigned long long)t.bad, t.sysId);
            CHECK(t.sysId == 20001, "engine cfo %.0f sigma %.1f: system %d", cfo, sigma, t.sysId);
            CHECK(t.messages == msgs.size() && t.rows.size() == msgs.size(), "engine cfo %.0f sigma %.1f: %llu messages", cfo, sigma, (unsigned long long)t.messages);
            bool gtc17 = false, ctl = false;
            for (const MptChannelRow& c : t.channels) {
                if (c.channel == 17 && !c.controlChannel && c.called == "5/1234" && c.caller == "ident 2003") gtc17 = true;
                if (c.channel == 3 && c.controlChannel) ctl = true;
            }
            CHECK(gtc17 && ctl, "channel table: GTC channel 17 %d, announced control channel 3 %d", (int)gtc17, (int)ctl);
            CHECK(!t.rows.empty() && t.rows[0].type == "GTC" && t.rows[0].called == "5/1234", "first row");
            CHECK(mpt1327Summary(t).find("system 20001") != std::string::npos, "summary: %s", mpt1327Summary(t).c_str());
        }
    }
    // the engine receiver with no signal: searching, nothing decoded
    {
        MptReceiver rx;
        rx.configure(240000);
        rx.setSignalOffset(-50000.0);
        std::mt19937 g(8);
        std::normal_distribution<float> nd(0.f, 0.3f);
        std::vector<std::complex<float>> iq(240000 * 3);
        for (auto& v : iq) v = {nd(g), nd(g)};
        rx.feed(iq.data(), iq.size());
        MptTelemetry t;
        CHECK(rx.telemetry(t, 0) && t.messages == 0 && t.good == 0 && !t.locked && t.state == 1, "noise only: state %d messages %llu", t.state, (unsigned long long)t.messages);
    }
    return testSummary("test_mpt1327");
}
