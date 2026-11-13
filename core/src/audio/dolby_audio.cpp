// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Dolby broadcast audio headers, the output layout and the IEC 61937 packer (dolby_audio.h). Written from the standards: ETSI TS 102 366
// V1.4.1 (AC-3 / E-AC-3, annex E bsi), ETSI TS 103 420 V1.2.1 clause 8.3 (Atmos flag in addbsi), ETSI TS 103 190-2 V1.3.1 annex E
// (AC-4 DSI), ETSI EN 300 468 V1.20.1 annex D.7 (AC-4 descriptor), IEC 61937-1/-3 burst framing.
#include "dect2/audio/dolby_audio.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {
struct Bits {
    const uint8_t* p; size_t n; size_t pos = 0;   // pos in bits
    Bits(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool left(size_t k) const { return pos + k <= n * 8; }
    uint32_t get(int k) {   // reads k <= 32 bits; past the end reads zeros and marks the reader overrun
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            const size_t b = pos + (size_t)i;
            const uint32_t bit = b < n * 8 ? (p[b >> 3] >> (7 - (b & 7))) & 1u : 0u;
            v = (v << 1) | bit;
        }
        pos += (size_t)k;
        return v;
    }
    void skip(size_t k) { pos += k; }
    bool over() const { return pos > n * 8; }
    void align() { pos = (pos + 7) & ~(size_t)7; }
};

const int kAc3Rates[3] = {48000, 44100, 32000};
const int kAc3Kbps[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
const int kEac3Blocks[4] = {1, 2, 3, 6};

int ac3FrameBytes(int fscod, int frmsizecod) {
    const int kbps = kAc3Kbps[frmsizecod >> 1];
    if (fscod == 0) return kbps * 2 * 2;                    // 48 kHz: 2 * kbps words
    if (fscod == 2) return kbps * 3 * 2;                    // 32 kHz: 3 * kbps words
    return (kbps * 1000 * 1536 / 44100 / 16 + (frmsizecod & 1)) * 2;   // 44.1 kHz: rounded down, the odd codes one word more
}
} // namespace

bool parseDolbyFrame(const uint8_t* p, size_t n, DolbyFrame& f) {
    f = DolbyFrame();
    if (!p || n < 8 || p[0] != 0x0B || p[1] != 0x77) return false;
    const int bsid = p[5] >> 3;
    if (bsid > 16 || bsid == 9 || bsid == 10) return false;   // 9, 10: half / quarter rate AC-3, not used in broadcast
    Bits b(p, n);
    b.skip(16);
    f.bsid = bsid;
    if (bsid <= 8) {   // AC-3 (A/52, TS 102 366 clause 4.3)
        b.skip(16);    // crc1
        const int fscod = (int)b.get(2), frmsizecod = (int)b.get(6);
        if (fscod == 3 || frmsizecod > 37) return false;
        b.skip(5);
        f.bsmod = (int)b.get(3);
        f.acmod = (int)b.get(3);
        if ((f.acmod & 1) && f.acmod != 1) b.skip(2);   // cmixlev
        if (f.acmod & 4) b.skip(2);                      // surmixlev
        if (f.acmod == 2) b.skip(2);                     // dsurmod
        f.lfe = (int)b.get(1);
        f.sampleRate = kAc3Rates[fscod];
        f.frameBytes = ac3FrameBytes(fscod, frmsizecod);
        f.blocks = 6;
        f.valid = !b.over();
        return f.valid;
    }
    // E-AC-3 bsi (TS 102 366 annex E.1.2.2)
    f.eac3 = true;
    f.frameType = (int)b.get(2);
    if (f.frameType == 3) return false;
    f.substreamId = (int)b.get(3);
    f.frameBytes = ((int)b.get(11) + 1) * 2;
    const int fscod = (int)b.get(2);
    int numblkscod = 3;
    if (fscod == 3) {
        const int fscod2 = (int)b.get(2);
        if (fscod2 == 3) return false;
        f.sampleRate = kAc3Rates[fscod2] / 2;
    } else {
        numblkscod = (int)b.get(2);
        f.sampleRate = kAc3Rates[fscod];
    }
    f.blocks = kEac3Blocks[numblkscod];
    f.acmod = (int)b.get(3);
    f.lfe = (int)b.get(1);
    b.skip(5);   // bsid
    for (int i = 0; i < (f.acmod ? 1 : 2); i++) {   // dialnorm, compr
        b.skip(5);
        if (b.get(1)) b.skip(8);
    }
    if (f.frameType == 1 && b.get(1)) f.chanmap = (int)b.get(16);
    if (b.get(1)) {   // mixmdate
        if (f.acmod > 2) b.skip(2);                                     // dmixmod
        if ((f.acmod & 1) && f.acmod > 2) b.skip(6);                    // ltrtcmixlev, lorocmixlev
        if (f.acmod & 4) b.skip(6);                                     // ltrtsurmixlev, lorosurmixlev
        if (f.lfe && b.get(1)) b.skip(5);                               // lfemixlevcod
        if (f.frameType == 0) {
            for (int i = 0; i < (f.acmod ? 1 : 2); i++) if (b.get(1)) b.skip(6);   // pgmscl
            if (b.get(1)) b.skip(6);                                    // extpgmscl
            const int mixdef = (int)b.get(2);
            if (mixdef == 1) b.skip(5);
            else if (mixdef == 2) b.skip(12);
            else if (mixdef == 3) { const int len = (int)b.get(5); b.skip((size_t)(len + 2) * 8); }
            if (f.acmod < 2) for (int i = 0; i < (f.acmod ? 1 : 2); i++) if (b.get(1)) b.skip(14);   // paninfo
            if (b.get(1)) {   // frmmixcfginfoe
                for (int blk = 0; blk < f.blocks; blk++) if (f.blocks == 1 || b.get(1)) b.skip(5);
            }
        }
    }
    if (b.get(1)) {   // infomdate
        f.bsmod = (int)b.get(3);
        b.skip(2);   // copyrightb, origbs
        if (f.acmod == 2) b.skip(4);                                    // dsurmod, dheadphonmod
        if (f.acmod >= 6) b.skip(2);                                    // dsurexmod
        for (int i = 0; i < (f.acmod ? 1 : 2); i++) if (b.get(1)) b.skip(8);   // mixlevel, roomtyp, adconvtyp
        if (fscod < 3) b.skip(1);                                       // sourcefscod
    }
    if (f.frameType == 0 && numblkscod != 3) b.skip(1);                 // convsync
    if (f.frameType == 2) {
        const bool blkid = numblkscod == 3 ? true : b.get(1) != 0;
        if (blkid) b.skip(6);                                           // frmsizecod
    }
    f.addbsiBit = (int)b.pos;
    if (b.get(1)) {   // addbsie: TS 103 420 8.3.1 reserved(7), flag_ec3_extension_type_a(1), complexity_index_type_a(8)
        const int addbsil = (int)b.get(6);
        b.skip(7);
        f.joc = b.get(1) != 0;
        if (f.joc && addbsil >= 1) f.complexity = (int)b.get(8);
    }
    if (b.over() || f.frameBytes < 8) return false;
    f.valid = true;
    return true;
}

uint32_t dolbyAcmodSpeakers(int acmod, int lfe) {
    static const uint32_t m[8] = {kSpkL | kSpkR, kSpkC, kSpkL | kSpkR, kSpkL | kSpkC | kSpkR, kSpkL | kSpkR | kSpkCs,
                                  kSpkL | kSpkC | kSpkR | kSpkCs, kSpkL | kSpkR | kSpkLs | kSpkRs, kSpkL | kSpkC | kSpkR | kSpkLs | kSpkRs};
    return m[acmod & 7] | (lfe ? kSpkLfe : 0u);
}

uint32_t dolbyChanmapSpeakers(int cm) {
    if (cm < 0) return 0;
    // TS 102 366 table E.1.4: bit 0 is the most significant of the 16
    static const uint32_t loc[16] = {kSpkL, kSpkC, kSpkR, kSpkLs, kSpkRs, kSpkLc | kSpkRc, kSpkLrs | kSpkRrs, kSpkCs, kSpkTs,
                                     kSpkLsd | kSpkRsd, kSpkLw | kSpkRw, kSpkVhl | kSpkVhr, kSpkVhc, kSpkLts | kSpkRts, kSpkLfe2, kSpkLfe};
    uint32_t s = 0;
    for (int i = 0; i < 16; i++) if (cm & (1 << (15 - i))) s |= loc[i];
    return s;
}

int dolbySpeakerCount(uint32_t s) { int c = 0; for (; s; s &= s - 1) c++; return c; }

std::string dolbyBedName(uint32_t s) {
    if (!s) return std::string();
    const int lfe = ((s & kSpkLfe) ? 1 : 0) + ((s & kSpkLfe2) ? 1 : 0);
    const int full = dolbySpeakerCount(s) - lfe;
    const int top = dolbySpeakerCount(s & (kSpkVhl | kSpkVhr | kSpkVhc | kSpkTs | kSpkLts | kSpkRts));
    char b[24];
    if (top) snprintf(b, sizeof b, "%d.%d.%d", full - top, lfe, top);
    else snprintf(b, sizeof b, "%d.%d", full, lfe);
    return b;
}

void DolbyStreamInfo::feed(const uint8_t* p, size_t n) {
    if (!p || !n) return;
    pend_.insert(pend_.end(), p, p + n);
    size_t i = 0;
    while (i + 8 <= pend_.size()) {
        if (pend_[i] != 0x0B || pend_[i + 1] != 0x77) { i++; continue; }
        DolbyFrame f;
        if (!parseDolbyFrame(&pend_[i], pend_.size() - i, f)) { i++; continue; }
        // the next frame should follow directly; when it does not, this was a false sync (or the stream is damaged): look further.
        // A frame is taken only once the start of the next one is here (one frame of delay), so a false sync cannot swallow real frames.
        if (i + (size_t)f.frameBytes + 2 > pend_.size()) break;
        if (!(pend_[i + f.frameBytes] == 0x0B && pend_[i + f.frameBytes + 1] == 0x77)) { i++; continue; }
        const bool program = !f.eac3 || f.frameType != 1;   // an independent (or AC-3) frame starts the next program's frames
        if (program && f.substreamId == 0) {
            if (curSpk_) {
                if (curSpk_ == prevSpk_) {
                    s_.speakers = curSpk_;
                    s_.channels = dolbySpeakerCount(curSpk_);
                    s_.bed = dolbyBedName(curSpk_);
                    if (curBlocks_ > 0 && s_.sampleRate > 0) s_.bitrateKbps = (int)((int64_t)curBytes_ * 8 * s_.sampleRate / (curBlocks_ * 256) / 1000);
                }
                prevSpk_ = curSpk_;
            }
            curSpk_ = dolbyAcmodSpeakers(f.acmod, f.lfe);
            curBytes_ = 0; curBlocks_ = f.blocks;
            s_.valid = true;
            s_.eac3 = f.eac3;
            s_.sampleRate = f.sampleRate;
            s_.frames++;
            if (!s_.speakers) { s_.speakers = curSpk_; s_.channels = dolbySpeakerCount(curSpk_); s_.bed = dolbyBedName(curSpk_); }
        } else if (f.eac3 && f.frameType == 1 && curSpk_) {
            // a dependent substream adds speakers; one it names that the core already has (Ls/Rs moved back in a 7.1) stays one speaker
            curSpk_ |= dolbyChanmapSpeakers(f.chanmap);
        }
        if (f.substreamId == 0 || f.frameType == 1) curBytes_ += f.frameBytes;
        if (f.joc) { s_.atmos = true; s_.complexity = std::max(s_.complexity, f.complexity); }
        i += (size_t)f.frameBytes;
    }
    pend_.erase(pend_.begin(), pend_.begin() + (std::ptrdiff_t)i);
    if (pend_.size() > 8192) pend_.erase(pend_.begin(), pend_.end() - 4096);   // no sync in a long while: keep only the tail
}

std::string dolbyFormatLine(const DolbyStreamSummary& s) {
    if (!s.valid) return std::string();
    std::string r = s.atmos ? "Dolby Atmos (E-AC-3 JOC)" : s.eac3 ? "Dolby Digital Plus (E-AC-3)" : "Dolby Digital (AC-3)";
    if (!s.bed.empty()) r += s.atmos ? ", " + s.bed + " bed" : " " + s.bed;
    return r;
}

// ---- AC-4 ----------------------------------------------------------------------------------------------------------------------------
const char* ac4ChModeName(int m) {
    static const char* const n[16] = {"mono", "stereo", "3.0", "5.0", "5.1", "7.0", "7.1", "7.0", "7.1", "5.0.2", "5.1.2",
                                      "7.0.4", "7.1.4", "9.0.4", "9.1.4", "22.2"};
    return m >= 0 && m < 16 ? n[m] : "";
}

Ac4DsiInfo parseAc4DsiV1(const uint8_t* p, size_t n) {
    Ac4DsiInfo d;
    if (!p || n < 3) return d;
    Bits b(p, n);
    const int ver = (int)b.get(3);
    if (ver != 1) return d;                  // E.6: a version above 1 is skipped by decoders; 0 is the old layout
    d.bitstreamVersion = (int)b.get(7);
    const int fs = (int)b.get(1);
    d.sampleRate = fs ? 48000 : 44100;
    b.skip(4);                               // frame_rate_index
    const int np = (int)b.get(9);
    if (d.bitstreamVersion > 1 && b.get(1)) {   // b_program_id
        b.skip(16);
        if (b.get(1)) b.skip(128);           // program_uuid
    }
    b.skip(2 + 32 + 32);                     // ac4_bitrate_dsi: bit_rate_mode, bit_rate, bit_rate_precision
    b.align();
    if (b.over()) return d;
    d.valid = true;
    for (int i = 0; i < np && i < 64 && b.left(16); i++) {
        Ac4Presentation pr;
        pr.version = (int)b.get(8);
        size_t bytes = b.get(8);
        if (bytes == 255) bytes += b.get(16);
        const size_t start = b.pos;
        if (pr.version == 1 && b.left(5)) {
            Bits q(p, n); q.pos = start;
            pr.config = (int)q.get(5);
            if (pr.config != 6) {
                q.skip(3);                   // md_compat
                if (q.get(1)) q.skip(5);     // presentation_id
                q.skip(2 + 2 + 5 + 10);      // frame rate multiply / fraction, emdf version, key id
                pr.channelCoded = q.get(1) != 0;
                if (pr.channelCoded) pr.chMode = (int)q.get(5);
            }
            if (q.pos > start + bytes * 8 || q.over()) { pr.chMode = -1; pr.config = -1; pr.channelCoded = false; }
        }
        b.pos = start + bytes * 8;
        if (b.over()) break;
        d.presentations.push_back(pr);
    }
    const Ac4Presentation* pick = nullptr;
    for (const Ac4Presentation& pr : d.presentations) if (pr.immersive()) { pick = &pr; break; }
    d.immersive = pick != nullptr;
    if (!pick) for (const Ac4Presentation& pr : d.presentations) if (pr.config >= 0) { pick = &pr; break; }
    if (pick) d.bed = pick->channelCoded ? ac4ChModeName(pick->chMode) : "objects";
    return d;
}

Ac4Descriptor parseAc4Descriptor(const uint8_t* p, size_t n) {
    Ac4Descriptor a;
    if (!p || n < 1) return a;
    const bool cfg = (p[0] & 0x80) != 0, toc = (p[0] & 0x40) != 0;
    size_t i = 1;
    if (cfg) {
        if (i >= n) return a;
        a.haveConfig = true;
        a.dialogueEnhancement = (p[i] & 0x80) != 0;
        a.channelMode = (p[i] >> 5) & 3;
        i++;
    }
    if (toc) {
        if (i >= n) return a;
        const size_t len = p[i++];
        if (i + len > n) return a;
        a.dsi.assign(p + i, p + i + len);
        a.dsiInfo = parseAc4DsiV1(a.dsi.data(), a.dsi.size());
    }
    a.valid = true;
    return a;
}

namespace {
std::mutex gDsiMu;
Ac4DsiInfo gDsi;
}
void ac4NoteDsi(const Ac4DsiInfo& d) { std::lock_guard<std::mutex> g(gDsiMu); gDsi = d; }
Ac4DsiInfo ac4LastDsi() { std::lock_guard<std::mutex> g(gDsiMu); return gDsi; }

// ---- output layout -------------------------------------------------------------------------------------------------------------------
DolbyOutputConfig& DolbyOutputConfig::global() { static DolbyOutputConfig c; return c; }

int dolbyOutputChannels(int layout, int streamCh, int deviceCh) {
    switch (layout) {
    case 1: case 2: return 2;
    case 3: return streamCh > 2 ? 6 : 2;
    case 4: return streamCh > 6 ? 8 : streamCh > 2 ? 6 : 2;   // a 5.1 stream is not spread over 7.1: the receiver or system does that better
    default: break;
    }
    if (streamCh <= 2 || deviceCh < 6) return 2;
    if (streamCh > 6 && deviceCh >= 8) return 8;
    return 6;
}

// ---- IEC 61937 -----------------------------------------------------------------------------------------------------------------------
bool Iec61937Packer::pack(const uint8_t* payload, size_t n, int dataType, int lengthCode, int period, IecBurst& b) {
    const size_t words = (size_t)period * 2;   // two 16-bit subframes per IEC 60958 frame
    const size_t need = 4 + (n + 1) / 2;
    if (need > words) return false;
    b.words.assign(words, 0);
    b.dataType = dataType; b.lengthCode = lengthCode;
    b.words[0] = (int16_t)kPa; b.words[1] = (int16_t)kPb;
    b.words[2] = (int16_t)(uint16_t)dataType; b.words[3] = (int16_t)(uint16_t)lengthCode;
    for (size_t k = 0; k < n / 2; k++) b.words[4 + k] = (int16_t)(uint16_t)((payload[2 * k] << 8) | payload[2 * k + 1]);
    if (n & 1) b.words[4 + n / 2] = (int16_t)(uint16_t)(payload[n - 1] << 8);   // a lone last byte goes in the high half
    return true;
}

void Iec61937Packer::flush(std::vector<IecBurst>& out) {
    if (kind_ == Eac3 && !acc_.empty()) {
        IecBurst b;
        if (pack(acc_.data(), acc_.size(), 0x15, (int)acc_.size(), 6144, b)) out.push_back(std::move(b)); else dropped_++;
    }
    acc_.clear(); accBlocks_ = 0;
}

void Iec61937Packer::push(const uint8_t* p, size_t n, std::vector<IecBurst>& out) {
    size_t i = 0;
    while (i + 8 <= n) {
        DolbyFrame f;
        if (p[i] != 0x0B || p[i + 1] != 0x77 || !parseDolbyFrame(p + i, n - i, f) || i + (size_t)f.frameBytes > n) { i++; continue; }
        const Kind k = f.eac3 ? Eac3 : Ac3;
        if (k != kind_ || f.sampleRate != rate_) { acc_.clear(); accBlocks_ = 0; kind_ = k; rate_ = f.sampleRate; }
        if (k == Ac3) {
            IecBurst b;
            // Pc: data type 1 with bsmod in bits 8..10; Pd: the length in bits
            if (pack(p + i, (size_t)f.frameBytes, 0x01 | (f.bsmod << 8), f.frameBytes * 8, 1536, b)) out.push_back(std::move(b));
            else dropped_++;
        } else {
            // E-AC-3: the frames of six audio blocks (independent substream 0 counts the blocks; its dependent substreams and the other
            // independent substreams travel with it) go into one burst; Pd is the length in bytes
            if (f.frameType != 1 && f.substreamId == 0 && accBlocks_ >= 6) {
                IecBurst b;
                if (pack(acc_.data(), acc_.size(), 0x15, (int)acc_.size(), 6144, b)) out.push_back(std::move(b)); else dropped_++;
                acc_.clear(); accBlocks_ = 0;
            }
            acc_.insert(acc_.end(), p + i, p + i + f.frameBytes);
            if (f.frameType != 1 && f.substreamId == 0) accBlocks_ += f.blocks;
        }
        i += (size_t)f.frameBytes;
    }
}

} // namespace dect2
