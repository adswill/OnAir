// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmit payload: turn the operator's own strings (callsign, text, a file) into a ModeSynth. Empty fields keep the mode's demo signal.
#include "dect2/tx/tx.h"
#include "dect2/acars/acars_gen.h"
#include "dect2/adsb/adsb_encode.h"
#include "dect2/adsb/adsb_gen.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/atv/atv_gen.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/drm/drm_gen.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/ale/ale_gen.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/cw/cw_gen.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/dect/dect_gen.h"
#include "dect2/dect/dect_rx.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_tx.h"
#include "dect2/dpmr/dpmr_gen.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dstar/dstar_gen.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_hamdrm.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/js8/js8_gen.h"
#include "dect2/js8/js8_proto.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/mesh/mesh_gen.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/mesh/mesh_tx.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/m17/m17_gen.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/marine/marine_gen.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/same/same_gen.h"
#include "dect2/same/same_rx.h"
#include "dect2/sonde/sonde_gen.h"
#include "dect2/vdl2/vdl2_gen.h"
#include "dect2/vdl2/vdl2_proto.h"
#include "dect2/vor/vor_gen.h"
#include "dect2/vor/vor_rx.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "dect2/common/str.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

std::unique_ptr<ModeSynth> makeTxVoiceSynth(const TxModeSpec& m, const TxRequest& r, double rate);
std::unique_ptr<ModeSynth> makeTxMixSynth(std::unique_ptr<ModeSynth> a, std::unique_ptr<ModeSynth> b, double rate);
void txPullPcm8k(const TxRequest& r, int16_t* pcm, int n);

namespace {
using dect2::math::kPi;

std::string upper(std::string s) { return dect2::str::upper(std::move(s)); }

// "CALL-SSID" as typed ("wide2-1", " A61HS-9 ") -> an AX.25 address: spaces dropped, capitals, at most six characters, SSID 0..15;
// a trailing '*' (as in a received path) is dropped too
ax25::Address ax25Addr(const std::string& in) {
    std::string s;
    for (char c : in) if (c != ' ' && c != '\t' && c != '*') s += c;
    ax25::Address a;
    const size_t d = s.find('-');
    a.call = upper(d == std::string::npos ? s : s.substr(0, d));
    if (a.call.size() > 6) a.call.resize(6);
    a.ssid = d == std::string::npos ? 0 : std::max(0, std::min(15, atoi(s.c_str() + (int)d + 1)));
    return a;
}

template<class G>
class WrapGen : public ModeSynth {
public:
    WrapGen(std::unique_ptr<G> g, double rate) : g_(std::move(g)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { g_->generate(out, n); }
private:
    std::unique_ptr<G> g_;
    double rate_;
};

class LoopIq : public ModeSynth {
public:
    LoopIq(std::vector<cf32> b, double rate) : b_(std::move(b)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        if (b_.empty()) { for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f); return; }
        for (size_t i = 0; i < n; i++) { out[i] = b_[pos_]; if (++pos_ >= b_.size()) pos_ = 0; }
    }
private:
    std::vector<cf32> b_;
    double rate_;
    size_t pos_ = 0;
};

class AnalogTone : public ModeSynth {
public:
    AnalogTone(double rate, double offHz, int kind, double toneHz, const std::vector<float>& wav)
        : rate_(rate), off_(offHz), kind_(kind), tone_(toneHz > 0 ? toneHz : 1000), wav_(wav) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double w0 = 2 * kPi * off_ / rate_;
        const double wt = 2 * kPi * tone_ / rate_;
        const double dev = kind_ == 0 ? 5000.0 : (kind_ == 0 && false ? 75000.0 : 5000.0);
        for (size_t i = 0; i < n; i++) {
            float a = 0.5f * (float)std::sin(tph_);
            if (!wav_.empty()) {
                const size_t k = (size_t)wpos_;
                a = wav_[k % wav_.size()];
                wpos_ += wavRate_ / rate_;
                if (wpos_ >= (double)wav_.size()) wpos_ -= (double)wav_.size();
            } else tph_ += wt;
            if (kind_ == 1) {   // AM
                const float m = 0.7f * (1.f + 0.8f * a);
                out[i] = cf32(m * (float)std::cos(cph_), m * (float)std::sin(cph_));
                cph_ += w0;
            } else if (kind_ == 2 || kind_ == 3) {   // USB / LSB: a tone at ±tone Hz from the dial
                const double f = off_ + (kind_ == 3 ? -tone_ : tone_);
                cph_ += 2 * kPi * f / rate_;
                const float m = 0.35f * (wav_.empty() ? 1.f : std::fabs(a) * 2.f);
                out[i] = cf32(m * (float)std::cos(cph_), m * (float)std::sin(cph_));
            } else {   // FM
                cph_ += w0 + 2 * kPi * (dev * a) / rate_;
                out[i] = cf32(0.35f * (float)std::cos(cph_), 0.35f * (float)std::sin(cph_));
            }
            if (cph_ > kPi) cph_ -= 2 * kPi; else if (cph_ < -kPi) cph_ += 2 * kPi;
        }
    }
    void setWavRate(double r) { wavRate_ = r > 0 ? r : 8000; }
private:
    double rate_, off_, tone_, wavRate_ = 8000, wpos_ = 0, cph_ = 0, tph_ = 0;
    int kind_ = 0;
    std::vector<float> wav_;
};

bool loadWav(const std::string& path, std::vector<float>& pcm, double& rate) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) { fclose(f); return false; }
    int16_t ch = 1, bits = 16;
    int32_t sr = 8000;
    for (;;) {
        char id[4];
        uint32_t n = 0;
        if (fread(id, 1, 4, f) != 4 || fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
        if (!memcmp(id, "fmt ", 4)) {
            uint8_t fmt[16] = {};
            if (n < 16 || fread(fmt, 1, 16, f) != 16) { fclose(f); return false; }
            if (n > 16) fseek(f, (long)n - 16, SEEK_CUR);
            memcpy(&ch, fmt + 2, 2);
            memcpy(&sr, fmt + 4, 4);
            memcpy(&bits, fmt + 14, 2);
        } else if (!memcmp(id, "data", 4)) {
            if (bits != 16) { fclose(f); return false; }
            std::vector<int16_t> raw(n / 2);
            if (fread(raw.data(), 2, raw.size(), f) != raw.size()) { fclose(f); return false; }
            fclose(f);
            rate = sr;
            pcm.resize(raw.size() / std::max(1, (int)ch));
            for (size_t i = 0; i < pcm.size(); i++) pcm[i] = raw[i * (size_t)ch] / 32768.f;
            return !pcm.empty();
        } else fseek(f, (long)n, SEEK_CUR);
    }
}

class FmWrap : public ModeSynth {
public:
    FmWrap(FmGenConfig c, double rate, float level) : rate_(rate), level_(level), g_(c) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { g_.generate(out, n, level_); }
private:
    double rate_;
    float level_;
    FmGenerator g_;
};

std::vector<uint8_t> loadBin(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool loadPpm(const std::string& path, int wantW, int wantH, std::vector<uint8_t>& rgb) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char mag[3] = {};
    if (fread(mag, 1, 2, f) != 2 || mag[0] != 'P' || mag[1] != '6') { fclose(f); return false; }
    auto skip = [&] {
        int c;
        do {
            c = fgetc(f);
            if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); }
        } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        if (c != EOF) ungetc(c, f);
    };
    int w = 0, h = 0, maxv = 0;
    skip(); if (fscanf(f, "%d", &w) != 1) { fclose(f); return false; }
    skip(); if (fscanf(f, "%d", &h) != 1) { fclose(f); return false; }
    skip(); if (fscanf(f, "%d", &maxv) != 1 || maxv != 255) { fclose(f); return false; }
    fgetc(f);
    std::vector<uint8_t> raw((size_t)w * h * 3);
    if (fread(raw.data(), 1, raw.size(), f) != raw.size()) { fclose(f); return false; }
    fclose(f);
    rgb.assign((size_t)wantW * wantH * 3, 0);
    for (int y = 0; y < wantH; y++) for (int x = 0; x < wantW; x++) {
        const int sx = x * w / wantW, sy = y * h / wantH;
        memcpy(&rgb[((size_t)y * wantW + x) * 3], &raw[((size_t)sy * w + sx) * 3], 3);
    }
    return true;
}

class AdsbIdent : public ModeSynth {
public:
    AdsbIdent(double rate, uint32_t icao, const std::string& call) : rate_(rate), mix_(rate, 0, 0) {
        fr_ = adsb::encodeIdentification(icao ? icao : 0xABCDEF, 5, 4, 0, call.empty() ? "TESTTX" : call);
        next_ = 0.01;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double t1 = mix_.time() + (double)n / rate_;
        while (next_ < t1 + 0.002) {
            AdsbTx tx;
            tx.t = next_;
            tx.frame = fr_;
            tx.amp = 0.25f;
            mix_.add(tx);
            next_ += 0.5;
        }
        mix_.render(out, n, true);
    }
private:
    double rate_, next_ = 0;
    AdsbMixer mix_;
    adsb::Frame fr_;
};

int ftxModeOf(int which) {
    if (which == 3) return 0;
    if (which == 4) return 1;
    if (which == 6) return 2;
    if (which == 5) return 3;
    if (which >= 21 && which <= 25) return 4 + (which - 21);
    return -1;
}

std::unique_ptr<ModeSynth> aisVessel(const TxRequest& r, const SynthConfig& s, double rate) {
    using namespace ais;
    Bits pos, stat;
    const uint32_t mmsi = r.address ? r.address : 247000000;
    putU(pos, 0, 6, 1);
    putU(pos, 8, 30, mmsi);
    putU(pos, 38, 4, 0);
    putS(pos, 42, 8, 0);
    putU(pos, 50, 10, 0);
    putU(pos, 60, 1, 1);
    putS(pos, 61, 28, (int32_t)std::lround(r.lon * 600000));
    putS(pos, 89, 27, (int32_t)std::lround(r.lat * 600000));
    putU(pos, 116, 12, 0);
    putU(pos, 128, 9, 511);
    putU(pos, 137, 6, 60);
    pos.resize(168, 0);
    putU(stat, 0, 6, 5);
    putU(stat, 8, 30, mmsi);
    putText(stat, 112, 120, r.name.empty() ? r.callsign : r.name);
    putText(stat, 70, 42, r.callsign);
    stat.resize(424, 0);
    const ModeTuning* mt = modeTuning(16);
    AisRenderConfig rc;
    rc.rate = rate;
    rc.durationSec = 6;
    rc.snrDb = 200;
    rc.cfoHz = 0;
    rc.tuneOffsetHz = mt ? mt->tuneOffsetHz : 0;
    std::vector<AisBurstSpec> b(2);
    b[0].payload = pos; b[0].channel = 'A'; b[0].startSec = 0.4;
    b[1].payload = stat; b[1].channel = 'B'; b[1].startSec = 2.0;
    return std::make_unique<LoopIq>(renderAisBursts(b, rc), rate);
}

// The APRS info field of APRS over AFSK and of LoRa APRS: a position (house symbol) with the comment when lat / lon are set, else a status
std::string aprsInfo(const TxRequest& r) {
    if (r.lat == 0 && r.lon == 0) return ">" + (r.text.empty() ? std::string("OnAir") : r.text);
    // n as `width` digits with leading zeros
    auto digits = [](long n, size_t width) { std::string d = std::to_string(n); return d.size() < width ? std::string(width - d.size(), '0') + d : d; };
    // degrees and minutes to hundredths of a minute, rounded first so 59.999' becomes the next degree and not "60.00"
    auto dm = [&](double v, size_t degWidth) {
        const long h = std::lround(std::fabs(v) * 6000.0);
        return digits(h / 6000, degWidth) + digits((h % 6000) / 100, 2) + "." + digits(h % 100, 2);
    };
    return "!" + dm(r.lat, 2) + (r.lat < 0 ? "S" : "N") + "/" + dm(r.lon, 3) + (r.lon < 0 ? "W" : "E") + "-" + r.text;
}

// a TNC2 header field as typed: spaces dropped, capitals
std::string tnc2Call(const std::string& in) {
    std::string s;
    for (char c : in) if (c != ' ' && c != '\t') s += c;
    return upper(s);
}

// LoRa APRS: the TNC2 packet with its prefix on the frame's radio settings, sent again after lora.gapSec. No callsign: the 70 cm test scene's
// LoRa APRS stations (mesh_gen.h region 2, tuned to 433.775 MHz), always on the EU settings.
std::unique_ptr<ModeSynth> loraAprsSynth(const TxRequest& r, const SynthConfig& s, double rate) {
    const std::string tnc2 = txLoraAprsTnc2(r);
    if (tnc2.empty()) {
        SynthConfig d = s;
        d.modeOpt[0] = 4;   // LoRa APRS only
        d.modeOpt[1] = 2;   // the 70 cm scene
        d.modeOpt[4] = 1;   // placed for a receiver tuned to 433.775 MHz
        return makeMeshSynth(d, rate);
    }
    const TxLoraAprsRadio lr = txLoraAprsRadio(r);
    LoraRawGenConfig c;
    c.rate = rate;
    c.dialOffsetHz = -meshTuning().tuneOffsetHz;
    c.snrDb = 99;
    c.loop = true;
    LoraRawGenFrame g;
    g.p.sf = lr.sf; g.p.bwHz = lr.bwHz; g.p.cr = lr.cr; g.p.preamble = lr.preamble; g.p.syncWord = (uint8_t)lr.syncWord;
    g.p.ldro = meshLoraLdro(lr.sf, lr.bwHz);
    g.p.crc = lr.crc;
    g.payload = loraAprsFrame(tnc2);
    if (g.payload.size() > 255) g.payload.resize(255);   // a LoRa frame carries at most 255 bytes
    g.gapSec = std::max(0.1, r.o("lora.gapSec", 10));
    c.frames.push_back(std::move(g));
    return std::make_unique<WrapGen<LoraRawGenerator>>(std::make_unique<LoraRawGenerator>(c), rate);
}

} // namespace

TxLoraAprsRadio txLoraAprsRadio(const TxRequest& r) {
    TxLoraAprsRadio p;
    switch ((int)std::lround(r.o("lora.preset", 0))) {
    case 1: p.name = "PL"; p.mhz = 434.855; p.sf = 9; p.cr = 7; break;
    case 2: p.name = "UK"; p.mhz = 439.9125; break;
    case 3: p.name = "915"; p.mhz = 915.0; break;
    case 4: {
        static const double kBw[] = {62500, 125000, 250000, 500000};
        p.name = "own";
        p.mhz = 0;
        p.sf = std::max(7, std::min(12, (int)std::lround(r.o("lora.sf", 12))));
        p.bwHz = kBw[std::max(0, std::min(3, (int)std::lround(r.o("lora.bwIdx", 1))))];
        p.cr = std::max(5, std::min(8, (int)std::lround(r.o("lora.cr", 5))));
        p.preamble = std::max(6, std::min(65535, (int)std::lround(r.o("lora.preamble", 8))));
        p.syncWord = std::max(0, std::min(255, (int)std::lround(r.o("lora.sync", 0x12))));
        p.crc = r.o("lora.crc", 1) > 0.5;
        break;
    }
    default: break;   // EU
    }
    return p;
}

std::string txLoraAprsTnc2(const TxRequest& r) {
    const std::string call = tnc2Call(r.callsign);
    if (call.empty()) return {};
    const std::string dest = r.dest.empty() ? "APRS" : tnc2Call(r.dest);
    std::string path = tnc2Call(r.path.empty() ? "WIDE1-1" : r.path);
    if (path == "-" || path == "NONE") path.clear();
    return call + ">" + dest + (path.empty() ? "" : "," + path) + ":" + aprsInfo(r);
}

double txModeDefaultMhz(const TxModeSpec& m) {
    if (m.id && std::string(m.id) == "loraaprs") return 433.775;   // LoRa APRS EU; the mode's engine tuning (mesh) starts at Meshtastic's 869.525
    const ModeTuning* mt = modeTuning(m.stdMode);
    return mt && mt->defMhz > 0 ? mt->defMhz : 0;
}

std::string txFtxMessage(const TxRequest& r) {
    const std::string call = upper(r.callsign), grid = upper(r.grid), dest = upper(r.dest);
    if (r.kind == 2 && !r.text.empty()) return r.text;
    if (r.kind == 3 || r.modeId == "wspr") {
        if (call.empty() || grid.empty()) return {};
        char b[40];
        snprintf(b, sizeof b, "%s %s %d", call.c_str(), grid.c_str(), r.dbm);
        return b;
    }
    if (r.kind == 1 && !call.empty() && !dest.empty()) return dest + " " + call + " -10";
    if (!call.empty() && !grid.empty()) return "CQ " + call + " " + grid;
    if (!call.empty()) return "CQ " + call;
    if (!r.text.empty()) return r.text;
    return {};
}

std::unique_ptr<ModeSynth> makeTxPayloadSynth(const TxModeSpec& m, const TxRequest& r, const SynthConfig& s, double rate) {
    const std::string id = m.id ? m.id : "";

    if (m.stdMode == 27) {
        const int which = m.variant;
        const int ftxm = ftxModeOf(which);
        if (ftxm >= 0) {
            const std::string msg = txFtxMessage(r);
            if (msg.empty()) return nullptr;
            auto audio = makeFtxTxAudio(ftxm, msg, r.toneHz > 100 ? r.toneHz : 1500, r.variant == 1, r.loop);
            if (!audio) return nullptr;   // not encodable: makeTxGenerator says so instead of sending the demo
            return makeHfdigSynthFromAudio(std::move(audio), s, rate, r.variant == 1);
        }
        if (which == 0) {
            auto audio = makeRttyTxAudio(r.text, r.baudIdx, r.shiftIdx, r.reverse, std::max(100.0, r.o("rtty.markHz", 2125)));
            return makeHfdigSynthFromAudio(std::move(audio), s, rate, r.variant == 1);
        }
        if (which >= 26 && which <= 31) {
            KbdTxConfig k;
            k.cfg.family = which - 26;
            if (!r.text.empty()) k.text = r.text;
            if (r.toneHz > 100) k.cfg.centreHz = r.toneHz;
            // the mode's own settings (tx_params.h): Olivia / Contestia tones and bandwidth, the variant of the others
            if (k.cfg.family == KbdOlivia || k.cfg.family == KbdContestia) {
                k.cfg.tones = (int)std::lround(r.o("kbd.tones", k.cfg.tones));
                k.cfg.bandwidth = (int)std::lround(r.o("kbd.bw", k.cfg.bandwidth));
            } else if (kbdVariantCount(k.cfg.family) > 1) {
                k.cfg.variant = std::max(0, std::min(kbdVariantCount(k.cfg.family) - 1, (int)std::lround(r.o("kbd.variant", kbdDefaultVariant(k.cfg.family)))));
            }
            k.cfg.autoParams = false;
            auto audio = makeKbdTx(k);
            // the sideband is the mode's own key kbd.lsb, not r.variant: that is another payload's field (the RTTY sideband, the PSK variant, the SSTV
            // mode), which put the signal on the lower sideband, below the dial, whenever it was 1 (#225)
            return makeHfdigSynthFromAudio(std::move(audio), s, rate, r.o("kbd.lsb", 0) > 0.5);
        }
        if (which == 2) {   // FreeDV
            SynthConfig sc = s;
            sc.modeOpt[0] = 2;
            auto held = std::make_shared<TxRequest>(r);
            auto pull = r.pullAudio ? std::function<void(float*, int)>{[held](float* mo, int n) {
                std::vector<int16_t> pcm((size_t)n);
                txPullPcm8k(*held, pcm.data(), n);
                for (int i = 0; i < n; i++) mo[i] = pcm[(size_t)i] / 32768.f;
            }} : std::function<void(float*, int)>{};
            auto audio = makeFreedvTxAudio(sc, pull);
            if (audio) return makeHfdigSynthFromAudio(std::move(audio), s, rate, r.variant == 1);
            if (r.analogVoice) return makeTxVoiceSynth(m, r, rate);
            return nullptr;
        }
        if (which == 1) {   // SSTV
            if (r.file.empty() && r.callsign.empty() && r.text.empty()) return nullptr;
            const auto& info = sstvModeInfo(std::max(0, r.variant));
            std::vector<uint8_t> rgb;
            if (!loadPpm(r.file, info.width, info.height, rgb)) sstvTestPicture(info.width, info.height, rgb);
            const std::string cap = !r.callsign.empty() ? r.callsign : r.text;
            if (!cap.empty()) sstvStampText(info.width, info.height, rgb, cap);
            auto audio = makeSstvTx(r.variant, rgb);
            return makeHfdigSynthFromAudio(std::move(audio), s, rate, false);
        }
        if (which == 7) {   // HamDRM
            if (r.file.empty() && r.callsign.empty()) return nullptr;
            HamdrmTxConfig c;
            c.callsign = r.callsign.empty() ? "N0CALL" : upper(r.callsign);
            auto file = loadBin(r.file);
            if (file.empty()) file = hamdrmTestPicture();
            if (!r.file.empty()) {
                const auto slash = r.file.find_last_of("/\\");
                c.name = slash == std::string::npos ? r.file : r.file.substr(slash + 1);
                if (c.name.size() > 24) c.name.resize(24);
            }
            auto audio = makeHamdrmTx(c, file);
            return makeHfdigSynthFromAudio(std::move(audio), s, rate, false, 0.65f);   // OFDM: peaks about 1.85 x the tone level, kept under the limiter's knee
        }
        return nullptr;
    }

    if (m.stdMode == 36 && !r.text.empty()) {
        CwGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -cwTuning().tuneOffsetHz;
        c.cfoHz = 0;
        c.noiseSigma = 0;
        CwGenSignal g;
        g.offsetHz = r.toneHz > 100 ? r.toneHz : 700;
        g.wpm = r.wpm > 0 ? r.wpm : 20;
        g.text = r.text;
        g.snrDb = 40;
        c.signals = {g};
        return std::make_unique<WrapGen<CwGenerator>>(std::make_unique<CwGenerator>(c), rate);
    }

    if (m.stdMode == 38 && !r.text.empty()) {
        PskGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -pskTuning().tuneOffsetHz;
        c.noiseSigma = 0;
        PskGenSignal g;
        g.offsetHz = r.toneHz > 100 ? r.toneHz : 1000;
        g.mode = std::max(0, std::min(kPskModes - 1, r.variant));
        g.text = r.text;
        g.snrDb = 40;
        c.signals = {g};
        return std::make_unique<WrapGen<PskGenerator>>(std::make_unique<PskGenerator>(c), rate);
    }

    if (m.stdMode == 39 && (!r.callsign.empty() || !r.text.empty())) {
        Js8GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -js8Tuning().tuneOffsetHz;
        c.noiseSigma = 0;
        Js8GenStation g;
        g.call = r.callsign.empty() ? "N0CALL" : upper(r.callsign);
        g.grid = r.grid.empty() ? "FN42" : upper(r.grid);
        g.to = upper(r.dest);
        if (g.to.empty() && !r.text.empty()) g.to = "@ALLCALL";   // a text needs a recipient in its first frame: without one nothing was sent at all
        g.text = r.text;
        g.kind = r.text.empty() ? Js8GenStation::Cq : Js8GenStation::Text;
        g.hz = r.toneHz > 100 ? r.toneHz : 1000;
        g.snrDb = 20;
        // JS8 frames start on the UTC 15 s slots (a receiver only looks there): the slot clock of sample 0 from the system clock, and the first
        // frame in the next whole slot. Without it the frames sat at the moment Start was pressed plus up to a minute of silence.
        c.slotPhaseSec = std::fmod(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(), 3600.0);
        g.firstSlot = (int)std::floor(c.slotPhaseSec / js8::spec(g.submode).period) + 1;
        c.stations = {g};
        return std::make_unique<WrapGen<Js8Generator>>(std::make_unique<Js8Generator>(c), rate);
    }

    if (m.stdMode == 25 && (r.address || !r.text.empty())) {
        const int type = r.text.empty() ? kPagerTone : kPagerAlpha;
        return makePagerSynthPage(s, rate, r.baudIdx, r.address, r.function, type, r.text);
    }

    if (m.stdMode == 63 && (!r.callsign.empty() || !r.text.empty())) {
        ax25::Frame f;
        f.from = ax25Addr(r.callsign.empty() ? "N0CALL" : r.callsign);
        f.to = ax25Addr(r.dest.empty() ? "CQ" : r.dest);
        f.info = r.text.empty() ? ">CubeSat" : r.text;
        return makePacketSynthFrame(s, rate, f, r.baudIdx == 1 ? 9600 : 1200);
    }

    if (m.stdMode == 26 && !r.callsign.empty()) {
        ax25::Frame f;
        f.from = ax25Addr(r.callsign);
        f.to = ax25Addr(r.dest.empty() ? "APRS" : r.dest);
        std::string path = r.path.empty() ? "WIDE1-1,WIDE2-1" : r.path;
        size_t i = 0;
        while (i < path.size() && f.path.size() < 8) {   // AX.25 carries at most eight digipeaters
            const size_t c = path.find(',', i);
            const ax25::Address a = ax25Addr(path.substr(i, c == std::string::npos ? std::string::npos : c - i));
            if (!a.call.empty()) f.path.push_back(a);
            if (c == std::string::npos) break;
            i = c + 1;
        }
        f.info = aprsInfo(r);
        if (f.info.size() > 255) f.info.resize(255);   // as the 256 byte buffer this replaced cut it
        return makePacketSynthFrame(s, rate, f, r.baudIdx == 1 ? 9600 : 1200);
    }

    if (m.stdMode == 16 && (r.address || !r.callsign.empty() || !r.name.empty() || r.lat != 0 || r.lon != 0))
        return aisVessel(r, s, rate);

    if (m.stdMode == 33 && (!r.callsign.empty() || r.pullAudio || r.analogVoice)) {
        if (r.analogVoice) return makeTxVoiceSynth(m, r, rate);
        M17GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -m17Tuning().tuneOffsetHz;
        c.snrDb = 200;
        c.voice = true;
        M17GenTx t;
        t.src = r.callsign.empty() ? "N0CALL" : upper(r.callsign);
        t.dst = r.dest.empty() ? "@ALL" : upper(r.dest);
        t.text = r.text;
        t.packet = !r.text.empty() && !r.pullAudio;
        t.durSec = r.pullAudio ? 3600 : 2;
        t.gapSec = r.pullAudio ? 0 : 1;
        if (r.pullAudio && !t.packet) {
            t.dataType = 2;
            auto held = std::make_shared<TxRequest>(r);
            t.pullPcm = [held](int16_t* pcm, int n) { txPullPcm8k(*held, pcm, n); };
        }
        c.txs = {t};
        return std::make_unique<WrapGen<M17Generator>>(std::make_unique<M17Generator>(c), rate);
    }

    if (r.analogVoice && (m.stdMode == 35 || m.stdMode == 52 || m.stdMode == 40 || m.stdMode == 47 || m.stdMode == 48))
        return makeTxVoiceSynth(m, r, rate);

    if (m.stdMode == 35 && !r.callsign.empty()) {
        DstarGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -dstarTuning().tuneOffsetHz;
        c.snrDb = 200;
        DstarGenTx t;
        t.hdr.my = upper(r.callsign);
        t.hdr.ur = r.dest.empty() ? "CQCQCQ" : upper(r.dest);
        t.text = r.text;
        c.txs = {t};
        return std::make_unique<WrapGen<DstarGenerator>>(std::make_unique<DstarGenerator>(c), rate);
    }

    if (m.stdMode == 52 && r.address) {
        P25GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -p25Tuning().tuneOffsetHz;
        c.snrDb = 200;
        P25GenTx t;
        t.kind = P25GenTx::Voice;
        t.src = (int)r.address;
        t.dst = r.dest.empty() ? 1201 : atoi(r.dest.c_str());
        c.txs = {t};
        return std::make_unique<WrapGen<P25Generator>>(std::make_unique<P25Generator>(c), rate);
    }

    if (m.stdMode == 40 && r.address) {
        NxdnGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -nxdnTuning().tuneOffsetHz;
        c.snrDb = 200;
        NxdnGenTx t;
        t.src = (int)r.address;
        t.dst = r.dest.empty() ? 200 : atoi(r.dest.c_str());
        t.text = r.text;
        if (!t.text.empty()) t.kind = NxdnGenTx::Data;   // the text goes out as short data (a voice call carries none)
        c.txs = {t};
        return std::make_unique<WrapGen<NxdnGenerator>>(std::make_unique<NxdnGenerator>(c), rate);
    }

    if (m.stdMode == 47 && r.address) {
        DpmrGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -dpmrTuning().tuneOffsetHz;
        c.snrDb = 200;
        DpmrGenTx t;
        t.own = r.address;
        t.called = r.dest.empty() ? 200 : (uint32_t)atoi(r.dest.c_str());
        t.text = r.text;
        if (!t.text.empty()) t.kind = DpmrGenTx::Message;   // the text goes out as a message (a voice call carries none)
        c.txs = {t};
        return std::make_unique<WrapGen<DpmrGenerator>>(std::make_unique<DpmrGenerator>(c), rate);
    }

    if (m.stdMode == 11) {   // DMR
        if (r.analogVoice) return makeTxVoiceSynth(m, r, rate);
        // Real subscriber transmit (dmr_tx.h): built from the operator's own DMR ID and the payload when an identity or message is given.
        const uint32_t destId = (uint32_t)(r.dest.empty() ? 0 : strtoul(r.dest.c_str(), nullptr, 10));
        const bool haveId = r.address != 0 || destId != 0 || r.talkgroup != 0 || !r.text.empty() || r.lat != 0 || r.lon != 0 || r.kind != 0;
        if (haveId) {
            DmrTxConfig t;
            t.rate = rate;
            t.myId = r.address;
            t.callsign = r.callsign;
            t.colourCode = r.colorCode ? r.colorCode : 1;
            t.slot = r.slot;
            t.mobile = true;                 // a repeater uplink by default
            t.call = (DmrTxCall)std::max(0, std::min(7, r.kind));
            t.text = r.text;
            t.lat = r.lat; t.lon = r.lon;
            // destination: a talkgroup for group voice / SMS / LRRP, a radio ID for private and Tier III
            const bool toUnit = t.call == DmrTxCall::PrivateVoice || t.call == DmrTxCall::Tier3Register || t.call == DmrTxCall::Tier3VoiceRequest;
            t.dest = toUnit ? (destId ? destId : (uint32_t)r.talkgroup) : ((uint32_t)r.talkgroup ? (uint32_t)r.talkgroup : destId);
            // slot sync (dmr_slot_sync.h): when the DMR receiver heard the repeater's output in the last 10 s, the first burst waits for the inbound
            // slot; otherwise the transmission starts at once (BS activation). dmr.latencyMs: the radio's delay from here to the air, set by the user.
            if (r.o("dmr.slotSync", 1) > 0.5 && t.mobile)
                dmrTxApplySlotSync(t, t.slot == 2 ? 2 : 1, r.o("dmr.aligned", 0) > 0.5, dmrSteadySec() + r.o("dmr.latencyMs", 0) / 1000.0);
            return makeDmrTxSynth(t);
        }
        // nothing entered: the demo base-station test signal
        SynthConfig s2 = s;
        if (r.colorCode) s2.modeOpt[0] = r.colorCode;
        if (r.slot == 2) s2.modeOpt[2] = 1;
        return makeDmrSynth(s2, rate);
    }

    if (m.stdMode == 10) {   // analog TV: test card / bars with ident; mic or file on the FM/AM sound carrier
        AtvGenConfig extra;
        extra.ident = r.name.empty() ? r.callsign : r.name;
        if (r.pullAudio) extra.pullAudio = r.pullAudio;
        if (r.pullVideo) extra.pullVideo = r.pullVideo;
        auto vid = makeAtvSynth(s, rate, extra);
        if (vid) return vid;
        return nullptr;
    }

    if (m.stdMode == 4) {   // DAB: ensemble and service labels
        dabgen::TxConfig tc = dabDemoConfig();
        if (!r.network.empty()) tc.ensembleLabel = r.network;
        else if (!r.provider.empty()) tc.ensembleLabel = r.provider;
        if (!r.services.empty()) {
            for (size_t i = 0; i < r.services.size() && i < tc.services.size(); i++)
                if (!r.services[i].empty()) tc.services[i].label = r.services[i].substr(0, 16);
        } else if (!r.name.empty() && !tc.services.empty()) tc.services[0].label = r.name.substr(0, 16);
        return makeDabSynth(tc, s, rate);
    }

    if (m.stdMode == 12) {   // DRM station label and text
        DrmTxConfig tc = drmTxConfigFromSynth(s);
        if (!r.name.empty()) tc.label = r.name;
        if (!r.text.empty()) tc.text = r.text;
        if (!r.callsign.empty() && r.name.empty()) tc.label = r.callsign;
        return makeDrmSynth(tc, s, rate);
    }

    if (m.stdMode == 15 && (!r.callsign.empty() || !r.text.empty())) {
        const std::string ser = r.callsign.empty() ? r.text : r.callsign;
        return makeSondeSynthEx(s, rate, ser, r.lat, r.lon);
    }

    if (m.stdMode == 17 && (!r.text.empty() || r.address || !r.callsign.empty())) {
        const int svc = s.modeOpt[0];
        std::string digits;
        const std::string raw = r.callsign.empty() ? r.text : r.callsign;
        for (char c : raw) if (std::isdigit((unsigned char)c)) digits += c;
        if (svc == 2 || svc == 4) {   // DSC was chosen: a DSC call, never NAVTEX (no MMSI given: the test identity 247000000)
            char mmsi[16];
            if (r.address) snprintf(mmsi, sizeof mmsi, "%09u", (unsigned)(r.address % 1000000000u));
            else {
                if (digits.size() < 5) digits = "247000000";
                while (digits.size() < 9) digits.insert(digits.begin(), '0');
                if (digits.size() > 9) digits.resize(9);
                snprintf(mmsi, sizeof mmsi, "%s", digits.c_str());
            }
            return makeMarineSynthDsc(s, rate, mmsi, svc == 4);
        }
        if (!r.text.empty()) return makeMarineSynthNavtex(s, rate, r.text);
    }

    if (m.stdMode == 18 && (!r.text.empty() || !r.callsign.empty())) {
        AcarsGenOptions o;
        o.aircraft = 1;
        o.uplinks = false;
        o.positions = false;
        o.cfoSpreadHz = 0;
        o.channelsHz = {131.525e6};
        o.rateFactor = 4;
        o.text = r.text.empty() ? "HELLO ACARS" : r.text;
        o.reg = r.callsign.empty() ? "A6-TX1" : r.callsign;
        return makeAcarsSynthEx(o, s, rate);
    }

    if (m.stdMode == 29 && (!r.text.empty() || !r.callsign.empty())) {
        Vdl2GenOptions o;
        o.aircraft = 1;
        o.channelsHz = {136.975e6};
        o.rateFactor = 4;
        o.cfoSpreadHz = 0;
        o.text = r.text.empty() ? "HELLO VDL2" : r.text;
        o.reg = r.callsign.empty() ? "G-TEST" : r.callsign;
        return makeVdl2SynthEx(o, s, rate);
    }

    if (m.stdMode == 37 && (!r.text.empty() || !r.callsign.empty())) {
        HfdlGenOptions o;
        o.aircraft = 1;
        o.channelsHz = {8.927e6};
        o.gsIds = {1};
        o.rateFactor = 4;
        o.cfoSpreadHz = 0;
        o.text = r.text.empty() ? "HELLO HFDL" : r.text;
        o.reg = r.callsign.empty() ? "N1TEST" : r.callsign;
        return makeHfdlSynthEx(o, s, rate);
    }

    if (m.stdMode == 78 && !r.text.empty()) {
        SameGenConfig c;
        c.rate = rate;
        c.snrDb = 200;
        c.cfoHz = 0;
        c.tuneOffsetHz = sameTuning().tuneOffsetHz;
        SameGenMessage msg;
        msg.text = r.text.find("ZCZC") == 0 ? r.text : "ZCZC-WXR-RWT-000000+" + r.text + "-";
        msg.repeats = 3;
        msg.toneSec = 0;
        c.messages = {msg};
        return std::make_unique<WrapGen<SameGenerator>>(std::make_unique<SameGenerator>(c), rate);
    }

    if (m.stdMode == 51 && (!r.callsign.empty() || !r.text.empty() || !r.dest.empty())) {
        AleGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -aleTuning().tuneOffsetHz;
        c.snrDb = 20;          // sets the tone level against the 0.02 reference (unchanged)
        c.noiseSigma = 0;      // but no noise: it filled the whole span, above the signal's own power
        c.repeat = true;
        AleGenCall g;
        g.kind = AleGenCall::Amd;
        g.from = r.callsign.empty() ? "N0CALL" : upper(r.callsign);
        g.to = r.dest.empty() ? "CQ" : upper(r.dest);
        g.text = r.text.empty() ? "TEST" : r.text;
        g.answer = false;
        c.calls = {g};
        return std::make_unique<WrapGen<AleGenerator>>(std::make_unique<AleGenerator>(c), rate);
    }

    if (m.stdMode == 32 && !r.callsign.empty()) {
        VorGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -vorTuning().tuneOffsetHz;
        c.snrDb = 99;
        c.ident = upper(r.callsign).substr(0, 4);
        c.voice = false;
        c.kind = std::max(0, std::min(6, s.modeOpt[0]));   // the navaid and its radial / DDM chosen with the ident (they were the demo's only)
        if (c.kind <= 1) c.radialDeg = s.modeVal[0];
        else if (c.kind <= 3) c.ddm = s.modeVal[0];
        return std::make_unique<WrapGen<VorGenerator>>(std::make_unique<VorGenerator>(c), rate);
    }

    if (id == "loraaprs") return loraAprsSynth(r, s, rate);   // before the Meshtastic text below: the same engine mode (22), its own packets

    if (m.stdMode == 22 && !r.text.empty() && !r.analogVoice) {   // Meshtastic text from the user's own node identity (mesh_tx.h); warnings only, never a refusal
        MeshLoraSettings ls;
        if (!meshtasticPreset(r.mesh.preset.empty() ? "LongFast" : r.mesh.preset, ls)) meshtasticPreset("LongFast", ls);
        LoraRawGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -meshTuning().tuneOffsetHz;
        c.snrDb = 99;
        c.loop = true;
        for (const MeshTxFrame& f : meshTxPackets(r.mesh, r.text, (uint32_t)std::chrono::system_clock::now().time_since_epoch().count() | 1u)) {
            LoraRawGenFrame g;
            g.p.sf = ls.sf; g.p.bwHz = ls.bwHz; g.p.cr = ls.cr; g.p.preamble = ls.preamble; g.p.syncWord = ls.syncWord; g.p.ldro = ls.ldro;
            g.p.crc = true;
            g.payload = f.bytes;
            g.gapSec = 2.0;
            c.frames.push_back(std::move(g));
        }
        if (!c.frames.empty()) return std::make_unique<WrapGen<LoraRawGenerator>>(std::make_unique<LoraRawGenerator>(c), rate);
    }

    if (m.stdMode == 73 && !r.text.empty()) {
        LoraRawGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -loraRawTuning().tuneOffsetHz;
        c.snrDb = 99;
        auto frames = loraRawTestSchedule(0x12);
        if (!frames.empty()) {
            frames.resize(1);
            frames[0].payload.assign(r.text.begin(), r.text.end());
            if (frames[0].payload.size() > 255) frames[0].payload.resize(255);
            frames[0].cfoHz = 0;
            // the LoRa settings (tx_params.h), the same keys as LoRa APRS's own settings
            static const double kBw[] = {62500, 125000, 250000, 500000};
            LoraRawGenFrame& f = frames[0];
            f.p.sf = std::max(6, std::min(12, (int)std::lround(r.o("lora.sf", 7))));
            f.p.bwHz = kBw[std::max(0, std::min(3, (int)std::lround(r.o("lora.bwIdx", 1))))];
            f.p.cr = std::max(5, std::min(8, (int)std::lround(r.o("lora.cr", 5))));
            f.p.preamble = std::max(6, std::min(65535, (int)std::lround(r.o("lora.preamble", 8))));
            f.p.syncWord = (uint8_t)std::max(0, std::min(255, (int)std::lround(r.o("lora.sync", 0x12))));
            f.p.crc = r.o("lora.crc", 1) > 0.5;
            f.p.ldro = meshLoraLdro(f.p.sf, f.p.bwHz);
            f.gapSec = std::max(0.1, r.o("lora.gapSec", 2));
        }
        c.loop = true;
        c.frames = std::move(frames);
        return std::make_unique<WrapGen<LoraRawGenerator>>(std::make_unique<LoraRawGenerator>(c), rate);
    }

    if (m.stdMode == 80 && r.address) {
        DectGenConfig c;
        c.rate = rate;
        c.snrDb = 99;
        c.dialOffsetHz = -dectTuning().tuneOffsetHz;
        DectGenBase b;
        b.rfpi = r.address;
        c.bases = {b};
        return std::make_unique<WrapGen<DectGenerator>>(std::make_unique<DectGenerator>(c), rate);
    }

    if (m.stdMode == 13 && !r.callsign.empty())
        return std::make_unique<AdsbIdent>(rate, r.address, upper(r.callsign));

    if (m.stdMode == 7 || m.stdMode == 28 || m.stdMode == 55 || r.analogVoice) {
        if (m.stdMode != 7) return makeTxVoiceSynth(m, r, rate);
        FmGenConfig g;
        float fmOutLevel = 0.3f;
        g.rate = rate;
        g.cnrDb = 200;
        g.leftHz = r.toneHz > 0 ? r.toneHz : 1000;
        g.rightHz = r.toneHz > 0 ? r.toneHz * 3 : 3000;
        g.preemphUs = r.preemphUs > 0 ? r.preemphUs : 50;
        g.deviationHz = r.deviationHz > 0 ? r.deviationHz : 75000;
        g.pi = r.rdsPi ? r.rdsPi : 0x4A21;
        // the advanced parameters (bfm.* keys of the panel): stereo and the pilot, the RDS, the programme type and flags, the test tones and the output level
        g.stereo = r.o("bfm.stereo", 1) > 0.5;
        g.pilotPct = r.o("bfm.pilotPct", 9);
        g.rds = r.o("bfm.rds", 1) > 0.5;
        g.rdsDevKhz = r.o("bfm.rdsDevKhz", 2);
        g.pty = (int)r.o("bfm.pty", 10);
        g.tp = r.o("bfm.tp", 1) > 0.5;
        g.ta = r.o("bfm.ta", 0) > 0.5;
        g.music = r.o("bfm.music", 1) > 0.5;
        g.leftAmp = (float)r.o("bfm.leftAmp", 0.5);
        g.rightAmp = (float)r.o("bfm.rightAmp", 0.5);
        if (r.o("bfm.rightHz", 0) > 0) g.rightHz = r.o("bfm.rightHz", 0);
        fmOutLevel = (float)r.o("bfm.level", 0.3);
        if (!r.name.empty()) { g.ps = r.name; if (g.ps.size() < 8) g.ps.append(8 - g.ps.size(), ' '); if (g.ps.size() > 8) g.ps.resize(8); }
        if (!r.text.empty()) g.rt = r.text;
        if (r.pullAudio) {
            auto pull = r.pullAudio;
            g.pull48k = [pull](float* st, int n) { if (pull) pull(st, n); else memset(st, 0, (size_t)n * 2 * sizeof(float)); };
        }
        {   // live RDS (rds_live.h): the request's values start it; the panel, the REST / MCP / text port and a watched file change it while on the air
            RdsState rs;
            rs.ps = g.ps; rs.ps.resize(8, ' '); rs.rt = g.rt; rs.pi = g.pi; rs.pty = g.pty; rs.tp = g.tp; rs.ta = g.ta; rs.music = g.music;
            rs.ct = r.o("bfm.ct", 0) > 0.5;
            RdsLive::global().seed(rs);
            g.live = std::shared_ptr<RdsLive>(&RdsLive::global(), [](RdsLive*) {});
        }
        return std::make_unique<FmWrap>(g, rate, fmOutLevel);
    }

    // Imagery / scene generators have no custom-string path: analog NFM of mic/file when asked, else the demo.
    if (r.analogVoice || r.pullAudio)
        return makeTxVoiceSynth(m, r, rate);
    (void)id;
    (void)s;
    return nullptr;
}

} // namespace dect2
