// KAWS in the engine's wrapper and the transmit session's generator: the transmit generator makes the one-way upper-sideband signal from a chat message
// or a beacon, KawsLiveReceiver (what the engine feeds) follows the UTC slot grid (a test clock here), finds the woven-lattice preamble, decodes the blocks
// and lists the message. Profiles P0 and P3 at an easy SNR with a carrier offset of +-50 Hz, noise only (no false message), a beacon, and a TxSession
// writing IQ files (250 ksps SigMF and 12 ksps SigMF / WAV) that are read back and decoded. Also: the mode is registered (tuning, transmit table).
// The channel is white Gaussian noise and a carrier offset: no fading. KAWS has not been tested on a real HF path.
#include "dect2/kaws/kaws_live.h"
#include "dect2/kaws/kaws_tel.h"
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/kaws/kaws_phy.h"
#include "dect2/kaws/kaws_sched.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqfile.h"
#include "dect2/source/iq_file.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>
#include <vector>
using namespace dect2;
namespace fs = std::filesystem;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

static constexpr double kBase = 1.8e9;   // a multiple of 240 s: slot index parity and every slot grid line up

// Mean power of the burst of a request at 12 kHz (the silence around it left out).
static double burstPower(const TxRequest& req) {
    std::string err;
    auto g = makeKawsTxGenerator(req, 12000, err);
    if (!g) return 0;
    std::vector<cf32> all, blk(12000);
    while (!g->finished() && all.size() < 12000u * 400) { g->generate(blk.data(), blk.size()); all.insert(all.end(), blk.begin(), blk.end()); }
    size_t a = 0, b = all.size();
    while (a < b && std::abs(all[a]) < 1e-4f) a++;
    while (b > a && std::abs(all[b - 1]) < 1e-4f) b--;
    double p = 0;
    for (size_t i = a; i < b; i++) p += std::norm(all[i]);
    return b > a ? p / (double)(b - a) : 0;
}

struct Run {
    KawsTelemetry t;
    double cpu = 0;
};

// The request's signal at `rate` (plus `ifHz` of intermediate frequency and `cfoHz` of carrier offset) with noise at snrDb in 2.4 kHz, into the receiver on the UTC
// grid; the stream starts 3 s before the first slot window... then runs until the slot of `profile` has been searched.
static Run run(const TxRequest& req, int profile, double rate, double ifHz, double cfoHz, double snrDb, bool withSignal = true, double extraSec = 0) {
    Run out;
    std::string err;
    auto gen = makeKawsTxGenerator(req, rate, err);
    CHECK(gen != nullptr, "generator: %s", err.c_str());
    if (!gen) return out;
    const double ps = burstPower(req);
    const double sigma = std::isfinite(snrDb) ? std::sqrt(ps * rate / (2400.0 * std::pow(10.0, snrDb / 10.0)) / 2.0) : 0.0;
    KawsLiveReceiver rx;
    double clockVal = kBase - 3.0;
    rx.setClock([&clockVal] { return clockVal; });
    rx.configure(rate);
    rx.setSignalOffset(ifHz);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    const int slotSec = kaws::kProfiles[profile].slotSec;
    const double streamSec = 3.0 + slotSec + 6.0 + extraSec;
    const size_t total = (size_t)(streamSec * rate);
    const double dph = 2 * 3.141592653589793 * (ifHz + cfoHz) / rate;
    double ph = 0;
    std::vector<cf32> blk(8192);
    uint64_t seq = 0;
    bool started = false;
    const double c0 = processCpuSecs();
    size_t pos = 0;
    // the first 3 s of the stream are noise only (the transmit generator's own start, at kBase, comes after them)
    const size_t lead = (size_t)(3.0 * rate);
    while (pos < total) {
        const size_t n = std::min(blk.size(), total - pos);
        std::vector<cf32> sig(n, cf32(0.f, 0.f));
        if (pos >= lead && withSignal) gen->generate(sig.data(), n);
        else if (pos < lead && pos + n > lead && withSignal) gen->generate(sig.data() + (lead - pos), n - (lead - pos));
        for (size_t i = 0; i < n; i++) {
            cf32 v = sig[i];
            if (ifHz + cfoHz != 0) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += dph; }
            if (sigma > 0) v += cf32((float)sigma * nd(rng), (float)sigma * nd(rng));
            blk[i] = v;
        }
        rx.feed(blk.data(), n);
        if (!started) { started = true; clockVal = kBase - 3.0 + 0.01; }
        pos += n;
    }
    rx.flush();
    std::fill(blk.begin(), blk.end(), cf32(0.f, 0.f));
    for (int k = 0; k < 40; k++) rx.feed(blk.data(), blk.size());   // a report after the searches
    KawsTelemetry t;
    for (int k = 0; k < 3; k++) if (rx.telemetry(t, seq)) { seq = t.seq; out.t = t; }
    out.cpu = (processCpuSecs() - c0);
    return out;
}

static TxRequest chatReq(int profile, const std::string& text) {
    TxRequest r;
    r.modeId = "kaws";
    r.callsign = "ly1abc";
    r.grid = "ko24rm";
    r.dest = "dl2xyz";
    r.text = text;
    r.opt["kaws.kind"] = 0;
    r.opt["kaws.profile"] = profile;
    r.opt["kaws.repeat"] = 1;
    r.opt["kaws.utc"] = kBase;
    r.opt["kaws.msgid"] = 5;
    return r;
}

static bool readAll(const std::string& path, std::vector<char>& d) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    d.resize((size_t)f.tellg());
    f.seekg(0);
    f.read(d.data(), (std::streamsize)d.size());
    return true;
}

int main() {
    // ---- registration: tuning, the transmit table, family numbers
    {
        const ModeTuning* t = modeTuning(135);
        CHECK(t && t->id == std::string("kaws") && t->name == std::string("KAWS") && t->stdMode == 135, "tuning of engine standard 135");
        if (t) CHECK(t->minMhz < 1.9 && t->maxMhz > 29 && t->minSampleRate == 250000 && std::fabs(t->bandwidthMhz - 0.0024) < 1e-9, "HF mode, 2.4 kHz wide, 250 ksps at least");
        CHECK(modeTuningById("kaws") == t, "modeTuningById");
        std::set<std::string> ids;
        std::set<int> stds;
        for (int s = 0; s < 400; s++) {
            const ModeTuning* m = modeTuning(s);
            if (!m || m->stdMode != s) continue;
            CHECK(ids.insert(m->id).second, "mode id %s is unique", m->id);
            CHECK(stds.insert(m->stdMode).second, "engine standard %d is unique", m->stdMode);
        }
        const TxModeSpec* m = txModeById("kaws");
        CHECK(m && m->stdMode == 135 && txPayloadKind("kaws") == TxPayloadKind::Kaws, "transmit table row");
        CHECK(txModeForList(133, "") == m, "family 133 maps to the KAWS transmit row");
        CHECK(txEngineStd(133) == 135, "family 133 is engine standard 135");
        CHECK(m && txModeFinite(*m), "KAWS is a finite transmission");
    }

    // ---- refusals in plain words
    {
        std::string err;
        TxRequest bad = chatReq(3, "HELLO");
        bad.callsign = "WAYTOOLONGCALL";
        CHECK(!makeKawsTxGenerator(bad, 12000, err) && !err.empty(), "a callsign of more than 9 characters is refused");
        bad = chatReq(6, "HELLO");
        CHECK(!makeKawsTxGenerator(bad, 12000, err) && err.find("codec") != std::string::npos, "a voice profile is refused for the missing codec: %s", err.c_str());
        bad = chatReq(3, "HELLO");
        bad.opt["kaws.profile"] = 9;
        CHECK(!makeKawsTxGenerator(bad, 12000, err), "profile 9 is refused");
        CHECK(!makeKawsTxGenerator(chatReq(3, "HELLO"), 8000, err), "a rate below 12 kHz is refused");
        bad = chatReq(3, std::string(120, 'A'));
        CHECK(!makeKawsTxGenerator(bad, 12000, err), "a chat message of more than 100 characters is refused");
    }

    // ---- noise only: no message, no block
    {
        const Run r = run(chatReq(3, "HELLO KAWS"), 3, 250000, 0, 0, -3.0, false);
        printf("noise only (P3 grid): messages %llu, blocks %llu/%llu, slots %llu, lattice hits %llu, CPU %.1f s\n", (unsigned long long)r.t.messages, (unsigned long long)r.t.blocksOk,
               (unsigned long long)r.t.blocksBad, (unsigned long long)r.t.slotsTried, (unsigned long long)r.t.latticeHits, r.cpu);
        CHECK(r.t.messages == 0 && r.t.blocksOk == 0 && r.t.beacons == 0, "noise gives no message");
        CHECK(r.t.slotsTried >= 2, "the slot windows were searched (%llu)", (unsigned long long)r.t.slotsTried);
    }

    // ---- P3 chat messages and P0 short beacons, +-50 Hz carrier offset, easy SNR (a P0 chat message is at least two blocks: it takes two slots, 240 s apart)
    struct Case { int profile, kind; double snr, cfo, ifHz; };
    const Case cases[] = {{3, 0, 3.0, 50, 20000}, {3, 0, 3.0, -50, 0}, {0, 1, -8.0, 50, 0}, {0, 1, -8.0, -50, 20000}};
    for (const Case& c : cases) {
        const std::string text = "HELLO KAWS P" + std::to_string(c.profile);
        TxRequest rq = chatReq(c.profile, text);
        rq.opt["kaws.kind"] = c.kind;
        const Run r = run(rq, c.profile, 250000, c.ifHz, c.cfo, c.snr);
        printf("P%d kind %d at %+.0f dB, CFO %+.0f Hz, IF %.0f: state %d, messages %llu, beacons %llu, blocks %llu/%llu, SNR est %.1f dB, CFO est %+.1f, clock err %+.2f s, lattice %d (stat %.1f), CPU %.1f s\n", c.profile,
               c.kind, c.snr, c.cfo, c.ifHz, r.t.state, (unsigned long long)r.t.messages, (unsigned long long)r.t.beacons, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad,
               (double)r.t.snrDb, (double)r.t.cfoHz, r.t.clockErrSec, (int)r.t.latticeSeen[c.profile], (double)r.t.latticeStat[c.profile], r.cpu);
        if (c.kind == 0) {
            CHECK(r.t.messages == 1 && r.t.chats.size() == 1, "P%d: %llu messages", c.profile, (unsigned long long)r.t.messages);
            if (!r.t.chats.empty()) {
                const KawsChat& m = r.t.chats[0];
                CHECK(m.from == "LY1ABC" && m.to == "DL2XYZ" && m.text == text && m.profile == c.profile, "P%d: message %s -> %s '%s'", c.profile, m.from.c_str(), m.to.c_str(), m.text.c_str());
            }
        } else {
            CHECK(r.t.beacons == 1 && r.t.beaconList.size() == 1, "P%d: %llu beacons", c.profile, (unsigned long long)r.t.beacons);
            if (!r.t.beaconList.empty()) CHECK(r.t.beaconList[0].call == "LY1ABC" && r.t.beaconList[0].grid == "KO24RM" && !r.t.beaconList[0].full && r.t.beaconList[0].profile == c.profile, "P%d: beacon", c.profile);
        }
        CHECK(r.t.latticeSeen[c.profile] && r.t.syncLock && r.t.profile == c.profile, "P%d: the preamble indicator", c.profile);
        CHECK(r.t.blocksOk >= 1 && r.t.blocksBad == 0 && r.t.state == 2 && r.t.dataValid, "P%d: blocks %llu good, %llu bad", c.profile, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
        CHECK(std::fabs(std::fabs((double)r.t.cfoHz) - std::fabs(c.cfo)) < 8.0, "P%d: carrier offset estimate %+.1f Hz for %+.0f", c.profile, (double)r.t.cfoHz, c.cfo);
        CHECK(std::fabs(r.t.clockErrSec) < 0.5, "P%d: slot timing %+.2f s", c.profile, r.t.clockErrSec);
        CHECK(r.t.snrDb > c.snr - 6 && r.t.snrDb < c.snr + 6, "P%d: SNR estimate %.1f dB for %+.0f", c.profile, (double)r.t.snrDb, c.snr);
        CHECK(r.t.spectrum.size() == 60, "the audio spectrum has 60 bins");
        CHECK(!r.t.voicePresent && r.t.voiceNote.find("not installed") != std::string::npos, "voice: honest note");
    }

    // ---- a P0 chat message: the first block of two arrives in the first slot (the message completes 240 s later)
    {
        const Run r = run(chatReq(0, "HELLO KAWS P0"), 0, 250000, 0, 30, -8.0);
        printf("P0 chat, first slot: messages %llu, blocks %llu/%llu, lattice %d (stat %.1f), slots %llu\n", (unsigned long long)r.t.messages, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, (int)r.t.latticeSeen[0], (double)r.t.latticeStat[0], (unsigned long long)r.t.slotsTried);
        CHECK(r.t.blocksOk == 1 && r.t.blocksBad == 0 && r.t.messages == 0 && r.t.profile == 0, "P0 chat: the first block alone, no message yet");
    }

    // ---- a two-block P0 chat message: the blocks come in two slots 240 s apart and the message completes with the second
    {
        const Run r = run(chatReq(0, "HELLO KAWS P0"), 0, 250000, 0, 30, INFINITY, true, 240.0);
        printf("P0 chat, both slots: messages %llu, blocks %llu/%llu\n", (unsigned long long)r.t.messages, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
        CHECK(r.t.messages == 1 && !r.t.chats.empty() && r.t.chats[0].text == "HELLO KAWS P0", "P0 chat completes across two slots (%llu messages, %llu blocks)", (unsigned long long)r.t.messages, (unsigned long long)r.t.blocksOk);
    }

    // ---- a full beacon (P3)
    {
        TxRequest f = chatReq(3, "");
        f.opt["kaws.kind"] = 2;
        f.text = "HELLO 73";
        f.lat = 54.6872; f.lon = 25.2797;
        const Run r2 = run(f, 3, 250000, 0, 0, 3.0);
        CHECK(r2.t.beaconList.size() == 1 && r2.t.beaconList[0].full && r2.t.beaconList[0].text == "HELLO 73" && std::fabs(r2.t.beaconList[0].lat - 54.6872) < 1e-3, "full beacon listed");
    }

    // ---- TxSession writing IQ files: 250 ksps SigMF (decoded through the receiver) and 12 ksps SigMF / WAV (decoded through the physical layer)
    {
        const fs::path dir = fs::temp_directory_path() / "kaws_engine_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::string err;
        const TxModeSpec* m = txModeById("kaws");
        CHECK(m != nullptr, "kaws transmit row");
        auto writeFile = [&](IqFileFormat fmt, double rate, const char* name, std::string& path) {
            IqFileTxConfig c;
            c.format = fmt;
            c.paced = false;
            c.centerHz = 14.1e6;
            c.path = iqFilePath(dir.string(), name, fmt);
            c.counters = std::make_shared<IqFileTxCounters>();
            path = c.path;
            TxRequest tr = chatReq(3, "FILE TEST");
            tr.loop = true;
            tr.tune.centerHz = 14.1e6;
            tr.tune.sampleRate = rate;
            TxSession s;
            const bool ok = s.start(makeIqFileTxSink(c), tr, {}, err);
            CHECK(ok, "session: %s", err.c_str());
            if (!ok) return false;
            const auto t0 = std::chrono::steady_clock::now();
            while (s.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const bool ended = !s.running();
            s.stop();
            CHECK(ended, "the session ended by itself");
            return ended;
        };
        std::string p12, p250;
        if (writeFile(IqFileFormat::SigmfCf32, 12000, "kaws12", p12)) {
            IqFileInfo info;
            CHECK(probeIqFile(p12, info, err) && std::fabs(info.sampleRate - 12000) < 1, "probe 12 ksps: %s", err.c_str());
            std::vector<char> d;
            CHECK(readAll(info.dataPath, d), "read the 12 ksps file");
            const size_t n = d.size() / sizeof(cf32);
            const cf32* x = (const cf32*)d.data();
            std::vector<float> audio(n);
            for (size_t i = 0; i < n; i++) audio[i] = x[i].real();   // the audio of an upper-sideband receiver
            // the file starts at the slot grid line... 1.5 s of silence, then the burst: the window starts 1.5 s before the first slot line in the file
            const size_t from = 0;
            std::vector<float> win(audio.begin() + (long)from, audio.end());
            std::mt19937 rng(5);
            std::normal_distribution<float> nd(0.f, 1.f);
            for (float& v : win) v += 0.02f * nd(rng);
            const std::vector<kaws::cf> iq = kaws::fromAudio12k(win);
            const kaws::RxBurst rb = kaws::receiveBurst(3, 1, iq, 0, kaws::kSearchHi);
            kaws::Chat ch;
            kaws::Bits pl;
            uint8_t mi = 0, ix = 0;
            const bool pOk = rb.sync && rb.iters[0] >= 0 && kaws::parseBlock(kaws::kProfiles[3].K, rb.blocks[0], mi, ix, pl);
            CHECK(pOk && mi == 5 && ix == 0, "the 12 ksps SigMF file: block (sync %d)", (int)rb.sync);
            CHECK(pOk && kaws::decodeChat(pl, ch) && ch.text == "FILE TEST" && ch.from == "LY1ABC", "the 12 ksps SigMF file decodes to the message");
            printf("12 ksps SigMF: %zu samples (%.1f s), SNR est %.1f dB\n", n, (double)n / 12000, rb.snrDb);
        }
        if (writeFile(IqFileFormat::WavIq16, 12000, "kaws12w", p12)) {
            std::vector<char> d;
            CHECK(readAll(p12, d) && d.size() > 44, "read the WAV file");
            const int16_t* s = (const int16_t*)(d.data() + 44);
            const size_t n = (d.size() - 44) / 4;
            std::vector<float> audio(n);
            for (size_t i = 0; i < n; i++) audio[i] = (float)s[2 * i] / 32768.f;
            const std::vector<kaws::cf> iq = kaws::fromAudio12k(audio);
            const kaws::RxBurst rb = kaws::receiveBurst(3, 1, iq, 0, kaws::kSearchHi);
            kaws::Chat ch;
            kaws::Bits pl;
            uint8_t mi = 0, ix = 0;
            const bool pOk = rb.sync && rb.iters[0] >= 0 && kaws::parseBlock(kaws::kProfiles[3].K, rb.blocks[0], mi, ix, pl);
            CHECK(pOk && kaws::decodeChat(pl, ch) && ch.text == "FILE TEST", "the 12 ksps WAV IQ file decodes to the message");
        }
        fs::remove_all(dir);
    }
    return testSummary("kaws_engine");
}
