// Mesh (LoRa) test signal: see mesh_gen.h.
// STOP: a change here also needs a run of the full mesh rates test, which ctest does not run: test_mesh_rates --full
// (tests/mesh/test_mesh_rates.cpp).
#include "dect2/mesh/mesh_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace dect2 {

namespace {

constexpr double kCycle = 60.0;          // the scene repeats with new packet ids every minute
constexpr uint32_t kUnix0 = 1791360000;  // 2026-10-07 UTC: the nodes' clocks
constexpr uint32_t kBroadcast = 0xFFFFFFFFu;

enum Kind { NodeInfo, Position, Telemetry, EnvTelemetry, Text, Ack, TraceReq, TraceReply, Neighbors, Advert, PublicText };

struct Ev {
    double t;                // offset in the cycle
    Kind kind;
    int node;
    int arg = -1;            // text line, or the node the packet goes to
    int hopStart = 3, hopLimit = 3;
    int path = 0;            // MeshCore: 1 = relayed by the repeater
};

// Meshtastic: (offset, kind, node, line or peer, hop start, hop limit)
const Ev kMt[] = {
    {0.2, NodeInfo, 0}, {1.8, Text, 0, 0}, {3.4, NodeInfo, 1}, {5.0, Text, 1, 1}, {6.6, Position, 0}, {8.2, Telemetry, 0},
    {9.8, NodeInfo, 2}, {11.4, Position, 1}, {13.0, Text, 2, 2}, {14.6, NodeInfo, 3, -1, 3, 2}, {16.2, Text, 3, 3, 3, 2},
    {17.8, Position, 2}, {19.4, Telemetry, 1}, {21.0, NodeInfo, 4}, {22.6, Text, 0, 4}, {24.2, Ack, 3, 0, 3, 2},
    {25.8, NodeInfo, 5, -1, 3, 1}, {27.4, TraceReq, 3, 5, 3, 2}, {29.0, TraceReply, 5, 3, 3, 1}, {30.6, Position, 3, -1, 3, 2},
    {32.2, Telemetry, 2}, {33.8, Text, 4, 5}, {35.4, Position, 4}, {37.0, EnvTelemetry, 3, -1, 3, 2}, {38.6, Telemetry, 3, -1, 3, 2},
    {40.2, Position, 5, -1, 3, 1}, {41.8, Text, 5, 6, 3, 1}, {43.4, Telemetry, 4}, {45.0, Neighbors, 5, -1, 3, 1}, {46.6, Telemetry, 5, -1, 3, 1},
    {48.2, Text, 1, 7},
};
const char* const kMtLines[] = {
    "Good morning mesh, Marina base is up",
    "Morning! Walking along JBR, signal is good here",
    "Kayaking off the Palm, battery at 80%",
    "Downtown rooftop here, I can see the Burj",
    "Stay safe out there on the water",
    "Stuck in traffic on Al Khail road",
    "Jebel Ali relay online, hearing 5 nodes",
    "Coffee at the Marina walk at 9?",
};
// replies (index of the line answered, -1 none)
const int kMtReplyTo[] = {-1, 0, -1, -1, 2, -1, -1, 0};

// MeshCore
const Ev kMc[] = {
    {0.5, Advert, 0}, {3.0, Advert, 1}, {6.0, PublicText, 0, 0}, {9.5, Advert, 2}, {13.0, PublicText, 2, 1},
    {15.0, PublicText, 2, 1, 0, 0, 1}, {20.0, PublicText, 0, 2}, {24.0, PublicText, 2, 3}, {40.0, Advert, 0}, {44.0, PublicText, 0, 4},
};
const char* const kMcLines[] = {
    "Hello from the Marina",
    "Deira checking in",
    "Anyone near Business Bay?",
    "On my way there now",
    "Repeater on the tower is working well",
};

// 70 cm scene (region 2): LoRa APRS stations on 433.775 MHz and MeshCom nodes on 433.175 MHz, both around Dubai (made-up calls)
struct AprsEv { double t; int station; const char* tnc2; };
const AprsEv kAprs[] = {
    {0.5, 0, "A61AB-7>APLRT1,WIDE1-1:!2504.83N/05508.42E>Marina tracker 12.6V"},
    {8.0, 1, "A61CD-10>APLRG1:=2511.10N/05516.50E&LoRa iGate Al Barsha"},
    {16.0, 2, "A61EF-9>APLRT1,A61CD-10*,WIDE1*:/071230z2512.00N/05518.00E_090/005g010t095h60b10080"},
    {24.0, 0, "A61AB-7>APLRT1,WIDE1-1::A61EF-9  :Meet at the Marina at 9{01"},
    {32.0, 1, "A61CD-10>APLRG1:;DXBMTG   *071200z2513.00N/05519.00E-Ham meeting Friday"},
    {40.0, 2, "A61EF-9>APLRT1:>On the road to Abu Dhabi, 35\xc2\xb0" "C"},
    {48.0, 0, "A61AB-7>APLRT1:T#005,199,000,255,073,123,01101001"},
};
struct McomEv { double t; int node; char type; const char* src; const char* dest; const char* payload; };
const McomEv kMcom[] = {
    {1.0, 0, '!', "A61MC-1", "*", "2505.10N/05510.20E#/B=085/A=000040"},
    {7.0, 0, ':', "A61MC-1", "*", "Hello MeshCom from the Marina"},
    {14.0, 1, '!', "A61MC-2,A61MC-1", "*", "2506.00N/05512.00E[/B=064"},
    {21.0, 1, ':', "A61MC-2", "A61MC-1", "QSL, 59 here"},
    {30.0, 1, ':', "A61MC-2", "262", "Group 262 check-in"},
};

// LoRaWAN scene (regions 3 EU868, 4 US915): end devices of a few networks on three uplink channels (made-up addresses and EUIs)
enum LwKind { LwUp, LwConfUp, LwLinkCheck, LwJoin };
struct LwDevDef { uint32_t devAddr; uint64_t devEui, joinEui; int chan, sfEu, sfUs, fport, len; bool adr; uint16_t fcnt0; };
const LwDevDef kLwDevs[] = {
    {0x260B1A2Cu, 0, 0, 0, 7, 7, 1, 10, true, 1200},                                   // The Things Network, temperature sensor
    {0x48000A31u, 0, 0, 1, 9, 9, 2, 11, true, 52},                                     // Helium, tracker
    {0x22A1B2C3u, 0, 0, 2, 12, 10, 3, 6, false, 7001},                                 // Orange, water meter (confirmed, DevStatusAns)
    {0, 0xA84041000181C61Aull, 0xA840410000000101ull, 1, 10, 10, -1, 0, false, 0},       // Dragino, join request
    {0x01ABCDEFu, 0, 0, 0, 8, 8, -1, 0, false, 33},                                    // a private network: LinkCheckReq, no payload
    {0, 0x24E124136B502217ull, 0x24E124C0002A0001ull, 2, 7, 7, -1, 0, false, 0},        // Milesight, join request
};
struct LwEv { double t; int dev; LwKind kind; };
const LwEv kLw[] = {
    {0.5, 0, LwUp}, {2.0, 3, LwJoin}, {4.0, 1, LwUp}, {6.0, 2, LwConfUp}, {10.0, 0, LwUp}, {12.0, 4, LwLinkCheck}, {16.0, 5, LwJoin},
    {20.0, 0, LwUp}, {24.0, 1, LwUp}, {30.0, 0, LwUp}, {36.0, 2, LwConfUp}, {40.0, 0, LwUp}, {46.0, 1, LwUp}, {50.0, 0, LwUp},
};
const double kLwEu[3] = {868.1e6, 868.3e6, 868.5e6}, kLwUs[3] = {903.9e6, 904.1e6, 904.3e6};

struct MtNodeDef { const char* longName; const char* shortName; const char* hw; double lat, lon, alt; int role; };
const MtNodeDef kMtNodes[6] = {
    {"Dubai Marina Base", "DMB1", "TBEAM", 25.0805, 55.1403, 12, 0},
    {"JBR Walker", "JBRW", "HELTEC_V3", 25.0780, 55.1340, 5, 0},
    {"Palm Jumeirah Kayak", "PJK", "T_ECHO", 25.1124, 55.1390, 1, 5},
    {"Downtown Rooftop", "DTR", "RAK4631", 25.1972, 55.2744, 120, 0},
    {"Al Barsha Car", "ABC", "HELTEC_WIRELESS_TRACKER", 25.1130, 55.2000, 20, 5},
    {"Jebel Ali Relay", "JAR", "STATION_G2", 25.0200, 55.1000, 35, 2},
};
struct McNodeDef { const char* name; double lat, lon; bool repeater; uint8_t seed; };
const McNodeDef kMcNodes[3] = {
    {"Marina Companion", 25.0790, 55.1380, false, 11},
    {"Business Bay Repeater", 25.1850, 55.2650, true, 12},
    {"Deira Companion", 25.2700, 55.3100, false, 13},
};

struct Net {
    bool on = false;
    lora::Params p;
    double offsetHz = 0;     // in the samples
    double bwHz = 0;
    double nextFree = 0;     // the frequency is free from here
    int nextEv = 0, cycle = 0;
};

class MeshSynth : public ModeSynth {
public:
    MeshSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfg_(cfg), noise_((uint32_t)(cfg.modeOpt[2] ? cfg.modeOpt[2] : 1) * 7919u + 22u) {
        const int prot = cfg.modeOpt[0] ? (cfg.modeOpt[0] & 31) : 31;
        region_ = cfg.modeOpt[1] >= 1 && cfg.modeOpt[1] <= 5 ? cfg.modeOpt[1] : 0;
        rng_.seed((uint32_t)(cfg.modeOpt[2] ? cfg.modeOpt[2] : 1));
        const double tuneOffset = meshTuning().tuneOffsetHz;
        const bool anz = region_ == 5;
        const double lf = region_ >= 2 && !anz ? 0 : meshtasticSlotHz(region_ == 1 ? "US" : anz ? "ANZ" : "EU_868", "LongFast");
        MeshLoraSettings s;
        if ((prot & 1) && meshtasticPreset("LongFast", s) && lf > 0) {
            mt_.on = true;
            mt_.p = toParams(s);
            mt_.offsetHz = -tuneOffset;
            mt_.bwHz = s.bwHz;
        }
        const MeshLoraSettings c = meshcoreDefaults(region_ == 1 ? "US" : anz ? "AU" : "EU");
        if ((prot & 2) && c.freqHz > 0 && lf > 0) {
            const double off = cfg.modeOpt[4] == 1 ? -tuneOffset : c.freqHz - lf - tuneOffset;   // modeOpt[4] 1: tuned to MeshCore itself
            if (std::fabs(off) + c.bwHz < 0.45 * rate) {
                mc_.on = true;
                mc_.p = toParams(c);
                mc_.offsetHz = off;
                mc_.bwHz = c.bwHz;
            }
        }
        if (region_ == 2) {
            // placed for a receiver tuned to MeshCom EU (433.175 MHz): MeshCom at -tuneOffsetHz, LoRa APRS 600 kHz above it
            // (modeOpt[4] 1: tuned to LoRa APRS EU, 433.775 MHz, instead: LoRa APRS at -tuneOffsetHz, MeshCom 600 kHz below when it fits)
            const double ref = cfg.modeOpt[4] == 1 ? 433.775e6 : 433.175e6;
            for (const auto& x : meshcomPresets()) if (x.name == "MeshCom EU" && (prot & 8)) {
                const double off = 433.175e6 - ref - tuneOffset;
                if (cfg.modeOpt[4] != 1 || std::fabs(off) + x.bwHz < 0.45 * rate) { mcm_.on = true; mcm_.p = toParams(x); mcm_.offsetHz = off; mcm_.bwHz = x.bwHz; }
            }
            for (const auto& x : loraAprsPresets()) if (x.name == "LoRa APRS EU" && (prot & 4)) {
                const double off = x.freqHz - ref - tuneOffset;
                if (std::fabs(off) + x.bwHz < 0.45 * rate) { ap_.on = true; ap_.p = toParams(x); ap_.offsetHz = off; ap_.bwHz = x.bwHz; }
            }
        }
        if (lorawan() && (prot & 16)) {
            // placed for a receiver tuned to the first channel (868.1 / 903.9 MHz): it at -tuneOffsetHz, the others 200 and 400 kHz above
            const double* fr = region_ == 3 ? kLwEu : kLwUs;
            for (int c = 0; c < 3; c++) {
                lwFreeAt_[c] = 0;
                lwOff_[c] = fr[c] - fr[0] - tuneOffset;
                lwOn_[c] = std::fabs(lwOff_[c]) + 125e3 < 0.45 * rate;
            }
            for (int i = 0; i < 6; i++) lwFcnt_[i] = kLwDevs[i].fcnt0;
        }
        // node SNRs: evenly from the strongest (snrDb) to the weakest
        const double strong = cfg.snrDb, weak = cfg.modeVal[0] != 0 ? std::min(cfg.modeVal[0], cfg.snrDb) : cfg.snrDb - 20;
        for (int i = 0; i < 6; i++) mtSnr_[i] = strong - (strong - weak) * i / 5.0;
        for (int i = 0; i < 3; i++) mcSnr_[i] = strong - (strong - weak) * (i + 0.5) / 3.0;
        for (int i = 0; i < 3; i++) apSnr_[i] = strong - (strong - weak) * i / 2.0;
        for (int i = 0; i < 2; i++) mcmSnr_[i] = strong - (strong - weak) * i;
        for (int i = 0; i < 6; i++) lwSnr_[i] = strong - (strong - weak) * i / 5.0;
        // the noise: the strongest LongFast node at an amplitude of 0.35
        noiseRms_ = 0.35 / std::sqrt(std::pow(10.0, strong / 10) * 250e3 / rate);
        dect2::uniform_real_distribution<double> u(-1500, 1500);
        for (int i = 0; i < 6; i++) mtCfo_[i] = u(rng_);
        for (int i = 0; i < 3; i++) mcCfo_[i] = u(rng_);
        for (int i = 0; i < 3; i++) apCfo_[i] = u(rng_);
        for (int i = 0; i < 2; i++) mcmCfo_[i] = u(rng_);
        if (lorawan()) for (int i = 0; i < 6; i++) lwCfo_[i] = u(rng_);   // only then: the other scenes keep their random draws
        for (int i = 0; i < 6; i++) {
            mtNodes_[i].num = 0x2a000000u + (rng_() & 0x00ffffffu);
            mtNodes_[i].longName = kMtNodes[i].longName;
            mtNodes_[i].shortName = kMtNodes[i].shortName;
            mtNodes_[i].hwModel = std::max(0, meshtasticHwModelValue(kMtNodes[i].hw));
            mtNodes_[i].lat = kMtNodes[i].lat; mtNodes_[i].lon = kMtNodes[i].lon; mtNodes_[i].altM = kMtNodes[i].alt;
            mtNodes_[i].role = kMtNodes[i].role;
        }
        for (int i = 0; i < 3; i++) {
            mcNodes_[i].seed = kMcNodes[i].seed;
            mcNodes_[i].name = kMcNodes[i].name;
            mcNodes_[i].lat = kMcNodes[i].lat; mcNodes_[i].lon = kMcNodes[i].lon;
            mcNodes_[i].repeater = kMcNodes[i].repeater;
        }
        uint8_t sd[32], pub[32];
        if (meshcoreNodeKeys(mcNodes_[1], sd, pub)) repeaterHash_ = pub[0];
        nextId_ = 0x10000000u + (rng_() & 0x0fffffffu);
        if (cfg.modeOpt[3] == 2) jitter_ = 0;
        if (cfg.modeOpt[3] == 1) { mt_.nextFree = 1.0; mc_.nextFree = 1.0; ap_.nextFree = 1.0; mcm_.nextFree = 1.0; for (double& x : lwFreeAt_) x = 1.0; }
    }

    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        const double t0 = (double)pos_ / rate_, t1 = (double)(pos_ + n) / rate_;
        schedule(mt_, true, t1 + 1);
        schedule(mc_, false, t1 + 1);
        schedule70(ap_, true, t1 + 1);
        schedule70(mcm_, false, t1 + 1);
        scheduleLw(t1 + 1);
        for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
        for (const auto& f : active_) f.render(out, n, t0, rate_);
        noise_.add(out, n, (float)(noiseRms_ / std::sqrt(2.0)));
        active_.erase(std::remove_if(active_.begin(), active_.end(), [&](const lora::TxFrame& f) { return f.endSec() < t1; }), active_.end());
        pos_ += (int64_t)n;
    }

private:
    static lora::Params toParams(const MeshLoraSettings& s) {
        lora::Params p;
        p.sf = s.sf; p.bwHz = s.bwHz; p.cr = s.cr; p.preamble = s.preamble; p.syncWord = s.syncWord; p.ldro = s.ldro; p.crc = true;
        return p;
    }

    void schedule(Net& net, bool isMt, double until) {
        if (!net.on) return;
        const Ev* evs = isMt ? kMt : kMc;
        const int nev = isMt ? (int)(sizeof(kMt) / sizeof(kMt[0])) : (int)(sizeof(kMc) / sizeof(kMc[0]));
        for (;;) {
            const Ev& e = evs[net.nextEv];
            const double want = net.cycle * kCycle + e.t;
            const double start = std::max(want, net.nextFree);
            if (start > until) return;
            std::vector<uint8_t> bytes = build(e, net.cycle, start);
            if (!bytes.empty()) {
                lora::TxFrame f;
                f.p = net.p;
                f.data = lora::encode(net.p, bytes.data(), bytes.size());
                f.startSec = start;
                f.sroPpm = cfg_.sroPpm;
                const double snr = (isMt ? mtSnr_[e.node] : mcSnr_[e.node]) + dect2::uniform_real_distribution<double>(-1.5, 1.5)(rng_);
                f.amp = (float)std::sqrt(std::pow(10.0, snr / 10) * noiseRms_ * noiseRms_ * net.bwHz / rate_);
                f.freqHz = net.offsetHz + cfg_.cfoHz + (isMt ? mtCfo_[e.node] : mcCfo_[e.node]);
                f.phase0 = dect2::uniform_real_distribution<double>(0, 1)(rng_);
                active_.push_back(f);
                // the next frame on this frequency waits for this one and a pause (the firmware's random back-off)
                net.nextFree = f.endSec() + dect2::uniform_real_distribution<double>(0.3, 0.9)(rng_);
            }
            if (++net.nextEv >= nev) { net.nextEv = 0; net.cycle++; }
        }
    }

    // the 70 cm scene: LoRa APRS (isAprs) or MeshCom frames from the tables above, the same every cycle except the MeshCom message ids
    void schedule70(Net& net, bool isAprs, double until) {
        if (!net.on) return;
        const int nev = isAprs ? (int)(sizeof(kAprs) / sizeof(kAprs[0])) : (int)(sizeof(kMcom) / sizeof(kMcom[0]));
        for (;;) {
            const double et = isAprs ? kAprs[net.nextEv].t : kMcom[net.nextEv].t;
            const int who = isAprs ? kAprs[net.nextEv].station : kMcom[net.nextEv].node;
            const double start = std::max(net.cycle * kCycle + et, net.nextFree);
            if (start > until) return;
            std::vector<uint8_t> bytes;
            if (isAprs) bytes = loraAprsFrame(kAprs[net.nextEv].tnc2);
            else {
                const McomEv& e = kMcom[net.nextEv];
                MeshComFrame m;
                m.type = e.type; m.msgId = newId(); m.sourcePath = e.src; m.dest = e.dest; m.payload = e.payload;
                m.maxHop = std::string(e.src).find(',') != std::string::npos ? 4 : 5;
                bytes = meshcomBuild(m);
            }
            lora::TxFrame f;
            f.p = net.p;
            f.data = lora::encode(net.p, bytes.data(), bytes.size());
            f.startSec = start;
            f.sroPpm = cfg_.sroPpm;
            const double snr = (isAprs ? apSnr_[who] : mcmSnr_[who]) + dect2::uniform_real_distribution<double>(-1.5, 1.5)(rng_) * jitter_;
            f.amp = (float)std::sqrt(std::pow(10.0, snr / 10) * noiseRms_ * noiseRms_ * net.bwHz / rate_);
            f.freqHz = net.offsetHz + cfg_.cfoHz + (isAprs ? apCfo_[who] : mcmCfo_[who]);
            f.phase0 = dect2::uniform_real_distribution<double>(0, 1)(rng_);
            active_.push_back(f);
            net.nextFree = f.endSec() + dect2::uniform_real_distribution<double>(0.3, 0.9)(rng_);
            if (++net.nextEv >= nev) { net.nextEv = 0; net.cycle++; }
        }
    }

    // LoRaWAN: the table above every cycle, frame counters going on; frames on one channel never overlap
    void scheduleLw(double until) {
        if (!lorawan() || !(lwOn_[0] || lwOn_[1] || lwOn_[2])) return;
        const int nev = (int)(sizeof(kLw) / sizeof(kLw[0]));
        for (;;) {
            const LwEv& e = kLw[lwNext_];
            const LwDevDef& d = kLwDevs[e.dev];
            const double start = std::max(lwCycle_ * kCycle + e.t, lwFreeAt_[d.chan]);
            if (start > until) return;
            if (++lwNext_ >= nev) { lwNext_ = 0; lwCycle_++; }
            if (!lwOn_[d.chan]) continue;
            const uint32_t mic = newId();
            std::vector<uint8_t> bytes;
            if (e.kind == LwJoin) bytes = lorawanJoinRequest(d.joinEui, d.devEui, (uint16_t)(newId() & 0xFFFF), mic);
            else {
                LorawanTx t;
                t.mtype = e.kind == LwConfUp ? 4 : 2;
                t.devAddr = d.devAddr; t.adr = d.adr; t.fcnt = lwFcnt_[e.dev]++; t.mic = mic;
                if (e.kind == LwLinkCheck) t.fopts = {0x02};
                if (e.kind == LwConfUp) t.fopts = {0x06, 0xC8, 0x07};   // DevStatusAns: battery 200 / 254, margin 7 dB
                t.fport = d.fport;
                if (d.fport >= 0) for (int k = 0; k < d.len; k++) t.frm.push_back((uint8_t)newId());
                bytes = lorawanBuild(t);
            }
            lora::Params p;
            p.sf = region_ == 3 ? d.sfEu : d.sfUs; p.bwHz = 125e3; p.cr = 5; p.preamble = 8; p.syncWord = 0x34; p.ldro = meshLoraLdro(p.sf, p.bwHz); p.crc = true;
            lora::TxFrame f;
            f.p = p;
            f.data = lora::encode(p, bytes.data(), bytes.size());
            f.startSec = start;
            f.sroPpm = cfg_.sroPpm;
            const double snr = lwSnr_[e.dev] + dect2::uniform_real_distribution<double>(-1.5, 1.5)(rng_) * jitter_;
            f.amp = (float)std::sqrt(std::pow(10.0, snr / 10) * noiseRms_ * noiseRms_ * p.bwHz / rate_);
            f.freqHz = lwOff_[d.chan] + cfg_.cfoHz + lwCfo_[e.dev];
            f.phase0 = dect2::uniform_real_distribution<double>(0, 1)(rng_);
            active_.push_back(f);
            lwFreeAt_[d.chan] = f.endSec() + dect2::uniform_real_distribution<double>(0.3, 0.9)(rng_);
        }
    }

    uint32_t newId() { nextId_ = nextId_ * 1103515245u + 12345u; return nextId_ | 1u; }

    std::vector<uint8_t> build(const Ev& e, int cycle, double t) {
        const uint32_t nowUnix = kUnix0 + (uint32_t)t;
        if (e.kind == Advert || e.kind == PublicText) {
            MeshCoreNode n = mcNodes_[e.node];
            if (e.kind == Advert) return meshcoreAdvert(n, nowUnix);
            const std::string text = kMcLines[e.arg];
            std::vector<uint8_t> path;
            if (e.path) path.push_back(repeaterHash_);
            // a relayed copy carries the sender's own time stamp
            const uint32_t when = e.path ? lastMcTime_ : nowUnix;
            lastMcTime_ = nowUnix;
            return meshcorePublicText(n, when, text, path);
        }
        MeshtasticNode n = mtNodes_[e.node];
        // the kayak and the car move a little every cycle
        if (e.node == 2 || e.node == 4) { n.lat += 0.002 * std::sin(cycle * 0.7 + e.node); n.lon += 0.003 * std::cos(cycle * 0.5 + e.node); }
        const uint32_t id = newId();
        switch (e.kind) {
        case NodeInfo: return meshtasticNodeInfo(n, id, e.hopLimit, e.hopStart);
        case Position: return meshtasticPosition(n, id, e.hopLimit, e.hopStart, nowUnix);
        case Telemetry: {
            const double batt = std::max(5.0, 95.0 - 7.0 * e.node - 0.5 * cycle);
            return meshtasticTelemetry(n, id, e.hopLimit, e.hopStart, batt, 3.3 + 0.009 * batt, 4.0 + e.node, 0.6 + 0.1 * e.node, (uint32_t)(3600 * (e.node + 1) + t));
        }
        case EnvTelemetry: return meshtasticEnvTelemetry(n, id, e.hopLimit, e.hopStart, 34.5, 61.0, 1007.8);
        case Text: {
            const int r = kMtReplyTo[e.arg];
            const uint32_t replyId = r >= 0 ? lineIds_[r] : 0;
            lineIds_[e.arg] = id;
            if (e.arg == 4) wantAckId_ = id;
            return meshtasticTextEx(n, kBroadcast, id, e.hopLimit, e.hopStart, kMtLines[e.arg], replyId, e.arg == 4);
        }
        case Ack: return meshtasticRoutingAck(n, mtNodes_[e.arg].num, id, e.hopLimit, e.hopStart, wantAckId_, 0);
        case TraceReq:
            traceId_ = id;
            return meshtasticTraceroute(n, mtNodes_[e.arg].num, id, e.hopLimit, e.hopStart, false, 0, {mtNodes_[0].num}, {22}, {}, {});
        case TraceReply:
            return meshtasticTraceroute(n, mtNodes_[e.arg].num, id, e.hopLimit, e.hopStart, true, traceId_, {mtNodes_[0].num}, {22, 18}, {mtNodes_[0].num}, {15, 26});
        case Neighbors: return meshtasticNeighborInfo(n, id, e.hopLimit, e.hopStart, 300, {{mtNodes_[0].num, 6.5f}, {mtNodes_[1].num, 2.25f}});
        default: return {};
        }
    }

    double rate_;
    SynthConfig cfg_;
    genutil::NoiseSource noise_;
    std::mt19937 rng_;
    int region_ = 0;
    bool lorawan() const { return region_ == 3 || region_ == 4; }
    Net mt_, mc_, ap_, mcm_;
    double apSnr_[3] = {}, mcmSnr_[2] = {}, apCfo_[3] = {}, mcmCfo_[2] = {};
    bool lwOn_[3] = {};
    double lwOff_[3] = {}, lwFreeAt_[3] = {}, lwSnr_[6] = {}, lwCfo_[6] = {};
    uint16_t lwFcnt_[6] = {};
    int lwNext_ = 0, lwCycle_ = 0;
    double jitter_ = 1.0;     // the +-1.5 dB packet-to-packet SNR variation (0 when modeOpt[3] == 2: exact SNR for the tests)
    double noiseRms_ = 0.01;
    double mtSnr_[6] = {}, mcSnr_[3] = {}, mtCfo_[6] = {}, mcCfo_[3] = {};
    MeshtasticNode mtNodes_[6];
    MeshCoreNode mcNodes_[3];
    uint8_t repeaterHash_ = 0;
    uint32_t nextId_ = 1, lineIds_[8] = {}, wantAckId_ = 0, traceId_ = 0, lastMcTime_ = kUnix0;
    std::vector<lora::TxFrame> active_;
    int64_t pos_ = 0;
};

} // namespace

std::unique_ptr<ModeSynth> makeMeshSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<MeshSynth>(cfg, sampleRate);
}

} // namespace dect2
