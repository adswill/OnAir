// Inmarsat all-carriers mode with an Aero C channel (voice) in the capture: the detector finds the wide carrier, the manager puts a C-channel
// decoder on it, the frames check and the coded voice frames reach the voice sink (what the VoiceMixer gets) bit-exact. Synthetic: one C
// channel at +50 kHz in a 1.2 Msps capture, Eb/N0 about 12 dB.
// With DECT2_INMSAT_CAPTURE set (cf32 I/Q, DECT2_INMSAT_RATE, default 10 Msps), the manager runs on that real capture and prints every carrier
// it decodes as a C channel with the voice frames it handed on (the reference: JAERO on the same capture, docs/INMARSAT.md).
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_demod.h"
#include "dect2/aero/aero_phy.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
using namespace dect2::inmsat;

#include "../common/test_util.h"

static void realCapture(const char* path) {
    const double rate = getenv("DECT2_INMSAT_RATE") ? atof(getenv("DECT2_INMSAT_RATE")) : 10e6;
    const double secs = getenv("DECT2_INMSAT_SECONDS") ? atof(getenv("DECT2_INMSAT_SECONDS")) : 40;
    FILE* f = fopen(path, "rb");
    if (!f) { printf("real capture: cannot open %s\n", path); return; }
    Manager m;
    ManagerConfig c; c.rate = rate; c.centerHz = 1546.153e6; c.budget = 24;
    m.configure(c);
    std::map<int, uint64_t> sinkFrames;
    m.setVoiceSink([&](int id, double, const std::vector<uint8_t>& fr, double) { if (fr.size() == 12) sinkFrames[id]++; });
    const size_t chunk = 1 << 18;
    std::vector<cf32> buf(chunk);
    size_t total = (size_t)(secs * rate), done = 0;
    while (done < total) {
        const size_t got = fread(buf.data(), sizeof(cf32), std::min(chunk, total - done), f);
        if (!got) break;
        m.feed(buf.data(), got);
        done += got;
    }
    fclose(f);
    Snapshot s;
    m.snapshot(s, 0);
    printf("real capture %s: %.0f s, %zu carriers, %zu windows, cost %.1f of %.1f\n", path, secs, s.carriers.size(), s.windows.size(), s.cpuCost, s.cpuBudget);
    for (const auto& r : s.carriers) {
        if (r.type == CarrierType::Unknown && !r.voiceCandidate && r.state == 0 && r.snrDb < 15) continue;
        printf("  %.4f MHz  width %5.1f kHz  snr %4.1f dB  %-14s rate %5d  state %d  Eb/N0 %4.1f dB  frames %5llu  voice frames %5llu (sink %llu)%s%s\n", r.freqHz / 1e6, r.widthHz / 1e3,
               r.snrDb, carrierTypeName(r.type), r.bitRate, r.state, r.ebn0Db, (unsigned long long)r.frames,
               (unsigned long long)r.voiceFrames, (unsigned long long)sinkFrames[r.id], r.voiceCandidate ? "  candidate" : "", r.parked ? "  parked" : "");
    }
    printf("  messages %llu, SUs %llu ok / %llu bad, state %d; satellite: valid %d, name '%s', confidence %.2f, summary: %s\n", (unsigned long long)s.messagesTotal,
           (unsigned long long)s.susOk, (unsigned long long)s.susBad, s.state, (int)s.sat.valid, s.sat.name.c_str(), s.sat.confidence, s.sat.summary.c_str());
    for (const auto& cnd : s.sat.ranked)
        printf("    candidate index %d: confidence %.2f, az %.0f el %.0f, visible %d, %zu pieces of evidence\n", cnd.index, cnd.confidence, cnd.azimuthDeg, cnd.elevationDeg, (int)cnd.visible, cnd.evidence.size());
}

int main() {
    // ---- synthetic: one C channel in a 1.2 Msps capture ----
    const double fs = 1.2e6, cfo = 50e3, secs = 14;
    std::mt19937 g(21);
    AeroFrameEncoder enc(8400);
    AeroModulator mod(8400, fs, 0);
    std::vector<std::vector<uint8_t>> sentVoice;       // 12-byte frames in order
    Manager m;
    ManagerConfig c; c.rate = fs; c.centerHz = 1546.0e6; c.budget = 6;
    m.configure(c);
    std::vector<std::vector<uint8_t>> got;
    std::set<int> ids;
    m.setVoiceSink([&](int id, double freqHz, const std::vector<uint8_t>& fr, double) {
        ids.insert(id);
        got.push_back(fr);
        static bool said = false;
        if (!said) { said = true; printf("first voice frame from carrier %d at %.4f MHz\n", id, freqHz / 1e6); }
    });
    const AeroFrameFormat* fmt = aeroFrameFormat(8400);
    const double ebn0Db = 12;
    const double sigma = std::sqrt(fs / (2.0 * 8400 * std::pow(10.0, ebn0Db / 10)));
    genutil::NoiseSource noise(5);
    const size_t total = (size_t)(secs * fs), chunk = 65536;
    std::vector<cf32> buf(chunk);
    double ph = 0;
    for (size_t done = 0; done < total;) {
        while (mod.queuedBits() < 2u * fmt->totalBits()) {
            std::vector<uint8_t> su(kAeroCSuBytes, 0), v(kAeroCVoiceBytes);
            for (int k = 0; k < 3; k++) { for (int i = 0; i < 10; i++) su[12 * k + i] = (uint8_t)g(); su[12 * k] = 0x30; aeroSuSetCrc(&su[12 * k]); }
            for (auto& b : v) b = (uint8_t)g();
            for (int k = 0; k < kAeroCVoiceFrames; k++) sentVoice.emplace_back(v.begin() + 12 * k, v.begin() + 12 * (k + 1));
            const auto bits = enc.cFrame(su.data(), v.data());
            mod.pushBits(bits.data(), bits.size());
        }
        const size_t n = std::min(chunk, total - done);
        mod.generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            buf[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.05f;       // a weak carrier in a quiet capture
            ph += 2 * M_PI * cfo / fs;
            if (ph > M_PI) ph -= 2 * M_PI;
        }
        noise.add(buf.data(), n, (float)(sigma * 0.05));
        m.feed(buf.data(), n);
        done += n;
    }
    Snapshot s;
    CHECK(m.snapshot(s, 0), "a snapshot");
    const CarrierRow* row = nullptr;
    for (const auto& r : s.carriers) if (std::fabs(r.offsetHz - cfo) < 3000) row = &r;
    CHECK(row != nullptr, "the C carrier is in the table (%zu carriers)", s.carriers.size());
    if (row) {
        printf("carrier: offset %.0f Hz, width %.1f kHz, class %s, type %s, state %d, Eb/N0 %.1f dB, frames %llu, voice frames %llu\n", row->offsetHz, row->widthHz / 1e3,
               carrierClassName(row->cls), carrierTypeName(row->type), row->state, row->ebn0Db, (unsigned long long)row->frames, (unsigned long long)row->voiceFrames);
        CHECK(row->cls == CarrierClass::Wide, "the detector calls it wide");
        CHECK(row->type == CarrierType::AeroC && row->state >= 2 && row->bitRate == 8400, "decoded as an Aero C channel");
        CHECK(row->frames >= 10, "frames %llu", (unsigned long long)row->frames);
        CHECK(!row->voiceCandidate, "not a mere candidate any more");
    }
    CHECK(ids.size() == 1, "voice from one carrier (%zu)", ids.size());
    // every frame the sink got is one the modulator sent, in order
    size_t matched = 0, pos = 0;
    for (const auto& fr : got) {
        while (pos < sentVoice.size() && sentVoice[pos] != fr) pos++;
        if (pos < sentVoice.size()) { matched++; pos++; }
    }
    printf("voice frames: %zu sent, %zu received, %zu matched in order\n", sentVoice.size(), got.size(), matched);
    CHECK(got.size() >= 250 && matched == got.size(), "voice frames reach the sink bit-exact (%zu of %zu)", matched, got.size());
    if (const char* path = getenv("DECT2_INMSAT_CAPTURE")) realCapture(path);
    return testSummary("inmsat_cchannel");
}
