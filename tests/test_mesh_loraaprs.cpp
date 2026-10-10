// LoRa APRS and MeshCom in the mesh mode.
// 1. Packet layer: LoRa APRS (prefix "<\xff\x01" + TNC2) and MeshCom 4 frames built as the firmwares build them decode to the exact
//    calls, positions, messages and objects; malformed, truncated, non-ASCII and random payloads are refused or cleaned, never crash.
// 2. Radio: single LoRa APRS frames (SF12, 125 kHz, sync 0x12) through MeshReceiver with the faults of tests/impair.h applied
//    independently of the generator: -15 dB SNR in the LoRa band, +-22 kHz tuning error (50 ppm at 433 MHz), +-100 ppm sample clock,
//    a capture that starts inside a frame, two colliding frames, a frame sent without CRC.
// 3. The 70 cm test scene (mesh_gen region 2): every LoRa APRS and MeshCom packet of a cycle at -15 dB / +22 kHz / +100 ppm.
#include "dect2/mesh_gen.h"
#include "dect2/mesh_lora.h"
#include "dect2/mesh_proto.h"
#include "dect2/mesh_rx.h"
#include "dect2/modes.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) < tol; }

static MeshDecodeResult aprsOf(const std::string& tnc2) {
    const auto b = loraAprsFrame(tnc2);
    return meshDecodeLoraAprs(b.data(), b.size());
}

static void packetLayer() {
    // position, uncompressed
    auto r = aprsOf("A61AB-7>APLRT1,WIDE1-1:!2504.83N/05508.42E>Marina tracker 12.6V");
    CHECK(r.ok && r.hasAprs, "position: %s", r.packet.note.c_str());
    CHECK(r.aprsSource == "A61AB-7" && r.aprsDest == "APLRT1" && r.aprsPath == "WIDE1-1", "header %s %s %s", r.aprsSource.c_str(), r.aprsDest.c_str(), r.aprsPath.c_str());
    CHECK(r.aprs.hasPos && near(r.aprs.lat, 25 + 4.83 / 60) && near(r.aprs.lon, 55 + 8.42 / 60), "pos %.5f %.5f", r.aprs.lat, r.aprs.lon);
    CHECK(r.aprs.symTable == '/' && r.aprs.symCode == '>' && r.aprs.comment == "Marina tracker 12.6V", "sym %c%c '%s'", r.aprs.symTable, r.aprs.symCode, r.aprs.comment.c_str());
    // message with number
    r = aprsOf("A61AB-7>APLRT1,WIDE1-1::A61EF-9  :Meet at the Marina at 9{01");
    CHECK(r.ok && r.aprs.type == "Message" && r.messages.size() == 1, "message type %s", r.aprs.type.c_str());
    if (r.messages.size() == 1) CHECK(r.messages[0].from == "A61AB-7" && r.messages[0].to == "A61EF-9" && r.messages[0].text == "Meet at the Marina at 9", "msg %s -> %s '%s'",
                                      r.messages[0].from.c_str(), r.messages[0].to.c_str(), r.messages[0].text.c_str());
    // object
    r = aprsOf("A61CD-10>APLRG1:;DXBMTG   *071200z2513.00N/05519.00E-Ham meeting Friday");
    CHECK(r.ok && r.aprs.type == "Object" && r.aprs.name.find("DXBMTG") == 0 && near(r.aprs.lat, 25 + 13.0 / 60) && near(r.aprs.lon, 55 + 19.0 / 60), "object %s '%s'", r.aprs.type.c_str(), r.aprs.name.c_str());
    // Mic-E (the destination carries the latitude; APRS101 example values are checked in the packet mode's tests, here only the routing)
    r = aprsOf("A61AB-7>T2SP0W:`(_fn\"Oj/]");
    CHECK(r.ok && r.aprs.type == "Mic-E", "mic-e type %s", r.aprs.type.c_str());
    // trailing NUL / CR LF as some firmwares send
    auto b = loraAprsFrame("A61AB-7>APLRT1:>status\r\n");
    b.push_back(0);
    r = meshDecodeLoraAprs(b.data(), b.size());
    CHECK(r.ok && r.aprs.type == "Status" && r.aprs.text == "status", "status '%s'", r.aprs.text.c_str());
    // UTF-8 kept, broken bytes and controls replaced
    r = aprsOf("A61EF-9>APLRT1:>35\xc2\xb0" "C \xff\x01 end");
    CHECK(r.ok && r.aprs.text == "35\xc2\xb0" "C ?? end", "utf8 '%s'", r.aprs.text.c_str());
    // refused: no prefix, no '>', no ':', bad call, empty
    const char* bad[] = {"A61AB-7>APLRT1:!x", "", "NOHEADER", "A61AB-7:APLRT1>x", ">APLRT1:x", "A61 AB>APLRT1:x", "A61AB>AP LRT1:x"};
    for (int i = 0; i < 7; i++) {
        std::vector<uint8_t> v = i == 0 ? std::vector<uint8_t>(bad[0], bad[0] + strlen(bad[0])) : loraAprsFrame(bad[i]);
        r = meshDecodeLoraAprs(v.data(), v.size());
        CHECK(!r.ok, "accepted bad %d", i);
    }
    // a TNC2 header with an info field the APRS parser does not know: still a packet, type "APRS?"
    r = aprsOf("A61AB-7>APLRT1:\x01\x02");
    CHECK(r.ok, "unknown info refused");

    // MeshCom
    MeshComFrame m;
    m.type = '!'; m.msgId = 0x11223344; m.maxHop = 4; m.sourcePath = "A61MC-2,A61MC-1"; m.dest = "*"; m.payload = "2506.00N/05512.00E[/B=064/A=000123";
    auto mb = meshcomBuild(m);
    r = meshDecodeMeshCom(mb.data(), mb.size());
    CHECK(r.ok && r.packet.type == "POSITION", "meshcom pos: %s", r.packet.note.c_str());
    CHECK(r.aprsSource == "A61MC-2" && r.aprsPath == "A61MC-1" && r.packet.packetId == 0x11223344 && r.packet.hopLimit == 4, "meshcom header %s %s %08x",
          r.aprsSource.c_str(), r.aprsPath.c_str(), r.packet.packetId);
    CHECK(r.aprs.hasPos && near(r.aprs.lat, 25.1) && near(r.aprs.lon, 55.2) && r.aprs.symCode == '[' && r.batteryPct == 64, "meshcom pos %.5f %.5f batt %d", r.aprs.lat, r.aprs.lon, r.batteryPct);
    CHECK(r.aprs.hasAlt && near(r.aprs.altM, 123 * 0.3048, 0.01), "meshcom alt %.2f", r.aprs.altM);
    // the firmware's own log line (aprs_functions.cpp) of a relayed weather position decodes the same way
    m = MeshComFrame(); m.type = ':'; m.msgId = 7; m.sourcePath = "A61MC-1"; m.dest = "A61MC-2"; m.payload = "QSL, 59 here";
    mb = meshcomBuild(m);
    r = meshDecodeMeshCom(mb.data(), mb.size());
    CHECK(r.ok && r.packet.type == "TEXT" && r.messages.size() == 1 && r.messages[0].text == "QSL, 59 here" && r.messages[0].to == "A61MC-2", "meshcom text");
    // checksum
    mb[10] ^= 0x01;
    r = meshDecodeMeshCom(mb.data(), mb.size());
    CHECK(!r.ok && r.packet.note.find("checksum") != std::string::npos, "meshcom bad checksum accepted: %s", r.packet.note.c_str());
    // ack (ack_functions.h): 0x41, id, 0x80 | hops, acknowledged id, 0x01, 0x00
    const uint8_t ack[12] = {0x41, 1, 2, 3, 4, 0x84, 0x44, 0x33, 0x22, 0x11, 0x01, 0x00};
    r = meshDecodeMeshCom(ack, sizeof ack);
    CHECK(r.ok && r.packet.type == "ACK" && r.packet.detail.find("11223344") != std::string::npos, "ack '%s'", r.packet.detail.c_str());
    // a LoRa APRS frame is not MeshCom and the reverse
    b = loraAprsFrame("A61AB-7>APLRT1:>x");
    CHECK(!meshDecodeMeshCom(b.data(), b.size()).ok, "LoRa APRS taken as MeshCom");
    CHECK(!meshDecodeLoraAprs(mb.data(), mb.size()).ok, "MeshCom taken as LoRa APRS");
    // random and truncated bytes: no crash, (almost) never accepted
    std::mt19937 g(5);
    int acc = 0;
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> v((size_t)(g() % 256));
        for (auto& c : v) c = (uint8_t)g();
        if (i % 4 == 0 && !v.empty()) v[0] = "!:@A"[g() % 4];
        acc += meshDecodeMeshCom(v.data(), v.size()).ok + meshDecodeLoraAprs(v.data(), v.size()).ok;
    }
    for (size_t k = 0; k < mb.size(); k++) meshDecodeMeshCom(mb.data(), k);
    CHECK(acc <= 2, "random payloads accepted %d times", acc);
    // presets
    const auto ap = loraAprsPresets();
    CHECK(ap.size() == 4 && ap[0].freqHz == 433.775e6 && ap[0].sf == 12 && ap[0].bwHz == 125000 && ap[0].cr == 5 && ap[0].syncWord == 0x12 && ap[0].ldro, "LoRa APRS EU preset");
    CHECK(ap[1].freqHz == 434.855e6 && ap[1].sf == 9 && ap[1].cr == 7 && !ap[1].ldro, "LoRa APRS PL preset");
    const auto mc = meshcomPresets();
    CHECK(!mc.empty() && mc[0].freqHz == 433.175e6 && mc[0].sf == 11 && mc[0].bwHz == 250000 && mc[0].cr == 6 && mc[0].syncWord == 0x2B, "MeshCom EU preset");
}

// ---- radio: single frames through the receiver with impair.h faults ----
struct Shot { std::string tnc2; double startSec; double relDb = 0; bool crc = true; };

static MeshTelemetry runFrames(const std::vector<Shot>& shots, double snrDb, double shiftHz, double ppm, double secs, size_t skipSamples, uint32_t seed, bool swapIq = false) {
    const double rate = 1.2e6, chan = -300e3;                // the receiver tuned to 433.775 MHz, the channel 300 kHz below the radio's centre
    std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
    lora::Params p;
    for (const auto& s : loraAprsPresets()) if (s.name == "LoRa APRS EU") { p.sf = s.sf; p.bwHz = s.bwHz; p.cr = s.cr; p.preamble = s.preamble; p.syncWord = s.syncWord; p.ldro = s.ldro; }
    const float amp = 0.2f;
    for (const auto& s : shots) {
        lora::Params q = p;
        q.crc = s.crc;
        const auto bytes = loraAprsFrame(s.tnc2);
        lora::TxFrame f;
        f.p = q; f.data = lora::encode(q, bytes.data(), bytes.size());
        f.startSec = s.startSec; f.freqHz = chan; f.amp = amp * (float)std::pow(10.0, s.relDb / 20); f.phase0 = 0.3;
        f.render(x.data(), x.size(), 0, rate);
    }
    impair::shift(x, shiftHz, rate);
    if (ppm != 0) x = impair::clock(x, ppm);
    // noise for snrDb in the LoRa bandwidth of the frame at relDb 0
    std::mt19937 g(seed);
    std::normal_distribution<float> nd(0.f, (float)std::sqrt(amp * amp * (rate / p.bwHz) / std::pow(10.0, snrDb / 10) / 2));
    for (auto& v : x) v += cf32(nd(g), nd(g));
    impair::skip(x, skipSamples);
    if (swapIq) impair::swapIq(x);
    MeshReceiver rx;
    rx.setTunedHz(433.775e6);
    rx.setSignalOffset(chan);
    rx.configure(rate);
    for (size_t i = 0; i < x.size(); i += 50000) rx.feed(x.data() + i, std::min<size_t>(50000, x.size() - i));
    MeshTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static const MeshAprsStation* station(const MeshTelemetry& t, const std::string& call) {
    for (const auto& s : t.aprsStations) if (s.st.call == call) return &s;
    return nullptr;
}

static void radio() {
    const std::string pos = "A61AB-7>APLRT1,WIDE1-1:!2504.83N/05508.42E>Marina tracker 12.6V";
    const std::string msg = "A61CD-10>APLRG1::A61AB-7  :Welcome to the net{12";
    struct Case { const char* name; double shift, ppm; };
    const Case cases[] = {{"clean", 0, 0}, {"+22 kHz +100 ppm", 22e3, 100}, {"-22 kHz -100 ppm", -22e3, -100}, {"+22 kHz -100 ppm", 22e3, -100}};
    for (const auto& c : cases) {
        const MeshTelemetry t = runFrames({{pos, 0.3}, {msg, 3.6}}, -15, c.shift, c.ppm, 7.0, 0, 11);
        const MeshAprsStation* s = station(t, "A61AB-7");
        const bool posOk = s && s->st.hasPos && near(s->st.lat, 25 + 4.83 / 60) && near(s->st.lon, 55 + 8.42 / 60) && s->st.comment == "Marina tracker 12.6V";
        bool msgOk = false;
        for (const auto& m : t.messages) msgOk |= m.protocol == 3 && m.from == "A61CD-10" && m.to == "A61AB-7" && m.text == "Welcome to the net";
        double cfo = 0; for (const auto& p : t.packets) if (p.crcOk) cfo = p.cfoHz;
        printf("  -15 dB %-18s position %s, message %s, %llu good / %llu bad frames, CFO %.0f Hz\n", c.name, posOk ? "exact" : "MISSING", msgOk ? "exact" : "MISSING",
               (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, cfo);
        CHECK(posOk && msgOk, "%s: position %d message %d", c.name, posOk, msgOk);
        CHECK(t.blocksBad == 0, "%s: %llu CRC errors", c.name, (unsigned long long)t.blocksBad);
    }
    // the capture starts 1 s into the first frame: it is lost (or fails), the next one decodes
    {
        const MeshTelemetry t = runFrames({{pos, 0.3}, {msg, 3.6}}, -10, 0, 0, 7.0, (size_t)(1.3 * 1.2e6), 3);
        bool msgOk = false;
        for (const auto& m : t.messages) msgOk |= m.text == "Welcome to the net";
        CHECK(msgOk && !station(t, "A61AB-7"), "mid-frame start: message %d, first frame %s", msgOk, station(t, "A61AB-7") ? "decoded?!" : "lost");
        printf("  start inside a frame: first frame lost, the next decodes: %s\n", msgOk ? "yes" : "NO");
    }
    // collision: a second station 10 dB weaker starts 0.5 s after the first; the stronger frame must survive (capture effect)
    {
        const MeshTelemetry t = runFrames({{pos, 0.3}, {msg, 0.8, -10}}, 0, 5e3, 20, 5.0, 0, 4);
        CHECK(station(t, "A61AB-7") && station(t, "A61AB-7")->st.hasPos, "collision: the stronger frame was lost");
        printf("  collision (second frame 10 dB weaker, 0.5 s later): stronger %s, weaker %s\n", station(t, "A61AB-7") ? "decoded" : "LOST",
               station(t, "A61CD-10") ? "decoded" : "lost (expected)");
    }
    // I and Q swapped: the spectrum is mirrored, up-chirps become down-chirps and the channel moves to +300 kHz. The receiver must not
    // make up packets from it (no LoRa radio decodes a mirrored frame either).
    {
        const MeshTelemetry t = runFrames({{pos, 0.3}, {msg, 3.6}}, 10, 0, 0, 7.0, 0, 6, true);
        CHECK(t.blocksOk == 0 && t.aprsStations.empty(), "swapped IQ: %llu frames decoded", (unsigned long long)t.blocksOk);
        printf("  I/Q swapped: %llu frames (none expected), %llu preambles\n", (unsigned long long)t.blocksOk, (unsigned long long)t.preambles);
    }
    // a frame sent without CRC still decodes (the prefix and the TNC2 header guard it)
    {
        const MeshTelemetry t = runFrames({{pos, 0.3, 0, false}}, -5, 0, 0, 3.5, 0, 5);
        CHECK(station(t, "A61AB-7") && station(t, "A61AB-7")->st.hasPos, "CRC off: not decoded");
    }
}

// ---- the 70 cm scene ----
static void scene() {
    const double rate = 1.2e6, secs = 61;
    SynthConfig cfg;
    cfg.mode = 22;
    cfg.snrDb = -15;
    cfg.modeVal[0] = -15;
    cfg.modeOpt[1] = 2;                      // 70 cm scene
    cfg.modeOpt[3] = 2;                      // exact SNR
    cfg.cfoHz = 22e3;
    cfg.sroPpm = 100;
    auto syn = makeMeshSynth(cfg, rate);
    MeshReceiver rx;
    rx.setTunedHz(433.175e6);
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    rx.configure(rate);
    std::vector<cf32> buf(60000);
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    MeshTelemetry t;
    rx.telemetry(t, 0);
    printf("%s\n", meshSummary(t).c_str());
    for (const auto& d : t.decoders) if (d.inBand) printf("  %-14s %.4f MHz SF%d %5.1f kHz sync 0x%02X: %llu frames, %llu CRC errors\n", d.preset.c_str(), d.freqHz / 1e6, d.sf, d.bwHz / 1e3, d.syncWord,
                                                          (unsigned long long)d.frames, (unsigned long long)d.crcBad);
    for (const auto& s : t.aprsStations)
        printf("  station %d %-9s %c%c pos %d %.5f %.5f batt %d path '%s' last %s, %u frames, '%s'\n", s.protocol, s.st.call.c_str(), s.st.symTable ? s.st.symTable : ' ', s.st.symCode ? s.st.symCode : ' ',
               s.st.hasPos, s.st.lat, s.st.lon, s.batteryPct, s.path.c_str(), s.lastType.c_str(), s.st.count, s.st.comment.c_str());
    for (const auto& m : t.messages) printf("  msg %d [%s] %s -> %s: %s\n", m.protocol, m.channel.c_str(), m.from.c_str(), m.to.c_str(), m.text.c_str());
    int ap = 0, mc = 0;
    for (const auto& p : t.packets) { ap += p.protocol == 3 && p.parsed; mc += p.protocol == 4 && p.parsed; }
    CHECK(ap == 7, "LoRa APRS packets %d of 7", ap);
    CHECK(mc == 5, "MeshCom packets %d of 5", mc);
    CHECK(t.blocksBad == 0, "%llu CRC errors", (unsigned long long)t.blocksBad);
    struct Want { int proto; const char* call; double lat, lon; };
    const Want want[] = {{3, "A61AB-7", 25 + 4.83 / 60, 55 + 8.42 / 60}, {3, "A61CD-10", 25 + 11.1 / 60, 55 + 16.5 / 60}, {3, "A61EF-9", 25.2, 55.3},
                         {3, "DXBMTG", 25 + 13.0 / 60, 55 + 19.0 / 60}, {4, "A61MC-1", 25.085, 55.17}, {4, "A61MC-2", 25.1, 55.2}};
    for (const auto& w : want) {
        const MeshAprsStation* f = nullptr;
        for (const auto& s : t.aprsStations) if (s.protocol == w.proto && s.st.call.rfind(w.call, 0) == 0) f = &s;
        CHECK(f && f->st.hasPos && near(f->st.lat, w.lat) && near(f->st.lon, w.lon), "station %s", w.call);
    }
    const MeshAprsStation* ef = station(t, "A61EF-9");
    CHECK(ef && ef->st.comment.find("Abu Dhabi, 35\xc2\xb0" "C") != std::string::npos, "status with UTF-8: '%s'", ef ? ef->st.comment.c_str() : "");
    CHECK(ef && ef->path == "A61CD-10*,WIDE1*" || (ef && ef->lastType == "Status"), "digipeated path");
    const MeshAprsStation* m2 = station(t, "A61MC-2");
    CHECK(m2 && m2->batteryPct == 64, "MeshCom battery");
    const char* texts[] = {"Meet at the Marina at 9", "Hello MeshCom from the Marina", "QSL, 59 here", "Group 262 check-in"};
    for (const char* x : texts) {
        bool ok = false;
        for (const auto& m : t.messages) ok |= m.text == x;
        CHECK(ok, "message '%s'", x);
    }
}

// MESH_LORAAPRS_SWEEP=1: frame error counts over SNR, tuning error and clock error (4 seeds each), for the record
static void sweep() {
    const std::string msg = "A61CD-10>APLRG1::A61AB-7  :Welcome to the net{12";
    const double snrs[] = {0, -12, -15, -18, -20};
    const double sh[] = {0, 22e3, -22e3}, pp[] = {0, 100, -100};
    for (double s : snrs) for (double a : sh) for (double b : pp) {
        int ok = 0, bad = 0;
        for (uint32_t seed = 1; seed <= 4; seed++) { const auto t = runFrames({{msg, 0.3}}, s, a, b, 3.6, 0, seed); ok += (int)t.blocksOk; bad += (int)t.blocksBad; }
        printf("SNR %5.0f dB, tuning %+6.0f Hz, clock %+4.0f ppm: %d of 4 good, %d CRC errors\n", s, a, b, ok, bad);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::getenv("MESH_LORAAPRS_SWEEP")) { sweep(); return 0; }
    packetLayer();
    radio();
    scene();
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
