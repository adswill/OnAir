// Diversity pair source: two fake radios (the same narrowband signal, independent noise, one started 18 ms after the other, a frequency offset
// between them) are combined into one stream. Checks the start-up alignment, the in-band SNR gain against radio A alone, the single-radio
// fall-back when one member fails, the saved-pair string format (old strings still load) and the radio-list entry.
#include "dect2/dsp/fftutil.h"
#include "dect2/source/diversity_source.h"
#include "dect2/common/stable_random.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;

constexpr int kLen = 1 << 18;
constexpr double kFs = 250e3, kBw = 12e3, kOff = 20e3;

static std::vector<cf32> makeSignal(unsigned seed, std::vector<char>& band) {
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> S(kLen);
    band.assign(kLen, 0);
    for (int k = 0; k < kLen; k++) {
        const double f = (k < kLen / 2 ? k : k - kLen) * kFs / kLen;
        if (std::fabs(f - kOff) < kBw / 2) { S[k] = cf32(nd(g), nd(g)); band[k] = 1; }
    }
    Fft fft(kLen);
    fft.inverse(S.data());
    double p = 0; for (auto& v : S) p += std::norm(v);
    const float sc = (float)std::sqrt(kLen / p);
    for (auto& v : S) v *= sc;
    return S;
}

// a radio that emits the shared signal (plus its own noise) from signal index `start`, shifted by cfoHz, in paced blocks
struct FakeRadio : IqSource {
    const std::vector<cf32>* sig; long first; double cfo, sigma, rate; unsigned seed; bool failStart;
    std::thread th; std::atomic<bool> run{false};
    FakeRadio(const std::vector<cf32>* s, long st, double c, double sg, double r, unsigned sd, bool fail = false) : sig(s), first(st), cfo(c), sigma(sg), rate(r), seed(sd), failStart(fail) {}
    ~FakeRadio() override { stop(); }
    bool start_(IqRing& ring) {
        run = true;
        th = std::thread([this, &ring] {
            std::mt19937 g(seed); dect2::normal_distribution<float> nd(0.f, 1.f);
            long i = 0; std::vector<cf32> blk(5000);
            while (run) {
                for (auto& v : blk) {
                    const long j = first + i;
                    const double ph = 2 * kPi * cfo * (double)i / rate;
                    v = (*sig)[(size_t)(((j % kLen) + kLen) % kLen)] * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32((float)(sigma * nd(g)), (float)(sigma * nd(g)));
                    i++;
                }
                ring.write(blk.data(), blk.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));   // 1 Msps of a 250 ksps stream: 4x real time
            }
        });
        return true;
    }
    bool start(const TuneSettings&, IqRing& ring, std::string& err) override { if (failStart) { err = "no such radio"; return false; } return start_(ring); }
    void stop() override { run = false; if (th.joinable()) th.join(); }
    bool retune(const TuneSettings&, std::string&) override { return true; }
    double sampleRate() const override { return rate; }
};

static DeviceInfo dev(const char* name, const char* serial) {
    DeviceInfo d; d.kind = DeviceInfo::Soapy; d.board = "rtlsdr"; d.name = name; d.serial = serial; d.minRateHz = 225e3; d.maxRateHz = 2.56e6; return d;
}

// in-band SNR of y against the clean signal: y is located in the periodic signal by cross-correlation, fitted by one complex constant
static double snrAgainst(const std::vector<cf32>& y, const std::vector<cf32>& s, const std::vector<char>&) {
    const int L = 1 << 16;
    if ((int)y.size() < L) return -99;
    Fft fft(kLen);
    std::vector<cf32> Y(kLen), S(s);
    for (int i = 0; i < L; i++) Y[i] = y[y.size() - L + i];
    fft.forward(Y.data()); fft.forward(S.data());
    for (int k = 0; k < kLen; k++) Y[k] = Y[k] * std::conj(S[k]);   // correlation of the segment with the signal; the carrier offset is zero for radio A
    fft.inverse(Y.data());
    int t0 = 0; float pk = 0;
    for (int n = 0; n < kLen; n++) { const float v = std::abs(Y[n]); if (v > pk) { pk = v; t0 = n; } }
    // y[i] ~ s[(t0 - ... )]: the cross-correlation peak at lag n means y[i] ~ s[i - n]... pick by direct test on both signs
    double best = -99;
    for (int sign : {1, -1}) {
        const long off = sign * (long)t0;
        cf32 num{0, 0}; double den = 0;
        for (int i = 0; i < L; i++) { const cf32 sv = s[(size_t)((((long)i - off) % kLen + kLen) % kLen)]; num += y[y.size() - L + i] * std::conj(sv); den += std::norm(sv); }
        const cf32 al = num / (float)den;
        std::vector<cf32> e(L), sg(L);
        for (int i = 0; i < L; i++) { const cf32 sv = s[(size_t)((((long)i - off) % kLen + kLen) % kLen)]; sg[i] = al * sv; e[i] = y[y.size() - L + i] - sg[i]; }
        Fft f2(L);
        f2.forward(e.data()); f2.forward(sg.data());
        double pe = 0, ps = 0;
        for (int k = 0; k < L; k++) {
            const double f = (k < L / 2 ? k : k - L) * kFs / L;
            if (std::fabs(f - kOff) < kBw / 2 - 600) { pe += std::norm(e[k]); ps += std::norm(sg[k]); }
        }
        best = std::max(best, 10 * std::log10(ps / pe));
    }
    return best;
}

static std::vector<cf32> drain(IqRing& r, std::vector<cf32>& out) {
    std::vector<cf32> buf(1 << 14);
    for (;;) { const size_t n = r.read(buf.data(), buf.size()); if (!n) break; out.insert(out.end(), buf.begin(), buf.begin() + (long)n); }
    return out;
}

int main() {
    std::vector<char> band;
    const std::vector<cf32> sig = makeSignal(5, band);
    const double sigma = std::sqrt(std::pow(10.0, -0.0 / 10) * (kFs / kBw) / 2.0);   // 0 dB SNR in the band, per component

    // ---- saved pair strings
    {
        SynthGroupDef g; g.name = "Pair 1"; g.diversity = true; g.diversityMode = 0; g.childRateHz = 1e6;
        g.members = {{"rtlsdr", "0001", "RTL A"}, {"rtlsdr", "0002", "RTL B"}};
        SynthGroupDef w; w.name = "Wide"; w.centerHz = 100e6; w.spanHz = 8e6;
        w.members = {{"rtlsdr", "1", "x"}, {"rtlsdr", "2", "y"}};
        const std::string s = synthGroupsToString({w, g});
        const auto v = synthGroupsFromString(s);
        CHECK(v.size() == 2, "two groups back (%zu)", v.size());
        if (v.size() == 2) {
            CHECK(!v[0].diversity && v[1].diversity && v[1].diversityMode == 0 && v[1].members.size() == 2 && v[1].name == "Pair 1", "pair fields round-trip");
        }
        // a string written before pairs existed still loads, and is not a pair
        const auto old = synthGroupsFromString("Old;100000000;8000000;0;rtlsdr,1,x|rtlsdr,2,y\n");
        CHECK(old.size() == 1 && !old[0].diversity, "old string loads as a wide group");
        // a pair with three members is refused
        const auto bad = synthGroupsFromString("Bad;0;0;0;a,1,x|a,2,y|a,3,z;d1\n");
        CHECK(bad.empty(), "a pair needs exactly two radios");
        // the radio list entry
        setSynthGroups({g});
        std::vector<DeviceInfo> found = {dev("RTL A", "0001"), dev("RTL B", "0002")};
        found[0].board = found[1].board = "rtlsdr";
        const auto ents = synthGroupDevices(found);
        CHECK(ents.size() == 1 && ents[0].name.find("Diversity: Pair 1") == 0 && ents[0].kind == DeviceInfo::Group && ents[0].board == "diversitygroup", "list entry");
        setSynthGroups({});
    }

    // ---- two radios, B started 18 ms (4500 samples) later and 2.1 kHz off: the pair gains SNR over radio A alone
    {
        SynthGroupDef def; def.name = "T1"; def.diversity = true; def.diversityMode = 1; def.childRateHz = kFs;
        std::vector<DeviceInfo> mem = {dev("A", "a"), dev("B", "b")};
        long startB = -4500;
        ChildFactory f = [&](const DeviceInfo& d) -> std::unique_ptr<IqSource> {
            if (d.serial == "a") return std::make_unique<FakeRadio>(&sig, 30000, 0.0, sigma, kFs, 11);
            return std::make_unique<FakeRadio>(&sig, 30000 + startB, 2100.0, sigma, kFs, 12);
        };
        DiversitySource src(def, mem, f);
        IqRing ring(1u << 22);
        TuneSettings ts; ts.sampleRate = kFs; ts.centerHz = 100e6;
        std::string err;
        CHECK(src.start(ts, ring, err), "pair starts (%s)", err.c_str());
        std::vector<cf32> out;
        DiversitySourceStatus st;
        bool combining = false;
        for (int i = 0; i < 120 && !combining; i++) {   // up to 12 s
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            drain(ring, out);
            st = src.status();
            combining = st.combining;
        }
        printf("state: %s  aligned=%d combining=%d cfo=%.0f Hz lag=%d\n", st.state.c_str(), st.aligned, st.combining, st.dsp.cfoHz, st.dsp.lagSamples);
        CHECK(st.aligned, "start-up alignment found the delay");
        CHECK(combining, "pair locks (%s)", st.state.c_str());
        CHECK(std::fabs(std::fabs(st.dsp.cfoHz) - 2100.0) < 100, "frequency offset found (%.0f)", st.dsp.cfoHz);
        // let it run, then compare the SNR of the last stretch with radio A's own input SNR (0 dB in band)
        out.clear();
        for (int i = 0; i < 40; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); drain(ring, out); }
        const double snr = snrAgainst(out, sig, band);
        printf("combined SNR %.2f dB against 0 dB for each radio alone (ideal +3.0)\n", snr);
        CHECK(snr > 1.5, "the pair gains more than 1.5 dB over one radio (%.2f)", snr);
        src.setMode(0);
        CHECK(diversityStatus("T1", st) && st.mode == 0, "mode switch is live");
        src.stop();
        CHECK(!diversityStatus("T1", st), "stopped pair is gone from the registry");
    }

    // ---- one member fails to start: the other passes through, with a plain note
    {
        SynthGroupDef def; def.name = "T2"; def.diversity = true; def.childRateHz = kFs;
        std::vector<DeviceInfo> mem = {dev("A", "a"), dev("B", "b")};
        ChildFactory f = [&](const DeviceInfo& d) -> std::unique_ptr<IqSource> {
            return std::make_unique<FakeRadio>(&sig, 1000, 0.0, 0.0, kFs, d.serial == "a" ? 1u : 2u, d.serial == "b");
        };
        DiversitySource src(def, mem, f);
        IqRing ring(1u << 20);
        TuneSettings ts; ts.sampleRate = kFs;
        std::string err;
        CHECK(src.start(ts, ring, err), "starts with one radio");
        CHECK(err.find("only one radio") != std::string::npos, "says so: %s", err.c_str());
        std::vector<cf32> out;
        for (int i = 0; i < 10; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); drain(ring, out); }
        CHECK(out.size() > 100000, "radio A passes through (%zu samples)", out.size());
        double e = 0; size_t n = std::min<size_t>(out.size(), 50000);
        // samples are A's exactly (sigma 0, no CFO): signal index 1000 + i, less the DC removal (a slow mean)
        for (size_t i = 20000; i < n; i++) e += std::norm(out[i] - sig[(1000 + i) % kLen]);
        CHECK(e / (double)(n - 20000) < 0.01, "pass-through carries the signal (err %.4f)", e / (double)(n - 20000));
        src.stop();
    }

    // ---- neither starts: an error
    {
        SynthGroupDef def; def.name = "T3"; def.diversity = true; def.childRateHz = kFs;
        std::vector<DeviceInfo> mem = {dev("A", "a"), dev("B", "b")};
        ChildFactory f = [&](const DeviceInfo&) -> std::unique_ptr<IqSource> { return std::make_unique<FakeRadio>(&sig, 0, 0.0, 0.0, kFs, 1u, true); };
        DiversitySource src(def, mem, f);
        IqRing ring(1u << 18);
        TuneSettings ts; ts.sampleRate = kFs;
        std::string err;
        CHECK(!src.start(ts, ring, err) && err.find("neither radio") != std::string::npos, "both failing is an error: %s", err.c_str());
    }

    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
