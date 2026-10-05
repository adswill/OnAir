#include "dect2/atsc3_alp.h"
#include <algorithm>

namespace dect2 {
namespace atsc3 {

namespace {

void put16(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

// Base header: packet_type(3) PC(1) HM or S/C (1) length(11)
void baseHeader(std::vector<uint8_t>& v, int type, int pc, int hmOrSc, int length11) {
    put16(v, (type << 13) | (pc << 12) | (hmOrSc << 11) | (length11 & 0x7FF));
}

void signalingHeader(std::vector<uint8_t>& v, const AlpPacket& p) {
    v.push_back((uint8_t)p.signalingType);
    put16(v, p.signalingExt);
    v.push_back((uint8_t)p.signalingVersion);
    v.push_back((uint8_t)((p.signalingFormat << 6) | (p.signalingEncoding << 4) | 0x0F));
}

} // namespace

std::vector<uint8_t> alpSingle(const AlpPacket& p) {
    std::vector<uint8_t> v;
    const int len = (int)p.data.size();
    if (p.type == AlpTs) return alpTs(p.data);
    const bool extra = p.sid >= 0;
    const bool hm = len > 2047 || extra;
    baseHeader(v, p.type, 0, hm ? 1 : 0, len);
    if (hm) v.push_back((uint8_t)(((len >> 11) & 0x1F) << 3 | 0x04 | (extra ? 2 : 0)));   // length_MSB, reserved 1, SIF, HEF = 0
    if (p.type == AlpSignaling) signalingHeader(v, p);
    if (p.type == AlpTypeExt) put16(v, p.extendedType);
    if (extra) v.push_back((uint8_t)p.sid);
    v.insert(v.end(), p.data.begin(), p.data.end());
    return v;
}

std::vector<std::vector<uint8_t>> alpSegments(const AlpPacket& p, int segBytes, int seq0) {
    std::vector<std::vector<uint8_t>> out;
    size_t pos = 0;
    int seq = seq0;
    while (pos < p.data.size()) {
        size_t n = std::min<size_t>(segBytes, p.data.size() - pos);
        bool last = pos + n == p.data.size();
        std::vector<uint8_t> v;
        baseHeader(v, p.type, 1, 0, (int)n);
        const bool extra = p.sid >= 0;
        v.push_back((uint8_t)((seq & 0x1F) << 3 | (last ? 4 : 0) | (extra ? 2 : 0)));   // seg_SN, LSI, SIF, HEF
        if (seq == seq0) {   // type specific headers and the sub-stream id belong to the first segment
            if (p.type == AlpSignaling) signalingHeader(v, p);
            if (p.type == AlpTypeExt) put16(v, p.extendedType);
        }
        if (extra) v.push_back((uint8_t)p.sid);
        v.insert(v.end(), p.data.begin() + pos, p.data.begin() + pos + n);
        out.push_back(v);
        pos += n;
        seq++;
    }
    return out;
}

std::vector<uint8_t> alpConcatenate(const std::vector<AlpPacket>& pk) {
    std::vector<uint8_t> v;
    const int n = (int)pk.size();
    if (n < 2 || n > 9) return v;
    int total = 0;
    for (auto& p : pk) total += (int)p.data.size();
    baseHeader(v, pk[0].type, 1, 1, total);
    const int count = n - 2;
    v.push_back((uint8_t)(((total >> 11) & 0x0F) << 4 | count << 1 | 0));   // length_MSB(4), count(3), SIF(1)
    // (count + 1) component lengths of 12 bits, then 4 stuffing bits when their number is odd
    std::vector<int> comp;
    for (int i = 0; i + 1 < n; i++) comp.push_back((int)pk[i].data.size());
    for (size_t i = 0; i < comp.size(); i += 2) {
        if (i + 1 < comp.size()) {
            v.push_back((uint8_t)(comp[i] >> 4));
            v.push_back((uint8_t)((comp[i] & 0xF) << 4 | (comp[i + 1] >> 8)));
            v.push_back((uint8_t)comp[i + 1]);
        } else {
            v.push_back((uint8_t)(comp[i] >> 4));
            v.push_back((uint8_t)((comp[i] & 0xF) << 4));   // stuffing bits 0000
        }
    }
    for (auto& p : pk) v.insert(v.end(), p.data.begin(), p.data.end());
    return v;
}

std::vector<uint8_t> alpTs(const std::vector<uint8_t>& ts, int deleted) {
    std::vector<uint8_t> v;
    int n = (int)ts.size() / 188;
    if (n < 1 || n > 16) return v;
    const bool ahf = deleted > 0;
    v.push_back((uint8_t)(7 << 5 | (n & 15) << 1 | (ahf ? 1 : 0)));
    if (ahf) v.push_back((uint8_t)(deleted & 0x7F));   // HDM = 0
    for (int i = 0; i < n; i++) v.insert(v.end(), ts.begin() + i * 188 + 1, ts.begin() + (i + 1) * 188);   // the sync byte is removed
    return v;
}

// ---- reassembly

namespace {

// header length of the ALP packet at buf (bytes available: n), the payload length; false if more bytes are needed; error flag set for invalid
struct Hdr { int header = 0; int payload = 0; bool ok = false; bool error = false; };

Hdr readHeader(const uint8_t* b, int n) {
    Hdr h;
    if (n < 1) return h;
    const int type = b[0] >> 5;
    if (type == 7) {
        int numts = (b[0] >> 1) & 15;
        if (numts == 0) numts = 16;
        bool ahf = b[0] & 1;
        if (ahf && n < 2) return h;
        h.header = ahf ? 2 : 1;
        h.payload = numts * 187;
        h.ok = true;
        return h;
    }
    if (n < 2) return h;
    const int pc = (b[0] >> 4) & 1, hm = (b[0] >> 3) & 1;
    int len = ((b[0] & 7) << 8) | b[1];
    int pos = 2;
    bool sif = false, hef = false;
    if (pc == 0) {
        if (hm) {
            if (n < 3) return h;
            len |= (b[2] >> 3) << 11;
            sif = (b[2] >> 1) & 1; hef = b[2] & 1;
            pos = 3;
        }
    } else if (hm == 0) {   // segmentation
        if (n < 3) return h;
        sif = (b[2] >> 1) & 1; hef = b[2] & 1;
        pos = 3;
    } else {                // concatenation
        if (n < 3) return h;
        len |= (b[2] >> 4) << 11;
        const int count = (b[2] >> 1) & 7;
        sif = b[2] & 1;
        const int bits = (count + 1) * 12;
        const int bytes = (bits + 7) / 8;
        if (n < 3 + bytes) return h;
        pos = 3 + bytes;
    }
    // type specific headers (the segmentation case carries them in the first segment only; the reassembler knows, see emit())
    h.header = pos;
    h.payload = len;
    h.ok = true;
    if (hef) h.error = true;   // header extensions are not parsed here (all values are reserved); resolved by the caller
    return h;
}

} // namespace

int AlpReassembler::packetLength(const std::vector<uint8_t>& buf) const {
    const uint8_t* b = buf.data();
    const int n = (int)buf.size();
    Hdr h = readHeader(b, n);
    if (!h.ok) return 0;
    const int type = b[0] >> 5;
    int extra = 0;
    if (type != 7) {
        const int pc = (b[0] >> 4) & 1, hm = (b[0] >> 3) & 1;
        bool first = true;   // for segments the extra headers are in the first segment (sequence number 0)
        bool sif = false, hef = false;
        if (pc == 0 && hm) { sif = (b[2] >> 1) & 1; hef = b[2] & 1; }
        else if (pc == 1 && hm == 0) { if (n < 3) return 0; sif = (b[2] >> 1) & 1; hef = b[2] & 1; first = ((b[2] >> 3) == 0); }
        else if (pc == 1 && hm == 1) { sif = b[2] & 1; }
        if (type == AlpSignaling && first) extra += 5;
        if (type == AlpTypeExt && first) extra += 2;
        if (sif) extra += 1;
        if (hef) {
            const int at = h.header + extra;
            if (n < at + 2) return 0;
            extra += 2 + (b[at + 1] + 1);
        }
    }
    return h.header + extra + h.payload;
}

bool alpParse(const std::vector<uint8_t>& a, std::vector<AlpPacket>& out, bool* isSeg, int* segSeq, bool* lastSeg) {
    const uint8_t* b = a.data();
    const int n = (int)a.size();
    if (isSeg) *isSeg = false;
    Hdr h = readHeader(b, n);
    if (!h.ok) return false;
    const int type = b[0] >> 5;
    if (type == 7) {
        int numts = (b[0] >> 1) & 15;
        if (numts == 0) numts = 16;
        AlpPacket p;
        p.type = AlpTs;
        int deleted = (b[0] & 1) ? (b[1] & 0x7F) : 0;
        (void)deleted;   // null packets are not re-inserted here; the count is available to callers that need constant bit rate
        for (int i = 0; i < numts; i++) {
            p.data.push_back(0x47);
            p.data.insert(p.data.end(), b + h.header + i * 187, b + h.header + (i + 1) * 187);
        }
        out.push_back(p);
        return true;
    }
    const int pc = (b[0] >> 4) & 1, hm = (b[0] >> 3) & 1;
    int pos = h.header;
    bool sif = false, hef = false, first = true;
    int sn = 0, lsi = 0;
    if (pc == 0 && hm) { sif = (b[2] >> 1) & 1; hef = b[2] & 1; }
    else if (pc == 1 && hm == 0) { sif = (b[2] >> 1) & 1; hef = b[2] & 1; sn = b[2] >> 3; lsi = (b[2] >> 2) & 1; first = sn == 0; }
    else if (pc == 1 && hm == 1) { sif = b[2] & 1; }
    AlpPacket p;
    p.type = type;
    if (type == AlpSignaling && first) {
        if (pos + 5 > n) return false;
        p.signalingType = b[pos]; p.signalingExt = (b[pos + 1] << 8) | b[pos + 2]; p.signalingVersion = b[pos + 3];
        p.signalingFormat = b[pos + 4] >> 6; p.signalingEncoding = (b[pos + 4] >> 4) & 3;
        pos += 5;
    }
    if (type == AlpTypeExt && first) { if (pos + 2 > n) return false; p.extendedType = (b[pos] << 8) | b[pos + 1]; pos += 2; }
    if (sif) { if (pos + 1 > n) return false; p.sid = b[pos++]; }
    if (hef) { if (pos + 2 > n) return false; pos += 2 + b[pos + 1] + 1; }
    if (pos > n) return false;
    const uint8_t* payload = b + pos;
    const int plen = n - pos;
    if (pc == 1 && hm == 0) {   // a segment
        if (isSeg) *isSeg = true;
        if (segSeq) *segSeq = sn;
        if (lastSeg) *lastSeg = lsi;
        p.data.assign(payload, payload + plen);
        out.push_back(p);
        return true;
    }
    if (pc == 1 && hm == 1) {   // concatenation
        const int count = (b[2] >> 1) & 7;
        std::vector<int> comp;
        int bitpos = 24;
        for (int i = 0; i <= count; i++) {
            int byte = bitpos >> 3;
            int v = (bitpos & 7) == 0 ? ((b[byte] << 4) | (b[byte + 1] >> 4)) : (((b[byte] & 0xF) << 8) | b[byte + 1]);
            comp.push_back(v);
            bitpos += 12;
        }
        int at = 0;
        for (size_t i = 0; i <= comp.size(); i++) {
            int l = i < comp.size() ? comp[i] : plen - at;
            if (l <= 0 || at + l > plen) return false;
            AlpPacket q = p;
            q.data.assign(payload + at, payload + at + l);
            out.push_back(q);
            at += l;
        }
        return true;
    }
    p.data.assign(payload, payload + plen);
    out.push_back(p);
    return true;
}

void AlpReassembler::reset() { buf_.clear(); synced_ = false; inSegments_ = false; segNext_ = 0; }

void AlpReassembler::emit(const std::vector<uint8_t>& a, std::vector<AlpPacket>& out) {
    std::vector<AlpPacket> parts;
    bool seg = false, last = false;
    int sn = 0;
    if (!alpParse(a, parts, &seg, &sn, &last)) { errors_++; return; }
    if (!seg) { for (auto& p : parts) { out.push_back(p); packets_++; } return; }
    const AlpPacket& s = parts[0];
    if (sn == 0) { inSegments_ = true; segNext_ = 1; segPacket_ = s; }
    else if (inSegments_ && sn == segNext_) { segPacket_.data.insert(segPacket_.data.end(), s.data.begin(), s.data.end()); segNext_++; }
    else { inSegments_ = false; errors_++; return; }
    if (last && inSegments_) { out.push_back(segPacket_); packets_++; inSegments_ = false; }
}

void AlpReassembler::push(const uint8_t* payload, int size, int pointer, std::vector<AlpPacket>& out) {
    int start = 0;
    if (pointer != 8191 && pointer <= size) {
        if (!synced_) { buf_.clear(); start = pointer; synced_ = true; }   // join at the first packet that starts here
        else {   // the bytes before the pointer finish the packet in progress
            buf_.insert(buf_.end(), payload, payload + pointer);
            start = pointer;
        }
    } else if (!synced_) {
        return;   // no start seen yet, nothing to join
    } else {
        buf_.insert(buf_.end(), payload, payload + size);
        start = size;
    }
    // now buf_ may hold the rest of an earlier packet; drain complete packets and continue with the new bytes
    auto drain = [&]() {
        for (;;) {
            if (buf_.empty()) return;
            int len = packetLength(buf_);
            if (len < 0) { errors_++; buf_.clear(); synced_ = false; return; }
            if (len == 0 || (int)buf_.size() < len) return;
            std::vector<uint8_t> one(buf_.begin(), buf_.begin() + len);
            buf_.erase(buf_.begin(), buf_.begin() + len);
            emit(one, out);
        }
    };
    drain();
    if (start < size && pointer != 8191) {
        buf_.insert(buf_.end(), payload + start, payload + size);
        drain();
    }
}

} // namespace atsc3
} // namespace dect2
