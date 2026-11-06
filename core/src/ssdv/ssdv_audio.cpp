// SSDV from a WAV recording: see ssdv_audio.h
#include "dect2/ssdv/ssdv_audio.h"
#include "dect2/dsp/resampler.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dect2 {

namespace {
uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
constexpr double kOutRate = 192000.0;   // the front end of the receiver works from 96 kHz; 192 kHz leaves room for 19200 baud
}

bool readWavAudio(const std::string& path, WavAudio& out, std::string& err) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    uint8_t h[12];
    if (!f.read((char*)h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { err = "not a WAV file: " + path; return false; }
    int tag = 0, ch = 0, bits = 0; uint32_t rate = 0;
    bool haveFmt = false;
    uint8_t ck[8];
    while (f.read((char*)ck, 8)) {
        uint64_t len = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            std::vector<uint8_t> b(std::max<uint64_t>(len, 24), 0);
            if (!f.read((char*)b.data(), (std::streamsize)len)) break;
            tag = le16(b.data()); ch = le16(b.data() + 2); rate = le32(b.data() + 4); bits = le16(b.data() + 14);
            if (tag == 0xFFFE && len >= 26) tag = le16(b.data() + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
            haveFmt = true;
            if (len & 1) f.seekg(1, std::ios::cur);
        } else if (!memcmp(ck, "data", 4)) {
            if (!haveFmt) { err = "WAV data before its format"; return false; }
            const bool pcm = tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
            const bool flt = tag == 3 && (bits == 32 || bits == 64);
            if (!pcm && !flt) { err = "WAV sample format not supported (8 / 16 / 24 / 32-bit PCM and 32 / 64-bit float are)"; return false; }
            if (ch < 1 || rate < 1000) { err = "WAV header is not usable"; return false; }
            std::error_code ec;
            const uint64_t avail = std::filesystem::file_size(std::filesystem::u8path(path), ec) - (uint64_t)f.tellg();
            if (len == 0 || len > avail) len = avail;   // a stream that was cut off, or a size field left at 0 / 0xFFFFFFFF
            const size_t bps = (size_t)bits / 8, frame = bps * (size_t)ch;
            const size_t n = (size_t)(len / frame);
            std::vector<uint8_t> raw(n * frame);
            if (!f.read((char*)raw.data(), (std::streamsize)raw.size())) raw.resize((size_t)f.gcount() / frame * frame);
            const size_t got = raw.size() / frame;
            out.rate = rate; out.channels = ch; out.mono.assign(got, 0.f);
            for (size_t i = 0; i < got; i++) {
                const uint8_t* p = raw.data() + i * frame;   // the first channel
                float v;
                if (flt) { if (bits == 32) { float x; memcpy(&x, p, 4); v = x; } else { double x; memcpy(&x, p, 8); v = (float)x; } }
                else if (bits == 8) v = ((int)p[0] - 128) / 128.f;
                else if (bits == 16) v = (int16_t)le16(p) / 32768.f;
                else if (bits == 24) v = ((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.f;
                else v = (int32_t)le32(p) / 2147483648.f;
                out.mono[i] = std::isfinite(v) ? v : 0.f;
            }
            return true;
        } else f.seekg((std::streamoff)len + (len & 1), std::ios::cur);
        if (!f) break;
    }
    err = "no audio data in " + path;
    return false;
}

bool ssdvAudioToIq(const std::vector<float>& audio, double audioRate, std::vector<std::complex<float>>& iq, double& outRate, std::string& err) {
    iq.clear();
    if (audio.empty() || audioRate < 1000) { err = "no audio"; return false; }
    // remove the DC offset (a sound card's) and scale to the peak: the slicers of the receiver follow the level, only the deviation matters
    double mean = 0;
    for (float v : audio) mean += v;
    mean /= (double)audio.size();
    float peak = 0;
    for (float v : audio) peak = std::max(peak, std::fabs((float)(v - mean)));
    if (peak < 1e-6f) { err = "the recording is silent"; return false; }
    outRate = kOutRate;
    RationalResampler rs;
    if (!rs.configure(audioRate, outRate)) { err = "cannot convert the sample rate of the recording"; return false; }
    std::vector<cf32> in(audio.size()), up;
    for (size_t i = 0; i < audio.size(); i++) in[i] = cf32((float)((audio[i] - mean) / peak), 0.f);
    rs.process(in.data(), in.size(), up);
    const double dev = 3000.0;   // Hz at full scale: inside the channel filter of the receiver for every speed it handles
    double ph = 0;
    iq.resize(up.size());
    for (size_t i = 0; i < up.size(); i++) {
        ph += 2.0 * math::kPi * dev * (double)up[i].real() / outRate;
        if (ph > math::kPi) ph -= 2.0 * math::kPi; else if (ph < -math::kPi) ph += 2.0 * math::kPi;
        iq[i] = std::complex<float>(0.4f * (float)std::cos(ph), 0.4f * (float)std::sin(ph));
    }
    return true;
}

bool ssdvWavToCf32File(const std::string& wavPath, const std::string& cf32Path, double& outRate, std::string& err) {
    WavAudio w;
    if (!readWavAudio(wavPath, w, err)) return false;
    std::vector<std::complex<float>> iq;
    if (!ssdvAudioToIq(w.mono, w.rate, iq, outRate, err)) return false;
    // a second of unmodulated carrier first and last: the front end settles, and the end of the picture is not cut at the last sample
    std::vector<std::complex<float>> lead((size_t)outRate / 2, iq.front()), tail((size_t)outRate / 2, iq.back());
    std::ofstream o(std::filesystem::u8path(cf32Path), std::ios::binary | std::ios::trunc);
    if (!o) { err = "cannot write " + cf32Path; return false; }
    o.write((const char*)lead.data(), (std::streamsize)(lead.size() * 8));
    o.write((const char*)iq.data(), (std::streamsize)(iq.size() * 8));
    o.write((const char*)tail.data(), (std::streamsize)(tail.size() * 8));
    if (!o) { err = "cannot write " + cf32Path; return false; }
    return true;
}

}  // namespace dect2
