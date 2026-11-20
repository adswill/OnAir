// Iridium short burst data: IDA reassembly, SBD headers, ACARS (see iridium_sbd.h for the sources).
#include "dect2/iridium/iridium_sbd.h"
#include "dect2/acars/acars_proto.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
std::vector<uint8_t> fromHex(const std::string& h) {
    std::vector<uint8_t> v;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        const int a = nib(h[i]), b = nib(h[i + 1]);
        if (a < 0 || b < 0) break;
        v.push_back((uint8_t)(a * 16 + b));
    }
    return v;
}
std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        const char c = (char)(p[i] & 0x7f);    // 7-bit characters (parity not checked)
        s += (c >= 32 && c < 127) || c == '\n' || c == '\r' ? c : '?';
    }
    return s;
}
} // namespace

std::vector<IridiumIdaPacket> IridiumIdaAssembler::feed(const IridiumFrame& f, bool downlink, double freqHz, double timeSec, uint64_t tag) {
    std::vector<IridiumIdaPacket> out;
    // give up on packets that waited too long (ida.py: 1000 ms)
    for (size_t i = 0; i < open_.size();) {
        if (timeSec > open_[i].lastT + 1.0) { open_[i].p.complete = false; out.push_back(open_[i].p); open_.erase(open_.begin() + i); }
        else i++;
    }
    if (f.type != IridiumType::IDA || !f.ok || f.idaCtr < 0) return out;
    const std::vector<uint8_t> bytes = fromHex(f.hex);
    for (size_t i = 0; i < open_.size(); i++) {
        Open& o = open_[i];
        if (std::fabs(freqHz - o.lastF) < 260 && timeSec >= o.lastT && timeSec <= o.lastT + 0.28 && (o.ctr + 1) % 8 == f.idaCtr && o.p.downlink == downlink) {
            o.p.data.insert(o.p.data.end(), bytes.begin(), bytes.end());
            o.p.fragments++;
            o.p.tags.push_back(tag);
            o.ctr = f.idaCtr; o.lastT = timeSec; o.lastF = freqHz;
            if (!f.idaCont) { out.push_back(o.p); open_.erase(open_.begin() + i); }
            return out;
        }
    }
    if (f.idaCtr != 0) return out;     // a middle part whose start was missed
    Open o;
    o.p.data = bytes; o.p.downlink = downlink; o.p.timeSec = timeSec; o.p.freqHz = freqHz; o.p.fragments = 1; o.p.tags.push_back(tag);
    o.ctr = 0; o.lastT = timeSec; o.lastF = freqHz;
    if (!f.idaCont) out.push_back(o.p);
    else if (open_.size() < 64) open_.push_back(o);
    return out;
}

IridiumSbd iridiumParseSbd(const std::vector<uint8_t>& pk, bool downlink) {
    IridiumSbd r;
    if (pk.size() < 3) return r;
    if (!(pk[0] == 0x76 || (pk[0] == 0x06 && pk[1] == 0x00))) return r;
    r.type = pk[0] << 8 | pk[1];
    static const int known[] = {0x0600, 0x760c, 0x760d, 0x760e, 0x7608, 0x7609, 0x760a};
    if (std::find(std::begin(known), std::end(known), r.type) == std::end(known)) return r;
    size_t p = 2;
    if (r.type == 0x0600) {
        if (pk.size() < p + 29) return r;
        p += 29;
    } else if (p < pk.size() && pk[p] == 0x26) p += 7;
    else if (p < pk.size() && pk[p] == 0x20) p += 5;
    if (pk.size() > p + 3 && pk[p] == 0x10) p += 3;
    if (p >= pk.size()) return r;
    r.valid = true;
    r.header.assign(pk.begin(), pk.begin() + p);
    r.payload.assign(pk.begin() + p, pk.end());
    const std::vector<uint8_t>& d = r.payload;
    if (d.size() >= 13 && d[0] == 0x01) {
        IridiumAcars& a = r.acars;
        a.valid = true;
        a.mode = (char)(d[1] & 0x7f);
        a.reg = printable(&d[2], 7);
        a.ack = (char)(d[9] & 0x7f);
        a.label = printable(&d[10], 2);
        a.blockId = (char)(d[12] & 0x7f);
        size_t b = 13, e = d.size();
        // The suffix (ETX / ETB) ends the text. Some links add the two check bytes (and DEL) after it: look for the suffix at the end first,
        // then 3 and 4 bytes before the end, and check the sequence (CRC-16 of ARINC 618 over mode ... check bytes, with and without the
        // parity bits) where it is there.
        auto isSuf = [&](size_t i) { const int c = d[i] & 0x7f; return c == 0x03 || c == 0x17; };
        auto crcGood = [&](size_t suf) {
            if (suf + 3 > d.size()) return false;
            std::vector<uint8_t> v(d.begin() + 1, d.begin() + suf + 3);
            if (acarsCrc(v.data(), v.size()) == 0) return true;
            for (auto& c : v) c &= 0x7f;     // (the check bytes themselves keep all 8 bits)
            v[v.size() - 1] = d[suf + 2]; v[v.size() - 2] = d[suf + 1];
            return acarsCrc(v.data(), v.size()) == 0;
        };
        size_t suf = std::string::npos;
        for (size_t back : {(size_t)3, (size_t)4}) {
            if (d.size() >= b + back && isSuf(d.size() - back) && crcGood(d.size() - back)) { suf = d.size() - back; a.crc = 1; break; }
        }
        if (suf == std::string::npos) {
            if (e > b && isSuf(e - 1)) suf = e - 1;
            else if (e >= b + 3 && isSuf(e - 3)) { suf = e - 3; a.crc = 2; }
            else if (e >= b + 4 && isSuf(e - 4) && (d[e - 1] & 0x7f) == 0x7f) { suf = e - 4; a.crc = 2; }
        }
        if (suf != std::string::npos) {
            if ((d[suf] & 0x7f) == 0x17) a.more = true;
            e = suf;
            // parity: odd over all 8 bits of every byte from the mode character to the suffix; none of the bytes using bit 7 = no parity in use
            bool any = false, bad = false;
            for (size_t i = 1; i <= suf; i++) { if (d[i] & 0x80) any = true; if (!(__builtin_popcount(d[i]) & 1)) bad = true; }
            a.parity = !any ? 0 : bad ? 2 : 1;
        }
        if (e > b && (d[b] & 0x7f) == 0x02) {
            b++;
            if (!downlink && e >= b + 10) {   // from the aircraft: sequence number and flight number first
                a.seq = printable(&d[b], 4);
                a.flight = printable(&d[b + 4], 6);
                b += 10;
            }
            a.text = printable(&d[b], e - b);
        }
    }
    return r;
}

std::vector<uint8_t> iridiumBuildSbdAcars(char mode, const std::string& reg, char ack, const std::string& label, char blockId, const std::string& text) {
    std::vector<uint8_t> a;
    a.push_back(0x01);
    a.push_back((uint8_t)mode);
    std::string r = reg;
    while (r.size() < 7) r = "." + r;
    for (int i = 0; i < 7; i++) a.push_back((uint8_t)r[i]);
    a.push_back((uint8_t)ack);
    a.push_back((uint8_t)(label.size() > 0 ? label[0] : '_'));
    a.push_back((uint8_t)(label.size() > 1 ? label[1] : '_'));
    a.push_back((uint8_t)blockId);
    a.push_back(0x02);
    for (char c : text) a.push_back((uint8_t)c);
    a.push_back(0x03);
    std::vector<uint8_t> p = {0x76, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00, 0x10, (uint8_t)std::min<size_t>(a.size(), 255), 0x01};
    p.insert(p.end(), a.begin(), a.end());
    return p;
}

std::vector<std::vector<uint8_t>> iridiumSplitIda(const std::vector<uint8_t>& packet) {
    std::vector<std::vector<uint8_t>> v;
    for (size_t i = 0; i < packet.size(); i += 20) v.emplace_back(packet.begin() + i, packet.begin() + std::min(packet.size(), i + 20));
    return v;
}

} // namespace dect2
