// Mercury tool: decode the audio of a Mercury station (a WAV file or raw 16-bit samples, any rate: resampled to 8 kHz), an IQ recording,
// or write the test signal as 8 kHz audio. Monitor only: nothing is transmitted.
//   mercurytool decode <file.wav | file.raw> [--rate Hz (raw files, default 8000)] [--calls CALL1,CALL2]
//   mercurytool iq <file.cf32> --rate Hz [--offset Hz (where the dial sits in the file, default -20000)] [--calls ...]
//   mercurytool gen <out.wav> [--script 0..3] [--mode DATAC3] [--snr dB] [--cfo Hz] [--fading 0..5] [--ppm n] [--seed n]
// decode prints every burst (mode, start, SNR, offset, FEC, the frame) and then the sessions, messages and files the monitor rebuilt.
#include "dect2/mercury/mercury_gen.h"
#include "dect2/mercury/mercury_monitor.h"
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_proto.h"
#include "dect2/mercury/mercury_rx.h"
#include "dect2/mercury/mercury_tel.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::mercury;

namespace {

bool readAll(const std::string& path, std::vector<uint8_t>& d) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

// PCM 16-bit or 32-bit float, any channel count (the first channel is used)
bool readWav(const std::vector<uint8_t>& d, std::vector<float>& out, int& rate) {
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t p = 12;
    int fmt = 0, ch = 0, bits = 0;
    while (p + 8 <= d.size()) {
        const uint32_t len = le32(&d[p + 4]);
        if (!std::memcmp(&d[p], "fmt ", 4) && p + 24 <= d.size()) { fmt = le16(&d[p + 8]); ch = le16(&d[p + 10]); rate = (int)le32(&d[p + 12]); bits = le16(&d[p + 22]); }
        if (!std::memcmp(&d[p], "data", 4)) {
            const size_t end = std::min(d.size(), p + 8 + (size_t)len);
            const size_t step = (size_t)(ch * bits / 8);
            if (!step) return false;
            for (size_t i = p + 8; i + step <= end; i += step) {
                if (fmt == 1 && bits == 16) out.push_back((float)(int16_t)le16(&d[i]) / 32768.f);
                else if (fmt == 3 && bits == 32) { float f; std::memcpy(&f, &d[i], 4); out.push_back(f); }
                else return false;
            }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

bool writeWav(const std::string& path, const std::vector<float>& a, int rate) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    auto w32 = [&](uint32_t v) { o.put((char)v); o.put((char)(v >> 8)); o.put((char)(v >> 16)); o.put((char)(v >> 24)); };
    auto w16 = [&](uint16_t v) { o.put((char)v); o.put((char)(v >> 8)); };
    o.write("RIFF", 4); w32((uint32_t)(36 + a.size() * 2)); o.write("WAVEfmt ", 8); w32(16); w16(1); w16(1); w32((uint32_t)rate); w32((uint32_t)rate * 2); w16(2); w16(16);
    o.write("data", 4); w32((uint32_t)(a.size() * 2));
    for (float v : a) w16((uint16_t)(int16_t)std::lround(std::max(-1.f, std::min(1.f, v)) * 32767.f));
    return (bool)o;
}

// windowed sinc resampling to 8 kHz (with the anti-alias low-pass when going down)
std::vector<float> to8k(const std::vector<float>& x, int rate) {
    if (rate == 8000) return x;
    const double r = (double)rate / 8000.0, cut = std::min(1.0, 1.0 / r) * 0.45;
    const int H = (int)std::ceil(16 * std::max(1.0, r));
    std::vector<float> y((size_t)std::floor((double)x.size() / r));
    for (size_t i = 0; i < y.size(); i++) {
        const double u = (double)i * r;
        const int64_t c = (int64_t)std::floor(u);
        double acc = 0, ws = 0;
        for (int64_t j = c - H + 1; j <= c + H; j++) {
            if (j < 0 || j >= (int64_t)x.size()) continue;
            const double t = (double)j - u;
            const double s = std::fabs(t) < 1e-9 ? 2 * cut : std::sin(2 * dect2::math::kPi * cut * t) / (dect2::math::kPi * t);
            const double w = 0.5 + 0.5 * std::cos(dect2::math::kPi * t / H);
            acc += x[(size_t)j] * s * w; ws += s * w;
        }
        y[i] = (float)(ws != 0 ? acc / ws : 0);
    }
    return y;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    std::string c;
    for (char ch : s) { if (ch == ',' || ch == ' ') { if (!c.empty()) v.push_back(c); c.clear(); } else c += ch; }
    if (!c.empty()) v.push_back(c);
    return v;
}

void printTelemetry(const MercuryTelemetry& t) {
    for (auto it = t.frames.rbegin(); it != t.frames.rend(); ++it)
        printf("%8.2f s  %-8s %-10s %-8s -> %-8s %+5.1f dB %+6.1f Hz %s  %s\n", it->timeSec, it->modeName.c_str(), it->kind.c_str(), it->from.empty() ? "-" : it->from.c_str(),
               it->to.empty() ? "-" : it->to.c_str(), it->snrDb, it->cfoHz, it->fecOk ? "FEC ok" : "FEC failed", it->detail.c_str());
    for (const MercurySession& s : t.sessions) {
        printf("\nsession %d: %s -> %s, %s, seed %04X%s%s\n", s.id, s.caller.c_str(), s.callee.c_str(), s.state.c_str(), s.seed, s.seedFromFrames ? " (from the frames)" : "",
               s.encrypted ? ", encrypted (not decoded)" : "");
        for (int d = 0; d < 2; d++)
            if (!s.d[d].text.empty()) printf("  %s -> %s (%llu bytes):\n%s\n", d ? s.callee.c_str() : s.caller.c_str(), d ? s.caller.c_str() : s.callee.c_str(), (unsigned long long)s.d[d].bytes, s.d[d].text.c_str());
    }
    if (!t.messages.empty()) printf("\nbroadcast messages:\n%s", t.messages.c_str());
    for (const MercuryFile& f : t.files) printf("\nfile %s: %u bytes, %s%s\n", f.name.c_str(), f.size, f.done ? "complete" : "incomplete", f.kind == "text" ? (":\n" + f.preview).c_str() : "");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mercurytool decode <file.wav|file.raw> [--rate Hz] [--calls A,B]\n"
                        "       mercurytool iq <file.cf32> --rate Hz [--offset Hz] [--calls A,B]\n"
                        "       mercurytool gen <out.wav> [--script 0..3] [--mode NAME] [--snr dB] [--cfo Hz] [--fading 0..5] [--ppm n] [--seed n]\n");
        return 2;
    }
    const std::string cmd = argv[1], path = argv[2];
    double rate = 0, offset = -20000, snr = 99, cfo = 0, ppm = 0;
    int script = 0, fading = 0, mode = kDatac3;
    uint64_t seed = 1;
    std::vector<std::string> calls;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--offset" && i + 1 < argc) offset = atof(argv[++i]);
        else if (a == "--snr" && i + 1 < argc) snr = atof(argv[++i]);
        else if (a == "--cfo" && i + 1 < argc) cfo = atof(argv[++i]);
        else if (a == "--ppm" && i + 1 < argc) ppm = atof(argv[++i]);
        else if (a == "--script" && i + 1 < argc) script = atoi(argv[++i]);
        else if (a == "--fading" && i + 1 < argc) fading = atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = (uint64_t)atoll(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) { mode = ofdmModeByName(argv[++i]); if (mode < 0) { fprintf(stderr, "unknown mode\n"); return 2; } }
        else if (a == "--calls" && i + 1 < argc) calls = split(argv[++i]);
    }
    if (cmd == "gen") {
        ChannelConfig ch;
        ch.snrDb = snr; ch.cfoHz = cfo; ch.sroPpm = ppm; ch.fading = fading; ch.seed = seed;
        const std::vector<cf> x = renderScript(demoScript(script, mode), ch, 0.5);
        std::vector<float> a(x.size());
        float peak = 1e-9f;
        for (size_t i = 0; i < x.size(); i++) { a[i] = x[i].real(); peak = std::max(peak, std::fabs(a[i])); }
        for (float& v : a) v *= 0.7f / peak;
        if (!writeWav(path, a, 8000)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        printf("wrote %s: %.1f s of 8 kHz audio\n", path.c_str(), a.size() / 8000.0);
        return 0;
    }
    std::vector<uint8_t> raw;
    if (!readAll(path, raw)) { fprintf(stderr, "cannot read %s\n", path.c_str()); return 1; }
    if (cmd == "decode") {
        std::vector<float> a;
        int r = 8000;
        if (!readWav(raw, a, r)) {
            r = rate > 0 ? (int)rate : 8000;
            for (size_t i = 0; i + 1 < raw.size(); i += 2) a.push_back((float)(int16_t)le16(&raw[i]) / 32768.f);
        }
        a = to8k(a, r);
        AudioToBaseband ab;
        std::vector<cf> bb;
        ab.process(a.data(), a.size(), bb);
        OfdmReceiver rx;
        Monitor mon;
        mon.setKnownCalls(calls);
        auto take = [&]() { for (const OfdmFrame& f : rx.take()) mon.onFrame(f, f.startSample / 8000.0, f.startSample / 8000.0, (f.endSample - f.startSample) / 8000.0); };
        for (size_t i = 0; i < bb.size(); i += 4000) { rx.push(bb.data() + i, std::min<size_t>(4000, bb.size() - i)); take(); }
        rx.flush();
        take();
        MercuryTelemetry t;
        mon.fill(t);
        printf("%s: %.1f s of audio at %d Hz, %llu bursts acquired, %llu not decoded\n\n", path.c_str(), a.size() / 8000.0, r, (unsigned long long)rx.detections(), (unsigned long long)rx.failures());
        printTelemetry(t);
        return 0;
    }
    if (cmd == "iq") {
        if (rate <= 0) { fprintf(stderr, "--rate is needed\n"); return 2; }
        std::vector<cf32> x(raw.size() / 8);
        std::memcpy(x.data(), raw.data(), x.size() * 8);
        MercuryReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(offset);
        rx.setKnownCalls(calls);
        for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        rx.flush();
        MercuryTelemetry t;
        rx.telemetry(t, 0);
        printf("%s: %.1f s of IQ at %.0f Hz\n\n", path.c_str(), x.size() / rate, rate);
        printTelemetry(t);
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
