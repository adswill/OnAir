// Instrument decoders of the X-band links (xband_proto.h). Packet layouts from SatDump's plugins jpss_support, eos_support, noaa_metop_support, gcom_support, metopsg_support,
// landsat_support, proba_support (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir, test signals in xband_gen.cpp. See THIRD_PARTY.md.
#include "dect2/xband/xband_proto.h"
#include "dect2/xband/xband_instr.h"
#include "dect2/xband/xband_products.h"
#include "dect2/ccsds/ccsds.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/hrpt/hrpt_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace dect2::xband {

// ---- VIIRS (CCSDS packets, 15-bit CCSDS 121 Rice detector zones) ----------------------------------------------------------------
namespace {
struct ViirsCh { int apid; const char* name; double um; int zw[6]; int zh; int width; int os[6]; int scale; int ref; int decim; };
const ViirsCh kViirs[] = {
    {804, "M1", 0.412, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, 803, 1},
    {803, "M2", 0.445, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, 802, 1},
    {802, "M3", 0.488, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, 800, 1},
    {800, "M4", 0.555, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, -1, 1},
    {801, "M5", 0.672, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, 800, 1},
    {805, "M6", 0.746, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {806, "M7", 0.865, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, -1, 1},
    {809, "M8", 1.24, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, 808, 1},
    {807, "M9", 1.378, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {808, "M10", 1.61, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {810, "M11", 2.25, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, 808, 1},
    {812, "M12", 3.7, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {811, "M13", 4.05, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 2, 3, 3, 2, 1}, 8, -1, 1},
    {816, "M14", 8.55, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, 815, 1},
    {815, "M15", 10.763, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {814, "M16", 12.013, {640, 368, 592, 592, 368, 640}, 16, 3200, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {818, "I1", 0.64, {1280, 736, 1184, 1184, 736, 1280}, 32, 6400, {1, 1, 1, 1, 1, 1}, 16, -1, 1},
    {819, "I2", 0.865, {1280, 736, 1184, 1184, 736, 1280}, 32, 6400, {1, 1, 1, 1, 1, 1}, 16, 818, 1},
    {820, "I3", 1.61, {1280, 736, 1184, 1184, 736, 1280}, 32, 6400, {1, 1, 1, 1, 1, 1}, 16, 819, 1},
    {813, "I4", 3.74, {1280, 736, 1184, 1184, 736, 1280}, 32, 6400, {1, 1, 1, 1, 1, 1}, 16, 812, 2},
    {817, "I5", 11.45, {1280, 736, 1184, 1184, 736, 1280}, 32, 6400, {1, 1, 1, 1, 1, 1}, 16, 815, 2},
    {821, "DNB", 0.7, {784, 488, 760, 760, 488, 784}, 16, 4064, {1, 1, 1, 1, 1, 1}, 2, -1, 1},
};
constexpr int kNumViirs = (int)(sizeof kViirs / sizeof kViirs[0]);
int viirsIdx(int apid) { for (int i = 0; i < kNumViirs; i++) if (kViirs[i].apid == apid) return i; return -1; }
}

struct ViirsDec {
    struct Seg { std::vector<std::vector<uint16_t>> det; bool done = false; };                      // det[line * 6 + zone]
    struct Group { int idx; Seg ch[kNumViirs]; };
    Store& s;
    int plane[kNumViirs] = {};
    std::map<uint64_t, Group> groups;
    int inSeg[kNumViirs] = {};
    uint64_t curKey[kNumViirs] = {}; bool have[kNumViirs] = {}; int endSeq[kNumViirs] = {};
    int nextIdx = 0;
    uint64_t lines = 0;
    explicit ViirsDec(Store& st) : s(st) {
        for (int i = 0; i < kNumViirs; i++) plane[i] = s.add(std::string("VIIRS ") + kViirs[i].name, kViirs[i].um, kViirs[i].width);
    }
    Seg makeSeg(int c) {
        Seg sg; const auto& k = kViirs[c];
        sg.det.resize((size_t)k.zh * 6);
        for (int l = 0; l < k.zh; l++) for (int z = 0; z < 6; z++) sg.det[(size_t)l * 6 + z].assign((size_t)k.zw[z] * k.os[z], 0);
        return sg;
    }
    void finalize(Group& g) {
        // the differential channels hold the difference from their reference channel, in this order
        static const int order[] = {801, 802, 803, 804, 809, 810, 816, 819, 820, 813, 817};
        for (int a : order) {
            int c = viirsIdx(a), r = viirsIdx(kViirs[c].ref);
            const int dec = kViirs[c].decim;
            Seg& sc = g.ch[c]; Seg& sr = g.ch[r];
            if (!sc.done) continue;
            if (!sr.done) { sc.done = false; continue; }
            const auto& k = kViirs[c];
            for (int l = 0; l < k.zh; l++) for (int z = 0; z < 6; z++) {
                auto& d = sc.det[(size_t)l * 6 + z];
                const auto& rd = sr.det[(size_t)(l / dec) * 6 + z];
                for (int x = 0; x < k.zw[z] && x / dec < (int)rd.size(); x++) d[(size_t)x] = (uint16_t)std::clamp((int)d[(size_t)x] + (int)rd[(size_t)(x / dec)] - 16383, 0, 65535);
            }
        }
        for (int c = 0; c < kNumViirs; c++) {
            Seg& sc = g.ch[c];
            if (!sc.done) continue;
            const auto& k = kViirs[c];
            Plane& p = s.plane(plane[c]);
            std::vector<uint16_t> row((size_t)k.width);
            for (int l = 0; l < k.zh; l++) {
                int w = 0;
                for (int z = 0; z < 6; z++) {
                    const auto& d = sc.det[(size_t)l * 6 + z];
                    for (int x = 0; x < k.zw[z]; x++) row[(size_t)w++] = (uint16_t)std::min<int>(d[(size_t)x] * k.scale, 65535);
                }
                p.setRow(g.idx * k.zh + (k.zh - 1 - l), row.data());
                lines++;
            }
        }
    }
    void flushOld() {
        while (groups.size() > 4) { finalize(groups.begin()->second); groups.erase(groups.begin()); }
    }
    void flushAll() { for (auto& kv : groups) finalize(kv.second); groups.clear(); }
    Group& group(uint64_t key) {
        auto it = groups.find(key);
        if (it != groups.end()) return it->second;
        Group& g = groups[key];
        g.idx = nextIdx++;
        flushOld();
        return groups[key];
    }
    void packet(const Packet& p) {
        const int c = viirsIdx(p.apid);
        if (c < 0) return;
        const auto& k = kViirs[c];
        if (p.seqFlag == 1) {
            if (p.data.size() < 12) return;
            uint64_t key = 0;
            for (int i = 0; i < 8; i++) key = key << 8 | p.data[(size_t)i];
            Group& g = group(key);
            Seg sg = makeSeg(c);
            sg.done = true;
            g.ch[c] = std::move(sg);
            curKey[c] = key; have[c] = true;
            endSeq[c] = p.seqCount + p.data[8] + 2;
            return;
        }
        if (!have[c] || p.seqCount > endSeq[c]) return;
        auto it = groups.find(curKey[c]);
        if (it == groups.end()) return;
        Seg& sg = it->second.ch[c];
        if (p.data.size() < 96) return;
        const int det = p.data[19];
        if (det >= k.zh) return;
        const uint32_t pat = (uint32_t)p.data[20] << 24 | p.data[21] << 16 | p.data[22] << 8 | p.data[23];
        size_t off = 88;
        ccsds::RiceParams rp; rp.bitsPerSample = 15; rp.blockSize = 8; rp.rsiBlocks = 128; rp.preprocess = true; rp.byteAlignRsi = false;
        for (int z = 0; z < 6 && off < p.data.size(); z++) {
            const uint8_t* d = &p.data[off];
            if (p.data.size() - off < 8) break;
            const int fill = d[0];
            const int co = d[2] << 8 | d[3];
            int sz = co - 4;
            if (sz <= 0) continue;
            if (co < 4 || (size_t)co + 8 > p.data.size() - off) continue;
            const uint32_t sw = (uint32_t)d[co + 4] << 24 | d[co + 5] << 16 | d[co + 6] << 8 | d[co + 7];
            int fs = fill, bits = 0;
            while (fs % 8 != 0) { bits++; fs--; }
            int bytes = sz - fs / 8;
            if (bytes >= 0 && bytes <= sz) sz = bytes + 1;
            if (sz > 8 && (sw == pat || sw == 0xC000FFEE)) {
                auto& out = sg.det[(size_t)det * 6 + z];
                ccsds::riceDecode(d + 4, (size_t)sz - 1, rp, out.data(), out.size());
                if (k.os[z] > 1) {
                    for (int x = 0; x < k.zw[z] * k.os[z]; x += k.os[z]) {
                        double v = 0;
                        for (int m = 0; m < k.os[z]; m++) v += out[(size_t)x + m];
                        out[(size_t)x / k.os[z]] = (uint16_t)(v / k.os[z]);
                    }
                }
            }
            off += (size_t)co + 8;
        }
    }
};

std::vector<uint8_t> viirsBuildSegmentHeader(const uint8_t time[8], int nPackets) {
    std::vector<uint8_t> h(32, 0);
    std::memcpy(h.data(), time, 8);
    h[8] = (uint8_t)nPackets;
    return h;
}

// One detector line as a VIIRS body packet: values are zw * os samples per zone, the checksum word follows each zone
std::vector<uint8_t> viirsBuildBody(int det, uint32_t syncPat, const std::vector<std::vector<uint16_t>>& zones) {
    std::vector<uint8_t> p(88, 0);
    p[19] = (uint8_t)det;
    p[20] = (uint8_t)(syncPat >> 24); p[21] = (uint8_t)(syncPat >> 16); p[22] = (uint8_t)(syncPat >> 8); p[23] = (uint8_t)syncPat;
    ccsds::RiceParams rp; rp.bitsPerSample = 15; rp.blockSize = 8; rp.rsiBlocks = 128; rp.preprocess = true; rp.byteAlignRsi = false;
    for (const auto& z : zones) {
        auto enc = ccsds::riceEncode(z.data(), z.size(), rp);
        const int co = 4 + (int)enc.size() + 3;   // data + padding; the sync word is at co + 4
        const size_t at = p.size();
        p.resize(at + (size_t)co + 8, 0);
        p[at] = 16;                               // two bytes of fill counted at the end (bit_slicer: fill in bits)
        p[at + 2] = (uint8_t)(co >> 8); p[at + 3] = (uint8_t)co;
        std::memcpy(&p[at + 4], enc.data(), enc.size());
        p[at + co + 4] = (uint8_t)(syncPat >> 24); p[at + co + 5] = (uint8_t)(syncPat >> 16); p[at + co + 6] = (uint8_t)(syncPat >> 8); p[at + co + 7] = (uint8_t)syncPat;
    }
    return p;
}

const char* viirsName(int c) { return kViirs[c].name; }
int viirsCount() { return kNumViirs; }
void viirsChannelInfo(int c, int& apid, int& zh, const int*& zw, const int*& os, int& ref, int& scale, int& decim) {
    apid = kViirs[c].apid; zh = kViirs[c].zh; zw = kViirs[c].zw; os = kViirs[c].os; ref = kViirs[c].ref; scale = kViirs[c].scale; decim = kViirs[c].decim;
}

// ---- MODIS ----------------------------------------------------------------------------------------------------------------
namespace {
const double kModis1kmUm[31] = {0.412, 0.443, 0.488, 0.531, 0.551, 0.667, 0.667, 0.678, 0.678, 0.748, 0.869, 0.905, 0.936, 0.94, 3.75, 3.959, 3.959, 4.05, 4.465, 4.515, 1.375, 6.715, 7.325, 8.55, 9.73, 11.03, 12.02, 13.335, 13.635, 13.935, 14.235};
const char* const kModis1kmName[31] = {"8", "9", "10", "11", "12", "13L", "13H", "14L", "14H", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32", "33", "34", "35", "36"};
const double kModis500Um[5] = {0.469, 0.555, 1.24, 1.64, 2.13};
const double kModis250Um[2] = {0.645, 0.858};
}

struct ModisDec {
    Store& s;
    int p250[2], p500[5], p1k[31];
    int lines = 0, lastScan = -1;
    uint64_t day = 0, night = 0, bad = 0;
    bool started = false;
    explicit ModisDec(Store& st) : s(st) {
        for (int i = 0; i < 2; i++) p250[i] = s.add("MODIS " + std::to_string(i + 1), kModis250Um[i], 1354 * 4);
        for (int i = 0; i < 5; i++) p500[i] = s.add("MODIS " + std::to_string(i + 3), kModis500Um[i], 1354 * 2);
        for (int i = 0; i < 31; i++) p1k[i] = s.add(std::string("MODIS ") + kModis1kmName[i], kModis1kmUm[i], 1354);
    }
    static uint16_t csum(const uint16_t* d, int n) { uint16_t c = 0; for (int i = 0; i < n; i++) c = (uint16_t)(c + d[i]); return (uint16_t)(c >> 4); }
    void packet(const Packet& p) {
        if (p.data.size() < 12) return;
        const int type = (p.data[8] >> 4) & 7, scan = (p.data[8] >> 1) & 7;
        const bool calib = p.data[9] >> 7;
        const int pos1 = ((p.data[9] & 0x7F) << 4) | (p.data[10] >> 4);
        uint16_t w[416];
        if (type == 0) {
            if (p.data.size() < 636) return;
            unpack12(&p.data[12], 416, w);
            if (csum(w, 415) != w[415]) { bad++; return; }
            day++;
            if (calib || pos1 > 1354) return;
            int pos = pos1 - 1;
            if (pos == 0 && p.seqFlag == 1 && lastScan != scan) { if (started) lines += 10; started = true; }
            lastScan = scan;
            if (!started) return;
            if (p.seqFlag == 1) put(pos, 5, w, true);
            else if (p.seqFlag == 2) put(pos, 0, w, true);
        } else if (type == 1) {
            if (p.data.size() < 270) return;
            unpack12(&p.data[12], 172, w);
            if (csum(w, 171) != w[171]) { bad++; return; }
            night++;
            if (calib || pos1 > 1354) return;
            int pos = pos1 - 1;
            if (pos == 0 && lastScan != scan) { if (started) lines += 10; started = true; }
            lastScan = scan;
            if (!started) return;
            for (int i = 0; i < 17; i++) for (int f = 0; f < 10; f++) s.plane(p1k[14 + i]).setPixel(lines + f, pos, (uint16_t)(w[(9 - f) * 17 + i] << 4));
        }
    }
    void put(int pos, int offLine, const uint16_t* w, bool) {
        for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++) for (int y = 0; y < 4; y++) for (int f = 0; f < 5; f++)
            s.plane(p250[c]).setPixel((lines + offLine + f) * 4 + (3 - y), pos * 4 + i, (uint16_t)(w[(4 - f) * 83 + (c == 0 ? 0 : 16) + i * 4 + y] << 4));
        for (int c = 0; c < 5; c++) for (int i = 0; i < 2; i++) for (int y = 0; y < 2; y++) for (int f = 0; f < 5; f++)
            s.plane(p500[c]).setPixel((lines + offLine + f) * 2 + (1 - y), pos * 2 + i, (uint16_t)(w[(4 - f) * 83 + 32 + c * 4 + i * 2 + y] << 4));
        for (int i = 0; i < 31; i++) for (int f = 0; f < 5; f++)
            s.plane(p1k[i]).setPixel(lines + offLine + f, pos, (uint16_t)(w[(4 - f) * 83 + 52 + i] << 4));
    }
};

// Index of the first band of each resolution in the 83 words of an IFOV (test signal)
void modisDayPacket(int scan, int pos1, bool partTwo, std::vector<uint8_t>& p, uint16_t (*pix)(int, int, int), int line) {
    p.assign(636, 0);
    p[8] = (uint8_t)((0 << 4) | (scan << 1));
    p[9] = (uint8_t)(pos1 >> 4);
    p[10] = (uint8_t)((pos1 & 15) << 4);
    uint16_t w[416] = {};
    const int off = partTwo ? 0 : 5;
    for (int f = 0; f < 5; f++) {
        int base = (4 - f) * 83;
        for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++) for (int y = 0; y < 4; y++) w[base + (c == 0 ? 0 : 16) + i * 4 + y] = pix(c, (pos1 - 1) * 4 + i, (line + off + f) * 4 + (3 - y));
        for (int c = 0; c < 5; c++) for (int i = 0; i < 2; i++) for (int y = 0; y < 2; y++) w[base + 32 + c * 4 + i * 2 + y] = pix(2 + c, (pos1 - 1) * 2 + i, (line + off + f) * 2 + (1 - y));
        for (int c = 0; c < 31; c++) w[base + 52 + c] = pix(7 + c, pos1 - 1, line + off + f);
    }
    uint16_t cs = 0;
    for (int i = 0; i < 415; i++) cs = (uint16_t)(cs + w[i]);
    w[415] = (uint16_t)(cs >> 4);
    pack12(w, 416, &p[12]);
}

// ---- AVHRR through the MetOp packets (existing AHRPT decoder) ------------------------------------------------------
struct AvhrrDec {
    Store& s;
    AvhrrCal* cal = nullptr;
    int plane[5];
    int lines = 0;
    uint32_t lastMs = 0; bool haveMs = false;
    explicit AvhrrDec(Store& st) : s(st) {
        static const double um[5] = {0.63, 0.865, 1.6, 10.8, 12.0};
        for (int i = 0; i < 5; i++) plane[i] = s.add("AVHRR " + std::to_string(i + 1), um[i], kHrptWidth);
    }
    void packet(const Packet& p) {
        if (p.apid != hrpt::kApid3a && p.apid != hrpt::kApid3b) return;
        hrpt::Line l;
        if (!hrpt::decodeAvhrrPacket(p.data, l)) return;
        int y;
        if (haveMs && l.ms >= lastMs && l.ms - lastMs < 3600000) { lines += std::max<int>(1, (int)((l.ms - lastMs + 83) / 167)); } else if (haveMs) lines++;
        lastMs = l.ms; haveMs = true;
        y = lines;
        std::vector<uint16_t> row(kHrptWidth);
        for (int c = 0; c < 5; c++) {
            for (int x = 0; x < kHrptWidth; x++) row[(size_t)x] = (uint16_t)(l.px[(size_t)c * kHrptWidth + x] << 6);
            s.plane(plane[c]).setRow(y, row.data());
        }
        // on-board targets: PRT counts, 10 black body and space views of channels 3b / 4 / 5 (SatDump's avhrr_reader: counts start at byte 14)
        if (cal) {
            std::vector<uint16_t> w((size_t)(12960 - 14) * 8 / 10, 0);
            for (size_t i = 0; i < w.size(); i++) { const int bit = (int)i * 10; int v = 0; for (int k = 0; k < 10; k++) { const int b = bit + k; v = v << 1 | ((p.data[(size_t)14 + b / 8] >> (7 - b % 8)) & 1); } w[i] = (uint16_t)v; }
            if ((int)cal->lines.size() <= y) cal->lines.resize((size_t)y + 1);
            AvhrrCal::Line& ln = cal->lines[(size_t)y];
            ln.is3b = p.apid == hrpt::kApid3b;
            for (int i = 0; i < 3; i++) { const uint16_t pv = w[(size_t)10295 + 2 + i]; if (pv > 100 && pv < 500) ln.prt[ln.nPrt++] = pv; }
            for (int j = 0; j < 3; j++) {
                double bbSum = 0, spSum = 0; int bbN = 0, spN = 0;
                for (int i = 0; i < 10; i++) {
                    const int bv = w[(size_t)10305 + 5 * i + j + 2], sv = w[(size_t)5 * i + j + 2];
                    if (bv > 100) { bbSum += bv; bbN++; }
                    if (sv > 880) { spSum += sv; spN++; }
                }
                ln.cb[j] = bbN ? (float)(bbSum / bbN) : 0; ln.cs[j] = spN ? (float)(spSum / spN) : 0;
            }
        }
    }
};

// ---- AMSR2 --------------------------------------------------------------------------------------------------------------
struct Amsr2Dec {
    Store& s;
    int plane[20];
    int lines = 0, cur = 0;
    explicit Amsr2Dec(Store& st) : s(st) {
        for (int i = 0; i < 20; i++) plane[i] = s.add("AMSR2 " + std::to_string(i + 1), 0, 243);
    }
    void packet(const Packet& p) {
        if (p.apid != 1576 || p.data.size() < 1018) return;
        if (cur < 20) {
            for (int i = 0; i < 25; i++) for (int c = 0; c < 20; c++) {
                const size_t off = (size_t)10 + 2 * (i * 20 + c);
                uint16_t v = (uint16_t)(p.data[off] << 8 | p.data[off + 1]);
                const bool sign = (v >> 11) & 1;
                v &= 0x7FF;
                if (!sign) v = (uint16_t)(2048 + v);
                const int px = cur * 25 + i;
                if (px < 243) s.plane(plane[c]).setPixel(lines, px, (uint16_t)(v << 4));
            }
        }
        cur++;
        if (p.seqFlag == 1) { lines++; cur = 0; }
    }
};

// ---- Landsat TIRS -----------------------------------------------------------------------------------------------------------
struct TirsDec {
    Store& s;
    int plane[3];
    int lines[3] = {0, 0, 0};
    explicit TirsDec(Store& st) : s(st) {
        static const double um[3] = {10.9, 12.0, 12.0};
        for (int i = 0; i < 3; i++) plane[i] = s.add("TIRS " + std::to_string(i + 1), um[i], 1280);
    }
    void packet(const Packet& p) {
        if (p.apid < 1792 || p.apid > 1794 || p.data.size() < 5848) return;
        const int r = p.apid - 1792;
        const int nw = (5848 - 31) * 2 / 3;
        std::vector<uint16_t> w((size_t)nw);
        unpack12(&p.data[31], nw, w.data());
        static const int offA[3] = {0, 647, 1294}, offB[3] = {1941, 2588, 3235};
        std::vector<uint16_t> row(1280);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 640; i++) { row[(size_t)i * 2] = (uint16_t)(w[(size_t)offA[c] + i] << 4); row[(size_t)i * 2 + 1] = (uint16_t)(w[(size_t)offB[c] + i] << 4); }
            if (c == 0) s.plane(plane[r]).setRow(lines[r], row.data());
        }
        lines[r]++;
    }
};

// ---- METimage ------------------------------------------------------------------------------------------------------------
struct MetimageDec {
    static constexpr int kSub = 56, kSeg = 58, kWidth = kSub * kSeg, kTrim = 56;
    struct Cfg { int marker, idx, depth; bool rev; };
    Store& s;
    int plane[20];
    std::vector<uint16_t> wip;   // [c * 24 + l] * width
    int segs = 0, channels = 20;
    bool any = false;
    uint64_t lineCount = 0;
    static const Cfg* cfg(int m) {
        static const Cfg t[] = {{30,0,18,true},{28,1,18,true},{26,2,18,true},{25,3,17,true},{23,4,17,true},{24,5,17,true},{22,6,17,true},{4,7,17,false},{12,8,17,false},{8,9,17,false},{17,10,17,false},{15,11,18,false},{16,12,18,false},{20,13,18,false},{40,14,18,false},{39,15,18,false},{37,16,17,false},{34,17,17,false},{35,18,17,false},{33,19,17,false}};
        for (const auto& e : t) if (e.marker == m) return &e;
        return nullptr;
    }
    explicit MetimageDec(Store& st) : s(st) {
        for (int i = 0; i < 20; i++) plane[i] = s.add("METimage " + std::to_string(i + 1), 0, kWidth - kTrim);
        wip.assign((size_t)20 * 24 * kWidth, 0);
    }
    void flush() {
        for (int c = 0; c < 20; c++) for (int l = 0; l < 24; l++) {
            uint16_t* r = &wip[(size_t)(c * 24 + l) * kWidth];
            std::reverse(r, r + kWidth);
            s.plane(plane[c]).setRow(segs * 24 + l, r + kTrim);
            std::fill(r, r + kWidth, (uint16_t)0);
        }
        segs++; lineCount += 24;
    }
    static void unpackBits(const uint8_t* in, int bits, int n, uint32_t* out) {
        uint64_t sr = 0; int have = 0; size_t b = 0;
        for (int i = 0; i < n; i++) {
            while (have < bits) { sr = sr << 8 | in[b++]; have += 8; }
            out[i] = (uint32_t)((sr >> (have - bits)) & ((1u << bits) - 1));
            have -= bits;
        }
    }
    void packet(const Packet& p) {
        if ((p.apid != 1443 && p.apid != 1444) || p.data.size() < 21) return;
        const int ch = p.data[15];
        const int offset = p.data[19] << 8 | p.data[20];
        if (offset == 0 && ch == 30) { if (any) flush(); any = true; }
        const Cfg* c = cfg(ch);
        if (!c || offset + kSub > kWidth) return;
        const size_t need = c->depth == 17 ? 3027 - 6 : 3202 - 6;
        if (p.data.size() < need) return;
        uint32_t vals[1400];
        const int bytes = c->depth == 17 ? 2975 : 3150;
        if ((size_t)44 + bytes > p.data.size()) return;
        unpackBits(&p.data[44], c->depth, 1400, vals);
        for (int i = 0; i < kSub; i++) for (int l = 0; l < 24; l++) {
            const uint32_t v = vals[i * 25 + 1 + (c->rev ? (23 - l) : l)] >> (c->depth - 16);
            wip[(size_t)(c->idx * 24 + l) * kWidth + offset + i] = (uint16_t)v;
        }
    }
};

// ---- DMSP OLS (RTD frames) ------------------------------------------------------------------------------------------------
struct DmspDec {
    Store& s;
    int vis, ir;
    int lines = 0, pos = 0, dir = 0, sync = 0;
    uint64_t alarmLines = 0;
    std::vector<uint16_t> vrow, irow;
    static constexpr int kW = 490 * 15;
    explicit DmspDec(Store& st) : s(st), vrow(kW, 0), irow(kW, 0) {
        vis = s.add("OLS visible", 0.65, kW);
        ir = s.add("OLS infrared", 11.0, kW);
    }
    void flushLine() {
        std::vector<uint16_t> a(kW), b(kW);
        for (int i = 0; i < kW; i++) { a[(size_t)i] = vrow[(size_t)(kW - 1 - i)]; b[(size_t)i] = irow[(size_t)(kW - 1 - i)]; }
        s.plane(vis).setRow(lines, a.data()); s.plane(ir).setRow(lines, b.data());
        std::fill(vrow.begin(), vrow.end(), (uint16_t)0); std::fill(irow.begin(), irow.end(), (uint16_t)0);
    }
    void frame(const uint8_t* f) {
        uint8_t w[19];
        for (int i = 0; i < 19; i++) w[i] = (uint8_t)((f[i] << 6) | (i < 18 ? f[i + 1] >> 2 : 0));
        int match = 0;
        for (int i = 1; i < 13; i++) {
            int code = (i & 1) == 0 ? 0x07 : 0xFB, v = w[i];
            for (int j = 1; j < 9; j++) { if ((v & 1) == (code & 1)) match++; v >>= 1; code >>= 1; }
        }
        if (match > 70 && (alarmLines == 0 || pos > 400)) {
            dir = (w[15] >> 2) & 1;
            sync = (w[13] & 0xFC) | w[14] >> 6;
            pos = 0;
            if (alarmLines++ > 0) { flushLine(); lines++; }
            return;
        }
        if (w[1] == 0 && w[2] == 0) return;
        if (alarmLines == 0 || pos >= 490) return;
        uint8_t fine[15], smooth[15];
        for (int i = 0; i < 15; i++) fine[i] = w[1 + i] & 0xFC;
        for (int k = 0; k < 3; k++) {
            const int a = 1 + k * 5;
            const uint8_t v = (uint8_t)((w[a] & 3) << 6 | (w[a + 1] & 3) << 4 | (w[a + 2] & 3) << 2 | (w[a + 3] & 3));
            for (int i = 0; i < 5; i++) smooth[k * 5 + i] = v;
        }
        for (int i = 0; i < 15; i++) {
            if (dir == 0) {
                const int px = pos * 15 + i - sync / 10;
                if (px > 0 && px < kW) { vrow[(size_t)px] = (uint16_t)(fine[i] << 8); irow[(size_t)px] = (uint16_t)(smooth[i] << 8); }
            } else {
                const int px = pos * 15 - i - sync / 10;
                if (px > 0 && px < kW) { vrow[(size_t)(kW - 1 - px)] = (uint16_t)(fine[14 - i] << 8); irow[(size_t)(kW - 1 - px)] = (uint16_t)(smooth[14 - i] << 8); }
            }
        }
        pos++;
    }
};

// ---- Proba-V S-band VEGETATION (12-bit lines) -----------------------------------------------------------------------------
struct VegsDec {
    Store& s;
    struct Spec { int apid, frm, width; };
    static const Spec* spec(int a, int& idx) {
        static const Spec t[18] = {{290, 7818, 5200},{274, 7818, 5200},{322, 7818, 5200},{282, 1554, 1024},{298, 1554, 1024},{330, 1554, 1024},
                                   {289, 7818, 5200},{273, 7818, 5200},{321, 7818, 5200},{281, 1554, 1024},{297, 1554, 1024},{329, 1554, 1024},
                                   {292, 7818, 5200},{276, 7818, 5200},{324, 7818, 5200},{284, 1554, 1024},{300, 1554, 1024},{332, 1554, 1024}};
        for (int i = 0; i < 18; i++) if (t[i].apid == a) { idx = i; return &t[i]; }
        return nullptr;
    }
    int plane[18]; int lines[18] = {};
    explicit VegsDec(Store& st) : s(st) {
        for (int i = 0; i < 18; i++) { int ii = 0; const Spec* sp = nullptr; (void)ii; (void)sp; static const char* gname[3] = {"VEG-1", "VEG-2", "VEG-3"};
            static const int w[6] = {5200, 5200, 5200, 1024, 1024, 1024}; plane[i] = s.add(std::string(gname[i / 6]) + " " + std::to_string(i % 6 + 1), 0, w[i % 6]); }
    }
    uint64_t lineCount() const { int m = 0; for (int l : lines) m = std::max(m, l); return (uint64_t)m; }
    void packet(const Packet& p) {
        int i; const Spec* sp = spec(p.apid, i);
        if (!sp || (int)p.data.size() < sp->frm) return;
        std::vector<uint16_t> w((size_t)sp->width + 8), row((size_t)sp->width);
        unpack12(&p.data[18], sp->width, w.data());
        for (int x = 0; x < sp->width; x++) row[(size_t)x] = (uint16_t)(w[(size_t)x] << 4);
        s.plane(plane[i]).setRow(lines[i]++, row.data());
    }
};

// ---- Oceansat-2 OCM (92220-byte frames) --------------------------------------------------------------------------------------
#include "xband_oceansat_pn.inc"
uint16_t xbTestPixelXb(int ch, int x, int y);
void ocmBuildFrame(std::vector<uint8_t>& f, int line) {
    // the OCM reader's layout, randomised as the link sends it; the sync word over the first 34 bits
    f.assign(92220, 0);
    size_t pos = 1889 + 964 * 15 + 2;
    std::vector<uint16_t> lb((size_t)4072 * 10);
    for (int x = 0; x < 4072; x++) for (int c = 0; c < 8; c++) lb[(size_t)x * 10 + c] = xbTestPixelXb(c, x, line);
    for (int i = 0; i < 4072 * 10; i += 2) {
        f[pos] = (uint8_t)(lb[(size_t)i] >> 4); f[pos + 1] = (uint8_t)((lb[(size_t)i] & 15) << 4 | lb[(size_t)i + 1] >> 8); f[pos + 2] = (uint8_t)lb[(size_t)i + 1];
        pos += 3;
    }
    std::vector<uint8_t> mask(102350); { int o = 0, b = 0; uint8_t sh = 0; for (int r = 0; r < 400 && o < 102350; r++) for (int i = 0; i < 2047 && o < 102350; i++) { sh = (uint8_t)(sh << 1 | (kOsPn[i] - '0')); if (++b == 8) { mask[(size_t)o++] = sh; b = 0; } } }
    for (int i = 0; i < 59; i++) f[(size_t)i] ^= (uint8_t)(0xFF - mask[(size_t)(i + 2047) - 59]);
    for (int i = 59; i < 92220; i++) f[(size_t)i] ^= (uint8_t)(0xFF - mask[(size_t)i - 59]);
    const uint64_t sync = 0b0000000111111011111000010011001101ull;
    for (int i = 0; i < 34; i++) { const int b = sync >> (33 - i) & 1; if (b) f[(size_t)i >> 3] |= (uint8_t)(0x80 >> (i & 7)); else f[(size_t)i >> 3] &= (uint8_t)~(0x80 >> (i & 7)); }
}
struct OcmDec {
    Store& s;
    int plane[8]; int lines = 0;
    std::vector<uint8_t> mask;
    explicit OcmDec(Store& st) : s(st) {
        for (int i = 0; i < 8; i++) plane[i] = s.add("OCM " + std::to_string(i + 1), 0, 4072);
        mask.resize(102350); int o = 0, b = 0; uint8_t sh = 0;
        for (int r = 0; r < 400 && o < 102350; r++) for (int i = 0; i < 2047 && o < 102350; i++) { sh = (uint8_t)(sh << 1 | (kOsPn[i] - '0')); if (++b == 8) { mask[(size_t)o++] = sh; b = 0; } }
    }
    void derand(uint8_t* d) const {
        for (int i = 0; i < 59; i++) d[i] ^= (uint8_t)(0xFF - mask[(size_t)(i + 2047) - 59]);
        for (int i = 59; i < 92220; i++) d[i] ^= (uint8_t)(0xFF - mask[(size_t)i - 59]);
    }
    void frame(uint8_t* d) {
        derand(d);
        size_t pos = 1889 + 964 * 15 + 2;
        std::vector<uint16_t> lb((size_t)4072 * 10);
        for (int i = 0; i < 4072 * 10; i += 2) {
            lb[(size_t)i] = (uint16_t)(d[pos] << 4 | d[pos + 1] >> 4);
            lb[(size_t)i + 1] = (uint16_t)((d[pos + 1] & 15) << 8 | d[pos + 2]);
            pos += 3;
        }
        std::vector<uint16_t> row(4072);
        for (int c = 0; c < 8; c++) { for (int x = 0; x < 4072; x++) row[(size_t)x] = (uint16_t)(lb[(size_t)x * 10 + c] << 4); s.plane(plane[c]).setRow(lines, row.data()); }
        lines++;
    }
};

// ---- Kanopus-V PMSS (1960-byte frames) ---------------------------------------------------------------------------------------
struct KanopusDec {
    Store& s;
    int plane[10]; int lines = 0, global = 0; bool cnt = false; int badCnt = 0;
    uint8_t wip[1928 * 495];
    int wipId[16] = {};
    uint64_t images = 0;
    int nLines[10] = {};
    explicit KanopusDec(Store& st) : s(st) {
        for (int i = 0; i < 6; i++) plane[i] = s.add("PSS swath " + std::to_string(i + 1), 0.64, 1920);
        for (int i = 0; i < 4; i++) plane[6 + i] = s.add("MSS " + std::to_string(i + 1), 0, 1920);
        std::memset(wip, 0, sizeof wip);
    }
    void frame(uint8_t* f) {
        int counter = ((f[1946] & 0xF) << 8 | f[1945]) >> 1;
        if (!cnt) { if (counter == (global + 1) % 495) cnt = true; else global = counter; }
        if (cnt) { const int orig = counter; counter = global + 1; global = counter; if (orig != global) badCnt++; else badCnt = 0; if (badCnt > 2) cnt = false; }
        global %= 495;
        if (!cnt) return;
        counter = global;
        if (counter >= 495) return;
        const int id = f[1944] & 0xF;
        std::memcpy(&wip[1928 * counter], f + 16, 1928);
        wipId[id & 15]++;
        if (counter == 494) {
            int cid = 0, best = -1;
            for (int i = 0; i < 16; i++) if (wipId[i] > best) { best = wipId[i]; cid = i; }
            std::memset(wipId, 0, sizeof wipId);
            std::vector<uint16_t> row(1920);
            int p = cid >= 1 && cid <= 6 ? cid - 1 : cid >= 7 && cid <= 10 ? 6 + cid - 7 : -1;
            if (p >= 0) {
                for (int y = 30; y < 493; y++) { for (int x = 0; x < 1920; x++) row[(size_t)x] = (uint16_t)(wip[(size_t)y * 1928 + 4 + x] << 8); s.plane(plane[p]).setRow(nLines[p]++, row.data()); }
                lines = *std::max_element(nLines, nLines + 10); images++;
            }
            std::memset(wip, 0, sizeof wip);
        }
    }
};

// ---- The instruments of a link ------------------------------------------------------------------------------------------------
struct Instruments::Impl {
    int P;
    const XbLink& L;
    Store& store;
    Demux dem[64];
    std::unique_ptr<ViirsDec> viirs;
    std::unique_ptr<ModisDec> modis;
    std::unique_ptr<AvhrrDec> avhrr;
    std::unique_ptr<Amsr2Dec> amsr2;
    std::unique_ptr<TirsDec> tirs;
    std::unique_ptr<MetimageDec> metimage;
    std::unique_ptr<DmspDec> dmsp;
    std::unique_ptr<VegsDec> vegs;
    std::unique_ptr<OcmDec> ocm;
    std::unique_ptr<KanopusDec> kano;
    AvhrrCal avhrrCal;
    std::string sat;
    int scid = -1;
    Impl(int p, Store& s) : P(p), L(xbLink(p)), store(s) {
        switch (P) {
        case kXbNpp: case kXbJpss2: viirs = std::make_unique<ViirsDec>(s); break;
        case kXbAquaAura: case kXbTerra: modis = std::make_unique<ModisDec>(s); break;
        case kXbMetopDump: avhrr = std::make_unique<AvhrrDec>(s); avhrr->cal = &avhrrCal; break;
        case kXbGcomW1: amsr2 = std::make_unique<Amsr2Dec>(s); break;
        case kXbLandsatX: tirs = std::make_unique<TirsDec>(s); break;
        case kXbMetopSg: metimage = std::make_unique<MetimageDec>(s); break;
        case kXbDmsp: dmsp = std::make_unique<DmspDec>(s); break;
        case kXbProbaS: vegs = std::make_unique<VegsDec>(s); break;
        case kXbOceansat2: ocm = std::make_unique<OcmDec>(s); break;
        case kXbKanopus: kano = std::make_unique<KanopusDec>(s); break;
        default: break;
        }
        for (auto& d : dem) d.onPacket = [this](const Packet& p) { packet(p); };
    }
    void packet(const Packet& p) {
        if (viirs && p.apid >= 800 && p.apid <= 823) viirs->packet(p);
        else if (modis && p.apid == 64) modis->packet(p);
        else if (avhrr) avhrr->packet(p);
        else if (amsr2) amsr2->packet(p);
        else if (tirs) tirs->packet(p);
        else if (metimage) metimage->packet(p);
        else if (vegs) vegs->packet(p);
    }
    void name(int id) {
        scid = id;
        const char* n = nullptr;
        switch (P) {
        case kXbNpp: case kXbJpss2: n = id == 157 ? "Suomi NPP" : id == 159 ? "NOAA-20" : id == 177 ? "NOAA-21" : nullptr; break;
        case kXbAquaAura: n = id == 154 ? "Aqua" : id == 204 ? "Aura" : nullptr; break;
        case kXbTerra: n = id == 42 ? "Terra" : nullptr; break;
        case kXbMetopDump: n = id == 12 ? "MetOp-A" : id == 11 ? "MetOp-B" : id == 13 ? "MetOp-C" : nullptr; avhrrCal.satIndex = id == 13 ? 1 : 0; break;
        case kXbMetopSg: n = id == 3 ? "MetOp-SG A1" : id == 0x42 ? "MetOp-SG B1" : nullptr; break;
        default: break;
        }
        if (n) sat = n;
    }
    bool wanted(int vcid) const {
        switch (P) {
        case kXbNpp: case kXbJpss2: return vcid == 16;
        case kXbAquaAura: return vcid == 30;
        case kXbTerra: return vcid == 42;
        case kXbMetopDump: return vcid == 9;
        case kXbGcomW1: return vcid == 17;
        case kXbLandsatX: return vcid == 5;
        case kXbMetopSg: return vcid == 16;
        case kXbProbaS: return vcid == 4;
        default: return false;
        }
    }
    void frame(const uint8_t* c) {
        Header h;
        parseHeader(L, c, h);
        name(h.scid);
        if (L.mpduData <= 0 || !wanted(h.vcid)) return;
        dem[h.vcid].push(h, L.mpduData, !L.tmFrames);
    }
    uint64_t pkts() const { uint64_t n = 0; for (const auto& d : dem) n += d.packets; return n; }
    uint64_t lines() const {
        if (viirs) return viirs->lines;
        if (modis) return (uint64_t)modis->lines;
        if (avhrr) return (uint64_t)avhrr->lines;
        if (amsr2) return (uint64_t)amsr2->lines;
        if (tirs) return (uint64_t)std::max({tirs->lines[0], tirs->lines[1], tirs->lines[2]});
        if (metimage) return metimage->lineCount;
        if (dmsp) return (uint64_t)dmsp->lines;
        if (vegs) return vegs->lineCount();
        if (ocm) return (uint64_t)ocm->lines;
        if (kano) return (uint64_t)kano->lines;
        return 0;
    }
    void finish() { if (viirs) viirs->flushAll(); if (metimage && metimage->any) { metimage->flush(); metimage->any = false; } }
};

Instruments::Instruments(int profile, Store& s) : p_(std::make_unique<Impl>(profile, s)) {}
Instruments::~Instruments() = default;
void Instruments::frame(const uint8_t* cadu) { p_->frame(cadu); }
uint64_t Instruments::packets() const { return p_->pkts(); }
uint64_t Instruments::lines() const { return p_->lines(); }
const std::string& Instruments::satName() const { return p_->sat; }
int Instruments::lastScid() const { return p_->scid; }
void Instruments::finish() { p_->finish(); }
const AvhrrCal* Instruments::avhrrCal() const { return p_->avhrr ? &p_->avhrrCal : nullptr; }
void Instruments::rawFrame(const uint8_t* f) { if (p_->dmsp) p_->dmsp->frame(f); }
void Instruments::oceansatFrame(uint8_t* f) { if (p_->ocm) p_->ocm->frame(f); }
void Instruments::kanopusFrame(uint8_t* f) { if (p_->kano) p_->kano->frame(f); }

} // namespace dect2::xband
