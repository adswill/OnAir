// SSB conversations: the voice analyzer on speech-like audio and on decoys, the scanner on synthetic wide captures (two alternating stations on one
// frequency with different offsets, drift, fading, a second conversation on the other sideband, adjacent AM, ALE / RTTY / PSK31 / FT8 / multi-tone
// decoys, a carrier, band noise, noise only), the conversation state machine, filters, auto-listen, sweep plan, callsigns, export and clips.
// No radio and no screen. The signals are synthetic: this proves the logic, not performance on real air (docs/SSB-CONVERSATIONS.md says so).
#include "dect2/ssbconv/ssbconv.h"
#include "../common/test_util.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace dect2::ssbconv;
using synth::Station;

namespace {
struct Run {
    ConversationBook book;
    std::vector<Detection> all;
    std::vector<BookEvent> events;
    Scanner sc;
    explicit Run(const ScannerOptions& so = ScannerOptions(), const BookOptions& bo = BookOptions()) : book(bo), sc(so) {}
    void feed(const std::vector<cf>& iq, double rate, double centre, double t0 = 0) {
        const size_t blk = 8192;
        for (size_t i = 0; i < iq.size(); i += blk) {
            const size_t n = std::min(blk, iq.size() - i);
            const double t = t0 + (double)(i + n) / rate;
            std::vector<Detection> d = sc.feed(iq.data() + i, n, rate, centre, t);
            all.insert(all.end(), d.begin(), d.end());
            std::vector<BookEvent> e = book.update(t, d);
            events.insert(events.end(), e.begin(), e.end());
            if (d.empty()) { std::vector<BookEvent> e2 = book.tick(t); events.insert(events.end(), e2.begin(), e2.end()); }
        }
    }
};

Station voice(double off, Side s, double f0, int seed, double talk, double sil, double start, double gain) {
    Station st; st.offsetHz = off; st.side = s; st.f0 = f0; st.seed = seed; st.talkSec = talk; st.silenceSec = sil; st.startSec = start; st.gain = gain;
    return st;
}
Station decoy(Station::Kind k, double off, int seed, double gain, double talk = 10, double sil = 3) {
    Station st; st.kind = k; st.offsetHz = off; st.seed = seed; st.gain = gain; st.talkSec = talk; st.silenceSec = sil; return st;
}
const Conversation* nearConv(const std::vector<Conversation>& l, double hz, double tol = 250) {
    for (const Conversation& c : l) if (std::fabs(c.dialHz - hz) <= tol) return &c;
    return nullptr;
}
}  // namespace

static void testFormats() {
    CHECK(sText(30) == "S6", "S text %s", sText(30).c_str());
    CHECK(sText(60) == "S9+12", "S9+ text %s", sText(60).c_str());
    CHECK(conventionSide(7.1e6) == Side::Lsb && conventionSide(14.2e6) == Side::Usb && conventionSide(5.3665e6) == Side::Usb && conventionSide(3.7e6) == Side::Lsb, "convention");
    CHECK(bandInfo(7.1e6).band == "40 m" && bandInfo(7.1e6).service == "Amateur", "40 m label");
    CHECK(bandInfo(4.125e6).service == "Maritime SSB", "maritime label %s", bandInfo(4.125e6).service.c_str());
    CHECK(bandInfo(5.5e6).service == "Aviation HF", "aviation label");
    CHECK(bandInfo(27.5e6 - 200e3).service == "CB (27 MHz)", "CB label");
    const auto c1 = findAmateurCallsigns("this is W1ABC calling CQ, DL1XYZ here, also whiskey two alpha bravo charlie and four x-ray one zulu");
    CHECK(c1.size() >= 3 && c1[0] == "W1ABC" && c1[1] == "DL1XYZ", "callsigns direct (%zu)", c1.size());
    const auto c2 = findAmateurCallsigns("kilo five tango echo sierra");
    CHECK(c2.size() == 1 && c2[0] == "K5TES", "phonetic callsign (%s)", c2.empty() ? "" : c2[0].c_str());
    CHECK(findAmateurCallsigns("the weather is fine and the band is open").empty(), "no callsign in plain text");
}

static VoiceFeatures analyse(const std::vector<Station>& st, double secs, double noisePow, float* tilt = nullptr) {
    const std::vector<cf> x = synth::make(8000, secs, st, noisePow, 3);
    VoiceAnalyzer va;
    const size_t w = 4000;
    for (size_t i = 0; i + w <= x.size(); i += w) va.process(x.data() + i, w, 8000, 10 * std::log10(noisePow));
    if (tilt) *tilt = va.features().tiltDb;
    return va.features();
}

static void testVoiceAnalyzer() {
    Station v = voice(0, Side::Usb, 120, 2, 1e9, 0, 0, 1.0);
    float tilt = 0;
    VoiceFeatures f = analyse({v}, 14, 1e-4, &tilt);
    printf("voice: score %.2f depth %.1f mod %.2f voiced %.2f tone %.1f band %.2f tilt %.1f pitch %.0f\n", f.score, f.depthDb, f.modRatio, f.voiced, f.toneDb, f.bandFrac, f.tiltDb, f.pitchHz);
    CHECK(f.score > 0.55f, "speech scores high (%.2f)", f.score);
    CHECK(f.pitchHz > 90 && f.pitchHz < 160, "pitch near 120 Hz (%.0f)", f.pitchHz);
    CHECK(tilt > 3, "USB speech has a positive tilt (%.1f)", tilt);
    Station v2 = voice(0, Side::Usb, 200, 9, 1e9, 0, 0, 1.0);
    f = analyse({v2}, 14, 1e-4);
    CHECK(f.score > 0.55f && f.pitchHz > 160 && f.pitchHz < 260, "female-range pitch scores too (%.2f, %.0f Hz)", f.score, f.pitchHz);
    Station l = voice(0, Side::Lsb, 120, 2, 1e9, 0, 0, 1.0);
    f = analyse({l}, 14, 1e-4, &tilt);
    CHECK(f.score < 0.2f, "the wrong sideband at the carrier is not taken for speech (%.2f)", f.score);
    struct D { Station::Kind k; const char* n; };
    const D ds[] = {{Station::Ale8Fsk, "ALE 8-FSK"}, {Station::MultiTone, "multi-tone modem"}, {Station::Noise, "band noise"}, {Station::Tone, "carrier"}, {Station::Rtty, "RTTY"}, {Station::Psk31, "PSK31"}};
    for (const D& d : ds) {
        Station s = decoy(d.k, 0, 5, 1.0, 1e9, 0);
        f = analyse({s}, 14, 1e-4);
        printf("decoy %-16s score %.2f depth %.1f mod %.2f voiced %.2f tone %.1f\n", d.n, f.score, f.depthDb, f.modRatio, f.voiced, f.toneDb);
        CHECK(f.score < 0.3f, "%s must not look like speech (%.2f)", d.n, f.score);
    }
    VoiceAnalyzer va;   // nothing but noise
    const std::vector<cf> n = synth::make(8000, 10, {}, 1e-4, 4);
    for (size_t i = 0; i + 4000 <= n.size(); i += 4000) va.process(n.data() + i, 4000, 8000, -40);
    CHECK(va.features().score < 0.1f, "noise is not speech (%.2f)", va.features().score);
}

static void testBook() {
    BookOptions bo; bo.conversationGapSec = 30;
    ConversationBook b(bo);
    auto det = [](double t, double hz, bool voice, float pitch, double since = 0) {
        Detection d; d.trackId = 1; d.time = t; d.dialHz = hz; d.confirmed = true; d.voiceNow = voice; d.side = Side::Lsb; d.bwHz = 2500; d.snrDb = 30; d.pitchHz = pitch; d.sinceT = since;
        d.audio.assign(4000, 0.f); d.audioRate = 8000; return d;
    };
    // alternate two speakers (pitch 110 / 190, offsets -40 / +40 Hz) every 6 s for 2 minutes with 3 s gaps, drifting +1 Hz/s
    double t = 100; int nOver = 0;
    std::vector<BookEvent> ev;
    for (int o = 0; o < 10; o++) {
        const bool a = (o % 2) == 0;
        for (int k = 0; k < 12; k++) {   // 6 s of 0.5 s windows
            t += 0.5;
            auto e = b.update(t, {det(t, 7.1e6 + (a ? -40 : 40) + (t - 100) * 1.0, true, a ? 110.f : 190.f, 100)});
            ev.insert(ev.end(), e.begin(), e.end());
        }
        nOver++;
        for (int k = 0; k < 6; k++) { t += 0.5; auto e = b.update(t, {det(t, 7.1e6 + (t - 100), false, 0.f)}); ev.insert(ev.end(), e.begin(), e.end()); }
    }
    Filter all; all.showMuted = true;
    auto l = b.list(t, all);
    CHECK(l.size() == 1, "one conversation for the alternating pair (%zu)", l.size());
    if (!l.empty()) {
        const Conversation& c = l[0];
        CHECK(c.overs == nOver, "10 overs counted (%d)", c.overs);
        CHECK(c.speakers == 2, "two speakers (%d)", c.speakers);
        CHECK(c.kind == Kind::Qso, "kind QSO (%s)", kindName(c.kind));
        CHECK(c.startT <= 100.5, "start time back-dated to the first activity (%.1f)", c.startT);
        CHECK(c.active, "active now");
        CHECK(std::fabs(c.dialHz - (7.1e6 + 50)) < 100, "frequency followed the drift (%.0f)", c.dialHz - 7.1e6);
        CHECK(c.band == "40 m" && c.service == "Amateur", "labels");
    }
    int nNew = 0; for (const auto& e : ev) if (e.type == EventType::New) nNew++;
    CHECK(nNew == 1, "one NEW event (%d)", nNew);
    // the end: 31 s without voice
    auto e1 = b.tick(t + 20);
    CHECK(e1.empty(), "not ended after 20 s");
    auto e2 = b.tick(t + 35);
    CHECK(e2.size() == 1 && e2[0].type == EventType::Ended, "ended event after the gap");
    l = b.list(t + 35, all);
    CHECK(!l.empty() && l[0].ended && !l[0].active, "ended and not active");
    // a net: 4 stations, short overs, on another frequency
    double t2 = 400;
    for (int o = 0; o < 14; o++) {
        const float p = 100.f + 35.f * (float)(o % 4);
        for (int k = 0; k < 4; k++) { t2 += 0.5; b.update(t2, {det(t2, 3.6e6 + 10 * (o % 4), true, p, 400)}); }
        for (int k = 0; k < 5; k++) { t2 += 0.5; b.update(t2, {det(t2, 3.6e6, false, 0)}); }
    }
    l = b.list(t2, all);
    const Conversation* net = nearConv(l, 3.6e6, 100);
    CHECK(net && net->kind == Kind::Net && net->speakers >= 3, "a net is recognised (%s, %d speakers)", net ? kindName(net->kind) : "none", net ? net->speakers : 0);
    // gap longer than the setting makes a new conversation on the same frequency
    b.tick(t2 + 40);
    for (int k = 0; k < 6; k++) { t2 += 0.5 + 0.0; b.update(t2 + 45 + k * 0.5, {det(t2 + 45 + k * 0.5, 3.6e6, true, 100.f, 0)}); }
    CHECK(b.size() == 3, "a second conversation after the gap (%zu)", b.size());
    // filters, sort, favourites, mute, best active
    Filter f; f.band = "40 m";
    CHECK(b.list(t2 + 60, f).size() == 1, "band filter");
    f = Filter(); f.activeOnly = true;
    auto act = b.list(t2 + 48, f);
    CHECK(act.size() == 1 && nearConv(act, 3.6e6, 100), "active only (%zu)", act.size());
    f = Filter(); f.hideShortSec = 100;
    CHECK(b.list(t2 + 48, f).size() < b.size(), "hide short");
    const uint32_t best = b.bestActive(t2 + 48);
    CHECK(best != 0 && b.find(best) && b.find(best)->active, "best active");
    CHECK(b.bestActive(t2 + 48, {best}) == 0, "excluded best leaves none");
    b.setFavorite(best, true);
    CHECK(b.find(best)->favorite && b.isFavoriteHz(3.6e6), "favourite");
    b.setMuted(best, true);
    CHECK(b.list(t2 + 48).size() == b.size() - 1, "muted hidden");
    CHECK(b.bestActive(t2 + 48) == 0, "muted is never the best");
    b.update(t2 + 50, {det(t2 + 50, 3.6e6 + 20, true, 100.f)});
    CHECK(b.size() == 3, "muted frequency creates nothing new");
    b.setMuted(best, false);
    b.addClue(best, "CQ this is W1ABC");
    CHECK(b.find(best)->clues.size() == 1 && b.find(best)->clues[0] == "W1ABC", "clue");
    const std::string csv = b.toCsv(t2), js = b.toJson(t2);
    CHECK(csv.find("id,frequency_mhz") == 0 && csv.find("W1ABC") != std::string::npos && csv.find("40 m") != std::string::npos, "csv");
    CHECK(js.find("\"conversations\"") != std::string::npos && js.find("W1ABC") != std::string::npos && js.find("overs_list") != std::string::npos, "json");
    auto tl = b.timeline(t2 + 60, 1.0);
    CHECK(tl.size() == 2 && !tl[0].bars.empty(), "timeline rows (%zu)", tl.size());
    CHECK(ConversationBook::fmtTime(0) == "1970-01-01 00:00:00", "time format");
}

static void testAutoListen() {
    ConversationBook b;
    auto det = [](double t, double hz, float snr, uint32_t tr) {
        Detection d; d.trackId = tr; d.time = t; d.dialHz = hz; d.confirmed = true; d.voiceNow = true; d.side = Side::Usb; d.snrDb = snr; d.audio.assign(4000, 0.f); d.audioRate = 8000; return d;
    };
    AutoListen al; al.enabled = true;
    double t = 10;
    b.update(t, {det(t, 14.2e6, 20, 1), det(t, 14.3e6, 35, 2), det(t, 14.4e6, 10, 3)});
    uint32_t cur = al.update(b, t);
    CHECK(cur != 0 && std::fabs(b.find(cur)->dialHz - 14.3e6) < 10, "auto-listen picks the strongest active conversation");
    al.skip(cur);
    cur = al.update(b, t);
    CHECK(cur != 0 && std::fabs(b.find(cur)->dialHz - 14.2e6) < 10, "skip moves to the next one");
    al.hold = true;
    // the held one goes quiet, the other stays active: with hold it stays
    for (int i = 0; i < 20; i++) { t += 0.5; b.update(t, {det(t, 14.3e6, 35, 2), det(t, 14.4e6, 10, 3)}); b.tick(t); al.update(b, t); }
    CHECK(al.current() == cur, "hold keeps the conversation");
    al.hold = false;
    uint32_t sw = 0;
    for (int i = 0; i < 20; i++) { t += 0.5; b.update(t, {det(t, 14.3e6, 35, 2), det(t, 14.4e6, 10, 3)}); b.tick(t); sw = al.update(b, t); }
    CHECK(sw != cur && std::fabs(b.find(sw)->dialHz - 14.4e6) < 10, "switches to the best non-skipped active one after the delay");
    CHECK(al.gain(t) >= 0.0 && al.gain(t) <= 1.0, "gain range");
    CHECK(al.gain(t - 1.0 + 0.0) <= 1.0, "fade");
}

static void testSweep() {
    SweepPlanner sp({{14.0e6, 14.35e6, "20 m"}, {7.0e6, 7.2e6, "40 m"}}, 192000, 5, 20);
    CHECK(sp.steps() >= 3, "steps (%zu)", sp.steps());
    CHECK(sp.update(0, 0), "first step retunes");
    const double c0 = sp.centreHz();
    CHECK(!sp.update(2, 0), "dwell");
    CHECK(sp.update(5.1, 0) && sp.centreHz() != c0, "moves on after the dwell");
    const double c1 = sp.centreHz();
    CHECK(!sp.update(15, 2), "stays while voice is active");
    CHECK(sp.update(26, 2) && sp.centreHz() != c1, "moves on after the maximum dwell");
    size_t n = 0; for (int i = 0; i < 100; i++) if (sp.update(100 + i * 6, 0)) n++;
    CHECK(n > 50, "keeps stepping");
}

static void testClips() {
    ClipRecorder r("/tmp");
    Conversation c; c.id = 777; c.dialHz = 7.143e6; c.side = Side::Lsb; c.band = "40 m"; c.service = "Amateur"; c.clues = {"DL1XYZ"};
    std::vector<float> a(8000, 0.1f);
    r.append(c, a.data(), a.size(), 8000, 1.7e9);
    r.append(c, a.data(), a.size(), 8000, 1.7e9 + 1);
    const std::string p = r.pathFor(777);
    CHECK(!p.empty(), "clip opened");
    const std::string w = r.finish(c, 1.7e9 + 2);
    CHECK(w == p, "clip finished");
    std::ifstream f(w, std::ios::binary);
    std::string hdr(44, 0); f.read(&hdr[0], 44);
    CHECK(hdr.substr(0, 4) == "RIFF" && hdr.substr(8, 4) == "WAVE", "wav header");
    f.seekg(0, std::ios::end);
    CHECK((long)f.tellg() == 44 + 16000 * 2, "wav size %ld", (long)f.tellg());
    std::ifstream m(w.substr(0, w.size() - 4) + ".json");
    std::stringstream ss; ss << m.rdbuf();
    CHECK(ss.str().find("DL1XYZ") != std::string::npos && ss.str().find("LSB") != std::string::npos, "metadata json");
    remove(w.c_str()); remove((w.substr(0, w.size() - 4) + ".json").c_str());
}

static void testScannerScenario() {
    const double rate = 96000, centre = 14.2e6;
    // A and B alternate on 14.2200 MHz (offsets +20000 and +20055, pitches 115 / 190); C is LSB on 14.1600 (against the convention) and drifts 2 Hz/s;
    // D is a second QSO far away; decoys everywhere else.
    std::vector<Station> st;
    st.push_back(voice(20000, Side::Usb, 115, 11, 9, 11, 0, 0.30));   st.back().fadeDepthDb = 8;
    st.push_back(voice(20055, Side::Usb, 190, 12, 9, 11, 10, 0.26));  st.back().fadeDepthDb = 8; st.back().fadeRateHz = 0.4;
    st.push_back(voice(-40000, Side::Lsb, 135, 13, 12, 6, 3, 0.28));  st.back().driftHzPerSec = 2.0;
    st.push_back(decoy(Station::Ale8Fsk, 36000, 21, 0.30));
    st.push_back(decoy(Station::MultiTone, -12000, 22, 0.20));
    st.push_back(decoy(Station::Rtty, 8000, 23, 0.25));
    st.push_back(decoy(Station::Psk31, -26000, 24, 0.25));
    st.push_back(decoy(Station::Ft8, 43000, 25, 0.25, 12.6, 2.4));
    st.push_back(decoy(Station::Noise, 30000, 26, 0.20, 14, 1));
    st.push_back(decoy(Station::Tone, -33000, 27, 0.15));
    Station am; am.kind = Station::Am; am.offsetHz = -4000 - 0; am.seed = 28; am.gain = 0.35; am.f0 = 130; st.push_back(am);
    const double T = 70;
    const std::vector<cf> iq = synth::make(rate, T, st, 0.05, 5);
    Run r;
    r.feed(iq, rate, centre);
    // end the conversations: a little silence at the end
    r.events = r.events; r.book.tick(T + 40);
    Filter all; all.showMuted = true;
    const auto l = r.book.list(T + 40, all, SortBy::Frequency);
    for (const Conversation& c : l) printf("  conv %u %.4f MHz %s %s overs %d speakers %d kind %s snr %.1f talk %.0f s dur %.0f\n", c.id, c.dialHz / 1e6, sideName(c.side), c.service.c_str(), c.overs, c.speakers, kindName(c.kind), c.snrDb, c.talkSec, c.lastVoiceT - c.startT);
    for (const Conversation& c : l) { printf("    overs:"); for (const Over& o : c.overList) printf(" [%.1f-%.1f s%d]", o.t0, o.t1, o.speaker); printf("\n"); }
    CHECK(l.size() == 2, "exactly two conversations (A+B, C), no decoy (got %zu)", l.size());
    const Conversation* ab = nearConv(l, centre + 20000, 200);
    CHECK(ab != nullptr, "A/B found near 14.220 MHz");
    if (ab) {
        CHECK(ab->side == Side::Usb, "A/B is USB");
        CHECK(ab->speakers == 2, "A/B has two speakers (%d)", ab->speakers);
        CHECK(ab->overs >= 4 && ab->overs <= 8, "A/B overs (%d)", ab->overs);
        CHECK(std::fabs(ab->snrDb) > 3, "A/B strength");
    }
    const Conversation* cc = nearConv(l, centre - 40000, 300);
    CHECK(cc != nullptr, "C found near 14.160 MHz (drifting)");
    if (cc) {
        CHECK(cc->side == Side::Lsb, "C is LSB although the band says USB");
        CHECK(cc->overs >= 3, "C overs (%d)", cc->overs);
        CHECK(cc->service == "Amateur" && cc->band == "20 m", "C is labelled 20 m amateur");
    }
    bool decoyConv = false;
    for (const Conversation& c : l) {
        const double off = c.dialHz - centre;
        for (double d : {36000.0, -12000.0, 8000.0, -26000.0, 43000.0, 30000.0, -33000.0, -4000.0}) if (std::fabs(off - d) < 3500) decoyConv = true;
    }
    CHECK(!decoyConv, "no conversation on a decoy");
    int newEv = 0;
    for (const auto& e : r.events) if (e.type == EventType::New) newEv++;
    CHECK(newEv == 2, "two NEW events (%d)", newEv);
    printf("  window analysis time: %.1f%% of real time\n", r.sc.busy() * 100);
    // a digital veto that says "digital" for everything keeps the book empty
    ScannerOptions so; so.digitalVeto = [](const cf*, size_t, double, double) { return true; };
    Run v(so);
    v.feed(iq, rate, centre);
    CHECK(v.book.size() == 0, "veto hook blocks everything (%zu)", v.book.size());
}

static void testNoiseAndConvention() {
    const double rate = 96000;
    Run n;
    n.feed(synth::make(rate, 30, {}, 0.05, 9), rate, 7.1e6);
    CHECK(n.book.size() == 0 && n.all.empty() && n.sc.trackCount() == 0, "noise alone: nothing (%zu tracks)", n.sc.trackCount());
    // USB on 40 m (against the LSB convention), a weaker signal, strong fading, and a second centre
    std::vector<Station> st;
    st.push_back(voice(15000, Side::Usb, 125, 31, 10, 4, 0, 0.25)); st.back().fadeDepthDb = 8;
    Run u;
    u.feed(synth::make(rate, 35, st, 0.05, 8), rate, 7.1e6);
    Filter all; all.showMuted = true;
    const auto l = u.book.list(40, all);
    CHECK(l.size() == 1 && l[0].side == Side::Usb, "USB on 40 m is found as USB, not forced to LSB (%zu)", l.size());
    if (!l.empty()) CHECK(std::fabs(l[0].dialHz - (7.1e6 + 15000)) < 200, "frequency %.0f", l[0].dialHz - 7.1e6);
}

static void testRates() {
    // a wider capture (1.92 Msps, 20 kHz-offset station) goes through the same path
    const double rate = 1920000;
    std::vector<Station> st;
    st.push_back(voice(-300000, Side::Lsb, 118, 41, 1e9, 0, 0, 0.5));
    Run r;
    r.feed(synth::make(rate, 9, st, 0.004, 2), rate, 3.7e6);
    Filter all; all.showMuted = true;
    const auto l = r.book.list(12, all);
    CHECK(l.size() == 1 && l[0].side == Side::Lsb, "1.92 Msps capture: LSB station found (%zu)", l.size());
    if (!l.empty()) CHECK(std::fabs(l[0].dialHz - (3.7e6 - 300000)) < 250, "1.92 Msps frequency %.0f", l[0].dialHz - 3.7e6);
    // 2.048 Msps: the decimation does not land on 48 kHz / 8 kHz exactly (47.6 kHz, 7.94 kHz audio): everything is measured with the real rates
    const double r2 = 2048000;
    std::vector<Station> s2;
    s2.push_back(voice(250000, Side::Usb, 128, 43, 1e9, 0, 0, 0.5));
    Run q;
    q.feed(synth::make(r2, 9, s2, 0.004, 4), r2, 10.12e6);
    const auto l2 = q.book.list(12, all);
    CHECK(l2.size() == 1 && l2[0].side == Side::Usb, "2.048 Msps capture: USB station found (%zu)", l2.size());
    if (!l2.empty()) CHECK(std::fabs(l2[0].dialHz - (10.12e6 + 250000)) < 250, "2.048 Msps frequency %.0f", l2[0].dialHz - 10.12e6);
}

static void testImpairmentSweep() {
    // every offset (near both edges of the span, close to the DC spike), both sidebands, three pitches, three levels: each is a station that must be found
    // once, with the right sideband and a frequency within 250 Hz. Losses are printed so that a weak spot is visible.
    const double rate = 96000, centre = 10.0e6;
    const double offs[] = {-43000, -31000, -9000, 1500, 7000, 26000, 43000};
    const double f0s[] = {105, 150, 210};
    const double gains[] = {0.5, 0.3, 0.2};   // about 24, 19 and 16 dB over the noise in the band (before the fading)
    int tried = 0, found = 0, wrongSide = 0, badFreq = 0, extra = 0, weakTried = 0, weakFound = 0;
    int k = 0;
    for (double off : offs) for (Side sd : {Side::Usb, Side::Lsb}) {
        const double f0 = f0s[k % 3]; const double g = gains[(k / 2) % 3]; k++;
        std::vector<Station> st; st.push_back(voice(off, sd, f0, 50 + k, 9, 3, 0, g)); st.back().fadeDepthDb = 6;
        Run r; r.feed(synth::make(rate, 22, st, 0.05, 60 + k), rate, centre);
        r.book.tick(100);
        Filter all; all.showMuted = true;
        const auto l = r.book.list(100, all);
        const bool weak = g < 0.25;   // ~16 dB nominal and 6 dB of fading: reported, not required (the limit of the method is documented)
        if (weak) { weakTried++; if (!l.empty()) weakFound++; } else tried++;
        if (l.empty()) { if (weak) continue; printf("  sweep: off %+.0f %s f0 %.0f gain %.2f: NOT FOUND\n", off, sideName(sd), f0, g); continue; }
        if (!weak) found++;
        extra += (int)l.size() - 1;
        if (l.size() > 1) for (const Conversation& c : l) printf("  sweep: off %+.0f %s f0 %.0f gain %.2f: conv at %+.0f %s overs %d talk %.1f\n", off, sideName(sd), f0, g, c.dialHz - centre, sideName(c.side), c.overs, c.talkSec);
        if (l[0].side != sd) { wrongSide++; printf("  sweep: off %+.0f %s: wrong side %s\n", off, sideName(sd), sideName(l[0].side)); }
        if (std::fabs(l[0].dialHz - (centre + off)) > 250) { badFreq++; printf("  sweep: off %+.0f %s: freq off by %.0f Hz\n", off, sideName(sd), l[0].dialHz - centre - off); }
    }
    printf("  sweep: weak stations (16 dB, fading): %d of %d found\n", weakFound, weakTried);
    printf("  sweep: %d stations, %d found, %d wrong side, %d bad frequency, %d extra conversations\n", tried, found, wrongSide, badFreq, extra);
    CHECK(found == tried, "all sweep stations found (%d of %d)", found, tried);
    CHECK(wrongSide == 0 && badFreq == 0 && extra == 0, "sweep: side / frequency / duplicates");
}

static void testDemoScenario() {
    // what the Demo of the panel shows: a QSO, an LSB conversation, a net that ended, a single call that ended, a weak station; nothing from the decoys
    const synth::Demo d = synth::demo();
    BookOptions bo; bo.conversationGapSec = 10;
    Run r(ScannerOptions(), bo);
    r.feed(synth::make(d.rateHz, d.seconds, d.stations, d.noisePower, 3), d.rateHz, d.centreHz);
    Filter all; all.showMuted = true;
    const auto l = r.book.list(d.seconds, all, SortBy::Frequency);
    for (const Conversation& c : l) printf("  demo conv %.4f MHz %s overs %d speakers %d %s %s %s\n", c.dialHz / 1e6, sideName(c.side), c.overs, c.speakers, kindName(c.kind), sText(c.snrDb).c_str(), c.ended ? "ended" : c.active ? "ACTIVE" : "quiet");
    int active = 0, ended = 0;
    for (const Conversation& c : l) { if (c.active && !c.ended) active++; if (c.ended) ended++; }
    CHECK(l.size() >= 4 && l.size() <= 6, "demo: four to six conversations (%zu)", l.size());
    CHECK(active >= 2 && ended >= 1, "demo: active %d, ended %d", active, ended);
    const Conversation* net = nearConv(l, d.centreHz + 33030, 300);
    CHECK(net != nullptr && net->ended, "demo: the net is found and has ended");
}

// Adjacent channels and narrow AM: two SSB stations 3.3 and 4 kHz apart that talk at the same time are two conversations (the neighbour's edge is inside
// the other's audio channel, on the far side of its carrier: the sideband filter keeps it out), and a 3 kHz wide AM (with and without carrier) is not SSB.
static void addAm(std::vector<cf>& iq, double rate, double off, double lpHz, double carrier, double gain, int seed) {
    std::vector<float> v = synth::voiceAudio((double)iq.size() / rate, 130, seed);
    const double a = std::exp(-2 * M_PI * lpHz / 8000);
    float y1 = 0, y2 = 0, y3 = 0, mx = 1e-9f;
    for (float& x : v) { y1 = (float)(a * y1 + (1 - a) * x); y2 = (float)(a * y2 + (1 - a) * y1); y3 = (float)(a * y3 + (1 - a) * y2); x = y3; mx = std::max(mx, std::fabs(x)); }
    double ph = 0;
    for (size_t i = 0; i < iq.size(); i++) {
        const double p = (double)i * 8000.0 / rate; const size_t k = (size_t)p;
        if (k + 1 >= v.size()) break;
        const double au = v[k] * (1 - (p - (double)k)) / mx + v[k + 1] * (p - (double)k) / mx;
        ph += 2 * M_PI * off / rate;
        const double env = gain * (carrier + 0.8 * au);
        iq[i] += cf((float)(env * std::cos(ph)), (float)(env * std::sin(ph)));
    }
}

static void testNeighboursAndAm() {
    const double rate = 96000, centre = 14.2e6;
    Filter all; all.showMuted = true;
    for (double sp : {4000.0, 3300.0}) {
        std::vector<Station> st;
        st.push_back(voice(10000, Side::Usb, 115, 11, 9, 5, 0, 0.3));
        st.push_back(voice(10000 + sp, Side::Usb, 170, 12, 7, 6, 1, 0.3));
        Run r; r.feed(synth::make(rate, 60, st, 0.05, 3), rate, centre);
        const auto l = r.book.list(120, all, SortBy::Frequency);
        CHECK(l.size() == 2, "two simultaneous stations %.0f Hz apart are two conversations (%zu)", sp, l.size());
        CHECK(nearConv(l, centre + 10000, 300) && nearConv(l, centre + 10000 + sp, 300), "both found at %.0f Hz spacing", sp);
    }
    for (double car : {1.0, 0.0}) {
        std::vector<cf> iq = synth::make(rate, 50, {}, 0.05, 3);
        addAm(iq, rate, 12000, 1500, car, car > 0 ? 0.4 : 0.5, 5);
        Run r; r.feed(iq, rate, centre);
        CHECK(r.book.list(120, all).empty(), "3 kHz wide AM (carrier %.0f) is not SSB", car);
    }
}

int main() {
    testFormats();
    testVoiceAnalyzer();
    testBook();
    testAutoListen();
    testSweep();
    testClips();
    testScannerScenario();
    testNoiseAndConvention();
    testRates();
    testImpairmentSweep();
    testNeighboursAndAm();
    testDemoScenario();
    return testSummary("ssbconv");
}
