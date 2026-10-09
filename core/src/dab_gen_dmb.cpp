// The DMB video service of the DAB test transmitter (see dab_gen.h): the test picture and its H.264 writer, AAC-LC sound from libavcodec,
// MPEG-4 Systems in an MPEG-2 transport stream (ETSI TS 102 428) and the outer code of TS 102 427 (RS(204,188) and the Forney interleaver).
#include "dect2/dab_gen.h"
#include "dect2/dvbt.h"
#include "dect2/ts.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <vector>

namespace dect2 {
namespace dabgen {

TxService dmbService() {
    TxService s;
    s.sid = 0xE0CEA001; s.label = "OnAir TV"; s.subId = 5; s.bitrate = 408; s.dmb = true; s.dabPlus = false;
    return s;
}

namespace dmb {
namespace {

int64_t floorDiv(int64_t a, int64_t b) { const int64_t q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }
constexpr uint64_t kMask33 = (1ull << 33) - 1;
constexpr int64_t kLoop90k = (int64_t)kLoopSeconds * 90000;
constexpr int64_t kFrame90k = 90000 * kLoopSeconds / kFrames;              // 7200: 12.5 pictures per second
constexpr int64_t kAudio90k = 90000 * 1024 / kAudioRate;                   // 2880
constexpr int64_t kDelay90k = 63000;                                       // composition time = release + 0.7 s

// ---------------------------------------------------------------- bits
struct Bits {
    std::vector<uint8_t> v;
    unsigned acc = 0;
    int n = 0;
    void bit(unsigned b) { acc = (acc << 1) | (b & 1); if (++n == 8) { v.push_back((uint8_t)acc); acc = 0; n = 0; } }
    void u(int bits, uint64_t x) { for (int i = bits - 1; i >= 0; i--) bit((unsigned)(x >> i) & 1); }
    void ue(uint32_t x) { const uint64_t y = (uint64_t)x + 1; int len = 0; while ((y >> len) > 1) len++; u(len, 0); u(len + 1, y); }   // Exp-Golomb
    void se(int x) { ue(x > 0 ? (uint32_t)(2 * x - 1) : (uint32_t)(-2 * x)); }
    void align() { while (n) bit(0); }
    void trailing() { bit(1); align(); }   // rbsp_trailing_bits
};

// a NAL unit with its start code; emulation prevention bytes where the payload would look like a start code
void nal(std::vector<uint8_t>& out, int refIdc, int type, const std::vector<uint8_t>& rbsp) {
    const uint8_t h[5] = {0, 0, 0, 1, (uint8_t)((refIdc << 5) | type)};
    out.insert(out.end(), h, h + 5);
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) { out.push_back(3); zeros = 0; }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

// ---------------------------------------------------------------- the picture
struct Yuv { uint8_t y, cb, cr; };
Yuv rgbToYuv(int r, int g, int b) {   // BT.601, video range
    const double y = 16 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0;
    const double cb = 128 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255.0;
    const double cr = 128 + (112.0 * r - 93.786 * g - 18.214 * b) / 255.0;
    return {(uint8_t)std::lround(y), (uint8_t)std::lround(cb), (uint8_t)std::lround(cr)};
}

int squareX(int64_t n) {   // left edge of the square: there and back in 75 pictures (6 s), even, 0 .. 160
    const int ph = (int)(((n % 75) + 75) % 75);
    const double t = ph / 75.0, tri = t < 0.5 ? 2 * t : 2 - 2 * t;
    return 2 * (int)std::lround(tri * (kWidth - 16) / 2);
}
constexpr int kSquareY = 64;   // the square fills macroblock row 4

// ---------------------------------------------------------------- MPEG-4 Systems descriptors (ISO/IEC 14496-1)
void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x >> 16); put16(v, x & 0xFFFF); }
std::vector<uint8_t> descr(int tag, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> d{(uint8_t)tag};
    const size_t n = body.size();
    if (n < 0x80) d.push_back((uint8_t)n);
    else { d.push_back((uint8_t)(0x80 | (n >> 7))); d.push_back((uint8_t)(n & 0x7F)); }   // the expandable size (up to 16383 here)
    d.insert(d.end(), body.begin(), body.end());
    return d;
}
// SLConfigDescriptor as TS 102 428 clause 5.2 and annex A want it: access unit start and end flags, time stamps of 33 bits at 90 kHz, idle flag
std::vector<uint8_t> slConfig(int instantBitrateLength) {
    std::vector<uint8_t> b{0x00, 0xC6};   // predefined 0; useAccessUnitStart/End, useTimeStamps, useIdle
    put32(b, 90000); put32(b, 90000);     // timeStampResolution, OCRResolution
    b.push_back(33); b.push_back(33);     // timeStampLength, OCRLength
    b.push_back(0);                       // AU_Length
    b.push_back((uint8_t)instantBitrateLength);
    put16(b, 0x0003);                     // degradationPriorityLength 0, AU_seqNumLength 0, packetSeqNumLength 0, reserved 11
    return descr(0x06, b);
}
std::vector<uint8_t> decConfig(int oti, int streamType, uint32_t bufferSize, uint32_t maxBr, uint32_t avgBr, const std::vector<uint8_t>& dsi) {
    std::vector<uint8_t> b{(uint8_t)oti, (uint8_t)((streamType << 2) | 1)};
    b.push_back((uint8_t)(bufferSize >> 16)); put16(b, bufferSize & 0xFFFF);
    put32(b, maxBr); put32(b, avgBr);
    if (!dsi.empty()) { const auto d = descr(0x05, dsi); b.insert(b.end(), d.begin(), d.end()); }
    return descr(0x04, b);
}
std::vector<uint8_t> esDescr(int esId, int ocrEsId, int prio, const std::vector<uint8_t>& dec, const std::vector<uint8_t>& sl) {
    std::vector<uint8_t> b;
    put16(b, (unsigned)esId);
    b.push_back((uint8_t)((ocrEsId >= 0 ? 0x20 : 0) | prio));   // no stream dependence, no URL, OCR stream flag, priority
    if (ocrEsId >= 0) put16(b, (unsigned)ocrEsId);
    b.insert(b.end(), dec.begin(), dec.end());
    b.insert(b.end(), sl.begin(), sl.end());
    return descr(0x03, b);
}

// SL packet header (14496-1 clause 10.2.4) for the configuration above: one SL packet is one whole access unit
std::vector<uint8_t> slHeader(int64_t cts, int64_t ocr, int instantBitrateLength, uint32_t instantBitrate) {
    Bits b;
    b.bit(1); b.bit(1);                   // accessUnitStartFlag, accessUnitEndFlag
    b.bit(ocr >= 0);                      // OCRflag (OCRLength > 0)
    b.bit(0);                             // idleFlag
    if (ocr >= 0) b.u(33, (uint64_t)ocr & kMask33);
    b.bit(0); b.bit(1);                   // decodingTimeStampFlag (no B pictures: DTS = CTS), compositionTimeStampFlag
    const bool ib = instantBitrateLength > 0 && ocr >= 0;   // the instant bit rate goes with the OCR (TS 102 428 clause 5.2 note 3)
    if (instantBitrateLength > 0) b.bit(ib);
    b.u(33, (uint64_t)cts & kMask33);
    if (ib) b.u(instantBitrateLength, instantBitrate);
    b.align();
    return b.v;
}

void putPts(std::vector<uint8_t>& v, int64_t t) {
    const uint64_t p = (uint64_t)t & kMask33;
    v.push_back((uint8_t)(0x21 | ((p >> 29) & 0x0E)));
    v.push_back((uint8_t)(p >> 22)); v.push_back((uint8_t)(((p >> 14) & 0xFE) | 1));
    v.push_back((uint8_t)(p >> 7)); v.push_back((uint8_t)(((p << 1) & 0xFE) | 1));
}
// PES packet of stream id 0xFA (14496-1 SL-packetized stream); the PTS only when the SL header carries an OCR (TS 102 428 table 6)
std::vector<uint8_t> slPes(const std::vector<uint8_t>& sl, const uint8_t* au, size_t n, int64_t pts) {
    std::vector<uint8_t> p{0x00, 0x00, 0x01, 0xFA, 0, 0, 0x84, (uint8_t)(pts >= 0 ? 0x80 : 0x00), (uint8_t)(pts >= 0 ? 5 : 0)};
    if (pts >= 0) putPts(p, pts);
    p.insert(p.end(), sl.begin(), sl.end());
    p.insert(p.end(), au, au + n);
    const size_t len = p.size() - 6;
    p[4] = (uint8_t)(len >> 8); p[5] = (uint8_t)len;   // all access units are far below 64 kB
    return p;
}

// ISO_IEC_14496_section (13818-1 clause 2.12) with one SL packet, behind a pointer field
std::vector<uint8_t> section14496(int tableId, int esId, const std::vector<uint8_t>& slPacket) {
    std::vector<uint8_t> s{(uint8_t)tableId, 0, 0};
    put16(s, (unsigned)esId);
    s.push_back(0xC1); s.push_back(0); s.push_back(0);   // version 0, current, section 0 of 0
    s.insert(s.end(), slPacket.begin(), slPacket.end());
    const size_t len = s.size() - 3 + 4;
    s[1] = (uint8_t)(0xF0 | (len >> 8)); s[2] = (uint8_t)len;   // section syntax, private indicator, reserved
    put32(s, mpegCrc32(s.data(), (int)s.size()));
    s.insert(s.begin(), 0x00);
    return s;
}
std::vector<uint8_t> psiSection(int tableId, int ext, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> s{(uint8_t)tableId, 0, 0};
    put16(s, (unsigned)ext);
    s.push_back(0xC1); s.push_back(0); s.push_back(0);
    s.insert(s.end(), body.begin(), body.end());
    const size_t len = s.size() - 3 + 4;
    s[1] = (uint8_t)(0xB0 | (len >> 8)); s[2] = (uint8_t)len;
    put32(s, mpegCrc32(s.data(), (int)s.size()));
    s.insert(s.begin(), 0x00);
    return s;
}

bool encodeAudio(std::vector<std::vector<uint8_t>>& aus, std::vector<uint8_t>& asc) {
    const AVCodec* c = avcodec_find_encoder_by_name("aac");
    if (!c) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(c);
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->sample_rate = kAudioRate;
    ctx->bit_rate = 48000;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;   // the AudioSpecificConfig, for the decoder configuration
    av_channel_layout_default(&ctx->ch_layout, 2);
    const int old = av_log_get_level();
    av_log_set_level(AV_LOG_ERROR);
    bool ok = avcodec_open2(ctx, c, nullptr) >= 0 && ctx->frame_size == 1024 && ctx->extradata_size >= 2;
    std::vector<std::vector<uint8_t>> out;
    if (ok) {
        AVFrame* fr = av_frame_alloc();
        fr->nb_samples = 1024; fr->format = AV_SAMPLE_FMT_FLTP; fr->sample_rate = kAudioRate;
        av_channel_layout_default(&fr->ch_layout, 2);
        av_frame_get_buffer(fr, 0);
        AVPacket* pk = av_packet_alloc();
        const int64_t loop = (int64_t)kAudioFrames * 1024;
        // two passes over the loop: the second one starts in the state the end of the loop leaves the encoder in
        for (int t = 0; t < 2 * kAudioFrames && ok; t++) {
            av_frame_make_writable(fr);
            for (int ch = 0; ch < 2; ch++) {
                float* d = (float*)fr->data[ch];
                const double hz = ch ? 500.0 : 1000.0;
                for (int i = 0; i < 1024; i++) {
                    const int64_t s = ((int64_t)t * 1024 + i) % loop;
                    d[i] = (float)(0.3 * std::sin(2 * M_PI * std::fmod(hz * (double)s, (double)kAudioRate) / kAudioRate));
                }
            }
            fr->pts = (int64_t)t * 1024;
            if (avcodec_send_frame(ctx, fr) < 0) { ok = false; break; }
            while (avcodec_receive_packet(ctx, pk) == 0) { out.emplace_back(pk->data, pk->data + pk->size); av_packet_unref(pk); }
        }
        av_packet_free(&pk);
        av_frame_free(&fr);
    }
    if (ok && (int)out.size() >= kAudioFrames + 2) {
        aus.assign(out.end() - kAudioFrames, out.end());
        asc.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
    } else ok = false;
    avcodec_free_context(&ctx);
    av_log_set_level(old);
    return ok;
}

} // namespace

void testPicture(int64_t n, std::vector<uint8_t>& y, std::vector<uint8_t>& cb, std::vector<uint8_t>& cr) {
    static const int bars[8][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191}, {191, 0, 0}, {0, 0, 191}, {0, 0, 0}};   // 75 % bars
    y.assign((size_t)kWidth * kHeight, 0);
    cb.assign((size_t)kWidth * kHeight / 4, 0);
    cr.assign((size_t)kWidth * kHeight / 4, 0);
    const int sx = squareX(n);
    const Yuv sq = rgbToYuv(255, 140, 0);   // an orange square, unlike every bar
    for (int r = 0; r < kHeight; r++)
        for (int c = 0; c < kWidth; c++) {
            const bool in = r >= kSquareY && r < kSquareY + 16 && c >= sx && c < sx + 16;
            const int* b = bars[c / (kWidth / 8)];
            const Yuv v = in ? sq : rgbToYuv(b[0], b[1], b[2]);
            y[(size_t)r * kWidth + (size_t)c] = v.y;
            if (!(r & 1) && !(c & 1)) { cb[(size_t)(r / 2) * (kWidth / 2) + (size_t)(c / 2)] = v.cb; cr[(size_t)(r / 2) * (kWidth / 2) + (size_t)(c / 2)] = v.cr; }
        }
}

std::vector<std::vector<uint8_t>> encodeTestVideo() {
    constexpr int mbw = kWidth / 16, mbh = kHeight / 16, cw = kWidth / 2;
    std::vector<uint8_t> sps, pps;
    {
        Bits s;
        s.u(8, 66); s.u(8, 0xC0); s.u(8, 13);          // Baseline (constraint sets 0 and 1), level 1.3
        s.ue(0);                                       // seq_parameter_set_id
        s.ue(0);                                       // log2_max_frame_num_minus4: frame_num of 4 bits
        s.ue(2);                                       // pic_order_cnt_type 2 (TS 102 428 clause 8.1.2.1): output order = decoding order
        s.ue(1);                                       // max_num_ref_frames
        s.u(1, 0);                                     // gaps_in_frame_num_value_allowed_flag
        s.ue(mbw - 1); s.ue(mbh - 1);
        s.u(1, 1); s.u(1, 1); s.u(1, 0);               // frame_mbs_only, direct_8x8_inference, no cropping
        s.u(1, 1);                                     // VUI: only the timing, 12.5 pictures per second
        s.u(1, 0); s.u(1, 0); s.u(1, 0); s.u(1, 0);
        s.u(1, 1); s.u(32, 4); s.u(32, 100); s.u(1, 1);
        s.u(1, 0); s.u(1, 0); s.u(1, 0); s.u(1, 0);
        s.trailing();
        sps = s.v;
        Bits p;
        p.ue(0); p.ue(0);                              // pic_parameter_set_id, seq_parameter_set_id
        p.u(1, 0); p.u(1, 0);                          // CAVLC, no bottom field POC
        p.ue(0);                                       // one slice group
        p.ue(0); p.ue(0);                              // num_ref_idx_l0/l1_default_active_minus1
        p.u(1, 0); p.u(2, 0);                          // no weighted prediction
        p.se(0); p.se(0); p.se(0);                     // pic_init_qp, pic_init_qs, chroma_qp_index_offset
        p.u(1, 1);                                     // deblocking_filter_control_present_flag: the slices switch the filter off
        p.u(1, 0); p.u(1, 0);                          // constrained_intra_pred, redundant_pic_cnt_present
        p.trailing();
        pps = p.v;
    }
    std::vector<std::vector<uint8_t>> aus;
    std::vector<uint8_t> Y, U, V, pY, pU, pV;
    int idrId = 0;
    for (int n = 0; n < kFrames; n++) {
        testPicture(n, Y, U, V);
        const bool idr = n % kGop == 0;
        std::vector<uint8_t> au;
        if (idr) { nal(au, 3, 7, sps); nal(au, 3, 8, pps); }
        Bits b;
        b.ue(0);                                       // first_mb_in_slice
        b.ue(idr ? 7 : 5);                             // I or P, all slices of the picture
        b.ue(0);                                       // pic_parameter_set_id
        b.u(4, (uint64_t)(n % kGop));                  // frame_num
        if (idr) b.ue((uint32_t)(idrId++ & 1));        // idr_pic_id (10 IDR pictures per loop: it alternates across the wrap too)
        if (!idr) { b.u(1, 0); b.u(1, 0); }            // num_ref_idx_active_override_flag, ref_pic_list_modification_flag_l0
        if (idr) { b.u(1, 0); b.u(1, 0); }             // no_output_of_prior_pics_flag, long_term_reference_flag
        else b.u(1, 0);                                // adaptive_ref_pic_marking_mode_flag: sliding window
        b.se(0);                                       // slice_qp_delta
        b.ue(1);                                       // disable_deblocking_filter_idc: off, the samples stay exactly as written
        auto same = [&](int mx, int my, bool vertical) {
            // vertical: the macroblock equals the bottom line of the one above repeated (Intra 16x16 / chroma vertical prediction)
            // otherwise: it equals the previous picture (P_Skip with a zero motion vector)
            for (int r = 0; r < 16; r++)
                for (int c = 0; c < 16; c++) {
                    const size_t i = (size_t)(my * 16 + r) * kWidth + (size_t)(mx * 16 + c);
                    const uint8_t ref = vertical ? Y[(size_t)(my * 16 - 1) * kWidth + (size_t)(mx * 16 + c)] : pY[i];
                    if (Y[i] != ref) return false;
                }
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++) {
                    const size_t i = (size_t)(my * 8 + r) * cw + (size_t)(mx * 8 + c);
                    const size_t j = vertical ? (size_t)(my * 8 - 1) * cw + (size_t)(mx * 8 + c) : i;
                    if (U[i] != (vertical ? U[j] : pU[j]) || V[i] != (vertical ? V[j] : pV[j])) return false;
                }
            return true;
        };
        std::vector<int> coeffs((size_t)(mbw * mbh), 0);   // total coefficients per 4x4 block of each macroblock (all blocks alike): 16 for I_PCM
        int skip = 0;
        for (int my = 0; my < mbh; my++)
            for (int mx = 0; mx < mbw; mx++) {
                const int i = my * mbw + mx;
                const bool pcm = idr ? (my == 0 || !same(mx, my, true)) : !same(mx, my, false);
                if (!idr) {
                    if (!pcm) { skip++; continue; }
                    b.ue((uint32_t)skip);              // mb_skip_run
                    skip = 0;
                }
                if (pcm) {
                    b.ue(idr ? 25 : 30);               // I_PCM (in a P slice after the 5 inter types)
                    b.align();                         // pcm_alignment_zero_bit
                    for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) b.u(8, Y[(size_t)(my * 16 + r) * kWidth + (size_t)(mx * 16 + c)]);
                    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) b.u(8, U[(size_t)(my * 8 + r) * cw + (size_t)(mx * 8 + c)]);
                    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) b.u(8, V[(size_t)(my * 8 + r) * cw + (size_t)(mx * 8 + c)]);
                    coeffs[(size_t)i] = 16;
                } else {
                    b.ue(1);                           // I_16x16_0_0_0: vertical prediction, no coded luma or chroma blocks
                    b.ue(2);                           // intra_chroma_pred_mode: vertical
                    b.se(0);                           // mb_qp_delta
                    // the Intra16x16DCLevel block without coefficients: coeff_token for TotalCoeff 0 from the table nC selects (clause 9.2.1)
                    const bool hA = mx > 0, hB = my > 0;
                    const int nA = hA ? coeffs[(size_t)(i - 1)] : 0, nB = hB ? coeffs[(size_t)(i - mbw)] : 0;
                    const int nC = hA && hB ? (nA + nB + 1) >> 1 : hA ? nA : hB ? nB : 0;
                    if (nC < 2) b.u(1, 1); else if (nC < 4) b.u(2, 3); else if (nC < 8) b.u(4, 15); else b.u(6, 3);
                    coeffs[(size_t)i] = 0;
                }
            }
        if (!idr && skip) b.ue((uint32_t)skip);
        b.trailing();
        nal(au, idr ? 3 : 2, idr ? 5 : 1, b.v);
        aus.push_back(std::move(au));
        pY.swap(Y); pU.swap(U); pV.swap(V);
    }
    return aus;
}

// ---------------------------------------------------------------- the service
struct Source::Impl {
    int bitrate = 0, frameBytes = 0, P = 0;   // P: transport stream packets per 12 s loop
    int64_t tick = 0;                         // 90 kHz ticks per packet
    bool ok = false, audio = false;
    std::vector<std::vector<uint8_t>> video, aac;
    std::vector<uint8_t> asc, pmt, odAu;
    std::map<int, int> perLoop;               // packets per loop of every PID (the continuity counters run on across the loops)
    std::map<int64_t, std::vector<uint8_t>> loops;            // built loops (P * 188 bytes)
    std::map<int64_t, std::array<uint8_t, 204>> rs;           // Reed-Solomon coded packets

    struct Unit { int pid; std::vector<uint8_t> data; size_t off = 0; int64_t order = 0; };

    void buildTables() {
        const std::vector<uint8_t> sl = slConfig(0);
        const std::vector<uint8_t> slA = slConfig(32);
        std::vector<uint8_t> iod;
        {
            std::vector<uint8_t> b;
            put16(b, (1u << 6) | 0x0F);                      // ObjectDescriptorID 1, no URL, no inline profiles, reserved
            b.push_back(0x01); b.push_back(0x0C);            // OD and scene profile levels (TS 102 428 table 1)
            b.push_back(0xFE); b.push_back(0xFE);            // audio and visual: not specified
            b.push_back(0x04);                               // graphics profile level
            const auto od = esDescr(kEsOd, -1, 0, decConfig(0x01, 0x01, 250, 0, 0, {}), sl);
            const auto bifs = esDescr(kEsBifs, -1, 0, decConfig(0x02, 0x03, 22, 0, 0, {}), sl);
            b.insert(b.end(), od.begin(), od.end());
            b.insert(b.end(), bifs.begin(), bifs.end());
            iod = descr(0x02, b);
        }
        {   // the object descriptors: the video (OD 20, ES 201, its clock from the audio stream) and the audio (OD 10, ES 101), annex A.2.2
            std::vector<uint8_t> v, a;
            put16(v, ((unsigned)kOdVideo << 6) | 0x1F);
            const auto ev = esDescr(kEsVideo, audio ? kEsAudio : -1, 4, decConfig(0x21, 0x04, 60000, 768000, 160000, {}), sl);   // parameter sets in the stream
            v.insert(v.end(), ev.begin(), ev.end());
            std::vector<uint8_t> cmds = descr(0x01, v);
            if (audio) {
                put16(a, ((unsigned)kOdAudio << 6) | 0x1F);
                const auto ea = esDescr(kEsAudio, -1, 5, decConfig(0x40, 0x05, 6144, 64000, 48000, asc), slA);
                a.insert(a.end(), ea.begin(), ea.end());
                const auto da = descr(0x01, a);
                cmds.insert(cmds.end(), da.begin(), da.end());
            }
            odAu = descr(0x01, cmds);                        // ObjectDescriptorUpdate
        }
        {   // PMT: the IOD descriptor, then BIFS and OD in sections, video and audio as SL-packetized PES, each with its SL descriptor (ES_ID)
            std::vector<uint8_t> b;
            put16(b, 0xE000 | (unsigned)(audio ? kPidAudio : kPidVideo));   // PCR PID
            std::vector<uint8_t> info{0x1D, (uint8_t)(iod.size() + 2), 0x10, 0x01};   // IOD_descriptor: scope 0x10 (this program), label 1
            info.insert(info.end(), iod.begin(), iod.end());
            put16(b, 0xF000 | (unsigned)info.size());
            b.insert(b.end(), info.begin(), info.end());
            auto es = [&](int type, int pid, int esId) {
                b.push_back((uint8_t)type); put16(b, 0xE000 | (unsigned)pid); put16(b, 0xF000 | 4);
                b.push_back(0x1E); b.push_back(2); put16(b, (unsigned)esId);   // SL_descriptor
            };
            es(0x13, kPidBifs, kEsBifs);
            es(0x13, kPidOd, kEsOd);
            es(0x12, kPidVideo, kEsVideo);
            if (audio) es(0x12, kPidAudio, kEsAudio);
            pmt = psiSection(0x02, 1, b);
        }
    }

    // one TS packet of a unit (PES or section); pcr >= 0 adds the PCR in the adaptation field
    void emit(Unit& u, int64_t pcr, std::map<int, int>& cc, uint8_t* o) {
        const bool start = u.off == 0;
        const size_t left = u.data.size() - u.off;
        const size_t af = pcr >= 0 ? 8 : 0;
        const size_t pay = std::min(left, 184 - af);
        const size_t afTotal = 184 - pay;                    // adaptation field incl. its length byte, stuffing included
        o[0] = 0x47; o[1] = (uint8_t)((start ? 0x40 : 0) | (u.pid >> 8)); o[2] = (uint8_t)u.pid;
        o[3] = (uint8_t)((afTotal ? 0x30 : 0x10) | (cc[u.pid] & 15));
        cc[u.pid]++;
        uint8_t* p = o + 4;
        if (afTotal) {
            p[0] = (uint8_t)(afTotal - 1);
            if (afTotal > 1) {
                std::memset(p + 1, 0xFF, afTotal - 1);
                p[1] = pcr >= 0 ? 0x10 : 0x00;
                if (pcr >= 0) {
                    const uint64_t base = (uint64_t)pcr & kMask33;
                    p[2] = (uint8_t)(base >> 25); p[3] = (uint8_t)(base >> 17); p[4] = (uint8_t)(base >> 9); p[5] = (uint8_t)(base >> 1);
                    p[6] = (uint8_t)(((base & 1) << 7) | 0x7E); p[7] = 0;
                }
            }
            p += afTotal;
        }
        std::memcpy(p, u.data.data() + u.off, pay);
        u.off += pay;
    }

    // The 12 s loop number L: the packets, time stamps and continuity counters as the endless stream has them there
    bool buildLoop(int64_t L, std::vector<uint8_t>& out, std::map<int, int>* counts) {
        const int64_t T0 = (int64_t)((uint64_t)(L * kLoop90k) & kMask33);   // the time stamps are modulo 2^33, never negative (the flags test >= 0)
        std::map<int, int> cc;
        if (!counts) for (const auto& kv : perLoop) cc[kv.first] = (int)(((int64_t)kv.second * L) & 15);
        out.assign((size_t)P * 188, 0);
        std::deque<Unit> psi, vq, aq;
        int nv = 0, na = 0;
        int64_t lastPcr = -1000;
        const int pcrPid = audio ? kPidAudio : kPidVideo;
        static const uint8_t kBifs[] = {0xC0, 0x10, 0x12, 0x81, 0x30, 0x2A, 0x05, 0x72, 0x61, 0x04, 0x88, 0x50, 0x45, 0x05, 0x3F, 0x00};   // annex A.3.2.2
        const int psiEvery = (int)(36000 / tick);           // 400 ms
        static const uint8_t pat[] = {0x00, 0x01, 0xE0 | (kPidPmt >> 8), kPidPmt & 0xFF};
        for (int k = 0; k < P; k++) {
            const int64_t t = (int64_t)k * tick;
            if (k % psiEvery == 0) {
                psi.push_back({0, psiSection(0x00, 1, std::vector<uint8_t>(pat, pat + 4))});
                psi.push_back({kPidPmt, pmt});
                psi.push_back({kPidOd, section14496(0x05, kEsOd, [&] { auto h = slHeader(T0 + t + kDelay90k, -1, 0, 0); h.insert(h.end(), odAu.begin(), odAu.end()); return h; }())});
                psi.push_back({kPidBifs, section14496(0x04, kEsBifs, [&] { auto h = slHeader(T0 + t + kDelay90k, -1, 0, 0); h.insert(h.end(), kBifs, kBifs + sizeof kBifs); return h; }())});
            }
            while (nv < kFrames && nv * kFrame90k <= t) {
                const int64_t cts = T0 + kDelay90k + nv * kFrame90k;
                const auto& a = video[(size_t)nv];
                vq.push_back({kPidVideo, slPes(slHeader(cts, -1, 0, 0), a.data(), a.size(), -1), 0, cts});
                nv++;
            }
            while (audio && na < kAudioFrames && na * kAudio90k <= t) {
                const int64_t cts = T0 + kDelay90k + na * kAudio90k;
                const int64_t ocr = na % 16 == 0 ? T0 + na * kAudio90k : -1;   // the object clock every 512 ms, from the audio (OCR_ES_ID of the video)
                const auto& a = aac[(size_t)na];
                aq.push_back({kPidAudio, slPes(slHeader(cts, ocr, 32, 48000), a.data(), a.size(), ocr >= 0 ? cts : -1), 0, cts});
                na++;
            }
            uint8_t* o = &out[(size_t)k * 188];
            const bool pcrDue = k - lastPcr >= (int64_t)(7200 / tick);   // 80 ms
            std::deque<Unit>* q = !psi.empty() ? &psi : (!vq.empty() && (aq.empty() || vq.front().order <= aq.front().order)) ? &vq : !aq.empty() ? &aq : nullptr;
            if (pcrDue && (!q || q->front().pid != pcrPid)) {
                // the PCR is due and the next packet is another PID's: a packet of only the adaptation field with the PCR
                o[0] = 0x47; o[1] = (uint8_t)(pcrPid >> 8); o[2] = (uint8_t)pcrPid; o[3] = (uint8_t)(0x20 | ((cc[pcrPid] - 1) & 15));   // no payload: the counter stays
                o[4] = 183; std::memset(o + 5, 0xFF, 183); o[5] = 0x10;
                const uint64_t base = (uint64_t)(T0 + t) & kMask33;
                o[6] = (uint8_t)(base >> 25); o[7] = (uint8_t)(base >> 17); o[8] = (uint8_t)(base >> 9); o[9] = (uint8_t)(base >> 1);
                o[10] = (uint8_t)(((base & 1) << 7) | 0x7E); o[11] = 0;
                lastPcr = k;
            } else if (q) {
                emit(q->front(), pcrDue ? T0 + t : -1, cc, o);
                if (pcrDue) lastPcr = k;
                if (q->front().off >= q->front().data.size()) q->pop_front();
            } else {   // a null packet
                o[0] = 0x47; o[1] = 0x1F; o[2] = 0xFF; o[3] = 0x10; std::memset(o + 4, 0xFF, 184);
            }
        }
        if (counts) *counts = cc;
        return psi.empty() && vq.empty() && aq.empty() && nv == kFrames && (!audio || na == kAudioFrames);
    }

    const std::vector<uint8_t>& loop(int64_t L) {
        auto it = loops.find(L);
        if (it != loops.end()) return it->second;
        if (loops.size() >= 3) loops.erase(loops.begin()->first < L ? loops.begin() : std::prev(loops.end()));
        std::vector<uint8_t>& v = loops[L];
        buildLoop(L, v, nullptr);
        return v;
    }
    void packet(int64_t k, uint8_t* out) {
        const int64_t L = floorDiv(k, P);
        std::memcpy(out, &loop(L)[(size_t)(k - L * P) * 188], 188);
    }
    uint8_t rsByte(int64_t i) {   // byte i of the Reed-Solomon coded stream
        const int64_t K = floorDiv(i, 204);
        auto it = rs.find(K);
        if (it == rs.end()) {
            if (rs.size() >= 96) rs.erase(rs.begin()->first < K ? rs.begin() : std::prev(rs.end()));
            uint8_t ts[188];
            packet(K, ts);
            it = rs.emplace(K, std::array<uint8_t, 204>{}).first;
            dvbt::rsEncode(ts, it->second.data());
        }
        return it->second[(size_t)(i - K * 204)];
    }
};

Source::Source(int bitrateKbps) : p_(new Impl) {
    Impl& I = *p_;
    I.bitrate = bitrateKbps;
    I.frameBytes = bitrateKbps * 3;
    const int64_t bytes = (int64_t)I.frameBytes * (kLoopSeconds * 1000 / 24);   // 500 logical frames in 12 s
    if (bitrateKbps <= 0 || bytes % 204 || kLoop90k % (bytes / 204)) return;     // whole packets in the loop, a whole number of ticks per packet
    I.P = (int)(bytes / 204);
    I.tick = kLoop90k / I.P;
    I.video = encodeTestVideo();
    I.audio = encodeAudio(I.aac, I.asc);
    I.buildTables();
    std::vector<uint8_t> tmp;
    I.ok = I.buildLoop(0, tmp, &I.perLoop);
}
Source::~Source() = default;
bool Source::ok() const { return p_->ok; }
int Source::packetsPerLoop() const { return p_->P; }
bool Source::hasAudio() const { return p_->audio; }
const std::vector<std::vector<uint8_t>>& Source::videoAccessUnits() const { return p_->video; }
const std::vector<std::vector<uint8_t>>& Source::audioAccessUnits() const { return p_->aac; }
const std::vector<uint8_t>& Source::audioSpecificConfig() const { return p_->asc; }
void Source::tsPacket(int64_t k, uint8_t out[188]) { p_->packet(k, out); }

void Source::logicalFrame(int64_t f, std::vector<uint8_t>& out) {
    // the outer interleaver as a pure function: output byte o comes from branch j = o mod 12, which delays by j * 17 cells of 12 bytes;
    // the sync bytes (every 204th byte, branch 0) pass undelayed
    Impl& I = *p_;
    out.resize((size_t)I.frameBytes);
    for (int j = 0; j < I.frameBytes; j++) {
        const int64_t o = f * I.frameBytes + j;
        const int64_t br = ((o % 12) + 12) % 12;
        out[(size_t)j] = I.rsByte(o - br * 204);
    }
}

} // namespace dmb
} // namespace dabgen
} // namespace dect2
