// See sd3_rx.h. Link parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/sd3/sd3_rx.h"
#include "dect2/sd3/sd3_proto.h"
#include "sd3_demod.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_tel.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/wxsat/wxsat_proto.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
using namespace sd3;

constexpr double kPubSec = 0.1;
double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
inline int8_t sat8(float v) { return (int8_t)std::clamp((int)std::lround(v), -127, 127); }

// One background thread for the disk writes, so the demodulator never waits.
class Writer {
public:
    ~Writer() { { std::lock_guard<std::mutex> lk(mu_); stop_ = true; } cv_.notify_all(); if (th_.joinable()) th_.join(); }
    void push(std::string path, std::vector<uint8_t> data) {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.size() > 2000) return;
        q_.push_back({std::move(path), std::move(data)});
        if (!th_.joinable()) th_ = std::thread([this] { run(); });
        cv_.notify_one();
    }
private:
    void run() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) { if (stop_) return; continue; }
            auto j = std::move(q_.front());
            q_.pop_front();
            lk.unlock();
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(j.first).parent_path(), ec);
            std::ofstream f(j.first, std::ios::binary);
            if (f) f.write((const char*)j.second.data(), (std::streamsize)j.second.size());
            lk.lock();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> q_;
    std::thread th_;
    bool stop_ = false;
};

uint64_t encodedSync(uint32_t sync, bool nrzm) {
    meteor::ConvEncoder e;
    uint64_t v = 0;
    int prev = 0;
    for (int i = 31; i >= 0; i--) {
        int bit = (int)(sync >> i & 1);
        if (nrzm) { prev ^= bit; bit = prev; }      // NRZ-M sits inside the code: the sync word on the air is the differentially coded one (a complement sync with the other start, which the search covers)
        uint8_t a, b;
        e.push(bit, a, b);
        v = v << 2 | (uint64_t)a << 1 | b;
    }
    return v;
}
constexpr uint64_t kCodedMask = (1ull << 52) - 1;     // the code bits that do not depend on the encoder's earlier state

void xform(int t, int a, int b, int& oa, int& ob) {
    if (t & 4) b = -b;
    switch (t & 3) { case 0: oa = a; ob = b; break; case 1: oa = -b; ob = a; break; case 2: oa = -a; ob = -b; break; default: oa = b; ob = -a; break; }
}

// Burst links: every sync word starts one frame, nothing is assumed about the next one.
class BurstDeframer {
public:
    std::function<void(const uint8_t*)> onFrame;
    void configure(uint32_t sync, int frameBits, int tol) { sync_ = sync; bits_ = frameBits; tol_ = tol; frame_.assign((size_t)(frameBits + 7) / 8, 0); reset(); }
    void reset() { sr_ = 0; n_ = 0; have_ = 0; }
    void push(int b) {
        if (n_ > 0) {
            int v = b ^ inv_;
            frame_[(size_t)have_ >> 3] = (uint8_t)(frame_[(size_t)have_ >> 3] | (v << (7 - (have_ & 7))));
            have_++;
            if (--n_ == 0) { if (onFrame) onFrame(frame_.data()); sr_ = 0; }
            return;
        }
        sr_ = (sr_ << 1) | (uint32_t)b;
        have_ = 0;
        const int e0 = __builtin_popcount(sr_ ^ sync_), e1 = 32 - e0;
        if (e0 <= tol_ || e1 <= tol_) {
            inv_ = e0 <= tol_ ? 0 : 1;
            std::fill(frame_.begin(), frame_.end(), 0);
            for (int i = 0; i < 4; i++) frame_[(size_t)i] = (uint8_t)(sync_ >> (24 - 8 * i));
            have_ = 32;
            n_ = bits_ - 32;
        }
    }
private:
    uint32_t sync_ = 0, sr_ = 0;
    int bits_ = 0, tol_ = 0, n_ = 0, have_ = 0, inv_ = 0;
    std::vector<uint8_t> frame_;
};

Mod toMod(int m) { return m == kSd3Bpsk ? Bpsk : m == kSd3Qpsk ? Qpsk : m == kSd3Oqpsk ? Oqpsk : m == kSd3Pm ? Pm : Fsk; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------
struct Sd3Receiver::Impl {
    const int P;
    const Sd3ProfileInfo info;
    mutable std::mutex mu;
    std::function<void(const std::string&)> logCb;
    double rate = 0, offset = 0;
    std::atomic<bool> resetReq{false}, clearReq{false}, blocking{false};
    std::atomic<int> linkReq{0}, productReq{-1};
    int linkIdx = 0;
    Sd3Link link{};
    Sd3Telemetry tel, pub;
    std::string saveDir;
    std::mutex dirMu;
    Writer writer;

    Demod dem;
    std::unique_ptr<DvbsReceiver> dvb;

    // table links
    bool simpleQpsk = false;
    int nrzPrev = 0;
    int qdiffPrev = 0, qdiffHave = 0;
    wxsat::Deframer def[4];
    BurstDeframer burst;
    bool burstLink = false;
    int defUse = -1;
    uint64_t codedPat = 0;
    std::vector<int8_t> soft;
    int softSince = 0, xt = 0, slip = 0;
    bool searchLock = false;
    hrpt::Viterbi vit;
    int frameBytes = 0;
    bool mdl = false, gvar = false, geoscan = false, lucky7 = false, spino = false, s2 = false;
    std::vector<uint8_t> luckyBits;
    SpinoDeframer spinoDef[2];
    // S2 links
    CaduStream caduS;
    std::unique_ptr<GrbDecoder> grb;
    std::unique_ptr<UhritDecoder> uhrit;
    std::map<int, uint64_t> apidCount;
    uint64_t dvbSeq = 0;
    int dvbState = 0;

    // pictures
    std::map<int, std::shared_ptr<WxImage>> grbImg;
    std::map<int, std::string> grbName;
    std::map<int, uint64_t> grbTouched;
    uint64_t grbClock = 0;
    int shownKey = -1;
    std::shared_ptr<WxImage> im[2];
    bool imgDirty[2] = {false, false};
    uint64_t imgId = 0;
    std::vector<uint16_t> visPx, ir0, ir1, ir2, ir3;
    double lastGood = 0, lastAcq = 0, tStart = 0, statStart = 0;
    int64_t nIn = 0, nextPub = 0;
    float levelAcc = 0;
    uint64_t levelN = 0;

    explicit Impl(int p) : P(std::clamp(p, 0, (int)kSd3NumProfiles - 1)), info(sd3Profile(P)) { tel.profile = pub.profile = P; }

    void say(const std::string& s) { if (logCb) logCb(std::string("sd3 ") + info.id + ": " + s); }

    std::shared_ptr<WxImage> mk(int w, int c, const char* n) {
        auto i = std::make_shared<WxImage>();
        i->name = n; i->width = w; i->channels = c; i->id = ++imgId;
        return i;
    }

    void initImages() {
        im[0].reset(); im[1].reset();
        if (P == kSd3Gvar) { im[0] = mk(kGvarIrWidth, 1, "Imager visible (reduced)"); im[1] = mk(kGvarIrWidth / 2, 4, "Imager infrared"); }
        grbImg.clear(); grbName.clear(); grbTouched.clear();
        tel.lines = 0;
    }

    void setLineChan(int w, int idx, int c, const uint8_t* px) {
        if (!im[w] || idx < 0 || idx > 20000) return;
        WxImage& I = *im[w];
        if ((size_t)idx >= I.line.size()) I.line.resize((size_t)idx + 1);
        auto ln = std::make_shared<WxLine>();
        ln->idx = idx;
        if (I.line[(size_t)idx]) ln->px = I.line[(size_t)idx]->px; else ln->px.assign((size_t)I.width * (size_t)I.channels, 0);
        std::memcpy(&ln->px[(size_t)c * (size_t)I.width], px, (size_t)I.width);
        I.line[(size_t)idx] = ln;
        I.lines = std::max(I.lines, idx + 1);
        tel.lines = (uint64_t)I.lines;
        imgDirty[w] = true;
    }

    void setup() {
        linkIdx = std::clamp(linkReq.load(), 0, sd3LinkCount(P) - 1);
        link = sd3LinkAt(P, linkIdx);
        tel.link = pub.link = linkIdx;
        tel.satName = link.name;
        mdl = P == kSd3Mdl; gvar = P == kSd3Gvar; geoscan = P == kSd3Geoscan; lucky7 = P == kSd3Lucky7; spino = P == kSd3Spino;
        s2 = P == kSd3Grb || P == kSd3Uhrit;
        frameBytes = (link.frameBits + 7) / 8;
        simpleQpsk = !link.conv && (link.mod == kSd3Qpsk || link.mod == kSd3Oqpsk) && !link.nrzm;
        auto frameCb = [this](const uint8_t* f) { onFrame(f); };
        for (auto& d : def) { d.onFrame = frameCb; d.configure(link.sync, 32, link.frameBits, 3, 9); d.hits = 0; }
        defUse = -1;
        if (mdl) for (auto& d : def) d.configure(0x0BEBCC80u, 32, 464 * 8, 3, 9);
        if (gvar) { def[0].configure(kGvarSync, 64, kGvarBytes * 8, 6, 14, false); }
        if (geoscan) def[0].configure(0x930B51DEu, 32, (kGeoscanData + 4) * 8, 4, 8, true);
        burstLink = geoscan || P == kSd3Erminaz;
        if (burstLink) { burst.onFrame = frameCb; burst.configure(link.sync, link.frameBits, 4); }
        codedPat = encodedSync(link.sync, link.nrzm);
        soft.clear(); softSince = 0; searchLock = false; vit.reset(); xt = 0; slip = 0;
        nrzPrev = 0; qdiffHave = 0; qdiffPrev = 0;
        luckyBits.clear(); spinoDef[0].reset(); spinoDef[1].reset();
        spinoDef[0].onFrame = spinoDef[1].onFrame = [this](const std::vector<uint8_t>& f) { logFrame(f.data(), f.size(), true); tel.blocksOk++; markGood(); };
        caduS.configure(kGrbCadu);
        tel.symbolRate = pub.symbolRate = link.sym;
        apidCount.clear();
        if (s2) {
            if (P == kSd3Grb) {
                grb = std::make_unique<GrbDecoder>();
                grb->onLine = [this](int apid, int w, int h, int y, const uint8_t* px) { grbLine(apid, w, h, y, px); };
                grb->onProduct = [this](int apid, const std::string& n) { grbName[apid] = n; };
                grb->onFile = [this](const std::string& n, const std::vector<uint8_t>& d) { saveFile("meta/" + n, d); tel.files++; };
                grb->onPacket = [this](const Packet& p) { apidCount[p.apid]++; };
            } else {
                uhrit = std::make_unique<UhritDecoder>();
                uhrit->onFile = [this](const std::vector<uint8_t>& f, int vc, int apid) { xritFile(f, vc, apid); };
                uhrit->onPacket = [this](const Packet& p) { apidCount[p.apid]++; };
            }
            if (rate > 0) {
                dvb = std::make_unique<DvbsReceiver>();
                dvb->configure(rate);
                dvb->setStandardHint(2);
                dvb->setSymbolRate(link.sym);
                dvb->setRollOff(link.rrc);
                dvb->setBlocking(blocking.load());
                if (offset != 0) dvb->selectCarrier(true, offset);
                dvb->setDataFieldCallback([this](const uint8_t* bb, size_t n, int, int, bool, int) { s2Field(bb, n); });
                dvb->setLogCallback([this](const std::string& s) { say(s); });
            }
        } else if (rate > 0) {
            dem.configure(rate, link.sym, toMod(link.mod), link.rrc);
            dem.setOffset(offset);
        }
        statStart = nowSec();
    }

    void s2Field(const uint8_t* bb, size_t n) {
        if (n <= 10) return;
        caduS.push(bb + 10, n - 10, [this](const uint8_t* c) {
            int scid, vcid; frameIds(c, kGrbCadu, scid, vcid);
            tel.scid = scid; tel.vcid = vcid;
            if (vcid >= 0 && vcid < 64) tel.vcidFrames[vcid]++;
            tel.blocksOk++;
            if (grb) grb->pushCadu(c);
            if (uhrit) uhrit->pushCadu(c);
            logFrame(c, kGrbCadu, true);
            markGood();
        });
    }

    void saveFile(const std::string& name, const std::vector<uint8_t>& data) {
        std::string dir;
        { std::lock_guard<std::mutex> lk(dirMu); dir = saveDir; }
        if (dir.empty()) return;
        writer.push(dir + "/" + name, data);
        tel.lastFile = name;
    }

    void xritFile(const std::vector<uint8_t>& f, int vc, int apid) {
        tel.files++;
        std::string name = xritAnnotation(f);
        if (name.empty()) { char b[48]; snprintf(b, sizeof b, "vc%d_apid%d_%llu.lrit", vc, apid, (unsigned long long)tel.files); name = b; }
        for (auto& ch : name) if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 32) ch = '_';
        tel.lastText = name;
        tel.lastFile = name;
        saveFile("files/" + name, f);
    }

    void grbLine(int apid, int w, int h, int y, const uint8_t* px) {
        auto& ptr = grbImg[apid];
        if (!ptr) { ptr = mk(w, 1, abiProductName(apid).c_str()); ptr->lines = 0; ptr->line.resize((size_t)h); say("picture started: " + std::string(ptr->name)); }
        auto ln = std::make_shared<WxLine>();
        ln->idx = y; ln->px.assign(px, px + w);
        if ((size_t)y < ptr->line.size()) ptr->line[(size_t)y] = ln;
        ptr->lines = std::max(ptr->lines, y + 1);
        grbTouched[apid] = ++grbClock;
        imgDirty[0] = true;
        tel.lines = (uint64_t)ptr->lines;
    }

    void logFrame(const uint8_t* f, size_t n, bool ok) {
        Sd3Frame fr;
        fr.timeSec = nowSec() - tStart;
        frameIds(f, n, fr.scid, fr.vcid);
        fr.bytes = (int)n;
        fr.rsOk = ok;
        char tmp[4];
        for (size_t i = 0; i < std::min<size_t>(n, 24); i++) { snprintf(tmp, sizeof tmp, "%02X", f[i]); fr.hex += tmp; }
        tel.frames.push_back(std::move(fr));
        while (tel.frames.size() > 64) tel.frames.pop_front();
    }

    void markGood() { lastGood = nowSec(); tel.frameLock = true; }

    void onFrame(const uint8_t* f) {
        tel.asmHits++;
        if (gvar) { gvarFrame(f); return; }
        if (mdl) { std::vector<uint8_t> v(f, f + 464); for (auto& b : v) b ^= 0xFF; tel.blocksOk++; logFrame(v.data(), v.size(), true); markGood(); return; }
        if (geoscan) {
            std::vector<uint8_t> v(f, f + kGeoscanData + 4);
            if (geoscanFrame(v.data() + 4)) { tel.blocksOk++; logFrame(v.data() + 4, kGeoscanData, true); markGood(); }
            else tel.blocksBad++;
            return;
        }
        std::vector<uint8_t> v(f, f + frameBytes);
        int fixed = 0;
        const bool ok = decodeFrame(link, v, fixed);
        if (ok) { tel.blocksOk++; tel.rsCorrected += (uint64_t)fixed; markGood(); } else tel.blocksBad++;
        int scid, vcid;
        frameIds(v.data(), v.size(), scid, vcid);
        if (ok && vcid >= 0) { tel.scid = scid; tel.vcid = vcid; if (vcid < 64) tel.vcidFrames[vcid]++; }
        logFrame(v.data(), v.size(), ok);
        if (ok) { std::string dir; { std::lock_guard<std::mutex> lk(dirMu); dir = saveDir; } if (!dir.empty()) saveCadu(v); }
    }

    std::vector<uint8_t> caduBuf;
    void saveCadu(const std::vector<uint8_t>& v) {
        caduBuf.insert(caduBuf.end(), v.begin(), v.end());
        if (caduBuf.size() > 1 << 20) { char b[64]; snprintf(b, sizeof b, "frames/%s_%llu.cadu", link.id, (unsigned long long)tel.blocksOk); saveFile(b, caduBuf); caduBuf.clear(); }
    }

    void gvarFrame(const uint8_t* fin) {
        std::vector<uint8_t> f(fin, fin + kGvarBytes);
        gvarScramble(f.data());
        const GvarBlock b = gvarHeader(f.data());
        if (b.id < 0 || b.id > 11) { tel.blocksBad++; return; }
        tel.blocksOk++;
        markGood();
        logFrame(f.data(), 64, b.crcOk);
        const int counter = gvarCounter(f.data());
        if (counter < 0) return;
        if (b.id == 1 || b.id == 2) {
            const int words = gvarIrWords(f.data());
            ir0.resize(kGvarIrWidth); ir1.resize(kGvarIrWidth); ir2.resize(kGvarIrWidth); ir3.resize(kGvarIrWidth);
            gvarIrPixels(f.data(), words, ir0.data(), ir1.data(), ir2.data(), ir3.data());
            std::vector<uint8_t> a(kGvarIrWidth / 2), c(kGvarIrWidth / 2);
            for (int i = 0; i < kGvarIrWidth / 2; i++) { a[(size_t)i] = (uint8_t)(ir0[(size_t)i * 2] >> 2); c[(size_t)i] = (uint8_t)(ir2[(size_t)i * 2] >> 2); }
            setLineChan(1, counter, (b.id - 1) * 2, a.data());
            setLineChan(1, counter, (b.id - 1) * 2 + 1, c.data());
        } else if (b.id == 3 || b.id == 7) {
            visPx.resize(kGvarVisWidth);
            gvarVisPixels(f.data(), visPx.data());
            std::vector<uint8_t> a(kGvarIrWidth);
            for (int i = 0; i < kGvarIrWidth; i++) a[(size_t)i] = (uint8_t)(visPx[(size_t)i * 4] >> 2);
            setLineChan(0, counter * 2 + (b.id == 7 ? 1 : 0), 0, a.data());
        }
    }

    void handleSym(float I, float Q) {
        const float sc = 60.f;
        const int8_t si = sat8(I * sc), sq = sat8(Q * sc);
        const int mod = link.mod;
        if (lucky7) { lucky7Bit(si > 0); return; }
        if (spino) { spinoDef[0].push(si > 0); spinoDef[1].push(si <= 0); return; }
        if (gvar) { int b = si > 0; const int d = (b ^ nrzPrev) ^ 1; nrzPrev = b; def[0].push(d); return; }
        if (burstLink) { burst.push(si > 0); return; }
        if (mdl || simpleQpsk) { qpskSimple(si > 0, sq > 0); return; }
        if (link.conv) { convSym(si, sq); return; }
        if (mod == kSd3Qpsk || mod == kSd3Oqpsk) {       // QPSK with NRZ-M: four-phase differential, as SatDump's QPSKDiff
            const int cur = (si > 0 ? 2 : 0) | (sq > 0 ? 1 : 0);
            if (qdiffHave) {
                const int X1 = qdiffPrev >> 1, Y1 = qdiffPrev & 1, X = cur >> 1, Y = cur & 1;
                int xo, yo;
                if ((X ^ Y) == 1) { xo = Y1 ^ Y; yo = X1 ^ X; } else { xo = X1 ^ X; yo = Y1 ^ Y; }
                def[0].push(yo); def[0].push(xo);
            }
            qdiffPrev = cur; qdiffHave = 1;
            return;
        }
        // BPSK, PM, FSK
        int b = si > 0;
        if (link.nrzm) { const int d = b ^ nrzPrev; nrzPrev = b; b = d; }
        def[0].push(b);
    }

    void qpskSimple(int i, int q) {
        // up to complement the four candidates are (i, q), (!q, i), (q, i), (!i, q)
        if (defUse < 0) {
            def[0].push(i); def[0].push(q);
            def[1].push(!q); def[1].push(i);
            def[2].push(q); def[2].push(i);
            def[3].push(!i); def[3].push(q);
            for (int k = 0; k < 4; k++) if (def[k].locked()) { defUse = k; for (int j = 0; j < 4; j++) if (j != k) def[j].reset(); break; }
            return;
        }
        switch (defUse) {
        case 0: def[0].push(i); def[0].push(q); break;
        case 1: def[1].push(!q); def[1].push(i); break;
        case 2: def[2].push(q); def[2].push(i); break;
        default: def[3].push(!i); def[3].push(q); break;
        }
    }

    void convSym(int8_t si, int8_t sq) {
        const bool two = link.mod == kSd3Qpsk || link.mod == kSd3Oqpsk;
        if (searchLock) { convLocked(two, si, sq); return; }
        soft.push_back(si);
        if (two) soft.push_back(sq);
        softSince += two ? 2 : 1;
        const int frameSoft = link.frameBits * 2;
        const int need = 2 * frameSoft + 128;
        if ((int)soft.size() > 3 * need) soft.erase(soft.begin(), soft.begin() + (std::ptrdiff_t)(soft.size() - (size_t)need * 2));
        if (softSince >= frameSoft / 2 && (int)soft.size() >= frameSoft + 128) { softSince = 0; convSearch(); }
    }
    int8_t convCarry = 0;
    bool convHalf = false;
    void convLocked(bool two, int8_t si, int8_t sq) {
        if (two) { feedPair(two, si, sq); return; }
        if (!convHalf) { convHalf = true; convCarry = si; return; }
        convHalf = false;
        int oa, ob;
        xform(xt, convCarry, si, oa, ob);
        vitPair(oa, ob);
    }
    void feedPair(bool, int8_t si, int8_t sq) {
        int oa, ob;
        if (!slip) { xform(xt, si, sq, oa, ob); vitPair(oa, ob); return; }
        // half a pair late: (previous Q, I), then the current Q waits
        xform(xt, slipCarry, si, oa, ob);
        slipCarry = sq;
        vitPair(oa, ob);
    }
    int8_t slipCarry = 0;
    void vitPair(int a, int b) {
        vit.push(a, b);
        auto& o = vit.out();
        for (uint8_t bb : o) {
            int d = bb;
            if (link.nrzm) { const int nd = d ^ nrzPrev; nrzPrev = d; d = nd; }
            def[0].push(d);
        }
        o.clear();
    }
    void convSearch() {
        const int nsoft = (int)soft.size();
        for (int s = 0; s < 2; s++) {
            const int np = (nsoft - s) / 2;
            for (int t = 0; t < 8; t++) {
                uint64_t w = 0;
                std::vector<int> hits;
                for (int i = 0; i < np; i++) {
                    int oa, ob;
                    xform(t, soft[(size_t)(2 * i + s)], soft[(size_t)(2 * i + s + 1)], oa, ob);
                    w = w << 2 | (uint64_t)(oa > 0) << 1 | (uint64_t)(ob > 0);
                    if (i >= 31 && __builtin_popcountll((w ^ codedPat) & kCodedMask) <= 7) hits.push_back(i);
                }
                int at = -1;
                for (size_t x = 0; x < hits.size() && at < 0; x++)
                    for (size_t y = x + 1; y < hits.size(); y++) if (hits[y] - hits[x] == link.frameBits) { at = hits[y]; break; }
                if (at < 0) continue;
                searchLock = true; xt = t; slip = s; vit.reset(); def[0].reset(); convHalf = false; slipCarry = 0; nrzPrev = 0;
                const int from = std::max(0, at - 31 - link.frameBits - 40);
                for (int i = from; i < np; i++) {
                    int oa, ob;
                    xform(xt, soft[(size_t)(2 * i + s)], soft[(size_t)(2 * i + s + 1)], oa, ob);
                    vitPair(oa, ob);
                }
                // what follows arrives as new symbols: keep the half pair the replay left
                const bool two = link.mod == kSd3Qpsk || link.mod == kSd3Oqpsk;
                if ((nsoft - s) % 2 == 1) { if (two) slipCarry = soft.back(); else { convHalf = true; convCarry = soft.back(); } }
                soft.clear();
                say("coded sync found, ambiguity " + std::to_string(t) + (s ? " (half slip)" : ""));
                return;
            }
        }
    }

    void lucky7Bit(int bit) {
        luckyBits.push_back((uint8_t)bit);
        if (luckyBits.size() > kLucky7Bits) luckyBits.erase(luckyBits.begin());
        if ((int)luckyBits.size() < kLucky7Bits) return;
        static const int kSync[16] = {0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0};
        int e0 = 0, e1 = 0;
        for (int i = 0; i < 16; i++) { if (luckyBits[(size_t)i] != kSync[i]) e0++; if (luckyBits[(size_t)i] == kSync[i]) e1++; }
        if (e0 > 2 && e1 > 2) return;
        uint8_t by[39] = {};
        const int inv = e1 <= 2 ? 1 : 0;
        for (int i = 0; i < kLucky7Bits; i++) by[i >> 3] = (uint8_t)(by[i >> 3] << 1 | (luckyBits[(size_t)i] ^ inv));
        if (lucky7Frame(by)) {
            tel.blocksOk++;
            markGood();
            tel.lastText = lucky7Kind(by + 2);
            logFrame(by + 2, 35, true);
            luckyBits.clear();
        } else tel.blocksBad++;
    }

    void process(const cf32* x, size_t n) {
        if (rate <= 0) return;
        if (s2) { if (dvb) dvb->feed(x, n); }
        else dem.feed(x, n, [this](float I, float Q) { handleSym(I, Q); });
        nIn += (int64_t)n;
        for (size_t i = 0; i < n; i += std::max<size_t>(1, n / 64)) levelAcc += x[i].real() * x[i].real() + x[i].imag() * x[i].imag(), levelN++;
    }

    void publish(bool force) {
        if (!force && nIn < nextPub) return;
        nextPub = nIn + (int64_t)(rate * kPubSec);
        const double now = nowSec();
        tel.seq++;
        tel.inputRate = rate;
        tel.timeSec = (double)nIn / std::max(1.0, rate);
        tel.levelDb = levelN ? (float)(10 * std::log10(std::max(1e-12, (double)levelAcc / (double)levelN))) : -200;
        levelAcc = 0; levelN = 0;
        tel.frameLock = tel.blocksOk > 0 && now - lastGood < 3;
        if (!s2) {
            tel.cfoHz = dem.currentCfo();
            tel.snrDb = dem.snrDb;
            tel.carrierLock = dem.carrierLock;
            tel.constellation = dem.cons;
            tel.state = (now - lastGood < 3) ? 3 : (tel.asmHits > 0 ? 2 : ((dem.acquired && dem.carrierLock > 0.15f) ? 1 : 0));
            const bool lost = searchLock && now - lastGood > 6;
            if (lost) { searchLock = false; soft.clear(); def[0].reset(); }
            if (!searchLock && tel.state < 2 && now - lastAcq > 3 && now - statStart > 3) { dem.reacquire(); lastAcq = now; }
        } else {
            int ds = 0;
            if (dvb) {
                DvbsTelemetry d;
                if (dvb->telemetry(d, dvbSeq)) { dvbSeq = d.seq; tel.cfoHz = d.cfoHz; tel.snrDb = (float)d.snrDb; dvbState = d.state; tel.carrierLock = d.state >= 2 ? 1.f : 0.f; if (d.modcod >= 0) { char m[48]; snprintf(m, sizeof m, "MODCOD %d", d.modcod); tel.hint = m; } }
                ds = dvbState;
            }
            tel.state = (now - lastGood < 3) ? 3 : (ds >= 1 ? 1 : 0);
            if (grb) { tel.packets = grb->packets; tel.crcBad = grb->crcBad; if (grb->j2kBad) { char e[160]; snprintf(e, sizeof e, "%llu JPEG 2000 blocks failed: %s", (unsigned long long)grb->j2kBad, grb->lastError.c_str()); tel.hint = e; } }
            if (uhrit) { tel.packets = uhrit->packets; tel.crcBad = uhrit->crcBad; tel.blocksBad = uhrit->frameCrcBad; }
        }
        std::lock_guard<std::mutex> lk(mu);
        // copy-on-write of the pictures for the interface thread (lines are shared)
        if (P == kSd3Grb) {
            int key = productReq.load();
            if (key < 0 || !grbImg.count(key)) { key = -1; uint64_t best = 0; for (auto& kv : grbTouched) if (kv.second > best) { best = kv.second; key = kv.first; } }
            if (key >= 0) {
                auto copy = std::make_shared<WxImage>(*grbImg[key]);
                tel.image = copy;
                tel.selectedProduct = key;
                if (imgDirty[0] || key != shownKey) tel.imageSeq++;
                shownKey = key;
            }
            imgDirty[0] = false;
            tel.products.clear();
            for (auto& kv : grbName) tel.products.push_back({kv.first, kv.second, grbTouched.count(kv.first) ? grbTouched[kv.first] : 0});
        } else {
            for (int w = 0; w < 2; w++) if (im[w] && imgDirty[w]) { (w ? tel.image2 : tel.image) = std::make_shared<WxImage>(*im[w]); imgDirty[w] = false; tel.imageSeq++; }
        }
        tel.apids.clear();
        for (auto& kv : apidCount) tel.apids.emplace_back(kv.first, kv.second);
        std::sort(tel.apids.begin(), tel.apids.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (tel.apids.size() > 32) tel.apids.resize(32);
        tel.dataValid = true;
        pub = tel;
    }
};

Sd3Receiver::Sd3Receiver(int profile) : p_(std::make_unique<Impl>(profile)) {}
Sd3Receiver::~Sd3Receiver() = default;
int Sd3Receiver::profile() const { return p_->P; }
void Sd3Receiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }

void Sd3Receiver::configure(double r) {
    p_->rate = r;
    p_->tStart = nowSec();
    p_->initImages();
    p_->setup();
}

void Sd3Receiver::setSignalOffset(double hz) {
    p_->offset = hz;
    if (p_->dvb && hz != 0) p_->dvb->selectCarrier(true, hz);
    if (p_->rate > 0 && !p_->s2) p_->dem.setOffset(hz);
}

bool Sd3Receiver::ready() const { return p_->rate > 0; }
void Sd3Receiver::reset() { p_->resetReq.store(true); }
void Sd3Receiver::clearImage() { p_->clearReq.store(true); }

void Sd3Receiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (I.rate <= 0) return;
    if (I.linkReq.load() != I.linkIdx) I.setup();
    if (I.resetReq.exchange(false)) {
        I.dvb.reset();
        I.setup();
        I.tel.asmHits = 0; I.tel.blocksOk = I.tel.blocksBad = 0; I.tel.packets = 0; I.tel.files = 0; I.tel.rsCorrected = 0;
        I.tel.frames.clear();
    }
    if (I.clearReq.exchange(false)) I.initImages();
    I.process(x, n);
    I.publish(false);
}

bool Sd3Receiver::telemetry(Sd3Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void Sd3Receiver::clearResults() { p_->clearReq.store(true); }
void Sd3Receiver::setLink(int index) { p_->linkReq.store(index); }
int Sd3Receiver::link() const { return p_->linkReq.load(); }
void Sd3Receiver::setSaveDir(const std::string& dir) { std::lock_guard<std::mutex> lk(p_->dirMu); p_->saveDir = dir; }
void Sd3Receiver::setProduct(int key) { p_->productReq.store(key); }

void Sd3Receiver::setBlocking(bool b) { p_->blocking.store(b); if (p_->dvb) p_->dvb->setBlocking(b); }

void Sd3Receiver::flush() {
    if (p_->dvb) p_->dvb->flush();
    p_->publish(true);
}

ModeTuning sd3Tuning(int p) {
    const Sd3ProfileInfo& i = sd3Profile(p);
    const Sd3Link& l = sd3LinkAt(p, 0);
    ModeTuning t;
    t.stdMode = i.family + 2; t.id = i.id; t.name = i.name;
    t.minMhz = 1; t.maxMhz = 12000; t.defMhz = l.mhz > 0 ? l.mhz : (p == kSd3Sband ? 2250 : 1690);
    t.sampleRate = i.sampleRate; t.basebandHz = 0;
    t.bandwidthMhz = (l.sym / 1e6) * 1.5 + 0.02;
    t.minSampleRate = i.minRate;
    t.tuneOffsetHz = (p >= kSd3Geoscan) ? 50000 : 0;
    t.carrierAtCentre = l.mod == kSd3Pm;
    return t;
}

int sd3LinkForFrequency(int profile, double mhz) {
    int best = -1;
    double bd = 1.0;
    for (int i = 0; i < sd3LinkCount(profile); i++) {
        const double m = sd3LinkAt(profile, i).mhz;
        if (m <= 0) continue;
        const double d = std::fabs(m - mhz);
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}

} // namespace dect2
