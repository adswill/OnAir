// DRM: write the test signal to a file (drmtool gen ...) or run the receiver on a recording (drmtool rx ...).
//
//   drmtool gen <file.cs8|file.cf32> [--secs 20] [--rate 2000000] [--mode A|B|C|D] [--occ 0..5] [--qam 16|64] [--prot 0..3] [--short] [--snr 30]
//                                    [--cfo 0] [--sro 0] [--channel 0..6] [--audio melody|tone|silence|noise|bytes] [--echo dB --delay samples] [--no-text]
//   drmtool rx  <file> --rate <Hz> [--format cs8|cu8|cf32|s16] [--shift Hz] [--secs N] [--chunk N] [--verbose] [--wav out.wav] [--dump prefix]
//
// The formats are interleaved signed 8 bit IQ (cs8), unsigned 8 bit IQ (cu8), float IQ (cf32), and s16: one channel of 16 bit audio that carries a DRM
// signal at an intermediate frequency (what the Dream decoder takes): it is turned into a complex signal first; --shift moves the signal by that many Hz.
#include "dect2/drm_gen.h"
#include "dect2/drm_rx.h"
#include "dect2/mode_synth.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

static const char* arg(int argc, char** argv, const char* name, const char* def = nullptr) {
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
static bool flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], name)) return true;
    return false;
}

static int gen(int argc, char** argv) {
    if (argc < 3) { printf("usage: drmtool gen <file.cs8|file.cf32> [options]\n"); return 2; }
    const std::string path = argv[2];
    const double secs = atof(arg(argc, argv, "--secs", "20"));
    const double rate = atof(arg(argc, argv, "--rate", "2000000"));
    SynthConfig sc;
    sc.mode = 12;
    sc.snrDb = atof(arg(argc, argv, "--snr", "30"));
    sc.cfoHz = atof(arg(argc, argv, "--cfo", "0"));
    sc.sroPpm = atof(arg(argc, argv, "--sro", "0"));
    sc.echoDb = atof(arg(argc, argv, "--echo", "0"));
    sc.echoDelay = atoi(arg(argc, argv, "--delay", "300"));
    const char* m = arg(argc, argv, "--mode", "B");
    sc.modeOpt[0] = (m[0] >= 'A' && m[0] <= 'D') ? m[0] - 'A' + 1 : 2;
    if (arg(argc, argv, "--occ")) sc.modeOpt[1] = atoi(arg(argc, argv, "--occ")) + 1;
    sc.modeOpt[2] = atoi(arg(argc, argv, "--qam", "64")) == 16 ? 1 : 0;
    if (arg(argc, argv, "--prot")) sc.modeOpt[3] = atoi(arg(argc, argv, "--prot")) + 1;
    sc.modeOpt[4] = flag(argc, argv, "--short") ? 1 : 0;
    const std::string au = arg(argc, argv, "--audio", "melody");
    sc.modeOpt[5] = au == "tone" ? 1 : au == "silence" ? 2 : au == "noise" ? 3 : au == "bytes" ? 4 : 0;
    sc.modeOpt[6] = atoi(arg(argc, argv, "--channel", "0"));
    sc.modeOpt[7] = flag(argc, argv, "--no-text") ? 1 : 0;
    auto syn = makeModeSynth(12, sc, rate);
    if (!syn) { printf("no test signal for this combination\n"); return 1; }
    const bool f32 = path.size() > 5 && path.substr(path.size() - 5) == ".cf32";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", path.c_str()); return 1; }
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(65536);
    std::vector<uint8_t> raw;
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t n = std::min(buf.size(), total - done);
        syn->generate(buf.data(), n);
        if (f32) fwrite(buf.data(), sizeof(cf32), n, f);
        else {
            raw.resize(n * 2);
            for (size_t i = 0; i < n; i++) {
                raw[2 * i] = (uint8_t)(int8_t)std::lround(std::max(-128.f, std::min(127.f, buf[i].real() * 128.f)));
                raw[2 * i + 1] = (uint8_t)(int8_t)std::lround(std::max(-128.f, std::min(127.f, buf[i].imag() * 128.f)));
            }
            fwrite(raw.data(), 1, raw.size(), f);
        }
    }
    fclose(f);
    printf("wrote %.1f s at %.0f sps to %s\n", secs, rate, path.c_str());
    return 0;
}

// analytic signal of a real signal: a long FIR Hilbert transformer, delayed to match the real part
class Hilbert {
public:
    Hilbert() {
        const int n = 255, m = n / 2;
        h_.assign(n, 0.f);
        for (int i = 0; i < n; i++) {
            const int k = i - m;
            if (k % 2 == 0) continue;
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
            h_[i] = (float)(2.0 / (M_PI * k) * w);
        }
        hist_.assign(n, 0.f);
    }
    void process(const float* x, size_t n, std::vector<cf32>& out) {
        const int nt = (int)h_.size(), m = nt / 2;
        for (size_t i = 0; i < n; i++) {
            hist_[pos_] = x[i];
            float im = 0;
            for (int k = 0; k < nt; k++) im += h_[k] * hist_[(pos_ + nt - k) % nt];
            const float re = hist_[(pos_ + nt - m) % nt];
            out.push_back(cf32(re, im));
            pos_ = (pos_ + 1) % nt;
        }
    }
private:
    std::vector<float> h_, hist_;
    int pos_ = 0;
};

static int rx(int argc, char** argv) {
    if (argc < 3) { printf("usage: drmtool rx <file> --rate <Hz> [options]\n"); return 2; }
    const std::string path = argv[2];
    const double rate = atof(arg(argc, argv, "--rate", "0"));
    if (rate <= 0) { printf("--rate is needed\n"); return 2; }
    std::string fmt = arg(argc, argv, "--format", "");
    if (fmt.empty()) { const size_t d = path.rfind('.'); fmt = d == std::string::npos ? "cs8" : path.substr(d + 1); }
    const double shift = atof(arg(argc, argv, "--shift", "0"));
    const double maxSecs = atof(arg(argc, argv, "--secs", "1e9"));
    const size_t chunk = (size_t)atoll(arg(argc, argv, "--chunk", "65536"));
    const bool verbose = flag(argc, argv, "--verbose");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", path.c_str()); return 1; }
    DrmReceiver r;
    r.setSilent(true);
    r.configure(rate);
    if (!r.ready()) { printf("the sample rate %.0f is too low\n", rate); return 1; }
    r.setLogCallback([](const std::string& s) { printf("  log: %s\n", s.c_str()); });
    const char* dump = arg(argc, argv, "--dump");
    uint64_t frames = 0;
    FILE* df = dump ? fopen((std::string(dump) + ".bin").c_str(), "wb") : nullptr;
    r.setStreamCallback([&](int stream, const uint8_t* b, int len, int lenA, const DrmTelemetry&) {
        frames++;
        if (df) { fwrite(b, 1, (size_t)len, df); }
        (void)stream; (void)lenA;
    });
    std::vector<float> wavL, wavR;
    if (arg(argc, argv, "--wav")) r.setAudioTap([&](const float* l, const float* rr, size_t n) { wavL.insert(wavL.end(), l, l + n); wavR.insert(wavR.end(), rr, rr + n); });
    const size_t bps = fmt == "cf32" ? 8 : fmt == "s16" ? 2 : 2;
    std::vector<uint8_t> raw(chunk * bps);
    std::vector<cf32> buf;
    Hilbert hil;
    double phase = 0;
    DrmTelemetry t, last;
    uint64_t seq = 0;
    size_t samples = 0;
    double nextReport = 1.0;
    const auto t0 = std::chrono::steady_clock::now();
    double cpu = 0;
    for (;;) {
        const size_t got = fread(raw.data(), bps, chunk, f);
        if (got == 0) break;
        buf.clear();
        if (fmt == "cs8") for (size_t i = 0; i < got; i++) buf.push_back(cf32((float)(int8_t)raw[2 * i] / 128.f, (float)(int8_t)raw[2 * i + 1] / 128.f));
        else if (fmt == "cu8") for (size_t i = 0; i < got; i++) buf.push_back(cf32(((float)raw[2 * i] - 127.5f) / 128.f, ((float)raw[2 * i + 1] - 127.5f) / 128.f));
        else if (fmt == "cf32") { buf.resize(got); memcpy(buf.data(), raw.data(), got * sizeof(cf32)); }
        else if (fmt == "s16") {
            std::vector<float> a(got);
            for (size_t i = 0; i < got; i++) a[i] = (float)(int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) / 32768.f;
            hil.process(a.data(), got, buf);
        } else { printf("unknown format %s\n", fmt.c_str()); return 2; }
        if (shift != 0) for (auto& v : buf) { v *= cf32(std::polar(1.0, phase)); phase -= 2 * M_PI * shift / rate; if (phase < -2 * M_PI) phase += 2 * M_PI; }
        const auto c0 = std::chrono::steady_clock::now();
        r.feed(buf.data(), buf.size());
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        samples += buf.size();
        if (r.telemetry(t, seq)) { seq = t.seq; last = t; }
        if (verbose && (double)samples / rate >= nextReport) { printf("%5.1f s  %s   [%s]\n", (double)samples / rate, drmSummary(last).c_str(), last.status.c_str()); nextReport += 1.0; }
        if ((double)samples / rate >= maxSecs) break;
    }
    fclose(f);
    if (df) fclose(df);
    if (r.telemetry(t, 0)) last = t;
    const double secs = (double)samples / rate;
    printf("--- %.1f s of signal, %.2f s of receiver time: %.1fx real time\n", secs, cpu, secs / std::max(cpu, 1e-9));
    printf("%s\n", drmSummary(last).c_str());
    printf("state %d  mode %c  occupancy %d (%.1f kHz)  interleaving %s  MSC %d-QAM  protection %d/%d  SNR %.1f dB  CFO %+.1f Hz  SRO %+.1f ppm  Doppler %.2f Hz  delay spread %.2f ms\n",
           last.state, last.modeName, last.occupancy, last.bandwidthKhz, last.longInterleave ? "long" : "short", last.mscQam, last.protA, last.protB, last.snrDb, last.cfoHz, last.sroPpm,
           last.dopplerHz, last.delaySpreadMs);
    printf("FAC %llu ok %llu bad   SDC %llu ok %llu bad   multiplex frames %llu   audio frames %llu ok %llu bad   logical frames %llu\n", (unsigned long long)last.facOk, (unsigned long long)last.facBad,
           (unsigned long long)last.sdcOk, (unsigned long long)last.sdcBad, (unsigned long long)last.mscFramesOk, (unsigned long long)last.blocksOk, (unsigned long long)last.blocksBad,
           (unsigned long long)frames);
    for (const auto& s : last.services)
        printf("service %d: %s  '%s'  id %06X  language %s  country %s  type %s  %s\n", s.shortId, s.audio ? "audio" : "data", s.label.c_str(), s.id, s.languageName.c_str(), s.country.c_str(),
               s.programmeName.c_str(), s.codecText.c_str());
    if (last.timeValid) printf("time %04d-%02d-%02d %02d:%02d UTC\n", last.year, last.month, last.day, last.hour, last.minute);
    if (!last.textMessage.empty()) printf("text: %s\n", last.textMessage.c_str());
    printf("audio: %s\n", last.audioInfo.c_str());
    if (arg(argc, argv, "--wav") && !wavL.empty()) {
        FILE* w = fopen(arg(argc, argv, "--wav"), "wb");
        if (w) {
            const uint32_t n = (uint32_t)wavL.size(), dataBytes = n * 4, rate48 = 48000, byteRate = rate48 * 4;
            const uint16_t fmt1 = 1, ch = 2, ba = 4, bits = 16;
            const uint32_t sub = 16, riff = 36 + dataBytes;
            fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&sub, 4, 1, w);
            fwrite(&fmt1, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate48, 4, 1, w); fwrite(&byteRate, 4, 1, w); fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
            fwrite("data", 1, 4, w); fwrite(&dataBytes, 4, 1, w);
            for (size_t i = 0; i < wavL.size(); i++) {
                for (float v : {wavL[i], wavR[i]}) { const int16_t s = (int16_t)std::lround(std::max(-1.f, std::min(1.f, v)) * 32767.f); fwrite(&s, 2, 1, w); }
            }
            fclose(w);
        }
    }
    return last.state >= 1 ? 0 : 3;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: drmtool gen|rx ...\n"); return 2; }
    if (!strcmp(argv[1], "gen")) return gen(argc, argv);
    if (!strcmp(argv[1], "rx")) return rx(argc, argv);
    printf("usage: drmtool gen|rx ...\n");
    return 2;
}
