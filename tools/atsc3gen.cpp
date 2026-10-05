// atsc3gen: makes a synthetic ATSC 3.0 broadcast as an IQ recording, from a fragmented MP4 video and audio file. Nothing here is a real
// transmission: it is the transmitter chain of this project (ROUTE, ALP, baseband packets, BICM, OFDM frames with bootstrap and Preamble) followed
// by a channel model (carrier offset, echo, noise), written as 8-bit I/Q like a HackRF would record it. Use it to try the ATSC 3.0 receiver.
//
//   atsc3gen --video v.mp4 --audio a.mp4 --out sample.cs8 [--rate 10e6] [--cfo 2500] [--snr 25] [--echo-us 2] [--echo-db -12] [--format cs8|cf32]
//
// The MP4 files must be fragmented with about one second per fragment, see tools/dev/make_atsc3_sample.sh.
#include "dect2/atsc3_alp.h"
#include "dect2/atsc3_bb.h"
#include "dect2/atsc3_frame.h"
#include "dect2/atsc3_ip.h"
#include "dect2/atsc3_route.h"
#include "dect2/atsc3_sync.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace dect2;
using namespace dect2::atsc3;

static std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void split(const std::vector<uint8_t>& f, std::vector<uint8_t>& init, std::vector<std::vector<uint8_t>>& segs) {
    size_t pos = 0;
    bool inInit = true;
    while (pos + 8 <= f.size()) {
        uint32_t sz = (f[pos] << 24) | (f[pos + 1] << 16) | (f[pos + 2] << 8) | f[pos + 3];
        if (sz < 8 || pos + sz > f.size()) break;
        char type[5] = {(char)f[pos + 4], (char)f[pos + 5], (char)f[pos + 6], (char)f[pos + 7], 0};
        if (!strcmp(type, "moof")) { inInit = false; segs.emplace_back(); }
        if (inInit) init.insert(init.end(), f.begin() + pos, f.begin() + pos + sz);
        else if (!segs.empty() && strcmp(type, "sidx") && strcmp(type, "styp")) segs.back().insert(segs.back().end(), f.begin() + pos, f.begin() + pos + sz);
        pos += sz;
    }
}

static const uint32_t kSrc = 0x0A010105u, kDst = 0xEFFF0501u;

struct Sender {
    std::vector<std::vector<uint8_t>> ip;
    int ipId = 1;
    void udp(uint32_t dst, int port, const std::vector<uint8_t>& payload) {
        for (auto& p : makeUdpPackets(kSrc, dst, 4000, port, payload, ipId++ & 0xFFFF)) ip.push_back(p);
    }
    void object(uint32_t tsi, uint32_t toi, int cp, const std::vector<uint8_t>& data) {
        const int piece = 1100;
        for (size_t off = 0; off < data.size(); off += piece) {
            LctPacket p;
            p.tsi = tsi; p.toi = toi; p.codePoint = cp; p.startOffset = (uint32_t)off;
            size_t n = std::min<size_t>(piece, data.size() - off);
            p.payload.assign(data.begin() + off, data.begin() + off + n);
            p.transferLength = (int64_t)data.size();
            udp(kDst, 3000, makeRoutePacket(p, true));
        }
    }
};

static double fileDuration(const std::string& path) {
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, path.c_str(), nullptr, nullptr) < 0) return 0;
    avformat_find_stream_info(ic, nullptr);
    double d = ic->duration > 0 ? (double)ic->duration / AV_TIME_BASE : 0;
    avformat_close_input(&ic);
    return d;
}

static void usage() {
    printf("usage: atsc3gen --video v.mp4 --audio a.mp4 --out file [--rate 10e6] [--cfo 2500] [--snr 25] [--echo-us 2] [--echo-db -12] [--format cs8|cf32] [--name TEST]\n");
}

int main(int argc, char** argv) {
    std::string video, audio, out, format = "cs8", name = "TEST";
    double rate = 10e6, cfo = 2500, snr = 25, echoUs = 2.0, echoDb = -12;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--video") video = val(); else if (a == "--audio") audio = val(); else if (a == "--out") out = val();
        else if (a == "--rate") rate = atof(val().c_str()); else if (a == "--cfo") cfo = atof(val().c_str()); else if (a == "--snr") snr = atof(val().c_str());
        else if (a == "--echo-us") echoUs = atof(val().c_str()); else if (a == "--echo-db") echoDb = atof(val().c_str());
        else if (a == "--format") format = val(); else if (a == "--name") name = val();
        else { usage(); return 1; }
    }
    if (video.empty() || audio.empty() || out.empty()) { usage(); return 1; }
    auto vf = slurp(video), af = slurp(audio);
    if (vf.empty() || af.empty()) { printf("cannot read the MP4 files\n"); return 1; }
    std::vector<uint8_t> vinit, ainit;
    std::vector<std::vector<uint8_t>> vseg, aseg;
    split(vf, vinit, vseg);
    split(af, ainit, aseg);
    if (vinit.empty() || ainit.empty() || vseg.empty() || aseg.empty()) { printf("the MP4 files must be fragmented (init segment, then moof + mdat fragments)\n"); return 1; }
    const double dur = std::max(fileDuration(video), fileDuration(audio));
    const int slots = (int)std::max(vseg.size(), aseg.size());
    const double slotSec = dur > 0 ? dur / slots : 1.0;
    printf("video: %zu fragments, audio: %zu fragments, %.1f s -> %d slots of %.2f s\n", vseg.size(), aseg.size(), dur, slots, slotSec);

    // ---- the PHY configuration
    FramePlp plp;
    plp.id = 1; plp.fecType = 1; plp.mod = 2; plp.cod = 6;   // BCH + 64K LDPC, 64QAM, 8/15
    Bicm bicm(plpBicm(plp));
    if (!bicm.ok()) { printf("PLP configuration not supported\n"); return 1; }
    const int bytes = bicm.kPayload() / 8;
    FrameSetup fs;
    fs.bs.minorVersion = 0; fs.bs.numSymbols = 4; fs.bs.systemBandwidth = 0; fs.bs.bsrCoefficient = 8;   // 6 MHz, 9.216 Msamples/s
    fs.bs.preambleStructure = 10;                                                                      // 8K, guard 512, pilot spacing 6, L1-Basic mode 1
    fs.bs.minTimeToNext = 1;                                                                           // the next frame comes after at least 100 ms
    fs.fftCode = 0; fs.guardCode = 3; fs.spPattern = 4; fs.numSymbols = 130; fs.l1DetailMode = 3; fs.sbsNullCells = 16;
    const double frameRate = postBootstrapRate(fs.bs);
    const size_t symLen = 8192 + 512;
    const double frameSec = (double)fs.bs.numSymbols * 3072 / kBootstrapRate + (double)(fs.numSymbols + 1 + fs.preambleSymbols - 1) * symLen / frameRate;
    const long periodSamples = (long)std::ceil(frameSec * rate) + 4;
    const double period = (double)periodSamples / rate;
    const int totalFrames = (int)std::ceil(dur / period) + 4;
    printf("frame: %.1f ms, %d frames, radio rate %.3f Msps, offset %.0f Hz, SNR %.0f dB, echo %.1f us at %.0f dB\n", period * 1e3, totalFrames, rate / 1e6, cfo, snr, echoUs, echoDb);

    // ---- the service: SLT, signaling package, init segments, then the fragments second by second
    const std::string slt = "<SLT bsid=\"4660\"><Service serviceId=\"1001\" majorChannelNo=\"7\" minorChannelNo=\"1\" serviceCategory=\"1\" shortServiceName=\"" + name + "\" sltSvcSeqNum=\"0\">"
        "<BroadcastSvcSignaling slsProtocol=\"1\" slsDestinationIpAddress=\"239.255.5.1\" slsDestinationUdpPort=\"3000\" slsSourceIpAddress=\"10.1.1.5\"/></Service></SLT>";
    const std::string stsid =
        "<S-TSID><RS dIpAddr=\"239.255.5.1\" dPort=\"3000\">"
        "<LS tsi=\"10\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"v1\" contentType=\"video\"/></ContentInfo></SrcFlow></LS>"
        "<LS tsi=\"20\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"a1\" contentType=\"audio\" lang=\"en\"/></ContentInfo></SrcFlow></LS></RS></S-TSID>";
    std::vector<MimePart> parts(3);
    parts[0].headers["content-type"] = "application/route-usd+xml";
    const std::string usbd = "<BundleDescription/>";
    parts[0].body.assign(usbd.begin(), usbd.end());
    parts[1].headers["content-type"] = "application/route-s-tsid+xml";
    parts[1].body.assign(stsid.begin(), stsid.end());
    const std::string mpd = "<MPD type=\"dynamic\"><Period/></MPD>";
    parts[2].headers["content-type"] = "application/dash+xml";
    parts[2].body.assign(mpd.begin(), mpd.end());
    const auto sls = makeMultipart(parts, "b1");
    const auto lls = makeLls(1, 0, 1, slt);

    std::vector<std::vector<std::vector<uint8_t>>> slotBb(slots);   // baseband packets of each slot
    size_t totalBb = 0, totalBytes = 0;
    for (int i = 0; i < slots; i++) {
        Sender s;
        s.udp(kLlsAddress, kLlsPort, lls);
        s.object(0, (uint32_t)i + 1, 3, sls);
        s.object(10, 0, i == 0 ? 5 : 7, vinit);
        s.object(20, 0, i == 0 ? 5 : 7, ainit);
        if (i < (int)vseg.size()) s.object(10, (uint32_t)i + 1, 8, vseg[i]);
        if (i < (int)aseg.size()) s.object(20, (uint32_t)i + 1, 8, aseg[i]);
        std::vector<uint8_t> alp;
        std::vector<size_t> starts;
        for (auto& p : s.ip) { AlpPacket a; a.type = AlpIpv4; a.data = p; auto w = alpSingle(a); starts.push_back(alp.size()); alp.insert(alp.end(), w.begin(), w.end()); }
        totalBytes += alp.size();
        size_t pos = 0, next = 0;
        while (pos < alp.size()) {
            const int room = bytes - 2;
            while (next < starts.size() && starts[next] < pos) next++;
            int pointer = (next < starts.size() && starts[next] < pos + room) ? (int)(starts[next] - pos) : 8191;
            size_t take = std::min<size_t>(room, alp.size() - pos);
            std::vector<uint8_t> chunk(alp.begin() + pos, alp.begin() + pos + take), pk;
            if ((int)take == room) { BbHeader h; h.pointer = pointer; h.twoByteBase = true; pk = makeBbHeader(h); pk.insert(pk.end(), chunk.begin(), chunk.end()); }
            else pk = makeBbPacket(bytes, chunk, pointer == 8191 ? -1 : pointer, -1);
            slotBb[i].push_back(pk);
            pos += take;
        }
        totalBb += slotBb[i].size();
    }
    printf("service '%s': %zu bytes of ALP stream in %zu baseband packets (about %.2f Mbit/s)\n", name.c_str(), totalBytes, totalBb, totalBytes * 8.0 / std::max(1.0, dur) / 1e6);

    // ---- the output
    std::ofstream of(out, std::ios::binary);
    if (!of) { printf("cannot write %s\n", out.c_str()); return 1; }
    const bool cs8 = format == "cs8";
    std::mt19937 rng(2026);
    std::normal_distribution<float> g(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -snr / 10.0) / 2.0);
    const long echoDelay = std::max<long>(1, (long)std::llround(echoUs * 1e-6 * rate));
    const float echoAmp = (float)std::pow(10.0, echoDb / 20.0);
    const cf32 echoRot(std::cos(0.8f), std::sin(0.8f));
    std::vector<cf32> carry(echoDelay, cf32(0, 0));   // the end of the previous frame, for the echo
    long gIndex = 0;                                  // sample index in the file, for a continuous carrier offset
    double phase = 0;
    const double dphi = 2.0 * M_PI * cfo / rate;
    auto emit = [&](std::vector<cf32>& x) {
        // echo
        std::vector<cf32> y(x.size());
        for (size_t i = 0; i < x.size(); i++) {
            cf32 prev = i >= (size_t)echoDelay ? x[i - echoDelay] : carry[carry.size() - echoDelay + i];
            y[i] = x[i] + prev * echoAmp * echoRot;
        }
        for (long i = 0; i < echoDelay; i++) carry[i] = x[x.size() - echoDelay + i];
        std::vector<int8_t> b8;
        std::vector<float> f32;
        for (size_t i = 0; i < y.size(); i++) {
            cf32 v = y[i] * cf32((float)std::cos(phase), (float)std::sin(phase)) + cf32(g(rng), g(rng)) * sigma;
            phase += dphi;
            if ((gIndex++ & 4095) == 4095) phase = std::fmod(phase, 2 * M_PI);
            if (cs8) {
                const float scale = 22.f;   // rms of 22 of 127: the peaks of the OFDM signal stay below full scale
                b8.push_back((int8_t)std::max(-127.f, std::min(127.f, std::round(v.real() * scale))));
                b8.push_back((int8_t)std::max(-127.f, std::min(127.f, std::round(v.imag() * scale))));
            } else { f32.push_back(v.real()); f32.push_back(v.imag()); }
        }
        if (cs8) of.write((const char*)b8.data(), (std::streamsize)b8.size());
        else of.write((const char*)f32.data(), (std::streamsize)(f32.size() * sizeof(float)));
    };
    // some silence first so the recording does not start exactly at a bootstrap
    { std::vector<cf32> lead((size_t)(0.0377 * rate), cf32(0, 0)); emit(lead); }
    int frame = 0;
    long written = 0;
    for (int slot = 0; slot < slots; slot++) {
        // frames of this slot: those whose start time falls into [slot * slotSec, (slot + 1) * slotSec)
        int f0 = (int)std::ceil(slot * slotSec / period - 1e-9), f1 = slot == slots - 1 ? std::max(totalFrames, f0 + 1) : (int)std::ceil((slot + 1) * slotSec / period - 1e-9);
        if (f1 <= f0) f1 = f0 + 1;
        const int nFrames = f1 - f0;
        const auto& bbs = slotBb[slot];
        for (int k = 0; k < nFrames; k++) {
            size_t a = bbs.size() * k / nFrames, b = bbs.size() * (k + 1) / nFrames;
            FramePlp fp = plp;
            for (size_t j = a; j < b; j++) fp.bbPackets.push_back(bbs[j]);
            if (fp.bbPackets.empty()) fp.bbPackets.push_back(makeBbPacket(bytes, {}, -1, -1));   // an all-padding packet
            auto fr = buildFrame(fs, {fp});
            if (fr.empty()) { printf("frame %d does not fit\n", frame); return 1; }
            std::vector<cf32> boot = generateBootstrap(fs.bs), b1, f1v;
            resampleExact(boot.data(), boot.size(), kBootstrapRate, rate, b1);
            resampleExact(fr.data(), fr.size(), frameRate, rate, f1v);
            std::vector<cf32> one(b1);
            one.insert(one.end(), f1v.begin(), f1v.end());
            one.resize((size_t)periodSamples, cf32(0, 0));
            emit(one);
            written += (long)one.size();
            frame++;
        }
        printf("\r  slot %d of %d, %d frames, %.1f s", slot + 1, slots, frame, written / rate);
        fflush(stdout);
    }
    printf("\nwrote %s: %.1f s, %ld samples at %.4f Msps, %s\n", out.c_str(), written / rate, written, rate / 1e6, cs8 ? "8-bit signed I/Q (CS8)" : "32-bit float I/Q (CF32)");
    return 0;
}
