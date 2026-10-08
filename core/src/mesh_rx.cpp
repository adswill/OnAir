// Mesh (LoRa) receiver: the channel plan of the region, one channel filter per frequency and bandwidth, one LoRa demodulator per
// setting, the packet layer on every good frame, and the tables of MeshTelemetry.
#include "dect2/mesh_rx.h"
#include "dect2/mesh_lora.h"
#include "dect2/mesh_proto.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

namespace dect2 {

namespace {

constexpr size_t kBlock = 4096;          // input is processed in blocks of this size (the result does not depend on the chunking)
// rows kept: the report stays under about 100 kB (a packet row is about 270 bytes, a node 280, a message 150 plus its text)
constexpr size_t kPacketCap = 100, kMessageCap = 100, kNodeCap = 150;

struct PlanEntry {
    int protocol = 1;
    std::string preset;
    double freqHz = 0;
    lora::Params p;
};

std::vector<PlanEntry> regionPlan(int region, int protocols, bool allPresets) {
    std::vector<PlanEntry> v;
    if (protocols & 1) {
        const char* reg = region == 1 ? "US" : "EU_868";
        std::vector<std::string> names = {"LongFast"};
        if (allPresets) names.insert(names.end(), {"MediumFast", "MediumSlow", "ShortFast", "ShortSlow", "ShortTurbo", "LongModerate", "LongSlow", "LongTurbo"});
        for (const auto& n : names) {
            MeshLoraSettings s;
            if (!meshtasticPreset(n, s)) continue;
            const double f = meshtasticSlotHz(reg, n);
            if (f <= 0) continue;
            PlanEntry e;
            e.protocol = 1; e.preset = n; e.freqHz = f;
            e.p.sf = s.sf; e.p.bwHz = s.bwHz; e.p.cr = s.cr; e.p.preamble = s.preamble; e.p.syncWord = s.syncWord; e.p.ldro = s.ldro;
            v.push_back(e);
        }
    }
    if (protocols & 2) {
        const MeshLoraSettings s = meshcoreDefaults(region == 1 ? "US" : "EU");
        if (s.freqHz > 0) {
            PlanEntry e;
            e.protocol = 2; e.preset = "MeshCore " + std::string(region == 1 ? "US" : "EU"); e.freqHz = s.freqHz;
            e.p.sf = s.sf; e.p.bwHz = s.bwHz; e.p.cr = s.cr; e.p.preamble = s.preamble; e.p.syncWord = s.syncWord; e.p.ldro = s.ldro;
            v.push_back(e);
        }
    }
    return v;
}

template <class T> void capPush(std::vector<T>& v, T&& x, size_t cap) {
    if (v.size() >= cap) v.erase(v.begin());
    v.push_back(std::move(x));
}

} // namespace

struct MeshReceiver::Impl {
    // settings, written by any thread
    std::mutex mu;
    double rate = 0, offsetHz = 0, tunedHz = 0;
    int region = 0, protocols = 3;
    bool presetSearch = false;
    std::atomic<bool> dirty{true};
    std::atomic<bool> resetReq{false};
    std::function<void(const std::string&)> log;
    MeshTelemetry pub;                     // the published report
    MeshProto proto;                       // locks internally

    // processing state (feed thread only)
    struct Dec {
        std::unique_ptr<lora::Demod> dm;
        size_t info = 0;                   // index into tel.decoders
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
    std::map<std::string, size_t> nodeIdx;
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
        double rt, off, tu; int reg, prot; bool all;
        {
            std::lock_guard<std::mutex> lk(mu);
            rt = rate; off = offsetHz; tu = tunedHz; reg = region; prot = protocols; all = presetSearch;
            dirty = false;
        }
        curRate = rt; curOffset = off; curTuned = tu;
        chans.clear();
        tel.decoders.clear();
        tel.inputRate = rt; tel.tunedHz = tu; tel.region = reg; tel.protocols = prot; tel.presetSearch = all;
        if (rt <= 0) return;
        const auto plan = regionPlan(reg, prot, all);
        std::string inBand;
        for (const auto& e : plan) {
            MeshDecoderInfo d;
            d.protocol = e.protocol; d.preset = e.preset; d.freqHz = e.freqHz; d.offsetHz = e.freqHz - tu;
            d.sf = e.p.sf; d.bwHz = e.p.bwHz; d.cr = e.p.cr; d.syncWord = e.p.syncWord; d.preamble = e.p.preamble; d.ldro = e.p.ldro;
            const double inOff = d.offsetHz + off;        // where it sits in the input
            d.inBand = std::fabs(inOff) + 0.5 * e.p.bwHz + 25e3 < 0.45 * rt && rt >= 2.2 * e.p.bwHz;
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
            c->decs.push_back(std::move(dc));
            char b[96];
            snprintf(b, sizeof b, "%s%s %.3f MHz SF%d", inBand.empty() ? "" : ", ", e.preset.c_str(), e.freqHz / 1e6, e.p.sf);
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
        if (!good) {
            row.note = "CRC error";
            count(di.protocol, "CRC error");
            capPush(tel.packets, std::move(row), kPacketCap);
            return;
        }
        MeshRadioInfo ri;
        ri.freqHz = row.freqHz; ri.bwHz = di.bwHz; ri.sf = di.sf; ri.cr = f.hdr.cr; ri.snrDb = f.snrDb; ri.levelDb = f.levelDb; ri.timeSec = t;
        const MeshDecodeResult r = di.protocol == 1 ? proto.decodeMeshtastic(f.payload.data(), f.payload.size(), ri)
                                                    : proto.decodeMeshCore(f.payload.data(), f.payload.size(), ri);
        if (!r.ok) {
            row.note = r.packet.note.empty() ? "not a " + std::string(di.protocol == 1 ? "Meshtastic" : "MeshCore") + " packet" : r.packet.note;
            count(di.protocol, "unknown");
            capPush(tel.packets, std::move(row), kPacketCap);
            return;
        }
        const MeshPacketInfo& pk = r.packet;
        row.parsed = true;
        row.type = pk.type; row.from = pk.from; row.to = pk.to; row.channel = pk.channel; row.packetId = pk.packetId;
        row.hopLimit = pk.hopLimit; row.hopStart = pk.hopStart; row.decrypted = pk.decrypted; row.note = pk.note; row.detail = pk.detail;
        row.path = pk.path;
        count(di.protocol, pk.type.empty() ? "?" : pk.type);
        const int hops = di.protocol == 1 ? (pk.hopStart > 0 && pk.hopLimit >= 0 && pk.hopStart >= pk.hopLimit ? pk.hopStart - pk.hopLimit : -1)
                                          : pk.pathHashCount;
        // the sender (Meshtastic: every packet names it; MeshCore: adverts carry the key)
        if (di.protocol == 1 && !pk.from.empty()) {
            const bool fresh = nodeIdx.find("1|" + pk.from) == nodeIdx.end();
            MeshNode& n = node(1, pk.from);
            n.lastHeard = t; n.lastSnrDb = (float)f.snrDb; n.lastLevelDb = (float)f.levelDb; n.packets++;
            if (hops >= 0) n.hopsAway = hops;
            if (fresh) logf("Meshtastic: new node " + pk.from);
        }
        for (const auto& u : r.nodes) {
            if (u.nodeId.empty()) continue;
            const int pr = u.protocol == MeshProtocol::MeshCore ? 2 : 1;
            const bool fresh = nodeIdx.find(std::to_string(pr) + "|" + u.nodeId) == nodeIdx.end();
            MeshNode& n = node(pr, u.nodeId);
            if (pr == 2) {
                n.lastHeard = t; n.lastSnrDb = (float)f.snrDb; n.lastLevelDb = (float)f.levelDb; n.packets++;
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
            if (u.hasEnv) { n.hasEnv = true; n.tempC = u.tempC; n.humidity = u.humidity; n.pressureHpa = u.pressureHpa; }
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

    void resetState() {
        // forget everything decoded; the report number goes on
        const uint64_t s = tel.seq;
        tel = MeshTelemetry();
        tel.seq = s;
        nodeIdx.clear(); msgSeen.clear();
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
    p_->pub = MeshTelemetry();
    p_->pub.seq = s; p_->pub.tunedHz = tu;
}

void MeshReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
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
void MeshReceiver::setRegion(int r) { std::lock_guard<std::mutex> lk(p_->mu); p_->region = r == 1 ? 1 : 0; p_->dirty = true; }
void MeshReceiver::setProtocols(int mask) { std::lock_guard<std::mutex> lk(p_->mu); p_->protocols = mask & 3; p_->dirty = true; }
void MeshReceiver::setPresetSearch(bool all) { std::lock_guard<std::mutex> lk(p_->mu); p_->presetSearch = all; p_->dirty = true; }
bool MeshReceiver::addMeshtasticChannel(const std::string& name, const std::string& psk) { return p_->proto.addMeshtasticChannel(name, psk); }
bool MeshReceiver::addMeshCoreChannel(const std::string& name, const std::string& secret) { return p_->proto.addMeshCoreChannel(name, secret); }
void MeshReceiver::clearUserChannels() { p_->proto.clearUserChannels(); }

ModeTuning meshTuning() {
    ModeTuning t;
    t.stdMode = 22; t.id = "mesh"; t.name = "Mesh (LoRa)";
    t.minMhz = 400; t.maxMhz = 1000;
    t.defMhz = 869.525;              // Meshtastic LongFast in EU_868; MeshCore EU (869.618 MHz) is 93 kHz above
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 300000;         // LongFast and MeshCore EU sit 300 and 207 kHz below the radio's centre, away from its DC spike
    return t;
}

} // namespace dect2
