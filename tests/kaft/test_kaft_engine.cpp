// KAFT in the engine's wrapper and the transmit session's generator: the transmit generator (the TX path) makes the one-way signal from a file,
// KaftLiveReceiver (what the engine feeds) finds the frames, follows the session, checks the hash and saves the file to a folder. All three widths,
// a carrier offset, noise, blocks lost in the middle of the first pass and made up by the second, a resume across two sessions, the repeat that is
// ignored, hostile file names from the air, and a TxSession writing an IQ file that the receiver decodes. Also: the mode is registered.
#include "dect2/kaft/kaft_live.h"
#include "dect2/kaft/kaft_tel.h"
#include "dect2/kaft/kaft_txgen.h"
#include "dect2/kaft/kaft_frame.h"
#include "dect2/kaft/kaft_session.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqfile.h"
#include "dect2/source/iq_file.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>
using namespace dect2;
namespace fs = std::filesystem;
#include "../common/cpu_report.h"
#include "../common/cpu_time.h"
#include "../common/test_util.h"
#include "../common/ci_slack.h"

static std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::vector<cf32> generate(const TxRequest& req, double rate, std::string& err) {
    auto g = makeKaftTxGenerator(req, rate, err);
    std::vector<cf32> all;
    if (!g) return all;
    std::vector<cf32> blk(16384);
    while (!g->finished() && all.size() < (size_t)(rate * 900)) {
        g->generate(blk.data(), blk.size());
        all.insert(all.end(), blk.begin(), blk.end());
    }
    return all;
}

struct Result {
    KaftTelemetry t;
    double secSignal = 0, secWork = 0;
};

// Feeds the signal (offset, noise, a blanked stretch when asked for) to `rx` and returns its last telemetry.
static Result feedAll(KaftLiveReceiver& rx, const std::vector<cf32>& sig, double rate, double cfoHz, double esn0Db, double blankFrom = 0, double blankTo = 0) {
    Result r;
    uint64_t seq = 0;
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double pSig = 0.35 * 0.35;
    // Es/N0 = P fs / (symRate sigma^2); the symbol rate is taken from the first frame's width: callers pass a rate-normalised value through esn0Db
    const float sigma = std::isfinite(esn0Db) ? (float)std::sqrt(pSig / std::pow(10.0, esn0Db / 10.0) / 2.0) : 0.f;   // per component, for a noise power of P/10^(x/10) in the sample rate
    std::vector<cf32> blk;
    const double t0 = processCpuSecs();
    double ph = 0;
    const double dph = 2 * 3.141592653589793 * cfoHz / rate;
    const size_t b0 = (size_t)(blankFrom * (double)sig.size()), b1 = (size_t)(blankTo * (double)sig.size());
    for (size_t pos = 0; pos < sig.size() + (size_t)(3 * rate); pos += 16384) {
        blk.assign(16384, cf32(0.f, 0.f));
        for (size_t i = 0; i < blk.size(); i++) {
            const size_t k = pos + i;
            if (k < sig.size() && !(k >= b0 && k < b1)) blk[i] = sig[k];
            if (cfoHz != 0) { blk[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += dph; }
            if (sigma > 0) blk[i] += cf32(sigma * nd(rng), sigma * nd(rng));
        }
        rx.feed(blk.data(), blk.size());
        KaftTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.t = t; }
    }
    r.secWork = (processCpuSecs() - t0);   // CPU seconds of this process
    r.secSignal = (double)sig.size() / rate;
    return r;
}


// A one-pass transmission built straight from the protocol API with a file name of our choosing (200 kHz, P2), at `rate`.
static std::vector<cf32> handmade(const std::string& name, const std::vector<uint8_t>& data, double rate) {
    using namespace dect2::kaft;
    const Width w = Width::W200k;
    const long long fsw = (long long)kSps * widthInfo(w).symRate, r = std::llround(rate), g = std::gcd(fsw, r);
    std::vector<cf32> out;
    auto put = [&](const std::vector<cf>& x4) {
        const std::vector<cf> y = resample(x4, (int)(r / g), (int)(fsw / g));
        out.insert(out.end(), (size_t)(0.02 * rate), cf32(0.f, 0.f));
        for (const cf& v : y) out.push_back(v * 0.35f);
        out.insert(out.end(), (size_t)(0.02 * rate), cf32(0.f, 0.f));
    };
    const Hash32 h = hashBytes(0, data.data(), data.size());
    OpenMsg o;
    o.session = 77; o.callTx = "ONAIR"; o.callRx = "CQ"; o.width = (uint8_t)w; o.fileName = name;
    o.payloadSize = data.size(); o.origSize = data.size(); o.hash = h; o.startProfile = 2; o.blockSize = (uint16_t)dataBytesPerBlock(2);
    put(buildFrame(frameForCtrl(encode(o), false, 77, 0, w, "ONAIR")));
    StartMsg st; st.session = 77; st.profile = 2;
    put(buildFrame(frameForCtrl(encode(st), false, 77, 1, w, "ONAIR")));
    TxBurst tb;
    tb.session = 77; tb.burstNo = 0; tb.profile = 2; tb.width = w;
    const size_t cap = (size_t)dataBytesPerBlock(2);
    const int nb = maxBlocksPerFrame(2, w);
    for (size_t off = 0; off < data.size();) {
        std::vector<DataBlock> fr;
        for (int k = 0; k < nb && off < data.size(); k++) {
            DataBlock b;
            b.offset = (uint32_t)off;
            b.data.assign(data.begin() + (long)off, data.begin() + (long)std::min(data.size(), off + cap));
            off += b.data.size();
            fr.push_back(std::move(b));
        }
        tb.frames.push_back(std::move(fr));
    }
    put(buildBurst(framesForBurst(tb, "ONAIR")));
    FinishMsg f; f.session = 77; f.payloadSize = data.size(); f.hash = h; f.origHash = h;
    put(buildFrame(frameForCtrl(encode(f), false, 77, 2, w, "ONAIR")));
    return out;
}

static std::vector<uint8_t> randomBytes(size_t n, unsigned seed) {
    std::vector<uint8_t> v(n);
    std::mt19937 rng(seed);
    for (auto& b : v) b = (uint8_t)rng();
    return v;
}

static void writeFile(const std::string& p, const std::vector<uint8_t>& d) {
    std::ofstream o(p, std::ios::binary);
    o.write((const char*)d.data(), (std::streamsize)d.size());
}

int main() {
    // ---- registration: tuning, the transmit table, family numbers
    {
        const ModeTuning* t = modeTuning(127);
        CHECK(t && t->id == std::string("kaft") && t->name == std::string("KAFT") && t->stdMode == 127, "tuning of engine standard 127");
        if (t) CHECK(t->minSampleRate == 1.25e6 && std::fabs(t->bandwidthMhz - 1.0) < 1e-9, "KAFT needs 1.25 Msps for a 1 MHz channel");
        CHECK(modeTuningById("kaft") == t, "modeTuningById");
        const TxModeSpec* m = txModeById("kaft");
        CHECK(m && m->stdMode == 127 && txPayloadKind("kaft") == TxPayloadKind::Kaft, "transmit table row");
        CHECK(txModeForList(125, "") == m, "family 125 maps to the KAFT transmit row");
        CHECK(m && txModeFinite(*m), "KAFT is a finite transmission");
        CHECK(kaftMinRateHz(0) == 1.25e6 && kaftMinRateHz(1) == 0.625e6 && kaftMinRateHz(2) == 0.25e6, "minimum rates");
    }

    const fs::path dir = fs::temp_directory_path() / "onair_kaft_engine_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "in");
    const std::string out = (dir / "out").string();

    // ---- a file at each width, clean: saved byte for byte, the repeat ignored
    struct W { int width; int profile; double rate; };
    for (const W w : {W{2, 2, 1.25e6}, W{1, 4, 2.4e6}, W{0, 3, 2.4e6}}) {
        const std::vector<uint8_t> data = randomBytes(w.width == 2 ? 3000 : 20000, 5 + (unsigned)w.width);
        const std::string src = (dir / "in" / ("f" + std::to_string(w.width) + ".bin")).string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.callsign = "ly2abc";
        rq.file = src;
        rq.opt["kaft.width"] = w.width;
        rq.opt["kaft.profile"] = w.profile;
        rq.opt["kaft.repeat"] = 2;
        std::string err;
        const std::vector<cf32> sig = generate(rq, w.rate, err);
        CHECK(!sig.empty(), "generator width %d: %s", w.width, err.c_str());
        KaftLiveReceiver rx;
        rx.setSaveFolder(out);
        rx.configure(w.rate);
        const Result r = feedAll(rx, sig, w.rate, 0, INFINITY);
        printf("width %d P%d at %.2f Msps: %.1f s of signal, CPU %.1f s, state %d, data frames %llu, blocks %llu/%llu, files %zu, session %d, estimate %.1f s\n", w.width, w.profile, w.rate / 1e6,
               r.secSignal, r.secWork, r.t.state, (unsigned long long)r.t.framesData, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, r.t.files.size(), r.t.session,
               kaftAirtimeSeconds(data.size(), w.profile, w.width, 2));
        CHECK(r.t.files.size() == 1, "width %d: %zu files (the repeat must be ignored)", w.width, r.t.files.size());
        if (!r.t.files.empty()) {
            const KaftFile& f = r.t.files[0];
            CHECK(f.saved && f.name == fs::path(src).filename().string() && f.bytes == data.size(), "width %d: file entry (%s, %llu bytes, %s)", w.width, f.name.c_str(), (unsigned long long)f.bytes, f.note.c_str());
            CHECK(f.saved && readFile(f.path) == data, "width %d: the saved file is identical", w.width);
        }
        CHECK(r.t.session == 3 && r.t.width == w.width && r.t.profile == w.profile && r.t.callsign == "LY2ABC", "width %d: session %d, width %d, profile %d, call %s", w.width, r.t.session, r.t.width, r.t.profile, r.t.callsign.c_str());
        CHECK(r.t.blocksBad == 0, "width %d: %llu bad blocks", w.width, (unsigned long long)r.t.blocksBad);
        CHECK(std::fabs(kaftAirtimeSeconds(data.size(), w.profile, w.width, 2) - r.secSignal) < 0.25 * r.secSignal, "width %d: estimate %.1f s against %.1f s made", w.width, kaftAirtimeSeconds(data.size(), w.profile, w.width, 2), r.secSignal);
    }

    // ---- impairments: a carrier offset and noise (Es/N0 6 dB; P2 is specified at 1.5 dB), 200 kHz
    {
        const std::vector<uint8_t> data = randomBytes(3000, 21);
        const std::string src = (dir / "in" / "noisy.bin").string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.file = src;
        rq.opt["kaft.width"] = 2; rq.opt["kaft.profile"] = 2; rq.opt["kaft.repeat"] = 2;
        std::string err;
        const double rate = 1.25e6;
        const std::vector<cf32> sig = generate(rq, rate, err);
        for (double cfo : {-200.0, 150.0, 2000.0, -2100.0}) {   // 5 ppm at 440 MHz is 2.2 kHz, the limit of root 7 at 200 kHz
            // Es/N0 = (P / Pn) x (rate / symbol rate): for 6 dB at 160 ksym/s the signal-to-noise ratio over the whole sample rate is 6 - 10log10(rate / 160e3)
            const double snrSample = 6.0 - 10.0 * std::log10(rate / 160e3);
            KaftLiveReceiver rx;
            rx.setSaveFolder((dir / "out_noisy").string());
            rx.configure(rate);
            const Result r = feedAll(rx, sig, rate, cfo, snrSample);
            printf("200 kHz, cfo %+.0f Hz, Es/N0 6 dB: files %zu, blocks %llu/%llu, measured Es/N0 %.1f dB\n", cfo, r.t.files.size(), (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, (double)r.t.esN0Db);
            CHECK(!r.t.files.empty() && r.t.files[0].saved && readFile(r.t.files[0].path) == data, "cfo %+.0f Hz with noise: file not delivered intact (%zu files)", cfo, r.t.files.size());
        }
    }

    // ---- 1 MHz width: carrier offsets up to +-5 kHz (about 5 ppm at 1 GHz) with noise (Es/N0 12 dB at 800 ksym/s)
    {
        const std::vector<uint8_t> data = randomBytes(6000, 71);
        const std::string src = (dir / "in" / "wide.bin").string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.file = src;
        rq.opt["kaft.width"] = 0; rq.opt["kaft.profile"] = 2; rq.opt["kaft.repeat"] = 2;
        std::string err;
        const double rate = 2.4e6;
        const std::vector<cf32> sig = generate(rq, rate, err);
        for (double cfo : {5000.0, -5000.0, 3000.0}) {
            KaftLiveReceiver rx;
            rx.setSaveFolder((dir / "out_wide").string());
            rx.configure(rate);
            const Result r = feedAll(rx, sig, rate, cfo, 12.0 - 10.0 * std::log10(rate / 800e3));
            printf("1 MHz, cfo %+.0f Hz, Es/N0 12 dB: files %zu, measured cfo %.0f Hz, blocks %llu/%llu\n", cfo, r.t.files.size(), (double)r.t.cfoHz, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
            CHECK(!r.t.files.empty() && r.t.files[0].saved && readFile(r.t.files[0].path) == data, "1 MHz with %+.0f Hz and noise: file not delivered intact", cfo);
        }
    }

    // ---- speed: 10 s of a 2.4 Msps signal (a 1 MHz transfer, repeated) must be processed in well under real time
    {
        const std::vector<uint8_t> data = randomBytes(30000, 81);
        const std::string src = (dir / "in" / "speed.bin").string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.file = src;
        rq.opt["kaft.width"] = 0; rq.opt["kaft.profile"] = 3; rq.opt["kaft.repeat"] = 1;
        std::string err;
        const double rate = 2.4e6;
        const std::vector<cf32> one = generate(rq, rate, err);
        std::vector<cf32> sig;
        while ((double)sig.size() < 10.0 * rate) {
            sig.insert(sig.end(), one.begin(), one.end());
            sig.insert(sig.end(), (size_t)(0.3 * rate), cf32(0.f, 0.f));
        }
        sig.resize((size_t)(10.0 * rate));
        KaftLiveReceiver rx;
        rx.setSaveFolder((dir / "out_speed").string());
        rx.configure(rate);
        const Result r = feedAll(rx, sig, rate, 0, INFINITY);
        const double cpuPerSec = r.secWork / (r.secSignal + 3.0 * 0.0);
        printf("speed: %.1f s of signal at 2.4 Msps: %.2f s of CPU (%.2f x real time), files %zu\n", r.secSignal, r.secWork, cpuPerSec, r.t.files.size());
        cpuCal("kaft_engine", "10 s at 2.4 Msps", r.secWork, "cpu-s", '<', 5.0 * ciSlack());
        CHECK(r.secWork < 5.0 * ciSlack(), "10 s of 2.4 Msps took %.2f s of CPU, the limit is %.1f s", r.secWork, 5.0 * ciSlack());
        CHECK(!r.t.files.empty(), "the speed run still delivers the file");
    }

    // ---- blocks lost in the first pass are made up by the second
    {
        const std::vector<uint8_t> data = randomBytes(12000, 31);
        const std::string src = (dir / "in" / "holes.bin").string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.file = src;
        rq.opt["kaft.width"] = 2; rq.opt["kaft.profile"] = 2; rq.opt["kaft.repeat"] = 2;
        std::string err;
        const double rate = 1.25e6;
        const std::vector<cf32> sig = generate(rq, rate, err);
        KaftLiveReceiver rx;
        rx.setSaveFolder((dir / "out_holes").string());
        rx.configure(rate);
        const Result r = feedAll(rx, sig, rate, 0, INFINITY, 0.22, 0.34);
        printf("holes: %.1f s of signal, files %zu, frames with a failed block %llu, held %d\n", r.secSignal, r.t.files.size(), (unsigned long long)r.t.framesBad, r.t.heldSessions);
        CHECK(!r.t.files.empty() && r.t.files[0].saved && readFile(r.t.files[0].path) == data, "the second pass completes the file (%zu files)", r.t.files.size());
        // the same signal with the second pass blanked as well: the file must NOT be delivered (no false success)
        KaftLiveReceiver rx2;
        rx2.setSaveFolder((dir / "out_holes2").string());
        rx2.configure(rate);
        std::vector<cf32> cut(sig.begin(), sig.begin() + (long)(sig.size() / 2));   // only the first pass
        const Result r2 = feedAll(rx2, cut, rate, 0, INFINITY, 0.2, 0.35);
        CHECK(r2.t.files.empty(), "a file with missing blocks is never delivered (%zu files)", r2.t.files.size());
    }

    // ---- a resume: the first transfer is cut off, a second session of the same file continues it
    {
        const std::vector<uint8_t> data = randomBytes(40000, 41);
        const std::string src = (dir / "in" / "resume.bin").string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.file = src;
        rq.opt["kaft.width"] = 2; rq.opt["kaft.profile"] = 3; rq.opt["kaft.repeat"] = 1;
        rq.opt["kaft.session"] = 5;
        std::string err;
        const double rate = 1.25e6;
        const std::vector<cf32> a = generate(rq, rate, err);
        rq.opt["kaft.session"] = 6;
        const std::vector<cf32> b = generate(rq, rate, err);
        KaftLiveReceiver rx;
        rx.setSaveFolder((dir / "out_resume").string());
        rx.configure(rate);
        std::vector<cf32> cutA(a.begin(), a.begin() + (long)(a.size() * 55 / 100));   // the link is lost in the middle of the data
        const Result r1 = feedAll(rx, cutA, rate, 0, INFINITY);
        CHECK(r1.t.files.empty() && r1.t.session == 2 && r1.t.haveBytes > 0 && r1.t.haveBytes < data.size(), "after the cut: session %d, %llu of %zu bytes", r1.t.session, (unsigned long long)r1.t.haveBytes, data.size());
        const Result r2 = feedAll(rx, b, rate, 0, INFINITY);
        printf("resume: first part held %llu bytes, then resumed %llu bytes, files %zu\n", (unsigned long long)r1.t.haveBytes, (unsigned long long)r2.t.resumedBytes, r2.t.files.size());
        CHECK(r2.t.resumedBytes > 0 && r2.t.sessionId == 6, "the second session resumed (%llu bytes, session %u)", (unsigned long long)r2.t.resumedBytes, (unsigned)r2.t.sessionId);
        CHECK(!r2.t.files.empty() && r2.t.files[0].saved && readFile(r2.t.files[0].path) == data, "the resumed file is identical");
    }

    // ---- no folder chosen: the file is checked but not kept; a hostile name is cleaned
    {
        std::vector<uint8_t> data = randomBytes(1500, 51);
        const std::string src = (dir / "in" / "x.bin").string();
        writeFile(src, data);
        TxRequest rq;
        rq.modeId = "kaft";
        rq.file = src;
        rq.opt["kaft.width"] = 2; rq.opt["kaft.profile"] = 2; rq.opt["kaft.repeat"] = 1;
        std::string err;
        const std::vector<cf32> sig = generate(rq, 1.25e6, err);
        KaftLiveReceiver rx;
        rx.configure(1.25e6);   // no folder
        const Result r = feedAll(rx, sig, 1.25e6, 0, INFINITY);
        CHECK(r.t.files.size() == 1 && !r.t.files[0].saved && r.t.files[0].path.empty() && r.t.files[0].note.find("no folder") != std::string::npos, "no folder: not saved");
        // a hostile name from the air (made with the protocol API, the transmitter never makes one): path parts, a leading dot, a device name
        const std::vector<cf32> sig2 = handmade("../../evil/..\\.hidden:name?.sh", data, 1.25e6);
        KaftLiveReceiver rx2;
        rx2.setSaveFolder((dir / "out_names").string());
        rx2.configure(1.25e6);
        const Result r2 = feedAll(rx2, sig2, 1.25e6, 0, INFINITY);
        CHECK(r2.t.files.size() == 1 && r2.t.files[0].saved, "the hostile-name file is received (%zu files)", r2.t.files.size());
        if (r2.t.files.size() == 1) {
            const fs::path pth = r2.t.files[0].path;
            printf("hostile name saved as %s\n", pth.string().c_str());
            CHECK(pth.parent_path() == fs::path((dir / "out_names").string()), "the file stays inside the chosen folder");
            const std::string fn = pth.filename().string();
            CHECK(fn.find("..") == std::string::npos && fn.find('/') == std::string::npos && fn.find('\\') == std::string::npos && fn.find(':') == std::string::npos && fn[0] != '.', "the name is clean: %s", fn.c_str());
            CHECK(readFile(pth.string()) == data, "content intact");
        }
        // the same file again from a later transfer: never overwritten, a number is added
        KaftLiveReceiver rx3;
        rx3.setSaveFolder((dir / "out_names").string());
        rx3.configure(1.25e6);
        const Result r3 = feedAll(rx3, sig2, 1.25e6, 0, INFINITY);
        CHECK(r3.t.files.size() == 1 && r3.t.files[0].saved && r3.t.files[0].path != r2.t.files[0].path && readFile(r2.t.files[0].path) == data, "an existing file is not overwritten (%s)", r3.t.files.empty() ? "" : r3.t.files[0].path.c_str());
    }

    // ---- a TxSession writing an IQ file (the destination of the TX panel), read back and decoded
    {
        const std::vector<uint8_t> data = randomBytes(2500, 61);
        const std::string src = (dir / "in" / "session.bin").string();
        writeFile(src, data);
        IqFileTxConfig c;
        c.format = IqFileFormat::SigmfCf32;
        c.paced = false;
        c.centerHz = 1290e6;
        c.path = iqFilePath(dir.string(), "kaft", c.format);
        c.counters = std::make_shared<IqFileTxCounters>();
        TxRequest tr;
        tr.modeId = "kaft";
        tr.file = src;
        tr.loop = true;
        tr.tune.centerHz = 1290e6;
        tr.tune.sampleRate = 1.25e6;
        tr.opt["kaft.width"] = 2; tr.opt["kaft.profile"] = 2; tr.opt["kaft.repeat"] = 1;
        TxSession s;
        std::string err;
        CHECK(s.start(makeIqFileTxSink(c), tr, {}, err), "session: %s", err.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        while (s.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool ended = !s.running();
        s.stop();
        CHECK(ended, "the session ended by itself after one pass");
        IqFileInfo info;
        CHECK(probeIqFile(c.path, info, err) && std::fabs(info.sampleRate - 1.25e6) < 1 && std::fabs(info.centerHz - 1290e6) < 1, "probe: %s", err.c_str());
        std::vector<cf32> iq;
        {
            std::ifstream f(info.dataPath, std::ios::binary);
            f.seekg(0, std::ios::end);
            const size_t n = (size_t)f.tellg() / sizeof(cf32);
            f.seekg(0);
            iq.resize(n);
            f.read((char*)iq.data(), (std::streamsize)(n * sizeof(cf32)));
        }
        KaftLiveReceiver rx;
        rx.setSaveFolder((dir / "out_session").string());
        rx.configure(1.25e6);
        const Result r = feedAll(rx, iq, 1.25e6, 0, INFINITY);
        printf("IQ file: %zu samples (%.1f s), files %zu\n", iq.size(), (double)iq.size() / 1.25e6, r.t.files.size());
        CHECK(!r.t.files.empty() && r.t.files[0].saved && readFile(r.t.files[0].path) == data, "the IQ file decodes to the file");
    }

    // ---- refusals in plain words
    {
        std::string err;
        TxRequest base;
        base.modeId = "kaft";
        TxRequest bad = base;
        bad.callsign = "TOOLONGCALL";
        CHECK(!makeKaftTxGenerator(bad, 2.4e6, err) && !err.empty(), "a callsign of more than 6 characters is refused");
        bad = base; bad.opt["kaft.profile"] = 10;
        CHECK(!makeKaftTxGenerator(bad, 2.4e6, err), "profile 10 is refused");
        bad = base; bad.opt["kaft.width"] = 0;
        CHECK(!makeKaftTxGenerator(bad, 1.0e6, err) && !err.empty(), "1 MHz at 1 Msps is refused");
        bad = base; bad.file = "/nonexistent/none.bin";
        CHECK(!makeKaftTxGenerator(bad, 2.4e6, err), "a missing file is refused");
    }
    fs::remove_all(dir);
    return testSummary("kaft_engine");
}
