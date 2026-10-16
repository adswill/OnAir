// Mesh (LoRa) receiver: the channel plan of the region, one channel filter per frequency and bandwidth, one LoRa demodulator per
// setting, the packet layer on every good frame, and the tables of MeshTelemetry.
#include "dect2/feeds.h"
#include "dect2/mesh_rx.h"
#include "dect2/results_io.h"
#include "dect2/mesh_lora.h"
#include "dect2/mesh_presets.h"
#include "dect2/mesh_proto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

namespace dect2 {

namespace {

constexpr size_t kBlock = 4096;          // input is processed in blocks of this size (the result does not depend on the chunking)
// rows kept: the report stays under about 100 kB (a packet row is about 270 bytes, a node 280, a message 150 plus its text)
constexpr size_t kReadingCap = 2000;
constexpr size_t kPacketCap = 100, kMessageCap = 100, kNodeCap = 150, kAprsCap = 150, kLorawanCap = 200;

struct PlanEntry {
    int protocol = 1;
    std::string preset;
    double freqHz = 0;
    lora::Params p;
    std::vector<int> also;                 // other protocols on exactly the same LoRa setting (tried on every frame)
};

void addEntry(std::vector<PlanEntry>& v, int protocol, const MeshLoraSettings& s) {
    for (auto& e : v)
        if (std::fabs(e.freqHz - s.freqHz) < 1 && e.p.sf == s.sf && std::fabs(e.p.bwHz - s.bwHz) < 1 && e.p.syncWord == s.syncWord) {
            // one demodulator serves both (e.g. MeshCom 868 on Meshtastic LongFast EU: the frame header carries the coding rate)
            if (e.protocol != protocol && std::find(e.also.begin(), e.also.end(), protocol) == e.also.end()) { e.also.push_back(protocol); e.preset += " + " + s.name; }
            return;
        }
    PlanEntry e;
    e.protocol = protocol; e.preset = s.name; e.freqHz = s.freqHz;
    e.p.sf = s.sf; e.p.bwHz = s.bwHz; e.p.cr = s.cr; e.p.preamble = s.preamble; e.p.syncWord = s.syncWord; e.p.ldro = s.ldro;
    v.push_back(e);
}

// the custom setting: one entry, tried as every enabled protocol its sync word may carry
std::vector<PlanEntry> customPlan(const MeshLoraSettings& s, double tunedHz, int protocols) {
    std::vector<int> ps;
    const uint8_t w = s.syncWord;
    const bool any = w != 0x2B && w != 0x12 && w != 0x34;
    for (int q : {3, 4, 2, 1, 5}) {   // the strict formats first (LoRa APRS prefix, MeshCom checksum)
        if (!(protocols & (1 << (q - 1)))) continue;
        const bool fits = any || ((q == 1 || q == 4) && w == 0x2B) || ((q == 2 || q == 3) && w == 0x12) || (q == 5 && w == 0x34);
        if (fits) ps.push_back(q);
    }
    if (ps.empty()) ps.push_back(w == 0x2B ? 1 : w == 0x34 ? 5 : 2);
    PlanEntry e;
    e.protocol = ps[0]; e.preset = "Custom"; e.freqHz = s.freqHz > 0 ? s.freqHz : tunedHz;
    e.p.sf = std::max(5, std::min(12, s.sf)); e.p.bwHz = s.bwHz; e.p.cr = std::max(5, std::min(8, s.cr));
    e.p.preamble = std::max(6, s.preamble); e.p.syncWord = s.syncWord; e.p.ldro = s.ldro;
    e.also.assign(ps.begin() + 1, ps.end());
    return {e};
}

// the plan's Meshtastic region and MeshCore preset: 0 EU, 1 US, 2 ANZ (Australia, New Zealand and the 915 - 928 MHz networks elsewhere),
// 3 ANZ with MeshCore Mid (915.075 MHz, SF9, 125 kHz, 4/5)
const char* mtRegionOf(int region) { return region == 1 ? "US" : region >= 2 ? "ANZ" : "EU_868"; }
const char* mcRegionOf(int region) { return region == 1 ? "US" : region == 2 ? "AU" : region == 3 ? "ANZ Mid" : "EU"; }

// api.meshcore.nz preset regions (Europe, North America, Oceania, …)
const char* meshCorePlanRegion(int region) { return region == 1 ? "North America" : region >= 2 ? "Oceania" : "Europe"; }

std::vector<PlanEntry> regionPlan(int region, int protocols, bool allPresets, const std::string& mtRegion, const std::string& mtPreset) {
    std::vector<PlanEntry> v;
    if (protocols & 1) {
        const std::string reg = !mtRegion.empty() ? mtRegion : mtRegionOf(region);
        std::vector<std::string> names = {mtPreset.empty() ? std::string("LongFast") : mtPreset};
        if (allPresets) for (const auto& n : meshtasticPresetNames()) if (n != names[0]) names.push_back(n);
        for (const auto& n : names) {
            MeshLoraSettings s;
            if (!meshtasticRegionPreset(reg, n, s)) continue;
            const double f = s.freqHz;
            PlanEntry e;
            e.protocol = 1; e.preset = n; e.freqHz = f;
            e.p.sf = s.sf; e.p.bwHz = s.bwHz; e.p.cr = s.cr; e.p.preamble = s.preamble; e.p.syncWord = s.syncWord; e.p.ldro = s.ldro;
            v.push_back(e);
        }
    }
    if (protocols & 2) {
        // One LoRa demod per (freq, SF, BW): the coding rate comes from each frame's header. EU networks often use SF7 4/5
        // or SF8 4/8 on 869.618 MHz — listening only to the default SF8 preset yields CRC errors on SF7 traffic.
        const char* mcReg = meshCorePlanRegion(region);
        bool anyMc = false;
        for (const auto& pe : meshCoreBuiltinPresets()) {
            if (pe.region != mcReg) continue;
            addEntry(v, 2, meshCorePresetToLora(pe));
            anyMc = true;
        }
        if (!anyMc) {
            const MeshLoraSettings s = meshcoreDefaults(mcRegionOf(region));
            if (s.freqHz > 0) addEntry(v, 2, s);
        }
    }
    // LoRa APRS and MeshCom: every known frequency (only those inside the captured band get a demodulator)
    if (protocols & 4) for (const auto& s : loraAprsPresets()) addEntry(v, 3, s);
    if (protocols & 8) for (const auto& s : meshcomPresets()) addEntry(v, 4, s);
    // LoRaWAN: the region's uplink channels, every spreading factor (one demodulator each)
    if (protocols & 16) for (const auto& s : lorawanPresets(region)) addEntry(v, 5, s);
    return v;
}

const char* protoName(int p) { return p == 1 ? "Meshtastic" : p == 2 ? "MeshCore" : p == 3 ? "LoRa APRS" : p == 4 ? "MeshCom" : "LoRaWAN"; }

template <class T> void capPush(std::vector<T>& v, T&& x, size_t cap) {
    if (v.size() >= cap) v.erase(v.begin());
    v.push_back(std::move(x));
}

} // namespace

struct MeshReceiver::Impl {
    // settings, written by any thread
    std::mutex mu;
    double rate = 0, offsetHz = 0, tunedHz = 0;
    int region = 0, protocols = 15;
    bool presetSearch = false;
    std::string mtRegion, mtPreset;
    bool customOn = false;
    MeshLoraSettings custom;
    std::atomic<bool> dirty{true};
    std::atomic<bool> resetReq{false};
    std::function<void(const std::string&)> log;
    MeshTelemetry pub;                     // the published report
    MeshProto proto;                       // locks internally
    // the decoded lists (packets, nodes, messages, stations, devices) stay through resets: Clear and a loaded file reach the feed
    // thread's copy through these requests (mu), and pub at once
    bool clearReq = false, loadReq = false;
    MeshTelemetry loaded;

    // processing state (feed thread only)
    struct Dec {
        std::unique_ptr<lora::Demod> dm;
        size_t info = 0;                   // index into tel.decoders
        std::vector<int> protos;           // protocols to try on a frame, in order
        lora::DemodStats seen;
    };
    struct Chan {
        std::unique_ptr<lora::Channelizer> ch;
        lora::ChanBuf buf;
        std::vector<Dec> decs;
        double freqHz = 0;
        int64_t base = 0;                  // input sample at which the channel started
    };
    std::vector<Chan> chans;
    std::vector<cf32> pend;
    int64_t nIn = 0;                       // input samples received
    int64_t nProc = 0;                     // input samples processed
    double nextReport = 0;
    double curRate = 0, curOffset = 0, curTuned = 0;
    MeshTelemetry tel;
    std::map<std::string, size_t> nodeIdx, aprsIdx, lwIdx;
    std::set<std::string> msgSeen;
    std::vector<lora::RxFrame> frames;
    double lastGood = -1e9, lastPreamble = -1e9;
    uint64_t preamblesSeen = 0;

    void logf(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    double now() const { return curRate > 0 ? (double)nProc / curRate : 0; }

    void rebuild() {
        double rt, off, tu; int reg, prot; bool all, cu; std::string mreg, mpre; MeshLoraSettings cs;
        {
            std::lock_guard<std::mutex> lk(mu);
            rt = rate; off = offsetHz; tu = tunedHz; reg = region; prot = protocols; all = presetSearch;
            mreg = mtRegion; mpre = mtPreset; cu = customOn; cs = custom;
            dirty = false;
        }
        curRate = rt; curOffset = off; curTuned = tu;
        chans.clear();
        tel.decoders.clear();
        tel.inputRate = rt; tel.tunedHz = tu; tel.region = reg; tel.protocols = prot; tel.presetSearch = all;
        tel.mtRegion = !mreg.empty() ? mreg : mtRegionOf(reg); tel.custom = cu;
        if (rt <= 0) return;
        const auto plan = cu ? customPlan(cs, tu, prot) : regionPlan(reg, prot, all, mreg, mpre);
        std::string inBand;
        for (const auto& e : plan) {
            MeshDecoderInfo d;
            d.protocol = e.protocol; d.preset = e.preset; d.freqHz = e.freqHz; d.offsetHz = e.freqHz - tu;
            d.sf = e.p.sf; d.bwHz = e.p.bwHz; d.cr = e.p.cr; d.syncWord = e.p.syncWord; d.preamble = e.p.preamble; d.ldro = e.p.ldro;
            const double inOff = d.offsetHz + off;        // where it sits in the input
            d.inBand = std::fabs(inOff) + 0.5 * e.p.bwHz + 25e3 < 0.45 * rt && rt >= 2.2 * e.p.bwHz;
            if (!d.inBand && e.protocol == 5) continue;   // the US plan has 264 settings: only the ones in the capture are listed
            tel.decoders.push_back(d);
            if (!d.inBand) continue;
            Chan* c = nullptr;
            for (auto& x : chans) if (std::fabs(x.freqHz - e.freqHz) < 1 && std::fabs(x.ch->bwHz() - e.p.bwHz) < 1) c = &x;
            if (!c) {
                chans.emplace_back();
                c = &chans.back();
                c->ch = std::make_unique<lora::Channelizer>(rt, inOff, e.p.bwHz);
                c->freqHz = e.freqHz;
                c->base = nProc;
                c->buf.clear(0);
            }
            Dec dc;
            dc.dm = std::make_unique<lora::Demod>(e.p, c->ch->outRate());
            dc.dm->reset(0);
            dc.info = tel.decoders.size() - 1;
            // the strict formats first (LoRa APRS prefix, MeshCom checksum): a Meshtastic header parses from almost any bytes
            for (int q : e.also) if (q >= 3) dc.protos.push_back(q);
            if (e.protocol >= 3) dc.protos.insert(dc.protos.begin(), e.protocol);
            else dc.protos.push_back(e.protocol);
            for (int q : e.also) if (q < 3) dc.protos.push_back(q);
            c->decs.push_back(std::move(dc));
            char b[96];
            if (e.protocol == 5 && e.p.bwHz < 130e3 && e.p.sf != 7) continue;   // LoRaWAN: one line per 125 kHz channel, not per SF
            const std::string sfs = e.protocol == 5 && e.p.bwHz < 130e3 ? std::string(reg == 1 ? "SF7-10" : "SF7-12")
                                  : "SF" + std::to_string(e.p.sf) + (e.protocol == 5 ? " " + std::to_string((int)(e.p.bwHz / 1e3)) + " kHz" : "");
            snprintf(b, sizeof b, "%s%s %.3f MHz %s", inBand.empty() ? "" : ", ", e.preset.c_str(), e.freqHz / 1e6, sfs.c_str());
            inBand += b;
        }
        logf("Mesh: listening to " + (inBand.empty() ? std::string("nothing (no channel of the region in the captured band)") : inBand));
    }

    void processBlock(const cf32* x, size_t n) {
        for (auto& c : chans) {
            c.ch->process(x, n, c.buf);
            int64_t keep = c.buf.end();
            for (auto& d : c.decs) {
                frames.clear();
                d.dm->process(c.buf, frames);
                for (auto& f : frames) onFrame(c, d, f);
                const auto& st = d.dm->stats();
                if (st.preambles != d.seen.preambles) { lastPreamble = now(); preamblesSeen += st.preambles - d.seen.preambles; }
                MeshDecoderInfo& di = tel.decoders[d.info];
                di.headerBad = st.headerBad; di.syncBad = st.syncBad;
                d.seen = st;
                keep = std::min(keep, d.dm->oldestNeeded());
                if (d.dm->busy()) lastPreamble = now();
            }
            c.buf.trim(keep);
        }
        nProc += (int64_t)n;
    }

    // a packet from the node: when, how strong (the report's first / last heard and SNR range)
    static void heard(MeshNode& n, double t, double snr, double level) {
        if (n.packets == 0 || n.firstHeard < 0) { n.firstHeard = t; n.snrMin = n.snrMax = (float)snr; }
        n.snrMin = std::min(n.snrMin, (float)snr); n.snrMax = std::max(n.snrMax, (float)snr);
        n.snrSum += snr;
        n.lastHeard = t; n.lastSnrDb = (float)snr; n.lastLevelDb = (float)level; n.packets++;
    }
    MeshNode& node(int protocol, const std::string& id) {
        const std::string key = std::to_string(protocol) + "|" + id;
        auto it = nodeIdx.find(key);
        if (it != nodeIdx.end()) return tel.nodes[it->second];
        if (tel.nodes.size() >= kNodeCap) {
            // the node heard longest ago leaves
            size_t old = 0;
            for (size_t i = 1; i < tel.nodes.size(); i++) if (tel.nodes[i].lastHeard < tel.nodes[old].lastHeard) old = i;
            tel.nodes.erase(tel.nodes.begin() + (long)old);
            nodeIdx.clear();
            for (size_t i = 0; i < tel.nodes.size(); i++) nodeIdx[std::to_string(tel.nodes[i].protocol) + "|" + tel.nodes[i].id] = i;
        }
        MeshNode m;
        m.protocol = protocol; m.id = id;
        tel.nodes.push_back(m);
        nodeIdx[key] = tel.nodes.size() - 1;
        return tel.nodes.back();
    }

    void count(int protocol, const std::string& type) {
        for (auto& c : tel.counts) if (c.protocol == protocol && c.type == type) { c.count++; return; }
        MeshTypeCount c;
        c.protocol = protocol; c.type = type; c.count = 1;
        tel.counts.push_back(c);
    }

    void onFrame(Chan& c, Dec& d, const lora::RxFrame& f) {
        MeshDecoderInfo& di = tel.decoders[d.info];
        // the frame's first sample in input samples, less the filters' delay
        const double t = ((double)c.base + f.startSample * c.ch->decim() - c.ch->delayIn()) / curRate;
        const bool good = f.crcOk || !f.hdr.crc;
        di.frames++;
        if (!good) di.crcBad++;
        di.lastSnrDb = (float)f.snrDb; di.lastCfoHz = f.cfoHz; di.lastFrameSec = t;
        if (f.crcOk) {
            tel.blocksOk++; tel.dataValid = true; lastGood = now();
            tel.cfoHz = f.cfoHz; tel.snrDb = (float)f.snrDb;
        } else if (f.hdr.crc) tel.blocksBad++;
        MeshPacket row;
        row.timeSec = t; row.protocol = di.protocol; row.preset = di.preset;
        row.freqHz = c.freqHz + f.cfoHz; row.cfoHz = f.cfoHz;
        row.sf = di.sf; row.cr = f.hdr.cr; row.bwHz = di.bwHz;
        row.levelDb = (float)f.levelDb; row.snrDb = (float)f.snrDb;
        row.size = (int)f.payload.size(); row.crcOk = f.crcOk;
        const bool crcBad = f.hdr.crc && !f.crcOk;
        if (crcBad) count(di.protocol, "CRC error");
        MeshRadioInfo ri;
        ri.freqHz = row.freqHz; ri.bwHz = di.bwHz; ri.sf = di.sf; ri.cr = f.hdr.cr; ri.snrDb = f.snrDb; ri.levelDb = f.levelDb; ri.timeSec = t;
        MeshDecodeResult r;
        int proto_ = di.protocol;
        for (int q : d.protos) {
            const uint8_t* pl = f.payload.data();
            const size_t pn = f.payload.size();
            r = q == 1 ? proto.decodeMeshtastic(pl, pn, ri) : q == 2 ? proto.decodeMeshCore(pl, pn, ri) : q == 3 ? meshDecodeLoraAprs(pl, pn)
              : q == 4 ? meshDecodeMeshCom(pl, pn) : meshDecodeLorawan(pl, pn);
            if (r.ok) { proto_ = q; break; }
        }
        row.protocol = proto_;
        if (Feeds::get().wantLora()) {
            const char* kind = proto_ == 1 ? "meshtastic" : proto_ == 2 ? "meshcore" : proto_ == 3 ? "loraaprs" : proto_ == 4 ? "meshcom" : "lorawan";
            std::string extra;
            if (r.ok) {
                extra = "\"from\":\"" + feedfmt::jsonEscape(r.packet.from) + "\",\"to\":\"" + feedfmt::jsonEscape(r.packet.to) + "\"";
                extra += ",\"parsed\":true";
            } else extra = "\"parsed\":false";
            Feeds::get().lora(kind, meshHexString(f.payload.data(), std::min<size_t>(f.payload.size(), 255)), (float)f.snrDb, row.freqHz, di.sf, extra);
        }
        if (proto_ == 5) row.raw = meshHexString(f.payload.data(), std::min<size_t>(f.payload.size(), 255));
        else if (proto_ >= 3 || !r.ok) row.raw = meshPrintable(f.payload.data(), std::min<size_t>(f.payload.size(), 255));
        if (!r.ok) {
            row.note = r.packet.note.empty() ? "not a " + std::string(protoName(di.protocol)) + " packet" : r.packet.note;   // (the first one tried)
            if (crcBad) row.note = row.note == "CRC error" ? row.note : std::string("CRC error") + (row.note.empty() ? "" : "; " + row.note);
            count(di.protocol, "unknown");
            capPush(tel.packets, std::move(row), kPacketCap);
            return;
        }
        if (proto_ == 5 && crcBad) {   // a LoRaWAN frame with a failed CRC is never trusted: counted and listed, but no device is made from it
            row.note = row.note.empty() ? "CRC error" : std::string("CRC error; ") + row.note;
            capPush(tel.packets, std::move(row), kPacketCap);
            return;
        }
        if (proto_ == 5) { onLorawan(c, t, f, r, std::move(row)); return; }
        if (proto_ >= 3) { onAprs(proto_, t, f, r, std::move(row)); return; }
        const MeshPacketInfo& pk = r.packet;
        row.parsed = true;
        row.type = pk.type; row.from = pk.from; row.to = pk.to; row.channel = pk.channel; row.packetId = pk.packetId;
        row.hopLimit = pk.hopLimit; row.hopStart = pk.hopStart; row.decrypted = pk.decrypted;
        row.note = pk.note;
        if (crcBad) row.note = row.note.empty() ? "CRC error" : std::string("CRC error; ") + row.note;
        row.detail = pk.detail;
        row.viaMqtt = pk.viaMqtt; row.nextHop = pk.nextHop; row.relayNode = pk.relayNode;
        row.path = pk.path;
        count(proto_, pk.type.empty() ? "?" : pk.type);
        const int hops = proto_ == 1 ? (pk.hopStart > 0 && pk.hopLimit >= 0 && pk.hopStart >= pk.hopLimit ? pk.hopStart - pk.hopLimit : -1)
                                          : pk.pathHashCount;
        // the sender (Meshtastic: every packet names it; MeshCore: adverts carry the key)
        if (proto_ == 1 && !pk.from.empty()) {
            const bool fresh = nodeIdx.find("1|" + pk.from) == nodeIdx.end();
            MeshNode& n = node(1, pk.from);
            heard(n, t, f.snrDb, f.levelDb);
            if (!pk.decrypted) n.encrypted++;
            if (hops >= 0) n.hopsAway = hops;
            if (fresh) logf("Meshtastic: new node " + pk.from);
        }
        for (const auto& u : r.nodes) {
            if (u.nodeId.empty()) continue;
            const int pr = u.protocol == MeshProtocol::MeshCore ? 2 : 1;
            const bool fresh = nodeIdx.find(std::to_string(pr) + "|" + u.nodeId) == nodeIdx.end();
            MeshNode& n = node(pr, u.nodeId);
            if (pr == 2) {
                heard(n, t, f.snrDb, f.levelDb);
                if (hops >= 0) n.hopsAway = hops;
            }
            if (!u.longName.empty()) n.longName = u.longName;
            if (!u.shortName.empty()) n.shortName = u.shortName;
            if (!u.hwModel.empty()) n.hwModel = u.hwModel;
            if (!u.role.empty()) n.role = u.role;
            if (u.hasPosition) { n.hasPosition = true; n.lat = u.lat; n.lon = u.lon; n.altM = u.altM; n.posSec = t; n.posUnix = u.positionTime; }
            if (u.batteryPct >= 0) n.batteryPct = u.batteryPct;
            if (u.voltage >= 0) n.voltage = u.voltage;
            if (u.channelUtilPct >= 0) n.channelUtilPct = u.channelUtilPct;
            if (u.airUtilTxPct >= 0) n.airUtilTxPct = u.airUtilTxPct;
            if (u.uptimeS >= 0) n.uptimeS = u.uptimeS;
            if (u.batteryPct >= 0 || u.voltage >= 0 || u.hasEnv || u.hasPosition) {   // a telemetry or position report: kept with its time
                MeshReading rd;
                rd.timeSec = t; rd.protocol = pr; rd.id = u.nodeId;
                rd.unixTime = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                rd.batteryPct = u.batteryPct; rd.voltage = u.voltage; rd.channelUtilPct = u.channelUtilPct; rd.airUtilTxPct = u.airUtilTxPct;
                rd.uptimeS = u.uptimeS; rd.hasEnv = u.hasEnv; rd.tempC = u.tempC; rd.humidity = u.humidity; rd.pressureHpa = u.pressureHpa;
                rd.hasPosition = u.hasPosition; rd.lat = u.lat; rd.lon = u.lon; rd.altM = u.altM;
                capPush(tel.readings, std::move(rd), kReadingCap);
            }
            if (u.batteryPct >= 0 || u.voltage >= 0) n.devSec = t;
            if (u.hasEnv) { n.envSec = t; n.hasEnv = true; n.tempC = u.tempC; n.humidity = u.humidity; n.pressureHpa = u.pressureHpa; }
            if (fresh || (!u.longName.empty() && u.longName != n.longName))
                logf(std::string(pr == 1 ? "Meshtastic" : "MeshCore") + ": node " + u.nodeId + (n.longName.empty() ? "" : " \"" + n.longName + "\""));
        }
        for (const auto& m : r.messages) {
            // a message relayed by several nodes arrives several times: keep the first
            const std::string key = std::to_string((int)m.protocol) + "|" + m.from + "|" + std::to_string(m.packetId) + "|" +
                                    std::to_string(m.senderTime) + "|" + (m.packetId ? std::string() : m.text);
            if (msgSeen.count(key)) continue;
            if (msgSeen.size() > 4000) msgSeen.clear();
            msgSeen.insert(key);
            MeshMessage mm;
            mm.timeSec = t; mm.protocol = m.protocol == MeshProtocol::MeshCore ? 2 : 1;
            mm.channel = m.channel; mm.from = m.from; mm.to = m.to; mm.text = m.text.substr(0, 240); mm.hops = m.hops; mm.packetId = m.packetId;
            auto it = nodeIdx.find(std::to_string(mm.protocol) + "|" + m.from);
            if (it != nodeIdx.end()) mm.fromName = tel.nodes[it->second].longName;
            else if (mm.protocol == 2) mm.fromName = m.from;   // MeshCore puts the sender's name in the text
            capPush(tel.messages, std::move(mm), kMessageCap);
        }
        capPush(tel.packets, std::move(row), kPacketCap);
    }

    MeshAprsStation& aprsStation(int protocol, const std::string& call) {
        const std::string key = std::to_string(protocol) + "|" + call;
        auto it = aprsIdx.find(key);
        if (it != aprsIdx.end()) return tel.aprsStations[it->second];
        if (tel.aprsStations.size() >= kAprsCap) {
            size_t old = 0;
            for (size_t i = 1; i < tel.aprsStations.size(); i++) if (tel.aprsStations[i].st.lastHeardSec < tel.aprsStations[old].st.lastHeardSec) old = i;
            tel.aprsStations.erase(tel.aprsStations.begin() + (long)old);
            aprsIdx.clear();
            for (size_t i = 0; i < tel.aprsStations.size(); i++) aprsIdx[std::to_string(tel.aprsStations[i].protocol) + "|" + tel.aprsStations[i].st.call] = i;
        }
        MeshAprsStation s;
        s.protocol = protocol; s.st.call = call;
        tel.aprsStations.push_back(s);
        aprsIdx[key] = tel.aprsStations.size() - 1;
        logf(std::string(protoName(protocol)) + ": new station " + call);
        return tel.aprsStations.back();
    }

    // LoRa APRS and MeshCom: the station table (the packet mode's PacketStation), messages and the packet row
    void onAprs(int pr, double t, const lora::RxFrame& f, const MeshDecodeResult& r, MeshPacket&& row) {
        const MeshPacketInfo& pk = r.packet;
        row.parsed = true;
        row.type = pk.type; row.from = pk.from; row.to = pk.to; row.packetId = pk.packetId; row.hopLimit = pk.hopLimit;
        row.decrypted = true; row.note = pk.note; row.detail = pk.detail; row.path = pk.path;
        count(pr, pk.type.empty() ? "?" : pk.type);
        if (r.hasAprs && !r.aprsSource.empty()) {
            const aprs::Info& a = r.aprs;
            const bool object = (a.type == "Object" || a.type == "Item") && !a.name.empty();
            auto touch = [&](const std::string& call, bool isObj) {
                MeshAprsStation& s = aprsStation(pr, call);
                s.st.lastHeardSec = t; s.st.count++; s.lastSnrDb = (float)f.snrDb; s.path = r.aprsPath; s.lastType = pk.type;
                s.st.isObject = isObj;
                s.st.via = isObj ? r.aprsSource : std::string();
                if (r.batteryPct >= 0) s.batteryPct = r.batteryPct;
                if (a.hasPos) {
                    s.st.hasPos = true; s.st.lat = a.lat; s.st.lon = a.lon;
                    s.st.hasAlt = a.hasAlt; s.st.altM = a.altM;
                    s.st.hasCourse = a.hasCourse; s.st.courseDeg = a.courseDeg;
                    s.st.hasSpeed = a.hasSpeed; s.st.speedKnots = a.speedKnots;
                    if (a.symTable) { s.st.symTable = a.symTable; s.st.symCode = a.symCode; }
                    s.st.comment = a.comment;
                } else if (a.type == "Status") s.st.comment = a.text;
            };
            if (object) {
                // the sender is heard too, the object gets the position
                MeshAprsStation& s = aprsStation(pr, r.aprsSource);
                s.st.lastHeardSec = t; s.st.count++; s.lastSnrDb = (float)f.snrDb; s.path = r.aprsPath; s.lastType = pk.type;
                touch(a.name, true);
            } else touch(r.aprsSource, false);
        }
        for (const auto& m : r.messages) {
            const std::string key = std::to_string(pr) + "|" + m.from + "|" + std::to_string(m.packetId) + "|" + m.to + "|" + m.text;
            if (msgSeen.count(key)) continue;
            if (msgSeen.size() > 4000) msgSeen.clear();
            msgSeen.insert(key);
            MeshMessage mm;
            mm.timeSec = t; mm.protocol = pr;
            mm.channel = m.channel; mm.from = m.from; mm.fromName = m.from; mm.to = m.to; mm.text = m.text.substr(0, 240); mm.packetId = m.packetId;
            capPush(tel.messages, std::move(mm), kMessageCap);
        }
        capPush(tel.packets, std::move(row), kPacketCap);
    }

    // LoRaWAN: the device table (by DevAddr; join requests by DevEUI) and the packet row. Nothing is decrypted.
    void onLorawan(const Chan& c, double t, const lora::RxFrame& f, const MeshDecodeResult& r, MeshPacket&& row) {
        const MeshPacketInfo& pk = r.packet;
        const MeshDecodeResult::Lorawan& w = r.lorawan;
        row.parsed = true;
        row.type = pk.type; row.from = pk.from; row.to = pk.to; row.packetId = pk.packetId;
        row.decrypted = false; row.note = pk.note; row.detail = pk.detail;
        row.network = w.network; row.mtype = w.mtype; row.fport = w.fport; row.frmLen = w.fport >= 0 || w.mtype == 1 ? (int)w.frmLen : -1; row.mic = w.mic;
        std::string fl;
        auto add = [](std::string& s, const std::string& x) { s += (s.empty() ? "" : ", ") + x; };
        if (w.adr) add(fl, "ADR");
        if (w.adrAckReq) add(fl, "ADRACKReq");
        if (w.ack) add(fl, "ACK");
        if (w.fpending) add(fl, "FPending");
        if (w.classB) add(fl, "class B");
        row.flags = fl;
        for (const auto& m : w.macCommands) add(row.mac, m);
        count(5, pk.type);
        std::string id;
        if (w.mtype == 0 || w.mtype == 6) id = "DevEUI " + lorawanEui(w.devEui);
        else if (w.mtype >= 2 && w.mtype <= 5) id = pk.from;
        if (!id.empty()) {
            auto it = lwIdx.find(id);
            if (it == lwIdx.end()) {
                if (tel.lorawanDevices.size() >= kLorawanCap) {
                    size_t old = 0;
                    for (size_t i = 1; i < tel.lorawanDevices.size(); i++) if (tel.lorawanDevices[i].lastHeard < tel.lorawanDevices[old].lastHeard) old = i;
                    tel.lorawanDevices.erase(tel.lorawanDevices.begin() + (long)old);
                    lwIdx.clear();
                    for (size_t i = 0; i < tel.lorawanDevices.size(); i++) lwIdx[tel.lorawanDevices[i].id] = i;
                }
                MeshLorawanDevice d;
                d.id = id;
                tel.lorawanDevices.push_back(d);
                it = lwIdx.emplace(id, tel.lorawanDevices.size() - 1).first;
                logf("LoRaWAN: new device " + id + (w.network.empty() ? "" : " (" + w.network + ")") + (w.vendor.empty() ? "" : " (" + w.vendor + ")"));
            }
            MeshLorawanDevice& d = tel.lorawanDevices[it->second];
            d.lastHeard = t; d.freqHz = row.freqHz; d.lastSnrDb = (float)f.snrDb; d.lastLevelDb = (float)f.levelDb; d.sf = row.sf;
            d.packets++; d.lastType = pk.type;
            if (w.mtype == 0 || w.mtype == 6) {
                d.joins++;
                d.devEui = lorawanEui(w.devEui); d.vendor = w.vendor;
                if (w.joinEui || w.mtype == 0) d.joinEui = lorawanEui(w.joinEui);
            } else {
                d.devAddr = pk.from; d.network = w.network; d.adr = w.adr;
                if (w.uplink) d.uplinks++; else d.downlinks++;
                if (w.confirmed) d.confirmed++;
                // a gap in the 16 counter bits that the radio sends: frames this receiver did not hear (a restart from 0 is not one)
                if (w.uplink && d.fcnt >= 0 && w.fcnt > (uint16_t)d.fcnt && w.fcnt - (uint16_t)d.fcnt < 1000) d.missed += (uint32_t)(w.fcnt - (uint16_t)d.fcnt - 1);
                if (w.uplink) d.fcnt = w.fcnt;
                d.lastFport = w.fport;
                if (!w.macCommands.empty()) d.lastMac = row.mac;
                if (w.devStatusBattery >= 0) { d.battery = w.devStatusBattery; d.marginDb = w.devStatusMargin; }
            }
        }
        (void)c;
        capPush(tel.packets, std::move(row), kPacketCap);
    }

    void report() {
        tel.seq++;
        tel.timeSec = now();
        const double t = now();
        tel.state = t - lastGood < 60 ? 2 : (t - lastPreamble < 10 ? 1 : 0);
        tel.preambles = preamblesSeen;
        uint64_t hb = 0;
        for (const auto& d : tel.decoders) hb += d.headerBad;
        tel.headerBad = hb;
        tel.userChannels.clear();
        for (const auto& n : proto.channelNames(MeshProtocol::Meshtastic)) tel.userChannels.push_back("Meshtastic: " + n);
        for (const auto& n : proto.channelNames(MeshProtocol::MeshCore)) tel.userChannels.push_back("MeshCore: " + n);
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    static void moveLists(MeshTelemetry& to, MeshTelemetry& from) {
        to.packets = std::move(from.packets); to.nodes = std::move(from.nodes); to.messages = std::move(from.messages);
        to.counts = std::move(from.counts); to.aprsStations = std::move(from.aprsStations); to.lorawanDevices = std::move(from.lorawanDevices);
        to.readings = std::move(from.readings);
        from.packets.clear(); from.nodes.clear(); from.messages.clear(); from.counts.clear(); from.aprsStations.clear(); from.lorawanDevices.clear();
        from.readings.clear();
    }
    static void shiftTimes(MeshTelemetry& t, double dt) {
        for (auto& p : t.packets) p.timeSec += dt;
        for (auto& m : t.messages) m.timeSec += dt;
        for (auto& r : t.readings) r.timeSec += dt;
        for (auto& n : t.nodes) { if (n.lastHeard >= 0) n.lastHeard += dt; if (n.firstHeard >= 0) n.firstHeard += dt; if (n.envSec >= 0) n.envSec += dt; if (n.devSec >= 0) n.devSec += dt; if (n.posSec >= 0) n.posSec += dt; }
        for (auto& a : t.aprsStations) a.st.lastHeardSec += dt;
        for (auto& d : t.lorawanDevices) if (d.lastHeard >= 0) d.lastHeard += dt;
    }
    void reindex() {
        nodeIdx.clear(); aprsIdx.clear(); lwIdx.clear();
        for (size_t i = 0; i < tel.nodes.size(); i++) nodeIdx[std::to_string(tel.nodes[i].protocol) + "|" + tel.nodes[i].id] = i;
        for (size_t i = 0; i < tel.aprsStations.size(); i++) aprsIdx[std::to_string(tel.aprsStations[i].protocol) + "|" + tel.aprsStations[i].st.call] = i;
        for (size_t i = 0; i < tel.lorawanDevices.size(); i++) lwIdx[tel.lorawanDevices[i].id] = i;
    }
    // feed thread: a Clear or a loaded file asked for from another thread
    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (!clearReq && !loadReq) return;
        if (clearReq) { MeshTelemetry e; moveLists(tel, e); msgSeen.clear(); clearReq = false; }
        if (loadReq) { moveLists(tel, loaded); msgSeen.clear(); loadReq = false; }
        reindex();
    }

    void resetState() {
        // forget the signal; the decoded lists stay (clearResults) and the report number goes on
        const uint64_t s = tel.seq;
        const double t0 = tel.timeSec;     // the clock starts again: the kept lists move back by the time it had reached
        MeshTelemetry keep;
        moveLists(keep, tel);
        tel = MeshTelemetry();
        tel.seq = s;
        moveLists(tel, keep);
        shiftTimes(tel, -t0);
        reindex();
        pend.clear();
        nIn = nProc = 0;
        nextReport = 0;
        lastGood = lastPreamble = -1e9;
        preamblesSeen = 0;
        dirty = true;
    }
};

MeshReceiver::MeshReceiver() : p_(std::make_unique<Impl>()) {
    p_->tunedHz = meshTuning().defMhz * 1e6;
    p_->pub.tunedHz = p_->tunedHz;
}
MeshReceiver::~MeshReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void MeshReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
void MeshReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->dirty = true; }
bool MeshReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= meshTuning().minSampleRate - 1;
}
void MeshReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    const double tu = p_->pub.tunedHz;
    MeshTelemetry t;
    Impl::moveLists(t, p_->pub);         // the lists stay (clearResults)
    Impl::shiftTimes(t, -p_->pub.timeSec);
    p_->pub = std::move(t);
    p_->pub.seq = s; p_->pub.tunedHz = tu;
}

void MeshReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    MeshTelemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true; p_->loadReq = false;
    p_->pub.seq++;
}
std::string MeshReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const MeshTelemetry& t = p_->pub;
    return results::pack(t.timeSec, t.packets, t.nodes, t.messages, t.counts, t.aprsStations, t.lorawanDevices, t.readings);
}
bool MeshReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    MeshTelemetry l;
    if (!results::unpack(blob, t0, l.packets, l.nodes, l.messages, l.counts, l.aprsStations, l.lorawanDevices, l.readings)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    MeshTelemetry copy;
    copy.packets = l.packets; copy.nodes = l.nodes; copy.messages = l.messages; copy.counts = l.counts; copy.aprsStations = l.aprsStations; copy.lorawanDevices = l.lorawanDevices; copy.readings = l.readings;
    Impl::moveLists(p_->pub, copy);
    Impl::moveLists(p_->loaded, l);
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}

void MeshReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    m.takeRequests();
    if (m.dirty.load()) m.rebuild();
    if (m.curRate <= 0) return;
    m.nIn += (int64_t)n;
    if (m.chans.empty()) m.nProc += (int64_t)n;      // nothing to search: only the clock runs
    else {
        size_t i = 0;
        if (!m.pend.empty()) {
            const size_t take = std::min(n, kBlock - m.pend.size());
            m.pend.insert(m.pend.end(), x, x + take);
            i = take;
            if (m.pend.size() == kBlock) { m.processBlock(m.pend.data(), kBlock); m.pend.clear(); }
        }
        for (; i + kBlock <= n; i += kBlock) m.processBlock(x + i, kBlock);
        if (i < n) m.pend.insert(m.pend.end(), x + i, x + n);
    }
    // about four reports a second of signal
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool MeshReceiver::telemetry(MeshTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void MeshReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void MeshReceiver::setTunedHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->tunedHz = hz; p_->dirty = true; }
void MeshReceiver::setRegion(int r) { std::lock_guard<std::mutex> lk(p_->mu); p_->region = r >= 1 && r <= 3 ? r : 0; p_->dirty = true; }
void MeshReceiver::setProtocols(int mask) { std::lock_guard<std::mutex> lk(p_->mu); p_->protocols = mask & 31; p_->dirty = true; }
void MeshReceiver::setPresetSearch(bool all) { std::lock_guard<std::mutex> lk(p_->mu); p_->presetSearch = all; p_->dirty = true; }
void MeshReceiver::setMeshtasticPlan(const std::string& region, const std::string& preset) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->mtRegion = region; p_->mtPreset = preset; p_->dirty = true;
}
void MeshReceiver::setCustom(bool on, const MeshLoraSettings& s) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->customOn = on; p_->custom = s; p_->dirty = true;
}
bool MeshReceiver::addMeshtasticChannel(const std::string& name, const std::string& psk) { return p_->proto.addMeshtasticChannel(name, psk); }
bool MeshReceiver::addMeshCoreChannel(const std::string& name, const std::string& secret) { return p_->proto.addMeshCoreChannel(name, secret); }
void MeshReceiver::clearUserChannels() { p_->proto.clearUserChannels(); }

ModeTuning meshTuning() {
    ModeTuning t;
    t.stdMode = 22; t.id = "mesh"; t.name = "Mesh (LoRa)";
    t.minMhz = 400; t.maxMhz = 2500;   // 2.4 GHz LoRa (Meshtastic LORA_24) too
    t.defMhz = 869.525;              // Meshtastic LongFast in EU_868; MeshCore EU (869.618 MHz) is 93 kHz above
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 300000;         // LongFast and MeshCore EU sit 300 and 207 kHz below the radio's centre, away from its DC spike
    return t;
}

} // namespace dect2
