// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Writes sample IQ files of the KAAL, KAPP, KAFT, KAIT and KAWS transmitters through the transmit IQ-file sink (tx_iqfile.h):
//   kris_samples <out folder> [<folder with 16 kHz mono voice WAV files for KAAL, one per file>]
// Per signal: a two-channel 16-bit WAV (I left, Q right, opens in SDR++), a SigMF cf32 and a SigMF ci16 file. The rates are the transmitters' own:
// KAAL 48 ksps, KAPP 320 ksps (the 640 ksps internal rate halved), KAFT 4 samples per symbol (640 ksps, 1.6 Msps, 3.2 Msps for 200 kHz, 500 kHz, 1 MHz), KAIT 632 ksps (4 samples per symbol, 10 s picture burst), KAWS 12 ksps (a P0 chat burst and a P3 beacon, upper-sideband audio as IQ).
#include "dect2/kaal/kaal_txgen.h"
#include "dect2/kaft/kaft_frame.h"
#include "dect2/kaft/kaft_rx.h"
#include "dect2/kapp/kapp_rx.h"
#include "dect2/kapp/kapp_frame.h"
#include "dect2/kait/kait_frame.h"
#include "dect2/kait/kait_rx.h"
#include "dect2/kavb/kavb_av.h"
#include "dect2/kavb/kavb_phy.h"
#include "dect2/kavb/kavb_rx.h"
#include "dect2/kaws/kaws_frame.h"
#include "dect2/kaws/kaws_phy.h"
#include "dect2/kaws/kaws_sched.h"
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqfile.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
typedef std::complex<float> cfx;

static bool g_ok = true;

static void writeAll(const std::string& dir, const std::string& base, double rate, const std::vector<cf32>& xin, const std::string& desc) {
    std::vector<cf32> x = xin;   // peak at 0.8 of full scale so the 16-bit files do not clip
    float pk = 0;
    for (const cf32& v : x) pk = std::max(pk, std::max(std::fabs(v.real()), std::fabs(v.imag())));
    if (pk > 0) for (cf32& v : x) v *= 0.8f / pk;
    const IqFileFormat fmts[3] = {IqFileFormat::WavIq16, IqFileFormat::SigmfCf32, IqFileFormat::SigmfCi16};
    for (IqFileFormat f : fmts) {
        IqFileTxConfig c;
        const char* sfx = f == IqFileFormat::WavIq16 ? "" : (f == IqFileFormat::SigmfCf32 ? "_cf32" : "_ci16");
        c.path = iqFilePath(dir + "/", base + sfx, f);
        c.format = f;
        c.centerHz = 0;
        c.description = desc;
        auto sink = makeIqFileTxSink(c);
        TxTune t;
        t.sampleRate = rate;
        std::string err;
        if (!sink->open(t, err)) { fprintf(stderr, "open %s: %s\n", c.path.c_str(), err.c_str()); g_ok = false; continue; }
        for (size_t i = 0; i < x.size(); i += 65536) {
            if (!sink->write(x.data() + i, std::min<size_t>(65536, x.size() - i), err)) { fprintf(stderr, "write: %s\n", err.c_str()); g_ok = false; break; }
        }
        sink->close();
        printf("%s  (%.2f s at %.0f sps)\n", c.path.c_str(), (double)x.size() / rate, rate);
    }
}

static std::vector<float> readWav16kMono(const std::string& path) {
    std::vector<float> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12) { fclose(f); return out; }
    int ch = 1;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        const uint32_t n = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (!memcmp(ck, "fmt ", 4)) {
            std::vector<unsigned char> b(n);
            if (fread(b.data(), 1, n, f) != n) break;
            ch = b[2] | (b[3] << 8);
        } else if (!memcmp(ck, "data", 4)) {
            std::vector<int16_t> s(n / 2);
            if (fread(s.data(), 2, s.size(), f) != s.size()) break;
            for (size_t i = 0; i + (size_t)ch <= s.size(); i += (size_t)ch) out.push_back((float)s[i] / 32768.f);
            break;
        } else fseek(f, (long)n + (n & 1), SEEK_CUR);
    }
    fclose(f);
    return out;
}

static void kaalVoice(const std::string& dir, const std::string& name, const std::vector<float>& pcm16k, int kbit = 16) {
    // 16 kHz to the 48 kHz stereo the generator pulls: linear interpolation (the transmitter low-passes at 7 kHz anyway)
    uint64_t pulled = 0;
    TxRequest r;
    r.modeId = "kaal";
    r.callsign = "LY2ABC";
    r.grid = "KO26ab";
    r.lat = 54.687; r.lon = 25.279;
    r.text = "KAAL sample: " + name;
    r.opt["kaal.powerDbm"] = 20;
    r.opt["kaal.beacon"] = 1;
    r.opt["kaal.bitrate"] = kbit;
    r.pullAudio = [&](float* st, int n) {
        for (int i = 0; i < n; i++, pulled++) {
            // the sentence repeats with a second of silence between, so a looping player does not run out of speech
            const size_t per = pcm16k.size() + 16000, k = (size_t)(pulled / 3) % per;
            const float a = k < pcm16k.size() ? pcm16k[k] : 0.f, b = k + 1 < pcm16k.size() ? pcm16k[k + 1] : 0.f;
            const float v = a + (b - a) * (float)(pulled % 3) / 3.f;
            st[2 * i] = st[2 * i + 1] = v;
        }
        return n;
    };
    std::string err;
    auto gen = makeKaalTxGenerator(r, 48000, err);
    if (!gen) { fprintf(stderr, "kaal: %s\n", err.c_str()); g_ok = false; return; }
    // 2 327 040 samples (48.48 s) = a whole number of 4-frame blocks (3840 samples) and of station-ID sweep cycles (24 periods of 24240 samples): the file loops without a jump
    const size_t n = 2327040;
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i += 4800) gen->generate(x.data() + i, std::min<size_t>(4800, n - i));
    writeAll(dir, "KAAL_" + name + (kbit == 16 ? "" : "_" + std::to_string(kbit) + "kbps"), 48000, x, "kaal");
}

static void addNoise(std::vector<cf32>& x, double snrDb, std::mt19937_64& rng) {
    double p = 0; size_t c = 0;
    for (const cf32& s : x) { const double e = std::norm(s); if (e > 1e-6) { p += e; c++; } }
    if (!c) return;
    const double sig = p / (double)c, sd = std::sqrt(sig / std::pow(10.0, snrDb / 10.0) / 2.0);
    std::normal_distribution<double> g(0.0, sd);
    for (cf32& s : x) s += cf32((float)g(rng), (float)g(rng));
}

static std::vector<uint8_t> textBytes(size_t n) {
    const std::string t = "KRIS custom mode test message from LY1TST: the quick brown fox jumps over the lazy dog 0123456789. ";
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)t[i % t.size()];
    return b;
}

static void kappProfile(const std::string& dir, int p, double snrDb, const char* label) {
    std::mt19937_64 rng(1000 + (unsigned)p);
    std::vector<cf32> all(32000, cf32(0, 0));   // 0.1 s of silence first
    const kapp::Profile& P = kapp::kProfiles[(size_t)p];
    const int nb = std::min(6, kapp::maxBlocksForMs(P, 100.0));
    for (int b = 0; b < 8; b++) {
        kapp::BurstSpec s;
        s.callsign = "LY1TST";
        s.profileId = p;
        s.seq = b;
        s.flags = 0;
        s.payload = textBytes((size_t)nb * (size_t)P.payloadBytes);
        const std::vector<cfx> burst = kapp::buildBurst(s);
        const std::vector<cfx> r = kapp::resample(burst, kapp::kInternalRate, 320000.0);
        all.insert(all.end(), r.begin(), r.end());
        all.insert(all.end(), 16000 + (size_t)(b * 1000), cf32(0, 0));
    }
    addNoise(all, snrDb, rng);
    char nm[64];
    {   // check: our own receiver decodes the file
        kapp::BurstReceiver::Config rc; rc.sampleRate = 320000;
        kapp::BurstReceiver rx(rc);
        rx.feed(all.data(), all.size());
        int ok = 0, tot = 0;
        for (const auto& b : rx.takeBursts()) { ok += b.blocksOk(); tot += b.nBlocks; }
        printf("check KAPP P%d: %d of %d blocks decoded (sent %d)\n", p, ok, tot, 8 * nb);
    }
    snprintf(nm, sizeof nm, "KAPP_P%d_%s", p, label);
    writeAll(dir, nm, 320000, all, "kapp");
}


// KAWS: one burst of the HF weak-signal mode as the complex upper-sideband signal at 12 ksps (the real part is the audio of a 12 kHz sound card), made by the
// transmit generator. The file starts on a UTC slot line (the burst begins 1.5 s into it), noise at snrDb in 2.4 kHz. KAWS is simulation-only: these
// files are made by the simulator, not recorded from a radio. kind 0 chat (block 1 of 2 at P0: a P0 chat message needs two slots), 1 short beacon.
static void kawsSample(const std::string& dir, int kind, int profile, double snrDb, const char* label) {
    TxRequest r;
    r.modeId = "kaws";
    r.callsign = "LY1TST";
    r.grid = "KO24RM";
    r.dest = "CQ";
    r.text = "KAWS TEST MESSAGE FROM LY1TST";
    r.opt["kaws.kind"] = kind;
    r.opt["kaws.profile"] = profile;
    r.opt["kaws.repeat"] = 1;
    r.opt["kaws.utc"] = 1.8e9;
    r.opt["kaws.msgid"] = 1;
    std::string err;
    auto g = makeKawsTxGenerator(r, 12000, err);
    if (!g) { fprintf(stderr, "kaws: %s\n", err.c_str()); g_ok = false; return; }
    std::vector<cf32> all, blk(12000);
    while (!g->finished() && all.size() < 12000u * 400) { g->generate(blk.data(), blk.size()); all.insert(all.end(), blk.begin(), blk.end()); }
    double ps = 0;
    size_t a = 0, b = all.size();
    while (a < b && std::abs(all[a]) < 1e-4f) a++;
    while (b > a && std::abs(all[b - 1]) < 1e-4f) b--;
    for (size_t i = a; i < b; i++) ps += std::norm(all[i]);
    ps /= (double)std::max<size_t>(1, b - a);
    std::mt19937_64 rng(2000 + (unsigned)profile + 10u * (unsigned)kind);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sd = (float)std::sqrt(ps * 12000.0 / (2400.0 * std::pow(10.0, snrDb / 10.0)) / 2.0);
    for (cf32& v : all) v += cf32(sd * nd(rng), sd * nd(rng));
    {   // check: the physical layer decodes the file
        std::vector<float> audio(all.size());
        for (size_t i = 0; i < all.size(); i++) audio[i] = all[i].real();
        const std::vector<kaws::cf> iq = kaws::fromAudio12k(audio);
        const kaws::RxBurst rb = kaws::receiveBurst(profile, 1, iq, 0, kaws::kSearchHi);
        uint8_t mi = 0, ix = 0;
        kaws::Bits pl;
        const bool ok = rb.sync && rb.iters[0] >= 0 && kaws::parseBlock(kaws::kProfiles[profile].K, rb.blocks[0], mi, ix, pl);
        printf("check KAWS P%d kind %d: preamble %d, first block %s, SNR estimate %.1f dB\n", profile, kind, (int)rb.sync, ok ? "ok" : "NOT decoded", rb.snrDb);
    }
    char nm[64];
    snprintf(nm, sizeof nm, "KAWS_P%d_%s_%s", profile, kind == 0 ? "chat" : "beacon", label);
    writeAll(dir, nm, 12000, all, "kaws");
}

// KAIT: one 10 s picture burst (the synthetic 1080p test picture) with noise at snrDb in the 190 kHz band, half a second of noise before and after
static void kaitSample(const std::string& dir, int profile, double snrDb, const char* label) {
    const kait::BurstPlan plan = kait::planBurst(kait::syntheticPicture(), "LY1TST", profile);
    if (!plan.ok) { fprintf(stderr, "kait: no plan for profile %d\n", profile); g_ok = false; return; }
    const std::vector<cfx> burst = kait::buildBurst(plan);
    const size_t pad = (size_t)(0.5 * kait::kInternalRate);
    std::vector<cf32> all(pad + burst.size() + pad, cf32(0, 0));
    std::copy(burst.begin(), burst.end(), all.begin() + (long)pad);
    std::mt19937_64 rng(3000 + (unsigned)profile);
    const double sd = std::sqrt(kait::kInternalRate / kait::kBandwidthHz / std::pow(10.0, snrDb / 10.0) / 2.0);
    std::normal_distribution<double> g(0.0, sd);
    for (cf32& s : all) s += cf32((float)g(rng), (float)g(rng));
    {   // check: our own receiver decodes the file
        const kait::RxResult r = kait::decodeBurst(all);
        printf("check KAIT profile %d at %.0f dB: header %d, SNR %.1f dB, %d of %d layers, %zu stream bytes, picture PSNR %.1f dB\n", profile, snrDb, r.headerOk, r.snrDb, r.layersComplete,
               r.hdr.nLayers, r.bytesGood, r.image.rgb.empty() ? 0.0 : kait::psnrRgb(kait::syntheticPicture(), r.image));
        if (!r.headerOk) g_ok = false;
    }
    char nm[64];
    snprintf(nm, sizeof nm, "KAIT_P%d_%s", profile, label);
    writeAll(dir, nm, kait::kInternalRate, all, "kait");
}

// KAVB: one burst of synthetic video / audio packets (profile A = id 0, D = id 3; FEC profile 1), about 1.5 s, with noise (Es/N0 snrDb) and 0.1 s of noise before and after.
// The payload is synthetic (AV1 and Opus are not integrated); our own receiver decodes the file's samples as a check. Sample rate = 4 x symbol rate.
static void kavbSample(const std::string& dir, int profileId, double snrDb) {
    using namespace dect2::kavb;
    const Profile p = profileOf(profileId, false);
    const int fec = 1;
    Packetiser pk(p, fec);
    const BitBudget bb = bitBudget(p, fec);
    SyntheticAv src(bb.forVideo * 0.9, 30.0, 96000.0, 1200);
    AvPacket a;
    while (src.timeSec() < 1.5) { src.next(a); pk.push(a); }
    const auto frames = pk.takeFrames(true);
    const Transmitter tx(profileId);
    const std::vector<cf> iq = tx.modulate(frames, fec, 0, 0, 0);
    const double rate = p.symRate * kSps;
    const size_t pad = (size_t)(0.1 * rate);
    std::vector<cf> all(pad + iq.size() + pad, cf(0, 0));
    std::copy(iq.begin(), iq.end(), all.begin() + (long)pad);
    std::mt19937_64 rng(5000 + (unsigned)profileId);
    const double sd = std::sqrt(kSps / std::pow(10.0, snrDb / 10.0) / 2.0);   // data power 1 per sample, Es/N0 as in tests/kavb
    std::normal_distribution<double> g(0.0, sd);
    for (cf& s : all) s += cf((float)g(rng), (float)g(rng));
    {
        Receiver rx;
        int ok = 0;
        const auto fr = rx.push(all.data(), all.size());
        for (const auto& x : fr) ok += x.allOk;
        printf("check KAVB profile %c at Es/N0 %.0f dB: %zu frames sent, %zu received, %d fully ok\n", p.name, snrDb, frames.size(), fr.size(), ok);
        if (fr.size() != frames.size() || ok != (int)frames.size()) g_ok = false;
    }
    std::vector<cf32> x(all.begin(), all.end());
    char nm[64];
    snprintf(nm, sizeof nm, "KAVB_P%c_%.0fksym", p.name, p.symRate / 1000.0);
    writeAll(dir, nm, rate, x, "kavb");
}

static void kaftProfile(const std::string& dir, kaft::Width w, int p, double snrDb, const char* label) {
    std::mt19937_64 rng(2000 + (unsigned)p * 7 + (unsigned)w);
    const kaft::WidthInfo& wi = kaft::widthInfo(w);
    const int rate = 4 * wi.symRate;
    std::vector<cf32> all((size_t)rate / 10, cf32(0, 0));
    const int nb = kaft::maxBlocksPerFrame(p, w);
    const int dataB = kaft::dataBytesPerBlock(p);
    uint32_t off = 0;
    const std::vector<uint8_t> msg = textBytes((size_t)dataB * (size_t)nb * 4);
    for (int burst = 0; burst < 2; burst++) {
        std::vector<kaft::FrameSpec> frames;
        for (int fr = 0; fr < 4; fr++) {
            kaft::FrameSpec f;
            f.header.width = w;
            f.header.profile = p;
            f.header.type = kaft::FrameType::Data;
            f.header.session = 7;
            f.header.frameNo = (uint16_t)(burst * 4 + fr);
            f.header.frameInBurst = (uint8_t)fr;
            f.header.lastInBurst = fr == 3;
            f.header.callsign = "LY1TST";
            for (int b = 0; b < nb; b++) {
                f.blocks.push_back(kaft::packDataBlock(off, msg.data() + off % msg.size(), (size_t)std::min<size_t>((size_t)dataB, msg.size() - off % msg.size())));
                off += (uint32_t)dataB;
            }
            frames.push_back(std::move(f));
        }
        const std::vector<cfx> w4 = kaft::buildBurst(frames);
        all.insert(all.end(), w4.begin(), w4.end());
        all.insert(all.end(), (size_t)rate / 20, cf32(0, 0));
    }
    addNoise(all, snrDb, rng);
    char nm[96];
    {
        kaft::FrameReceiverConfig rc; rc.inputRate = rate;
        int ok = 0, tot = 0;
        for (const auto& f : kaft::decodeWaveform(all, rc)) { ok += f.blocksOk(); tot += (int)f.blocks.size(); }
        printf("check KAFT %s P%d: %d of %d blocks decoded (sent %d)\n", wi.name, p, ok, tot, 8 * nb);
    }
    snprintf(nm, sizeof nm, "KAFT_%s_P%d_%s", wi.name, p, label);
    for (char* c = nm; *c; c++) if (*c == ' ') *c = '_';
    writeAll(dir, nm, rate, all, "kaft");
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: kris_samples <out folder> [<voice WAV 16 kHz mono> ...]\n"); return 2; }
    const std::string out = argv[1];
    for (int i = 2; i < argc; i++) {
        std::string nm = argv[i];   // named after the folder of the file (voice1_Samantha/0_original.wav)
        const size_t sl = nm.find_last_of('/');
        if (sl != std::string::npos) nm = nm.substr(0, sl);
        const size_t sl2 = nm.find_last_of('/');
        if (sl2 != std::string::npos) nm = nm.substr(sl2 + 1);
        const std::vector<float> pcm = readWav16kMono(argv[i]);
        if (pcm.empty()) { fprintf(stderr, "cannot read %s\n", argv[i]); g_ok = false; continue; }
        kaalVoice(out + "/KAAL", nm, pcm);
        if (i == 2) for (int kb : {12, 24, 32}) kaalVoice(out + "/KAAL", nm, pcm, kb);   // the first voice at the other three bitrates
    }
    const char* only = std::getenv("KRIS_ONLY");   // e.g. KRIS_ONLY=KAVB writes only that folder's files
    auto want = [&](const char* n) { return !only || !*only || std::strstr(only, n); };
    // low / mid / high profiles at a fixed channel quality
    if (want("KAPP")) kappProfile(out + "/KAPP", 1, 12.0, "low");
    if (want("KAPP")) kappProfile(out + "/KAPP", 4, 22.0, "mid");
    if (want("KAPP")) kappProfile(out + "/KAPP", 7, 34.0, "high");
    if (want("KAWS")) kawsSample(out + "/KAWS", 0, 0, -12.0, "weak");
    if (want("KAWS")) kawsSample(out + "/KAWS", 1, 3, 3.0, "mid");
    if (want("KAIT")) kaitSample(out + "/KAIT", 0, 20.0, "20dB");
    if (want("KAIT")) kaitSample(out + "/KAIT", 0, 8.0, "8dB");
    if (want("KAIT")) kaitSample(out + "/KAIT", 0, -3.0, "m3dB");
    if (want("KAVB")) kavbSample(out + "/KAVB", 0, 25.0);
    if (want("KAVB")) kavbSample(out + "/KAVB", 3, 25.0);
    if (want("KAFT")) kaftProfile(out + "/KAFT", kaft::Width::W200k, 1, 12.0, "low");
    if (want("KAFT")) kaftProfile(out + "/KAFT", kaft::Width::W500k, 4, 24.0, "mid");
    if (want("KAFT")) kaftProfile(out + "/KAFT", kaft::Width::W1M, 1, 12.0, "low");
    if (want("KAFT")) kaftProfile(out + "/KAFT", kaft::Width::W1M, 4, 24.0, "mid");
    if (want("KAFT")) kaftProfile(out + "/KAFT", kaft::Width::W1M, 8, 36.0, "high");
    return g_ok ? 0 : 1;
}
