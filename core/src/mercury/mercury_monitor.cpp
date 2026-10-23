// Mercury monitor (see mercury_monitor.h).
#include "dect2/mercury/mercury_monitor.h"
#include "dect2/mercury/mercury_proto.h"
#include "dect2/mercury/mercury_raptorq.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {
namespace mercury {

namespace {

constexpr size_t kStreamMax = 4u << 20;     // reassembled bytes kept per direction
constexpr double kKeydownGap = 0.6;         // s between bursts: a new keydown

std::string hexOf(const std::vector<uint8_t>& p, size_t max = 48) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < p.size() && i < max; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += b; if (i + 1 < p.size() && i + 1 < max) s += ' '; }
    if (p.size() > max) s += " ...";
    return s;
}

std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (c == '\n' || c == '\r') s += '\n';
        else if (c == '\t') s += ' ';
        else if (c >= 0x20 && c < 0x7F) s += (char)c;
        else if (c >= 0x80) s += (char)c;   // UTF-8 bytes go through (the UI shows them); a broken sequence shows as a replacement
        else s += '.';
    }
    return s;
}

bool mostlyText(const uint8_t* p, size_t n) {
    if (!n) return false;
    size_t good = 0;
    for (size_t i = 0; i < n; i++) { const uint8_t c = p[i]; good += (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t' || c >= 0x80; }
    return good * 10 >= n * 9;
}

std::string kindOfFile(const std::vector<uint8_t>& d, size_t off, size_t len) {
    const uint8_t* p = d.data() + off;
    if (len >= 8 && !std::memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return "image";
    if (len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image";
    if (len >= 6 && (!std::memcmp(p, "GIF87a", 6) || !std::memcmp(p, "GIF89a", 6))) return "image";
    if (len >= 2 && p[0] == 'B' && p[1] == 'M') return "image";
    if (mostlyText(p, std::min<size_t>(len, 4096))) return "text";
    return "binary";
}

std::string crcName(uint16_t c) { char b[16]; snprintf(b, sizeof b, "CRC %04X", c); return b; }

struct Block {
    int K = 0, len = 0, have = 0;
    bool done = false, failed = false;
    std::vector<int> idx;
    std::vector<std::vector<uint8_t>> pcs;
    std::vector<uint8_t> got;
};

struct Dir {
    int base = 0;
    std::map<int, Block> blocks;
    std::vector<uint8_t> stream;
    MercuryDirection pub;
};

struct Sess {
    MercurySession pub;
    Dir d[2];
    double lastEnd = -1e9;
    int lastDir = 1;             // after the ACCEPT the callee spoke last
    double lastHeard = 0;        // signal seconds
};

struct FileRx {
    MercuryFile pub;
    uint32_t F = 0, T = 0, Z = 1;
    std::vector<std::unique_ptr<RqBlockDecoder>> blk;
    std::vector<uint8_t> data;   // the object, then (bundle) the whole bundle
    size_t off = 0, len = 0;
};

} // namespace

struct Monitor::Impl {
    mutable std::mutex mu;
    uint64_t ver = 1;
    std::vector<MercuryFrameLog> frames;
    std::vector<Sess> sess;
    std::vector<MercuryStation> stations;
    std::vector<FileRx> files;
    std::string messages;
    std::vector<std::string> known;      // callsigns heard or given, upper case
    uint64_t plain = 0, seeded = 0, fecFailed = 0, bcast = 0, enc = 0, decoded = 0, bursts = 0;
    uint64_t perMode[kNumModes] = {};
    int lastMode = -1;
    double lastCfo = 0;
    float lastSnr = 0;
    std::vector<float> constel;
    int nextKey = 0;

    void addKnown(const std::string& c) {
        if (c.empty()) return;
        for (const std::string& k : known) if (k == c) return;
        known.push_back(c);
        // a session waiting for the callee's name may get it now
        for (Sess& s : sess) resolveCallee(s);
    }

    static bool named(const std::string& c) { return !c.empty() && c != "?" && c.rfind("CRC ", 0) != 0; }
    void resolveCallee(Sess& s) {
        if (s.pub.callee.rfind("CRC ", 0) == 0)
            for (const std::string& k : known) if (callsignCrc(k) == s.pub.calledCrc) { s.pub.callee = k; break; }
        // both callsigns and the ACCEPT: the seed as the stations compute it
        if (!s.pub.seedKnown && named(s.pub.caller) && named(s.pub.callee) && s.pub.startLevel >= 0 && s.pub.id >= 0) {
            s.pub.seed = sessionSeed(s.pub.id, s.pub.nonce, s.pub.ctlDeaf ? 0 : s.pub.startLevel, s.pub.caller, s.pub.callee);
            s.pub.seedKnown = true;
        }
    }

    void station(const std::string& call, const char* kind, int64_t utc, const std::string& to, int bw) {
        if (call.empty()) return;
        addKnown(call);
        for (size_t i = 0; i < stations.size(); i++) {
            MercuryStation& s = stations[i];
            if (s.call != call) continue;
            s.count++; s.lastSeen = utc; s.last = kind;
            if (!to.empty()) s.heardTo = to;
            if (bw) s.bwHz = bw;
            std::rotate(stations.begin(), stations.begin() + (long)i, stations.begin() + (long)i + 1);
            return;
        }
        MercuryStation s;
        s.call = call; s.count = 1; s.firstSeen = s.lastSeen = utc; s.last = kind; s.heardTo = to; s.bwHz = bw;
        stations.insert(stations.begin(), s);
        if (stations.size() > kMercuryStationsMax) stations.pop_back();
    }

    Sess* sessionById(int id, int64_t utc) {
        for (Sess& s : sess) if (s.pub.id == id && utc - s.pub.lastUtc < 3600) return &s;
        return nullptr;
    }
    Sess* sessionBySeed(uint16_t seed) {
        for (Sess& s : sess) if (s.pub.seedKnown && s.pub.seed == seed) return &s;
        return nullptr;
    }
    Sess& newSession(int64_t utc) {
        Sess s;
        s.pub.key = nextKey++;
        s.pub.firstUtc = s.pub.lastUtc = utc;
        sess.insert(sess.begin(), std::move(s));
        if (sess.size() > kMercurySessionsMax) sess.pop_back();
        return sess.front();
    }

    void log(MercuryFrameLog&& l) {
        frames.insert(frames.begin(), std::move(l));
        if (frames.size() > kMercuryFramesMax) frames.pop_back();
    }

    // which station of the session sent this burst
    int senderOf(Sess& s, double cfo, double start) {
        int dir;
        const MercuryDirection &a = s.d[0].pub, &b = s.d[1].pub;
        if (a.cfoKnown && b.cfoKnown && std::fabs(a.cfoHz - b.cfoHz) >= 1.5) dir = std::fabs(cfo - a.cfoHz) <= std::fabs(cfo - b.cfoHz) ? 0 : 1;
        else if (start - s.lastEnd < kKeydownGap) dir = s.lastDir;
        else dir = 1 - s.lastDir;
        return dir;
    }
    void heardFrom(Sess& s, int dir, double cfo, double end) {
        MercuryDirection& d = s.d[dir].pub;
        d.cfoHz = d.cfoKnown ? 0.7 * d.cfoHz + 0.3 * cfo : cfo;
        d.cfoKnown = true;
        s.lastDir = dir;
        s.lastEnd = end;
    }

    void takeData(Sess& s, int dir, const CarData& cd, const uint8_t* bytes, int P) {
        Dir& D = s.d[dir];
        for (const CarSegment& g : cd.segs) {
            int dd = (g.block - D.base) & 0x0F;
            if (dd >= 8) dd -= 16;
            const int id = D.base + dd;
            if (id < D.base) continue;   // delivered already
            Block& b = D.blocks[id];
            if (b.K == 0) { b.K = g.K; b.len = g.K * P - g.pad; b.got.assign(256, 0); }
            if (b.K != g.K || b.len != g.K * P - g.pad || b.done) continue;
            for (int p = 0; p < g.count; p++) {
                const int i = (g.first + p) % 256;
                if (b.got[(size_t)i] || b.have >= b.K) continue;
                b.got[(size_t)i] = 1;
                b.idx.push_back(i);
                b.pcs.emplace_back(bytes + g.offset + (size_t)p * (size_t)P, bytes + g.offset + (size_t)(p + 1) * (size_t)P);
                b.have++;
            }
            if (b.have >= b.K && !b.done) {
                std::vector<std::vector<uint8_t>> out;
                b.done = true;
                if (!rsDecode(b.K, P, b.idx, b.pcs, out)) { b.failed = true; D.pub.blocksFailed++; continue; }
                std::vector<uint8_t> all;
                for (auto& v : out) all.insert(all.end(), v.begin(), v.end());
                all.resize((size_t)std::max(0, b.len));
                const int app = b.len - 4;
                uint32_t want = 0;
                for (int i = 0; i < 4 && app + i >= 0; i++) want |= (uint32_t)all[(size_t)(app + i)] << (8 * i);
                if (app < 0 || (s.pub.seedKnown && blockCheck(s.pub.seed, id & 0xFF, b.K, all.data(), (size_t)app) != want)) {
                    b.failed = true; D.pub.blocksFailed++;
                    continue;
                }
                D.pub.blocksOk++;
                b.pcs.clear();
                b.pcs.push_back(std::vector<uint8_t>(all.begin(), all.begin() + app));
            }
        }
        // deliver the blocks at the base in order
        for (;;) {
            auto it = D.blocks.find(D.base);
            if (it == D.blocks.end() || !it->second.done) break;
            if (!it->second.failed && !it->second.pcs.empty()) {
                const std::vector<uint8_t>& v = it->second.pcs[0];
                if (D.stream.size() + v.size() <= kStreamMax) D.stream.insert(D.stream.end(), v.begin(), v.end());
                D.pub.bytes += v.size();
                D.pub.text += printable(v.data(), v.size());
                if (D.pub.text.size() > kMercuryTextMax) D.pub.text.erase(0, D.pub.text.size() - kMercuryTextMax);
            }
            D.blocks.erase(it);
            D.base++;
        }
        // blocks far behind the base that never completed (lost) do not hold the window for ever
        while (!D.blocks.empty() && D.blocks.begin()->first < D.base) D.blocks.erase(D.blocks.begin());
    }

    void onFile(const BcastFileFrame& ff, const uint8_t* p, int64_t utc, const char* modeName, MercuryFrameLog& l) {
        FileRx* f = nullptr;
        for (FileRx& x : files) if (x.pub.session == ff.session && x.F == ff.F && x.T == (uint32_t)ff.T && x.Z == (uint32_t)ff.Z) f = &x;
        if (!f) {
            if (ff.N != 1 || ff.F > (16u << 20)) { l.detail = "RaptorQ with sub-blocks: not supported"; return; }
            FileRx x;
            x.pub.id = files.empty() ? 0 : files.back().pub.id + 1;
            x.pub.session = ff.session;
            x.pub.size = ff.F;
            x.pub.firstUtc = utc;
            x.pub.mode = modeName;
            x.F = ff.F; x.T = (uint32_t)ff.T; x.Z = (uint32_t)ff.Z;
            for (uint32_t z = 0; z < x.Z; z++) x.blk.push_back(std::make_unique<RqBlockDecoder>(rqBlockSymbols(x.F, x.T, x.Z, z), x.T));
            x.pub.needed = (x.F + x.T - 1) / x.T;
            files.push_back(std::move(x));
            if (files.size() > kMercuryFilesMax) files.erase(files.begin());
            f = &files.back();
        }
        if (f->pub.done) { l.detail = "file " + f->pub.name + " (complete)"; return; }
        RqBlockDecoder& d = *f->blk[(size_t)ff.sbn];
        for (int i = 0; i < ff.symbols; i++) {
            if (ff.esi + (uint32_t)i > 65535) break;
            if (d.add(ff.esi + (uint32_t)i, p + ff.offset + (size_t)i * (size_t)ff.T)) f->pub.symbols++;
        }
        d.tryDecode();
        bool all = true;
        for (auto& b : f->blk) all &= b->done();
        char b[160];
        snprintf(b, sizeof b, "file session %d: %u bytes, block %d, symbols %u+%d, %llu of %llu", ff.session, ff.F, ff.sbn, ff.esi, ff.symbols,
                 (unsigned long long)f->pub.symbols, (unsigned long long)f->pub.needed);
        l.detail = b;
        if (!all) return;
        f->data.clear();
        for (auto& bk : f->blk) f->data.insert(f->data.end(), bk->data().begin(), bk->data().end());
        f->data.resize(f->F);
        std::string name;
        size_t off = 0, len = f->data.size();
        if (parseBundle(f->data, name, off, len)) { f->pub.name = name; f->pub.bundle = true; }
        else {
            const time_t t = (time_t)utc;
            struct tm g {};
#ifdef _WIN32
            gmtime_s(&g, &t);
#else
            gmtime_r(&t, &g);
#endif
            char nb[64];
            strftime(nb, sizeof nb, "broadcast_%Y%m%d_%H%M%S.bin", &g);
            f->pub.name = nb;
        }
        f->off = off; f->len = len;
        f->pub.kind = kindOfFile(f->data, off, len);
        if (f->pub.kind == "text") f->pub.preview = printable(f->data.data() + off, std::min<size_t>(len, 2000));
        f->pub.done = true;
        f->pub.doneUtc = utc;
        l.detail = "file " + f->pub.name + " complete, " + std::to_string(len) + " bytes";
    }

    void onFrame(const OfdmFrame& f, double utc, double start, double air) {
        std::lock_guard<std::mutex> lk(mu);
        ver++;
        bursts++;
        MercuryFrameLog l;
        l.utc = (int64_t)utc; l.timeSec = start; l.airSec = air;
        l.mode = f.mode; l.modeName = modeName(f.mode);
        l.snrDb = f.snrDb; l.cfoHz = f.cfoHz;
        l.fecOk = f.ldpcOk; l.ldpcIters = f.ldpcIters; l.rawErrors = f.rawErrors;
        l.payloadLen = (int)f.payload.size();
        lastMode = f.mode; lastCfo = f.cfoHz; lastSnr = f.snrDb;
        constel.clear();
        for (const cf& z : f.constellation) { constel.push_back(z.real()); constel.push_back(z.imag()); }
        if (!f.ldpcOk) {
            fecFailed++;
            l.kind = "burst";
            l.detail = "FEC did not converge";
            log(std::move(l));
            return;
        }
        perMode[f.mode]++;
        const uint8_t* p = f.payload.data();
        const size_t n = f.payload.size();
        const int64_t now = (int64_t)utc;
        if (f.crcPlain) {
            plain++; decoded++;
            l.crcOk = true;
            l.hex = hexOf(f.payload);
            const int type = n ? packetType(p[0]) : -1;
            CallFrame cf; AcceptFrame af; CqFrame qf; DisconnectFrame df; BcastMessage bm; BcastFileFrame bf;
            if (type == kPtCall2 && parseCall(p, n, cf)) {
                l.kind = "CALL"; l.sessionId = cf.sessionId; l.from = cf.caller;
                Sess* s = sessionById(cf.sessionId, now);
                if (!s || s->pub.caller != cf.caller) s = &newSession(now);
                s->pub.id = cf.sessionId; s->pub.caller = cf.caller; s->pub.calledCrc = cf.calledCrc; s->pub.bwHz = cf.bwHz;
                if (s->pub.callee.empty()) s->pub.callee = crcName(cf.calledCrc);
                if (s->pub.state.empty()) s->pub.state = "calling";
                s->pub.lastUtc = now; s->pub.frames++;
                resolveCallee(*s);
                heardFrom(*s, 0, f.cfoHz, start + air);
                l.to = s->pub.callee; l.sessionKey = s->pub.key; l.dir = 0;
                char b[96]; snprintf(b, sizeof b, "bandwidth %d Hz%s", cf.bwHz, cf.minor ? ", later 2.x revision" : "");
                l.detail = b;
                station(cf.caller, "CALL", now, l.to, cf.bwHz);
            } else if (type == kPtCall2 && parseAccept(p, n, af)) {
                l.kind = "ACCEPT"; l.sessionId = af.sessionId;
                Sess* s = sessionById(af.sessionId, now);
                if (s && !s->pub.caller.empty() && s->pub.caller.rfind("CRC ", 0) != 0 && callsignCrc(s->pub.caller) != af.callerCrc) s = nullptr;
                if (!s) { s = &newSession(now); s->pub.id = af.sessionId; s->pub.caller = crcName(af.callerCrc); s->pub.callee = crcName(af.calledCrc); s->pub.calledCrc = af.calledCrc; }
                s->pub.nonce = af.nonce; s->pub.startLevel = af.startLevel; s->pub.ctlDeaf = af.ctlDeaf; s->pub.bwHz = af.bwHz;
                if (s->pub.calledCrc == 0) s->pub.calledCrc = af.calledCrc;
                s->pub.encrypted = s->pub.encrypted || af.reservedSet || af.minor != 0;
                s->pub.state = "connected";
                s->pub.lastUtc = now; s->pub.frames++;
                resolveCallee(*s);
                heardFrom(*s, 1, f.cfoHz, start + air);
                l.from = s->pub.callee; l.to = s->pub.caller; l.sessionKey = s->pub.key; l.dir = 1;
                char b[160];
                snprintf(b, sizeof b, "start rung %s%s, nonce %04X, bandwidth %d Hz%s%s", rungName(af.ctlDeaf ? 0 : af.startLevel), af.ctlDeaf ? " (hears the caller below the control mode)" : "",
                         af.nonce, af.bwHz, af.callSnrDb > -999 ? (", CALL heard at " + std::to_string((int)af.callSnrDb) + " dB").c_str() : "",
                         s->pub.encrypted ? ", encrypted session" : "");
                l.detail = b;
                l.encrypted = s->pub.encrypted;
            } else if (type == kPtCq && parseCq(p, n, qf)) {
                l.kind = "CQ"; l.from = qf.caller;
                char b[64]; snprintf(b, sizeof b, "bandwidth %d Hz", qf.bwHz);
                l.detail = b;
                station(qf.caller, "CQ", now, "", qf.bwHz);
            } else if (type == kPtArqControl && parseDisconnect(p, n, df)) {
                l.kind = "DISCONNECT"; l.sessionId = df.sessionId;
                if (Sess* s = sessionById(df.sessionId, now)) {
                    const int dir = senderOf(*s, f.cfoHz, start);
                    heardFrom(*s, dir, f.cfoHz, start + air);
                    s->pub.state = "disconnected"; s->pub.lastUtc = now; s->pub.frames++;
                    l.sessionKey = s->pub.key; l.dir = dir;
                    l.from = dir == 0 ? s->pub.caller : s->pub.callee; l.to = dir == 0 ? s->pub.callee : s->pub.caller;
                }
                if (df.snrDb > -999) l.detail = "SNR report " + std::to_string((int)df.snrDb) + " dB";
            } else if (type == kPtBcastData && parseBcastMessage(p, n, bm)) {
                bcast++;
                l.kind = "MESSAGE";
                const std::string txt = printable(bm.bytes.data(), bm.bytes.size());
                const bool text = mostlyText(bm.bytes.data(), bm.bytes.size());
                l.detail = (bm.lenPrefix ? "" : "(no length) ") + std::string(bm.ext & 2 ? "KISS " : bm.ext & 4 ? "data " : "") + (text ? txt : hexOf(bm.bytes, 64));
                // "CALL: message" (Mercury's chat) names the sender
                const size_t c = txt.find(": ");
                if (text && c != std::string::npos && c > 0 && c < 16) {
                    bool ok = true;
                    for (size_t i = 0; i < c; i++) ok &= std::isalnum((unsigned char)txt[i]) || txt[i] == '-' || txt[i] == '/';
                    if (ok) { l.from = txt.substr(0, c); station(l.from, "message", now, "", 0); }
                }
                char tb[16];
                const time_t tt = (time_t)utc;
                struct tm g {};
#ifdef _WIN32
                gmtime_s(&g, &tt);
#else
                gmtime_r(&tt, &g);
#endif
                strftime(tb, sizeof tb, "%H:%M:%S", &g);
                messages += std::string(tb) + " " + l.modeName + ": " + (text ? txt : hexOf(bm.bytes, 96)) + "\n";
                if (messages.size() > kMercuryTextMax) messages.erase(0, messages.size() - kMercuryTextMax);
            } else if (type == kPtBcastControl && parseBcastFile(p, n, bf)) {
                bcast++;
                l.kind = "FILE"; l.sessionId = bf.session;
                onFile(bf, p, now, l.modeName.c_str(), l);
            } else if (type == kPtArqData19 || type == kPtCall19 || type == kPtArqControl) {
                l.kind = "FRAME"; l.detail = std::string(packetTypeName(type)) + ": another protocol version (Mercury 1.9.x?)";
            } else {
                l.kind = "FRAME"; l.detail = std::string("unknown frame, type ") + std::to_string(type);
            }
            log(std::move(l));
            return;
        }
        // a seeded frame: a session's carousel
        seeded++; decoded++;
        const uint16_t seed = (uint16_t)(f.crcRx ^ f.crcCalc);
        l.seeded = true; l.seed = seed; l.crcOk = true;
        Sess* s = sessionBySeed(seed);
        if (!s) {
            // a connect heard without both callsigns: the most recent session still waiting for its seed takes it
            for (Sess& x : sess) if (!x.pub.seedKnown && x.pub.state == "connected" && now - x.pub.lastUtc < 600) { s = &x; break; }
            if (s) { s->pub.seed = seed; s->pub.seedKnown = true; s->pub.seedFromFrames = true; }
        }
        if (!s) {
            s = &newSession(now);
            s->pub.seed = seed; s->pub.seedKnown = true; s->pub.seedFromFrames = true;
            s->pub.caller = "?"; s->pub.callee = "?";
            s->pub.state = "heard (connect not heard)";
        }
        s->pub.lastUtc = now; s->pub.frames++;
        const int dir = senderOf(*s, f.cfoHz, start);
        heardFrom(*s, dir, f.cfoHz, start + air);
        l.sessionKey = s->pub.key; l.sessionId = s->pub.id; l.dir = dir;
        l.from = dir == 0 ? s->pub.caller : s->pub.callee; l.to = dir == 0 ? s->pub.callee : s->pub.caller;
        if (s->pub.encrypted) {
            enc++;
            l.kind = f.mode == kDatac16 || (n && p[0] == 0x07 && (f.mode == kMfsk || f.mode == kMfsk500)) ? "CONTROL" : "DATA";
            l.encrypted = true;
            l.detail = "encrypted";
            log(std::move(l));
            return;
        }
        l.hex = hexOf(f.payload);
        const int P = pieceSize(s->pub.startLevel < 0 ? 1 : s->pub.startLevel, s->pub.ctlDeaf);
        CarCtl cc;
        CarData cd;
        const bool mfsk = f.mode == kMfsk || f.mode == kMfsk500;
        // a control frame: on the control mode, or on the floor behind its mark 0x07 (to a station that hears its peer below DATAC16)
        if ((f.mode == kDatac16 && parseCarCtl(p, n, cc)) || (mfsk && n > 14 && p[0] == 0x07 && parseCarCtl(p + 1, n - 1, cc))) {
            static const char* kTypes[4] = {"POLL", "REQ", "STATUS", "POLL"};
            l.kind = kTypes[cc.type & 3];
            // a poll from the receiver of a direction is about the other station's data
            MercuryDirection& other = s->d[1 - dir].pub;
            s->d[dir].pub.ctlFrames++;
            other.askedRung = cc.level; other.askedFrames = cc.frames;
            if (cc.snrDb > -999) other.reportedSnrDb = cc.snrDb;
            char b[200];
            int k = snprintf(b, sizeof b, "asks for %d frame%s on %s", cc.frames, cc.frames == 1 ? "" : "s", rungName(cc.level));
            if (cc.snrDb > -999) k += snprintf(b + k, sizeof b - (size_t)k, ", hears the peer at %+.0f dB", cc.snrDb);
            if (cc.hFrames) k += snprintf(b + k, sizeof b - (size_t)k, ", handover: %d frame%s on %s", cc.hFrames, cc.hFrames == 1 ? "" : "s", rungName(cc.hLevel));
            if (cc.type == 3) k += snprintf(b + k, sizeof b - (size_t)k, ", its application has hung up");
            if (cc.hasData) k += snprintf(b + k, sizeof b - (size_t)k, ", has data");
            snprintf(b + k, sizeof b - (size_t)k, ", loss %d/16", cc.loss16);
            l.detail = b;
        } else if (parseCarData(p, n, P, cd)) {
            l.kind = "DATA";
            Dir& D = s->d[dir];
            D.pub.dataFrames++;
            D.pub.lastRung = rungOfMode(f.mode);
            takeData(*s, dir, cd, p, P);
            char b[200];
            int k = snprintf(b, sizeof b, "%d more in the round", cd.left);
            for (const CarSegment& g : cd.segs) k += snprintf(b + k, sizeof b - (size_t)k, ", block %d: pieces %d..%d of K %d", g.block, g.first, g.first + g.count - 1, g.K);
            if (cd.hasInlineCtl) snprintf(b + k, sizeof b - (size_t)k, ", with a poll for %d frame%s on %s", cd.inlineCtl.frames, cd.inlineCtl.frames == 1 ? "" : "s", rungName(cd.inlineCtl.level));
            l.detail = b;
        } else {
            l.kind = "FRAME";
            l.detail = "session frame not understood";
        }
        log(std::move(l));
    }

    void fill(MercuryTelemetry& t) const {
        std::lock_guard<std::mutex> lk(mu);
        t.frames = frames;
        t.sessions.clear();
        for (const Sess& s : sess) {
            MercurySession m = s.pub;
            m.d[0] = s.d[0].pub; m.d[1] = s.d[1].pub;
            t.sessions.push_back(std::move(m));
        }
        t.stations = stations;
        t.files.clear();
        for (const FileRx& f : files) t.files.push_back(f.pub);
        t.messages = messages;
        t.plainFrames = plain; t.seededFrames = seeded; t.fecFailed = fecFailed; t.broadcastFrames = bcast; t.encryptedFrames = enc;
        t.bursts = bursts;
        t.blocksOk = decoded; t.blocksBad = fecFailed;
        for (int i = 0; i < kNumModes && i < kMercuryModes; i++) t.perMode[i] = perMode[i];
        t.lastMode = lastMode;
        t.constellation = constel;
        t.cfoHz = lastCfo; t.snrDb = lastSnr;
    }
};

Monitor::Monitor() : p_(std::make_unique<Impl>()) {}
Monitor::~Monitor() = default;
void Monitor::onFrame(const OfdmFrame& f, double utc, double startSec, double airSec) { p_->onFrame(f, utc, startSec, airSec); }
void Monitor::setKnownCalls(const std::vector<std::string>& calls) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const std::string& c : calls) {
        std::string u;
        for (char ch : c) if (!std::isspace((unsigned char)ch)) u += (char)std::toupper((unsigned char)ch);
        p_->addKnown(u);
    }
    p_->ver++;
}
void Monitor::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.frames.clear(); m.sess.clear(); m.stations.clear(); m.files.clear(); m.messages.clear();
    m.plain = m.seeded = m.fecFailed = m.bcast = m.enc = m.decoded = m.bursts = 0;
    std::fill(std::begin(m.perMode), std::end(m.perMode), 0);
    m.constel.clear();
    m.ver++;
}
void Monitor::fill(MercuryTelemetry& t) const { p_->fill(t); }
uint64_t Monitor::version() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->ver; }
bool Monitor::fileBytes(int id, std::vector<uint8_t>& data, std::string& name) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const FileRx& f : p_->files) {
        if (f.pub.id != id || !f.pub.done) continue;
        data.assign(f.data.begin() + (long)f.off, f.data.begin() + (long)(f.off + f.len));
        name = f.pub.name;
        return true;
    }
    return false;
}
bool Monitor::sessionBytes(int key, int dir, std::vector<uint8_t>& data) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const Sess& s : p_->sess) {
        if (s.pub.key != key || dir < 0 || dir > 1 || s.pub.encrypted) continue;
        data = s.d[dir].stream;
        return true;
    }
    return false;
}

} // namespace mercury
} // namespace dect2
