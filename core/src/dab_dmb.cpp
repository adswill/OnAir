// DMB video services (see dab_dmb.h): the outer decoder of TS 102 427 and the MPEG-4 Systems remux of TS 102 428.
#include "dect2/dab_dmb.h"
#include "dect2/dvbt.h"
#include "dect2/ts.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

namespace {

struct BitReader {
    const uint8_t* d;
    size_t n, pos = 0;   // in bits
    BitReader(const uint8_t* p, size_t bytes) : d(p), n(bytes * 8) {}
    uint64_t u(int bits) {
        uint64_t v = 0;
        for (int i = 0; i < bits; i++, pos++) v = (v << 1) | (pos < n ? (uint64_t)((d[pos >> 3] >> (7 - (pos & 7))) & 1) : 0);
        return v;
    }
    bool over() const { return pos > n; }
    size_t bytes() const { return (pos + 7) / 8; }
};

uint32_t rb16(const uint8_t* p) { return (uint32_t)(p[0] << 8 | p[1]); }
uint32_t rb32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// an MPEG-4 descriptor: tag and expandable size (ISO/IEC 14496-1 clause 8.3.3)
bool readDescr(const uint8_t*& p, const uint8_t* end, int& tag, size_t& len) {
    if (p >= end) return false;
    tag = *p++;
    len = 0;
    for (int i = 0; i < 4; i++) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len <= (size_t)(end - p);
}

// SLConfigDescriptor (14496-1 clause 10.2.3)
struct SlConfig {
    bool none = false;                       // predefined: no SL packet header (every SL packet is one access unit)
    bool useStart = false, useEnd = false, rap = false, padding = false, timestamps = false, idle = false;
    uint32_t tsRes = 90000;
    int tsLen = 0, ocrLen = 0, auLen = 0, ibLen = 0, degLen = 0, auSeqLen = 0, pktSeqLen = 0;
};
SlConfig parseSlConfig(const uint8_t* p, size_t n) {
    SlConfig c;
    if (n < 16 || p[0] != 0) { c.none = true; return c; }   // predefined 1 (null header) and 2 (MP4 files) carry nothing a stream needs here
    const int f = p[1];
    c.useStart = f & 0x80; c.useEnd = f & 0x40; c.rap = f & 0x20; c.padding = f & 0x08; c.timestamps = f & 0x04; c.idle = f & 0x02;
    c.tsRes = rb32(p + 2);
    c.tsLen = std::min<int>(p[10], 64); c.ocrLen = std::min<int>(p[11], 64); c.auLen = std::min<int>(p[12], 32); c.ibLen = std::min<int>(p[13], 32);
    const uint32_t l = rb16(p + 14);
    c.degLen = (int)(l >> 12); c.auSeqLen = (int)((l >> 7) & 31); c.pktSeqLen = (int)((l >> 2) & 31);
    return c;
}

// SL packet header (14496-1 clause 10.2.4): start/end of an access unit, time stamps, its length in bytes
struct SlHeader { bool start = true, end = true, idle = false; int64_t dts = -1, cts = -1; size_t len = 0; bool ok = true; };
SlHeader parseSl(const SlConfig& c, const uint8_t* p, size_t n, bool prevEnded) {
    SlHeader h;
    if (c.none) return h;
    BitReader b(p, n);
    h.start = c.useStart ? b.u(1) != 0 : (c.useEnd ? prevEnded : true);   // inferred as 14496-1 says when the flag is not used
    h.end = c.useEnd ? b.u(1) != 0 : !c.useStart;                          // without an end flag the next start ends the access unit
    const bool ocr = c.ocrLen > 0 && b.u(1);
    h.idle = c.idle && b.u(1);
    const bool pad = c.padding && b.u(1);
    const int padBits = pad ? (int)b.u(3) : 0;
    if (!h.idle && (!pad || padBits != 0)) {
        if (c.pktSeqLen) b.u(c.pktSeqLen);
        if (c.degLen && b.u(1)) b.u(c.degLen);
        if (ocr) b.u(c.ocrLen);
        if (h.start) {
            if (c.rap) b.u(1);
            if (c.auSeqLen) b.u(c.auSeqLen);
            bool dtsF = false, ctsF = false;
            if (c.timestamps) { dtsF = b.u(1) != 0; ctsF = b.u(1) != 0; }
            const bool ibF = c.ibLen > 0 && b.u(1);
            auto ts = [&](uint64_t v) {   // to 90 kHz
                if (c.tsRes == 90000 || !c.tsRes) return (int64_t)v;
                return (int64_t)((v / c.tsRes) * 90000 + (v % c.tsRes) * 90000 / c.tsRes);
            };
            if (dtsF) h.dts = ts(b.u(c.tsLen));
            if (ctsF) h.cts = ts(b.u(c.tsLen));
            if (c.auLen) b.u(c.auLen);
            if (ibF) b.u(c.ibLen);
        }
    } else if (pad && padBits == 0) h.idle = true;   // a packet of padding only
    h.len = b.bytes();
    h.ok = !b.over();
    return h;
}

// ES_Descriptor (14496-1 clause 7.2.6.5) with its decoder and SL configurations
struct EsInfo { int esId = 0, oti = 0, streamType = 0; std::vector<uint8_t> dsi; SlConfig sl; };
bool parseEsDescr(const uint8_t* p, size_t n, EsInfo& e) {
    if (n < 3) return false;
    const uint8_t* end = p + n;
    e.esId = (int)rb16(p);
    const int f = p[2];
    p += 3;
    if (f & 0x80) p += 2;                    // dependsOn_ES_ID
    if (f & 0x40) { if (p >= end) return false; p += 1 + *p; }   // URL
    if (f & 0x20) p += 2;                    // OCR_ES_Id
    if (p > end) return false;
    int tag;
    size_t len;
    while (readDescr(p, end, tag, len)) {
        if (tag == 0x04 && len >= 13) {      // DecoderConfigDescriptor
            e.oti = p[0];
            e.streamType = p[1] >> 2;
            const uint8_t* q = p + 13;
            int t2;
            size_t l2;
            while (readDescr(q, p + len, t2, l2)) { if (t2 == 0x05) e.dsi.assign(q, q + l2); q += l2; }
        } else if (tag == 0x06) e.sl = parseSlConfig(p, len);
        p += len;
    }
    return true;
}

// AudioSpecificConfig (14496-3 clause 1.6.2.1): enough for an ADTS header and the service info
struct Asc { int aot = 0, coreAot = 0, sfi = 15, rate = 0, channels = 0; bool sbr = false, ps = false; };
Asc parseAsc(const std::vector<uint8_t>& d) {
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    Asc a;
    if (d.size() < 2) return a;
    BitReader b(d.data(), d.size());
    auto aot = [&] { int t = (int)b.u(5); return t == 31 ? 32 + (int)b.u(6) : t; };
    auto rate = [&](int& sfi) { sfi = (int)b.u(4); return sfi == 15 ? (int)b.u(24) : sfi < 13 ? rates[sfi] : 0; };
    a.aot = a.coreAot = aot();
    a.rate = rate(a.sfi);
    a.channels = (int)b.u(4);
    if (a.aot == 5 || a.aot == 29) {         // explicit SBR (and PS): the core follows
        a.sbr = true; a.ps = a.aot == 29;
        int esfi;
        const int ext = rate(esfi);
        a.coreAot = aot();
        a.rate = ext ? ext : 2 * a.rate;
    }
    return a;
}

void putPts(std::vector<uint8_t>& v, int prefix, int64_t t) {
    const uint64_t p = (uint64_t)t & ((1ull << 33) - 1);
    v.push_back((uint8_t)((prefix << 4) | ((p >> 29) & 0x0E) | 1));
    v.push_back((uint8_t)(p >> 22)); v.push_back((uint8_t)(((p >> 14) & 0xFE) | 1));
    v.push_back((uint8_t)(p >> 7)); v.push_back((uint8_t)(((p << 1) & 0xFE) | 1));
}

} // namespace

// ================================================================ the remux
struct DmbRemux::Impl {
    std::function<void(const uint8_t*)> sink;
    std::function<void(const std::vector<uint8_t>&, const uint8_t*, int, int64_t)> bsac;
    // input: PSI and the MPEG-4 Systems tables
    struct SecBuf { std::vector<uint8_t> d; bool on = false; };
    std::map<int, SecBuf> sec;
    int pmtPid = -1, pcrPid = -1;
    std::map<int, int> pidType, pidEs;        // stream type and ES_ID of every elementary PID of the PMT
    std::map<int, EsInfo> es;                 // by ES_ID: from the IOD (OD and scene streams) and the object descriptors (audio, video)
    // input: the SL-packetized elementary streams
    struct Pes { std::vector<uint8_t> buf; bool on = false; int cc = -1; std::vector<uint8_t> au; bool auOn = false, prevEnded = true; int64_t cts = -1, dts = -1; };
    std::map<int, Pes> pes;
    // output
    int videoPid = -1, audioPid = -1;         // input PIDs that are played
    EsInfo vEs, aEs;
    Asc asc;
    bool audioOut = false;                    // AAC in ADTS (else none, or BSAC to the hook)
    int64_t lastAudioPts = -1;
    std::vector<uint8_t> videoParams;         // SPS and PPS of a decoder configuration (Annex B), for streams that do not repeat them
    int lengthSize = 0;                       // > 0: the video comes as NAL units with a length prefix of this many bytes (from an avcC record)
    int cc[8192] = {};
    int pmtVersion = 0;
    std::string pmtKey;
    uint64_t out = 0, bsacAus = 0;
    std::string video, audio, note;
    mutable std::mutex mu;                    // the strings for stats()

    void reset() {
        sec.clear(); pmtPid = pcrPid = -1; pidType.clear(); pidEs.clear(); es.clear(); pes.clear();
        videoPid = audioPid = -1; vEs = aEs = EsInfo(); asc = Asc(); audioOut = false; lastAudioPts = -1; videoParams.clear(); lengthSize = 0;
        std::fill(cc, cc + 8192, 0); pmtKey.clear(); out = 0; bsacAus = 0;
        std::lock_guard<std::mutex> lk(mu);
        video.clear(); audio.clear(); note.clear();
    }

    // ---------------------------------------------------------------- output packets
    void emit(int pid, const uint8_t* d, size_t n, int64_t pcr = -1) {
        size_t off = 0;
        do {
            uint8_t o[188];
            const bool start = off == 0;
            const size_t af = pcr >= 0 && start ? 8 : 0;
            const size_t pay = std::min(n - off, 184 - af);
            const size_t afTotal = 184 - pay;
            o[0] = 0x47; o[1] = (uint8_t)((start ? 0x40 : 0) | (pid >> 8)); o[2] = (uint8_t)pid;
            if (n == 0) {   // a packet of only the PCR
                o[1] = (uint8_t)(pid >> 8); o[3] = (uint8_t)(0x20 | ((cc[pid] - 1) & 15));
            } else o[3] = (uint8_t)((afTotal ? 0x30 : 0x10) | (cc[pid]++ & 15));
            uint8_t* p = o + 4;
            if (afTotal) {
                p[0] = (uint8_t)(afTotal - 1);
                if (afTotal > 1) {
                    std::memset(p + 1, 0xFF, afTotal - 1);
                    p[1] = af ? 0x10 : 0x00;
                    if (af) {
                        const uint64_t base = (uint64_t)pcr >> 9, ext = (uint64_t)pcr & 0x1FF;   // pcr: base << 9 | extension
                        p[2] = (uint8_t)(base >> 25); p[3] = (uint8_t)(base >> 17); p[4] = (uint8_t)(base >> 9); p[5] = (uint8_t)(base >> 1);
                        p[6] = (uint8_t)(((base & 1) << 7) | 0x7E | (ext >> 8)); p[7] = (uint8_t)ext;
                    }
                }
                p += afTotal;
            }
            if (pay) std::memcpy(p, d + off, pay);
            off += pay;
            out++;
            if (sink) sink(o);
        } while (off < n);
    }
    void psi(int pid, int tableId, int ext, int version, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> s{0x00, (uint8_t)tableId, 0, 0, (uint8_t)(ext >> 8), (uint8_t)ext, (uint8_t)(0xC1 | ((version & 31) << 1)), 0, 0};
        s.insert(s.end(), body.begin(), body.end());
        const size_t len = s.size() - 4 + 4;
        s[2] = (uint8_t)(0xB0 | (len >> 8)); s[3] = (uint8_t)len;
        const uint32_t crc = mpegCrc32(s.data() + 1, (int)s.size() - 1);
        for (int i = 3; i >= 0; i--) s.push_back((uint8_t)(crc >> (8 * i)));
        emit(pid, s.data(), s.size());
    }
    void emitTables() {
        if (videoPid < 0 && !audioOut) return;
        std::vector<uint8_t> pat{0x00, (uint8_t)kProgram, (uint8_t)(0xE0 | (kPmtPid >> 8)), (uint8_t)kPmtPid};
        psi(0, 0x00, 1, 0, pat);
        std::vector<uint8_t> b{(uint8_t)(0xE0 | (kPcrPid >> 8)), (uint8_t)kPcrPid, 0xF0, 0x00};
        if (videoPid >= 0) { b.push_back(0x1B); b.push_back((uint8_t)(0xE0 | (kVideoPid >> 8))); b.push_back((uint8_t)kVideoPid); b.push_back(0xF0); b.push_back(0); }
        if (audioOut) { b.push_back(0x0F); b.push_back((uint8_t)(0xE0 | (kAudioPid >> 8))); b.push_back((uint8_t)kAudioPid); b.push_back(0xF0); b.push_back(0); }
        const std::string key(b.begin(), b.end());
        if (key != pmtKey) { pmtKey = key; pmtVersion++; }
        psi(kPmtPid, 0x02, kProgram, pmtVersion, b);
    }
    void emitPes(int pid, int streamId, const std::vector<uint8_t>& es, int64_t pts, int64_t dts) {
        std::vector<uint8_t> p{0x00, 0x00, 0x01, (uint8_t)streamId, 0, 0, 0x84, 0, 0};
        if (pts >= 0) {
            const bool d = dts >= 0 && dts != pts;
            p[7] = d ? 0xC0 : 0x80; p[8] = d ? 10 : 5;
            putPts(p, d ? 3 : 2, pts);
            if (d) putPts(p, 1, dts);
        }
        p.insert(p.end(), es.begin(), es.end());
        const size_t len = p.size() - 6;
        if (len <= 0xFFFF) { p[4] = (uint8_t)(len >> 8); p[5] = (uint8_t)len; }   // 0: unbounded (video only)
        emit(pid, p.data(), p.size());
    }

    // ---------------------------------------------------------------- access units
    void videoAu(std::vector<uint8_t>& au, int64_t cts, int64_t dts) {
        std::vector<uint8_t> o;
        auto startCode = [](const std::vector<uint8_t>& v) { return v.size() >= 4 && v[0] == 0 && v[1] == 0 && (v[2] == 1 || (v[2] == 0 && v[3] == 1)); };
        if (startCode(au) || vEs.oti != 0x21) o.swap(au);
        else {   // NAL units with a length prefix (an avcC configuration): to Annex B
            const int ls = lengthSize ? lengthSize : 4;
            size_t i = 0;
            while (i + (size_t)ls <= au.size()) {
                size_t l = 0;
                for (int k = 0; k < ls; k++) l = (l << 8) | au[i + (size_t)k];
                i += (size_t)ls;
                if (l > au.size() - i) break;
                o.insert(o.end(), {0, 0, 0, 1});
                o.insert(o.end(), au.begin() + (long)i, au.begin() + (long)(i + l));
                i += l;
            }
        }
        if (vEs.oti == 0x21 && !videoParams.empty()) {   // an IDR picture without parameter sets in front: the ones of the decoder configuration
            bool idr = false, sps = false;
            for (size_t i = 0; i + 3 < o.size(); i++)
                if (o[i] == 0 && o[i + 1] == 0 && o[i + 2] == 1) { const int t = o[i + 3] & 31; idr |= t == 5; sps |= t == 7; }
            if (idr && !sps) o.insert(o.begin(), videoParams.begin(), videoParams.end());
        }
        emitPes(kVideoPid, 0xE0, o, cts >= 0 ? cts : dts, dts);
    }
    void audioAu(const std::vector<uint8_t>& au, int64_t cts) {
        const int64_t dur = asc.rate ? 1024LL * 90000 / (asc.sbr ? asc.rate / 2 : asc.rate) : 1920;
        if (cts < 0 && lastAudioPts >= 0) cts = lastAudioPts + dur;   // SL time stamps may be sent only now and then (at least every 700 ms)
        if (cts >= 0) lastAudioPts = cts;
        if (asc.coreAot == 22) {   // ER BSAC (Korea): not supported; the one place a BSAC decoder would take the access units
            bsacAus++;
            if (bsac) bsac(aEs.dsi, au.data(), (int)au.size(), cts);
            return;
        }
        if (!audioOut) return;
        std::vector<uint8_t> o(7);   // ADTS header (MPEG-4, no CRC): the core object type, its sampling rate and the channels
        const size_t len = au.size() + 7;
        const int profile = asc.coreAot - 1;
        o[0] = 0xFF; o[1] = 0xF1;
        o[2] = (uint8_t)((profile << 6) | (asc.sfi << 2) | ((asc.channels >> 2) & 1));
        o[3] = (uint8_t)(((asc.channels & 3) << 6) | ((len >> 11) & 3));
        o[4] = (uint8_t)(len >> 3); o[5] = (uint8_t)(((len & 7) << 5) | 0x1F); o[6] = 0xFC;
        o.insert(o.end(), au.begin(), au.end());
        emitPes(kAudioPid, 0xC0, o, cts, -1);
    }

    // ---------------------------------------------------------------- the stream description
    void chooseStreams() {
        // the first video and the first audio elementary stream of the PMT that the object descriptors describe (TS 102 428 clause 5.3)
        int v = -1, a = -1;
        for (const auto& kv : pidEs) {
            if (pidType[kv.first] != 0x12) continue;
            auto it = es.find(kv.second);
            if (it == es.end()) continue;
            if (it->second.streamType == 0x04 && v < 0) v = kv.first;
            if (it->second.streamType == 0x05 && a < 0) a = kv.first;
        }
        if (v != videoPid) { videoPid = v; vEs = v >= 0 ? es[pidEs[v]] : EsInfo(); setVideoConfig(); }
        if (a != audioPid) {
            audioPid = a;
            aEs = a >= 0 ? es[pidEs[a]] : EsInfo();
            asc = aEs.oti == 0x40 ? parseAsc(aEs.dsi) : Asc();
            audioOut = aEs.oti == 0x40 && asc.coreAot >= 1 && asc.coreAot <= 4 && asc.sfi < 13;   // ADTS can say AAC main, LC, SSR and LTP
            lastAudioPts = -1;
        }
        std::lock_guard<std::mutex> lk(mu);
        video = videoPid < 0 ? "" : vEs.oti == 0x21 ? "H.264" : vEs.oti == 0x20 ? "MPEG-4 Visual" : vEs.oti == 0x6C ? "JPEG" : "video";
        if (audioPid < 0) audio.clear();
        else if (aEs.oti != 0x40) audio = "audio";
        else if (asc.coreAot == 22) audio = "BSAC";
        else audio = asc.ps ? "HE-AAC v2" : asc.sbr ? "HE-AAC" : asc.coreAot == 2 ? "AAC-LC" : "AAC";
        note = audioPid >= 0 && asc.coreAot == 22 ? "BSAC sound (Korea) is not supported" : audioPid >= 0 && !audioOut ? "this sound format is not supported" : "";
    }
    void setVideoConfig() {
        videoParams.clear();
        lengthSize = 0;
        const auto& d = vEs.dsi;
        if (d.size() >= 7 && d[0] == 1) {   // AVCDecoderConfigurationRecord (14496-15): length size, then the SPS and PPS
            lengthSize = (d[4] & 3) + 1;
            size_t i = 5;
            for (int set = 0; set < 2 && i < d.size(); set++) {
                const int cnt = set == 0 ? d[i] & 31 : d[i];
                i++;
                for (int k = 0; k < cnt && i + 2 <= d.size(); k++) {
                    const size_t l = rb16(&d[i]);
                    i += 2;
                    if (i + l > d.size()) break;
                    videoParams.insert(videoParams.end(), {0, 0, 0, 1});
                    videoParams.insert(videoParams.end(), d.begin() + (long)i, d.begin() + (long)(i + l));
                    i += l;
                }
            }
        } else if (d.size() >= 4 && d[0] == 0 && d[1] == 0) videoParams = d;   // already Annex B
    }

    void parseIod(const uint8_t* p, size_t n) {
        const uint8_t* end = p + n;
        int tag;
        size_t len;
        if (!readDescr(p, end, tag, len) || (tag != 0x02 && tag != 0x10) || len < 2) return;
        const uint8_t* q = p;
        const uint8_t* qe = p + len;
        const bool url = q[1] & 0x20;
        q += 2;
        if (url) return;
        q += 5;   // the profile levels
        while (q < qe && readDescr(q, qe, tag, len)) {
            if (tag == 0x03) { EsInfo e; if (parseEsDescr(q, len, e)) es[e.esId] = e; }
            q += len;
        }
    }
    // an ObjectDescriptor's body (without a URL): the object descriptor id, then its ES descriptors
    bool parseObjectDescr(const uint8_t* q, size_t n) {
        if (n < 2 || (q[1] & 0x20)) return false;
        const uint8_t* end = q + n;
        const uint8_t* r = q + 2;
        int tag;
        size_t len;
        bool any = false;
        while (r < end && readDescr(r, end, tag, len)) {
            if (tag == 0x03) { EsInfo e; if (parseEsDescr(r, len, e) && e.esId) { es[e.esId] = e; any = true; } }
            r += len;
        }
        return any;
    }
    // the commands of an object descriptor access unit (14496-1 clause 7.2.2): object descriptor updates with ES descriptors
    bool parseOd(const uint8_t* p, size_t n) {
        const uint8_t* end = p + n;
        int tag;
        size_t len;
        bool any = false;
        while (p < end && readDescr(p, end, tag, len)) {
            if (tag == 0x01) {   // ObjectDescriptorUpdate (or, in some streams, a bare ObjectDescriptor, which has the same tag)
                const uint8_t* q = p;
                const uint8_t* qe = p + len;
                int t2;
                size_t l2;
                bool found = false;
                while (q < qe && readDescr(q, qe, t2, l2)) {
                    if (t2 == 0x01 || t2 == 0x11) found |= parseObjectDescr(q, l2);
                    q += l2;
                }
                if (!found) found = parseObjectDescr(p, len);
                any |= found;
            } else if (tag < 0x02 || tag > 0x06) return any;   // not an OD command
            p += len;
        }
        return any;
    }

    void section(int pid, const uint8_t* s, size_t len) {
        if (len < 12 || !(s[1] & 0x80) || mpegCrc32(s, (int)len) != 0) return;
        const int tid = s[0];
        if (pid == 0 && tid == 0x00) {
            for (size_t i = 8; i + 4 <= len - 4; i += 4) {
                const int prog = (int)rb16(s + i), p = (int)(rb16(s + i + 2) & 0x1FFF);
                if (prog != 0) { if (p != pmtPid) { pmtPid = p; pidType.clear(); pidEs.clear(); } break; }
            }
            emitTables();   // the output tables go with the input's (every 500 ms or more often)
        } else if (pid == pmtPid && tid == 0x02) {
            pcrPid = (int)(rb16(s + 8) & 0x1FFF);
            const size_t pil = rb16(s + 10) & 0xFFF;
            const uint8_t* end = s + len - 4;
            const uint8_t* d = s + 12;
            if (d + pil > end) return;
            for (const uint8_t* q = d; q + 2 <= d + pil && q + 2 + q[1] <= d + pil; q += 2 + q[1])
                if (q[0] == 0x1D && q[1] > 2) parseIod(q + 4, q[1] - 2);   // IOD_descriptor: scope and label, then the InitialObjectDescriptor
            std::map<int, int> types, ids;
            for (const uint8_t* q = d + pil; q + 5 <= end;) {
                const int type = q[0], epid = (int)(rb16(q + 1) & 0x1FFF);
                const size_t il = rb16(q + 3) & 0xFFF;
                if (q + 5 + il > end) break;
                types[epid] = type;
                for (const uint8_t* r = q + 5; r + 2 <= q + 5 + il && r + 2 + r[1] <= q + 5 + il; r += 2 + r[1])
                    if (r[0] == 0x1E && r[1] >= 2) ids[epid] = (int)rb16(r + 2);   // SL_descriptor: the ES_ID
                q += 5 + il;
            }
            pidType = types;
            pidEs = ids;
            chooseStreams();
        } else if (tid == 0x05 && pidType.count(pid) && pidType[pid] == 0x13) {   // object descriptors in an ISO_IEC_14496_section
            const uint8_t* p = s + 8;
            const size_t n = len - 12;
            auto it = pidEs.find(pid);
            const SlConfig sl = it != pidEs.end() && es.count(it->second) ? es[it->second].sl : SlConfig();
            const SlHeader h = parseSl(sl, p, n, true);
            // one SL packet per section; streams that leave the SL header out of their sections are read as well
            if (!(h.ok && h.len <= n && parseOd(p + h.len, n - h.len))) parseOd(p, n);
            chooseStreams();
        }
    }

    void sectionData(int pid, const uint8_t* p, size_t n, bool pusi) {
        SecBuf& s = sec[pid];
        auto drain = [&] {
            while (s.on && s.d.size() >= 3) {
                if (s.d[0] == 0xFF) { s.on = false; s.d.clear(); break; }
                const size_t len = 3 + (rb16(&s.d[1]) & 0xFFF);
                if (s.d.size() < len) break;
                section(pid, s.d.data(), len);
                s.d.erase(s.d.begin(), s.d.begin() + (long)len);
            }
        };
        if (pusi) {
            if (n < 1) return;
            const size_t ptr = p[0];
            if (1 + ptr > n) { s.on = false; s.d.clear(); return; }
            if (s.on) { s.d.insert(s.d.end(), p + 1, p + 1 + ptr); drain(); }
            s.d.clear();
            s.on = true;
            p += 1 + ptr; n -= 1 + ptr;
        } else if (!s.on) return;
        s.d.insert(s.d.end(), p, p + n);
        drain();
    }

    // one complete PES packet of an SL-packetized stream
    void pesPacket(int pid, Pes& e) {
        const std::vector<uint8_t>& b = e.buf;
        if (b.size() < 9 || b[0] || b[1] || b[2] != 1) return;
        const size_t hl = b[8];
        if (9 + hl > b.size()) return;
        const uint8_t* p = b.data() + 9 + hl;
        size_t n = b.size() - 9 - hl;
        const size_t plen = rb16(&b[4]);
        if (plen && 6 + plen < b.size()) n = 6 + plen - 9 - hl;
        auto ei = pidEs.find(pid);
        if (ei == pidEs.end() || !es.count(ei->second)) return;
        const EsInfo& info = es[ei->second];
        const SlHeader h = parseSl(info.sl, p, n, e.prevEnded);
        if (!h.ok || h.len > n || h.idle) return;
        p += h.len; n -= h.len;
        if (h.start) {
            if (e.auOn && !e.au.empty()) finishAu(pid, e);   // the start of the next access unit ends this one
            e.au.assign(p, p + n);
            e.auOn = true;
            e.cts = h.cts; e.dts = h.dts;
        } else if (e.auOn) e.au.insert(e.au.end(), p, p + n);
        e.prevEnded = h.end;
        if (h.end && e.auOn) finishAu(pid, e);
    }
    void finishAu(int pid, Pes& e) {
        if (pid == videoPid) videoAu(e.au, e.cts, e.dts);
        else if (pid == audioPid) audioAu(e.au, e.cts);
        e.au.clear();
        e.auOn = false;
    }

    void feed(const uint8_t* k) {
        if (k[0] != 0x47) return;
        const int pid = ((k[1] & 0x1F) << 8) | k[2];
        const bool tei = k[1] & 0x80, pusi = k[1] & 0x40;
        const int afc = (k[3] >> 4) & 3;
        size_t off = 4;
        if (afc & 2) {
            off += 1 + (size_t)k[4];
            // the clock: a PCR of the input's PCR PID goes out on the output's
            if (pid == pcrPid && !tei && k[4] >= 7 && (k[5] & 0x10)) {
                const uint64_t base = (uint64_t)k[6] << 25 | (uint64_t)k[7] << 17 | (uint64_t)k[8] << 9 | (uint64_t)k[9] << 1 | (k[10] >> 7);
                const uint64_t ext = (uint64_t)(k[10] & 1) << 8 | k[11];
                if (videoPid >= 0 || audioOut) emit(kPcrPid, nullptr, 0, (int64_t)(base << 9 | ext));
            }
        }
        if (!(afc & 1) || off >= 188) return;
        const uint8_t* p = k + off;
        const size_t n = 188 - off;
        if (pid == 0 || pid == pmtPid || (pidType.count(pid) && pidType[pid] == 0x13)) { if (!tei) sectionData(pid, p, n, pusi); return; }
        if (pid != videoPid && pid != audioPid) return;
        Pes& e = pes[pid];
        const int c = k[3] & 15;
        const bool lost = e.cc >= 0 && c != ((e.cc + 1) & 15) && c != e.cc;
        e.cc = c;
        if (tei || lost) { e.on = false; e.buf.clear(); e.auOn = false; e.au.clear(); if (tei) return; }   // a damaged packet: drop what it belongs to
        if (pusi) {
            if (e.on) pesPacket(pid, e);
            e.buf.assign(p, p + n);
            e.on = true;
        } else if (e.on) e.buf.insert(e.buf.end(), p, p + n);
        if (e.on && e.buf.size() >= 6) {
            const size_t plen = rb16(&e.buf[4]);
            if (plen && e.buf.size() >= 6 + plen) { pesPacket(pid, e); e.on = false; e.buf.clear(); }
        }
    }
};

DmbRemux::DmbRemux() : p_(new Impl) {}
DmbRemux::~DmbRemux() = default;
void DmbRemux::reset() { p_->reset(); }
void DmbRemux::feed(const uint8_t* pkt) { p_->feed(pkt); }
void DmbRemux::setSink(std::function<void(const uint8_t*)> cb) { p_->sink = std::move(cb); }
void DmbRemux::setBsacSink(std::function<void(const std::vector<uint8_t>&, const uint8_t*, int, int64_t)> cb) { p_->bsac = std::move(cb); }
void DmbRemux::stats(DmbStats& s) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    s.video = p_->video; s.audio = p_->audio; s.note = p_->note;
    s.sampleRate = p_->asc.rate; s.channels = p_->asc.channels;
    s.tsOut = p_->out;
}

// ================================================================ the outer decoder
struct DmbDecoder::Impl {
    std::vector<uint8_t> in, outBuf, batch;
    bool locked = false;
    dvbt::ConvInterleaver deint{true};
    int warm = 0, badSync = 0;
    DmbRemux remux;
    std::function<void(const uint8_t*, size_t)> sink;
    std::function<void(const uint8_t*)> raw;
    mutable std::mutex mu;
    DmbStats st;

    Impl() { remux.setSink([this](const uint8_t* k) { batch.insert(batch.end(), k, k + 188); }); }

    void reset() {
        in.clear(); outBuf.clear(); batch.clear(); locked = false; deint = dvbt::ConvInterleaver(true); warm = badSync = 0;
        remux.reset();
        std::lock_guard<std::mutex> lk(mu);
        st = DmbStats();
    }

    // the packet sync: 0x47 every 204 bytes (the sync bytes take branch 0 of the interleaver, without delay)
    bool findSync() {
        const size_t need = 204 * 8;
        if (in.size() < need) return false;
        int best = -1, bestScore = 0;
        for (int p = 0; p < 204; p++) {
            int score = 0;
            for (size_t k = 0; (size_t)p + 204 * k < in.size() && k < 16; k++) score += in[(size_t)p + 204 * k] == 0x47;
            if (score > bestScore) { bestScore = score; best = p; }
        }
        const int blocks = (int)std::min<size_t>(16, (in.size() - (size_t)std::max(0, best)) / 204);
        if (best < 0 || bestScore < 6 || bestScore * 4 < blocks * 3) {
            in.erase(in.begin(), in.end() - (long)(204 * 4));   // nothing yet: keep looking in the newer bytes
            return false;
        }
        in.erase(in.begin(), in.begin() + best);
        deint = dvbt::ConvInterleaver(true);
        outBuf.clear();
        warm = 11;   // the de-interleaver's first 11 packets are its empty memory
        badSync = 0;
        locked = true;
        return true;
    }

    void push(const uint8_t* f, int n) {
        in.insert(in.end(), f, f + n);
        if (!locked && !findSync()) return;
        const size_t nb = in.size() / 12 * 12;
        if (nb) {
            std::vector<uint8_t> d(nb);
            deint.process(in.data(), d.data(), nb);
            in.erase(in.begin(), in.begin() + (long)nb);
            outBuf.insert(outBuf.end(), d.begin(), d.end());
        }
        size_t pos = 0;
        uint64_t ok = 0, bad = 0, fixed = 0;
        while (outBuf.size() - pos >= 204) {
            uint8_t* b = &outBuf[pos];
            pos += 204;
            if (warm > 0) { warm--; continue; }
            const int r = dvbt::rsDecode(b);
            if (r < 0) bad++; else { ok++; fixed += (uint64_t)r; }
            if (b[0] != 0x47) {
                if (++badSync >= 8) { locked = false; in.clear(); pos = outBuf.size(); break; }   // lost the packet sync: search again
                continue;
            }
            badSync = 0;
            if (r < 0) b[1] |= 0x80;   // transport_error_indicator: the remux drops what the packet belongs to
            if (raw) raw(b);
            remux.feed(b);
        }
        outBuf.erase(outBuf.begin(), outBuf.begin() + (long)std::min(pos, outBuf.size()));
        {
            std::lock_guard<std::mutex> lk(mu);
            st.sync = locked;
            st.rsOk += ok; st.rsFailed += bad; st.rsCorrected += fixed;
        }
        if (!batch.empty()) {
            if (sink) sink(batch.data(), batch.size() / 188);
            batch.clear();
        }
    }
};

DmbDecoder::DmbDecoder() : p_(new Impl) {}
DmbDecoder::~DmbDecoder() = default;
void DmbDecoder::reset() { p_->reset(); }
void DmbDecoder::push(const uint8_t* frame, int n) { p_->push(frame, n); }
void DmbDecoder::setPacketSink(std::function<void(const uint8_t*, size_t)> cb) { p_->sink = std::move(cb); }
void DmbDecoder::setRawTap(std::function<void(const uint8_t*)> cb) { p_->raw = std::move(cb); }
DmbRemux& DmbDecoder::remux() { return p_->remux; }
DmbStats DmbDecoder::stats() const {
    DmbStats s;
    { std::lock_guard<std::mutex> lk(p_->mu); s = p_->st; }
    p_->remux.stats(s);
    return s;
}

} // namespace dect2
