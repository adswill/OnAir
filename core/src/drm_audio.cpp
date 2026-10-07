// DRM audio: xHE-AAC super frames and the glue to libavcodec's USAC decoder (see drm_audio.h).
#include "dect2/drm_audio.h"
#include "dect2/drm_aac.h"
#include "dect2/drm_fec.h"
#include "dect2/exact_resampler.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace dect2 { namespace drm {

namespace {

struct BitIn {
    const uint8_t* p; size_t nbits; size_t pos = 0; bool bad = false;
    BitIn(const uint8_t* d, size_t bytes) : p(d), nbits(bytes * 8) {}
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if (pos >= nbits) { bad = true; v <<= 1; continue; }
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1u);
            pos++;
        }
        return v;
    }
    uint32_t escaped(int n1, int n2, int n3) {                  // escapedValue() of ISO/IEC 23003-3, Table 8C
        uint32_t v = get(n1);
        if (v == (1u << n1) - 1) { const uint32_t a = get(n2); v += a; if (a == (1u << n2) - 1) v += get(n3); }
        return v;
    }
};
struct BitOut {
    std::vector<uint8_t> b; int n = 0;
    void put(uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; i--) {
            if ((n & 7) == 0) b.push_back(0);
            if ((v >> i) & 1u) b.back() |= (uint8_t)(0x80 >> (n & 7));
            n++;
        }
    }
    void escaped(uint32_t v, int n1, int n2, int n3) {
        const uint32_t m1 = (1u << n1) - 1, m2 = (1u << n2) - 1;
        if (v < m1) { put(v, n1); return; }
        put(m1, n1); v -= m1;
        if (v < m2) { put(v, n2); return; }
        put(m2, n2); put(v - m2, n3);
    }
};

// Table 71 of ISO/IEC 23003-3: usacSamplingFrequencyIndex (5 bits), -1 = no entry
int usacRateIndex(int hz) {
    switch (hz) {
    case 96000: return 0; case 88200: return 1; case 64000: return 2; case 48000: return 3; case 44100: return 4; case 32000: return 5; case 24000: return 6;
    case 22050: return 7; case 16000: return 8; case 12000: return 9; case 11025: return 10; case 8000: return 11; case 7350: return 12;
    case 57600: return 15; case 51200: return 16; case 40000: return 17; case 38400: return 18; case 34150: return 19; case 28800: return 20; case 25600: return 21;
    case 20000: return 22; case 19200: return 23; case 17075: return 24; case 14400: return 25; case 12800: return 26; case 9600: return 27;
    default: return -1;
    }
}
// MPEG-4 audio samplingFrequencyIndex (4 bits), -1 = none (use the escape)
int asc4RateIndex(int hz) {
    static const int t[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; i++) if (t[i] == hz) return i;
    return -1;
}

} // namespace

// Clause 5.3.2, Tables 4 to 8: the DRM static config is a UsacConfig without the elements that DRM fixes; the output is the full UsacConfig of
// ISO/IEC 23003-3 inside an AudioSpecificConfig (audio object type 42), which is what the decoder expects.
std::vector<uint8_t> xheAscFromSdc(const SdcAudio& a) {
    if (a.coding != 3 || a.config.empty()) return {};
    const int rate = a.rateHz();
    const bool stereo = a.mode == 2;
    if (!rate || (a.mode != 0 && a.mode != 2)) return {};
    BitIn in(a.config.data(), a.config.size());
    const int coreIdx = (int)in.get(2) + 1;                         // coreSbrFrameLengthIndexDrm + 1
    const int sbrRatioIndex = coreIdx == 2 ? 2 : coreIdx == 3 ? 3 : coreIdx == 4 ? 1 : 0;
    BitOut out;
    // AudioSpecificConfig
    out.put(31, 5); out.put(42 - 32, 6);
    const int r4 = asc4RateIndex(rate);
    if (r4 >= 0) out.put((uint32_t)r4, 4); else { out.put(15, 4); out.put((uint32_t)rate, 24); }
    out.put(stereo ? 2 : 1, 4);
    // UsacConfig
    const int r5 = usacRateIndex(rate);
    if (r5 >= 0) out.put((uint32_t)r5, 5); else { out.put(31, 5); out.put((uint32_t)rate, 24); }
    out.put((uint32_t)coreIdx, 3);
    out.put(stereo ? 2 : 1, 5);
    // the element
    const uint32_t noiseFilling = in.get(1);
    struct Ext { uint32_t type, cfgLen; bool defLenPresent; uint32_t defLen; bool frag; std::vector<uint8_t> cfg; };
    std::vector<uint8_t> sbrBits;                                  // UsacSbrConfig as it goes to the output
    BitOut sbr;
    int stereoConfigIndex = 0;
    BitOut mps;
    if (sbrRatioIndex > 0) {
        const uint32_t harmonic = in.get(1), interTes = in.get(1), pvc = in.get(1);
        sbr.put(harmonic, 1); sbr.put(interTes, 1); sbr.put(pvc, 1);
        sbr.put(in.get(4), 4); sbr.put(in.get(4), 4);              // dflt_start_freq, dflt_stop_freq
        const uint32_t e1 = in.get(1), e2 = in.get(1);
        sbr.put(e1, 1); sbr.put(e2, 1);
        if (e1) { sbr.put(in.get(2), 2); sbr.put(in.get(1), 1); sbr.put(in.get(2), 2); }
        if (e2) { sbr.put(in.get(2), 2); sbr.put(in.get(2), 2); sbr.put(in.get(1), 1); sbr.put(in.get(1), 1); }
        if (stereo) {
            stereoConfigIndex = (int)in.get(2);
            if (stereoConfigIndex > 0) {                           // xHEAACMps212Config -> Mps212Config of ISO/IEC 23003-3
                mps.put(in.get(3), 3);                             // bsFreqRes
                mps.put(in.get(3), 3);                             // bsFixedGainDMX
                const uint32_t tsd = in.get(1);                    // bsTempShapeConfigDrm: 1 = TSD (3 in the ISO field)
                mps.put(tsd ? 3 : 0, 2);
                mps.put(0, 2);                                     // bsDecorrConfig is 0 in DRM
                mps.put(in.get(1), 1);                             // bsHighRateMode
                mps.put(in.get(1), 1);                             // bsPhaseCoding
                const uint32_t present = in.get(1);
                mps.put(present, 1);
                if (present) mps.put(in.get(5), 5);                // bsOttBandsPhase
                if (stereoConfigIndex >= 2) { mps.put(in.get(5), 5); mps.put(in.get(1), 1); }   // bsResidualBands, bsPseudoLr
            }
        }
    }
    const uint32_t numExt = in.escaped(2, 4, 8);
    std::vector<Ext> ext;
    for (uint32_t i = 0; i < numExt && !in.bad && i < 8; i++) {
        Ext e;
        e.type = in.escaped(4, 8, 16);
        e.cfgLen = in.escaped(4, 8, 16);
        e.defLenPresent = in.get(1) != 0;
        e.defLen = e.defLenPresent ? in.escaped(8, 16, 0) : 0;
        e.frag = in.get(1) != 0;
        for (uint32_t k = 0; k < e.cfgLen && !in.bad; k++) e.cfg.push_back((uint8_t)in.get(8));
        ext.push_back(e);
    }
    if (in.bad) return {};
    // UsacDecoderConfig: numElements - 1 as escapedValue(4, 8, 16)
    out.escaped(numExt, 4, 8, 16);
    out.put(stereo ? 1 : 0, 2);                                     // usacElementType: SCE 0, CPE 1
    out.put(0, 1); out.put(noiseFilling, 1);                        // tw_mdct, noiseFilling
    for (int i = 0; i < sbr.n; i++) out.put((sbr.b[(size_t)i / 8] >> (7 - i % 8)) & 1, 1);
    if (stereo && sbrRatioIndex > 0) {
        out.put((uint32_t)stereoConfigIndex, 2);
        for (int i = 0; i < mps.n; i++) out.put((mps.b[(size_t)i / 8] >> (7 - i % 8)) & 1, 1);
    }
    for (const Ext& e : ext) {
        out.put(2, 2);                                              // ID_USAC_EXT
        out.escaped(e.type, 4, 8, 16);
        out.escaped(e.cfgLen, 4, 8, 16);
        out.put(e.defLenPresent ? 1 : 0, 1);
        if (e.defLenPresent) out.escaped(e.defLen, 8, 16, 0);
        out.put(e.frag ? 1 : 0, 1);
        for (uint8_t c : e.cfg) out.put(c, 8);
    }
    out.put(0, 1);                                                  // usacConfigExtensionPresent (the loudness metadata of DRM is not needed)
    return out.b;
}

// ---------------------------------------------------------------- xHE-AAC audio super frame (clause 5.3.1)

void XheSuperFrameParser::reset() { buf_.clear(); inFrame_ = false; tail_.clear(); }

bool XheSuperFrameParser::parse(const uint8_t* d, int len, std::vector<XheFrame>& frames) {
    frames.clear();
    if (len < 2) { reset(); return false; }
    // header: frame border count (4 bits), bit reservoir level (4 bits), CRC-8 over those 8 bits
    if (crc8Bytes(d, 1) != d[1]) { reset(); return false; }
    const int b = d[0] >> 4;
    lastBorders_ = b;
    lastReservoir_ = d[0] & 15;
    const int payload = len - 2 - 2 * b;
    if (payload < 0) { reset(); return false; }
    // directory: the last element describes the first border
    std::vector<int> border;                                       // in the coordinates of `comb` below
    std::vector<int> rawIdx;
    for (int k = 0; k < b; k++) {
        const int off = len - 2 * (k + 1);
        const int e = (d[off] << 8) | d[off + 1];
        rawIdx.push_back(e >> 4);
    }
    // combined bytes: the frame in progress (or the last 2 bytes of the previous payload) followed by this payload
    std::vector<uint8_t> comb;
    int base;
    if (inFrame_) { comb = buf_; base = (int)buf_.size(); }
    else { comb = tail_; base = (int)tail_.size(); }
    comb.insert(comb.end(), d + 2, d + 2 + payload);
    int prev = -1;
    for (int v : rawIdx) {
        int pos;
        if (v == 0xFFF) pos = base - 1;
        else if (v == 0xFFE) pos = base - 2;
        else pos = base + v;
        if (pos < 0) continue;                                      // a delayed border into bytes we never had (the stream just started)
        if (pos > (int)comb.size() || pos <= prev) { reset(); return false; }
        border.push_back(pos); prev = pos;
    }
    if (payload >= 2) tail_.assign(d + 2 + payload - 2, d + 2 + payload); else { tail_ = comb.size() >= 2 ? std::vector<uint8_t>(comb.end() - 2, comb.end()) : comb; }
    auto emit = [&](int from, int to) {
        if (to - from < 3) return;
        XheFrame f;
        f.au.assign(comb.begin() + from, comb.begin() + to - 2);
        const uint32_t crc = crc16Bytes(f.au.data(), f.au.size());
        f.crcOk = crc == (uint32_t)((comb[(size_t)to - 2] << 8) | comb[(size_t)to - 1]);
        frames.push_back(std::move(f));
    };
    if (border.empty()) {
        if (inFrame_) { buf_ = comb; if (buf_.size() > 8192) { buf_.clear(); inFrame_ = false; } }
        return true;
    }
    if (inFrame_) emit(0, border[0]);
    for (size_t i = 0; i + 1 < border.size(); i++) emit(border[i], border[i + 1]);
    buf_.assign(comb.begin() + border.back(), comb.end());
    inFrame_ = true;
    return true;
}

// ---------------------------------------------------------------- the decoder

struct AudioDecoder::Impl {
    SdcAudio cfg;
    bool modeE = false, configured = false, open = false;
    std::string info;
    int state = 0;
    uint64_t ok = 0, bad = 0, samples = 0;
    // xHE-AAC
    XheSuperFrameParser xhe;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    bool started = false;                  // the decoder has seen an independent frame since it was (re)opened
    int decFails = 0;                      // frames in a row that the decoder refused
    std::string lastErr, baseInfo;
    // AAC
    AacCoreDecoder aac;
    bool aacOk = false;
    std::vector<float> core;
    int outRate = 0, outCh = 0;
    ExactResampler rs;
    bool rsReady = false;
    std::vector<cf32> rin, rout;

    ~Impl() { closeCodec(); }
    void closeCodec() {
        if (ctx) avcodec_free_context(&ctx);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        open = false; started = false; rsReady = false;
    }

    bool openXhe() {
        closeCodec();
        const std::vector<uint8_t> asc = xheAscFromSdc(cfg);
        if (asc.empty()) { info = "xHE-AAC: the configuration in the SDC cannot be read"; return false; }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) { info = "xHE-AAC: no AAC decoder in this build"; return false; }
        ctx = avcodec_alloc_context3(codec);
        ctx->log_level_offset = 24;               // the decoder's complaints about a stream it cannot read would fill the log
        ctx->extradata = (uint8_t*)av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(ctx->extradata, asc.data(), asc.size());
        ctx->extradata_size = (int)asc.size();
        const int r = avcodec_open2(ctx, codec, nullptr);
        if (r < 0) {
            closeCodec();
            char b[160];
            snprintf(b, sizeof b, "xHE-AAC, %.1f kHz %s: this FFmpeg cannot decode the stream (it has no harmonic SBR, PVC, inter-TES or MPS212)", cfg.rateHz() / 1000.0, cfg.mode == 2 ? "stereo" : "mono");
            info = b;
            return false;
        }
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        open = true;
        char b[120];
        snprintf(b, sizeof b, "xHE-AAC, %.1f kHz %s", cfg.rateHz() / 1000.0, cfg.mode == 2 ? "stereo" : "mono");
        info = b; baseInfo = b;
        return true;
    }

    // decoded samples -> 48 kHz stereo
    void emitFrame(std::vector<float>& out) {
        const int n = frame->nb_samples, ch = frame->ch_layout.nb_channels;
        const int rate = frame->sample_rate > 0 ? frame->sample_rate : ctx->sample_rate;
        if (!rsReady || rate != outRate) { outRate = rate; rsReady = rs.configure((double)rate, 48000.0); rs.reset(); }
        rin.resize((size_t)n);
        auto sample = [&](int c, int i) -> float {
            c = std::min(c, ch - 1);
            switch (frame->format) {
            case AV_SAMPLE_FMT_FLTP: return ((const float*)frame->data[c])[i];
            case AV_SAMPLE_FMT_FLT: return ((const float*)frame->data[0])[i * ch + c];
            case AV_SAMPLE_FMT_S16P: return ((const int16_t*)frame->data[c])[i] / 32768.f;
            case AV_SAMPLE_FMT_S16: return ((const int16_t*)frame->data[0])[i * ch + c] / 32768.f;
            case AV_SAMPLE_FMT_S32P: return (float)(((const int32_t*)frame->data[c])[i] / 2147483648.0);
            case AV_SAMPLE_FMT_S32: return (float)(((const int32_t*)frame->data[0])[i * ch + c] / 2147483648.0);
            default: return 0.f;
            }
        };
        for (int i = 0; i < n; i++) rin[(size_t)i] = cf32(sample(0, i), sample(1, i));
        rout.clear();
        if (rsReady) rs.process(rin.data(), rin.size(), rout);
        const size_t o = out.size();
        out.resize(o + rout.size() * 2);
        for (size_t i = 0; i < rout.size(); i++) { out[o + 2 * i] = rout[i].real(); out[o + 2 * i + 1] = rout[i].imag(); }
        samples += rout.size();
    }

    bool decodeAu(const std::vector<uint8_t>& au, std::vector<float>& out) {
        if (!open) return false;
        if (!started) {
            if (au.empty() || !(au[0] & 0x80)) return false;       // usacIndependencyFlag: the first frame the decoder can start with
            started = true;
        }
        av_new_packet(pkt, (int)au.size());
        std::memcpy(pkt->data, au.data(), au.size());
        const int r = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (r < 0) { decFails++; char eb[100]; av_strerror(r, eb, sizeof eb); lastErr = eb; return false; }
        while (avcodec_receive_frame(ctx, frame) == 0) { emitFrame(out); av_frame_unref(frame); }
        decFails = 0;
        return true;
    }

    void superFrameXhe(const uint8_t* d, int len, std::vector<float>& out) {
        std::vector<XheFrame> frames;
        if (!xhe.parse(d, len, frames)) { bad++; if (open && started) { avcodec_flush_buffers(ctx); started = false; } return; }
        const uint64_t before = samples;
        for (const XheFrame& f : frames) {
            if (!f.crcOk) { bad++; if (open && started) { avcodec_flush_buffers(ctx); started = false; } continue; }
            ok++;
            decodeAu(f.au, out);
        }
        // the frames are good (CRC); whether sound comes out depends on the decoder
        if (samples > before) { state = 2; info = baseInfo; }
        else if (decFails >= 3) { state = 1; info = baseInfo + ": the frames are fine but FFmpeg's xHE-AAC decoder refuses this stream (" + lastErr + "; it has no MPS212 and no eSBR tools)"; }
        else if (state != 2) state = 1;
    }

    bool openAac() {
        closeCodec();
        std::string why;
        const int rate = cfg.rateHz();
        aacOk = aac.configure(rate, cfg.mode == 2, &why);
        char b[200];
        const char* what = cfg.sbr && cfg.mode == 1 ? "AAC + SBR + PS" : cfg.sbr ? "AAC + SBR" : "AAC";
        if (!aacOk) {
            snprintf(b, sizeof b, "%s, %.0f kHz: %s", what, rate / 1000.0, why.c_str());
            info = b;
            return false;
        }
        if (cfg.sbr) snprintf(b, sizeof b, "%s, %.0f kHz core: the core plays, SBR%s is not decoded (FFmpeg has no SBR for 960 frames)", what, rate / 1000.0, cfg.mode == 1 ? " and PS" : "");
        else snprintf(b, sizeof b, "%s, %.0f kHz mono", what, rate / 1000.0);
        info = b; baseInfo = b;
        rsReady = rs.configure((double)rate, 48000.0); rs.reset(); outRate = rate;
        return true;
    }

    void emitCore(std::vector<float>& out) {
        rin.resize(core.size());
        for (size_t i = 0; i < core.size(); i++) rin[i] = cf32(core[i], core[i]);
        rout.clear();
        if (rsReady) rs.process(rin.data(), rin.size(), rout);
        const size_t o = out.size();
        out.resize(o + rout.size() * 2);
        for (size_t i = 0; i < rout.size(); i++) { out[o + 2 * i] = rout[i].real(); out[o + 2 * i + 1] = rout[i].imag(); }
        samples += rout.size();
        core.clear();
    }

    void superFrameAac(const uint8_t* d, int len, int lenA, std::vector<float>& out) {
        const int n = aacNumFrames(modeE, cfg.rateHz());
        AacSuperFrame sf;
        if (!n) return;
        if (!aacSuperFrameParse(d, len, lenA, n, sf)) {
            bad += (uint64_t)n;
            if (aacOk) { core.assign((size_t)n * 960, 0.f); emitCore(out); aac.reset(); }
            return;
        }
        for (int i = 0; i < n; i++) {
            const auto& f = sf.frames[(size_t)i];
            const AacFrameStatus st = aacOk ? aac.frame(f.data(), (int)f.size(), sf.crc[(size_t)i], core) : AacFrameStatus::kDamaged;
            if (st == AacFrameStatus::kDamaged) bad++; else ok++;
            if (aacOk) emitCore(out);
        }
        state = aacOk ? 2 : 1;
    }
};

AudioDecoder::AudioDecoder() : p_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::configure(const SdcAudio& a, bool modeE) {
    Impl& d = *p_;
    d.cfg = a; d.modeE = modeE; d.configured = true;
    d.xhe.reset();
    d.state = 1; d.ok = d.bad = 0;
    d.closeCodec();
    if (a.coding == 3) return d.openXhe();
    if (a.coding == 0) return d.openAac();
    d.info = "audio coding not supported";
    return false;
}
bool AudioDecoder::configured() const { return p_->configured; }
bool AudioDecoder::decodable() const { return p_->open; }
void AudioDecoder::reset() {
    Impl& d = *p_;
    d.xhe.reset();
    if (d.open && d.started) avcodec_flush_buffers(d.ctx);
    d.started = false;
    if (d.aacOk) d.aac.reset();
}
void AudioDecoder::superFrame(const uint8_t* data, int len, int lenA, std::vector<float>& out) {
    Impl& d = *p_;
    if (!d.configured) return;
    if (d.cfg.coding == 3) d.superFrameXhe(data, len, out);
    else if (d.cfg.coding == 0) d.superFrameAac(data, len, lenA, out);
}
std::string AudioDecoder::info() const { return p_->info; }
int AudioDecoder::state() const { return p_->state; }
uint64_t AudioDecoder::framesOk() const { return p_->ok; }
uint64_t AudioDecoder::framesBad() const { return p_->bad; }
uint64_t AudioDecoder::samples() const { return p_->samples; }

}} // namespace dect2::drm
