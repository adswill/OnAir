// Audio control (audio_control.h): filter responses, notch depth, EQ gains, squelch hysteresis and hang, CTCSS and DCS detection, AGC level,
// the auto notch, stereo modes, the clarifier shifting a tone, tail delay, settings round trip, device fallback, pass-through when off.
#include "dect2/audio/audio_control.h"
#include "dect2/tones/tones_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::actl;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;
static const float R = 48000.f;

static float toneDb(const std::vector<float>& x, float hz, size_t from = 0) {   // amplitude of one frequency, dBFS (correlation)
    double re = 0, im = 0; const size_t n = x.size() - from;
    for (size_t i = 0; i < n; i++) { const double w = 2 * kPi * hz * (double)i / R; re += x[from + i] * std::cos(w); im += x[from + i] * std::sin(w); }
    return 20.f * std::log10((float)(2 * std::sqrt(re * re + im * im) / (double)n) + 1e-9f);
}
static std::vector<float> runChain(AudioControl& ac, const std::vector<float>& st) {   // interleaved stereo in, out
    std::vector<float> out(st.size()), sc(AudioControl::kMaxChunk * 2);
    const int total = (int)(st.size() / 2);
    for (int off = 0; off < total; off += 512) {
        const int n = std::min(512, total - off);
        const float* o = ac.process(&st[2 * (size_t)off], n, sc.data());
        std::memcpy(&out[2 * (size_t)off], o, (size_t)n * 2 * sizeof(float));
    }
    return out;
}
static std::vector<float> chan(const std::vector<float>& st, int c) { std::vector<float> m(st.size() / 2); for (size_t i = 0; i < m.size(); i++) m[i] = st[2 * i + c]; return m; }
static std::vector<float> sineStereo(float hz, float amp, float sec, float amp2 = -1) {
    std::vector<float> v((size_t)(R * sec) * 2);
    for (size_t i = 0; i < v.size() / 2; i++) { v[2 * i] = amp * (float)std::sin(2 * kPi * hz * i / R); v[2 * i + 1] = (amp2 < 0 ? amp : amp2) * (float)std::sin(2 * kPi * hz * i / R); }
    return v;
}

int main() {
    // filters
    { const BiquadCoef hp = bqHighpass(300, 0.7071f, R), lp = bqLowpass(3000, 0.7071f, R);
      CHECK(std::fabs(bqMagnitudeDb(hp, 300, R) + 3.f) < 0.3f, "hp corner %.2f", bqMagnitudeDb(hp, 300, R));
      CHECK(std::fabs(bqMagnitudeDb(lp, 3000, R) + 3.f) < 0.3f, "lp corner %.2f", bqMagnitudeDb(lp, 3000, R));
      CHECK(bqMagnitudeDb(hp, 30, R) < -38.f && bqMagnitudeDb(hp, 3000, R) > -0.2f, "hp stop/pass");
      CHECK(bqMagnitudeDb(lp, 20000, R) < -30.f && bqMagnitudeDb(lp, 300, R) > -0.2f, "lp stop/pass"); }
    // notch depth
    { const BiquadCoef n = bqNotch(1000, 10, R);
      CHECK(bqMagnitudeDb(n, 1000, R) < -60.f, "notch depth %.1f", bqMagnitudeDb(n, 1000, R));
      CHECK(std::fabs(bqMagnitudeDb(n, 2000, R)) < 0.5f && std::fabs(bqMagnitudeDb(n, 500, R)) < 0.5f, "notch leaves the rest");
      CHECK(bqMagnitudeDb(bqNotch(1000, 40, R), 1100, R) > bqMagnitudeDb(bqNotch(1000, 4, R), 1100, R), "higher Q is narrower"); }
    // EQ
    { float g[kEqBands] = {0, 0, 0, 0, 0};
      for (int f = 50; f < 20000; f = f * 3 / 2) CHECK(std::fabs(eqResponseDb(g, (float)f, R)) < 0.01f, "flat eq at %d", f);
      for (int b = 1; b < 4; b++) { float h[kEqBands] = {0, 0, 0, 0, 0}; h[b] = 9.f; const float r = eqResponseDb(h, kEqHz[b], R); CHECK(std::fabs(r - 9.f) < 0.6f, "band %d gain %.2f", b, r); }
      { float h[kEqBands] = {-9, 0, 0, 0, 0}; CHECK(eqResponseDb(h, 40, R) < -8.f && std::fabs(eqResponseDb(h, 4000, R)) < 0.5f, "low shelf"); }
      { float h[kEqBands] = {0, 0, 0, 0, 6}; CHECK(eqResponseDb(h, 15000, R) > 5.f && std::fabs(eqResponseDb(h, 300, R)) < 0.5f, "high shelf"); }
      float v[kEqBands]; eqPresetGains(EqPreset::Voice, v); CHECK(eqResponseDb(v, 2500, R) > 2.f && eqResponseDb(v, 100, R) < -4.f, "voice preset");
      eqPresetGains(EqPreset::CW, v); CHECK(eqResponseDb(v, 1000, R) > 4.f, "cw preset"); }
    // squelch hysteresis, hang
    { SquelchCore q; q.openDb = -50; q.closeDb = -54; q.hangMs = 100;
      CHECK(!q.update(-60, 10), "closed at -60");
      CHECK(!q.update(-51, 10), "-51 is below the open threshold");
      CHECK(q.update(-49, 10), "opens at -49");
      CHECK(q.update(-52, 10) && q.update(-53, 10), "between close and open stays open (hysteresis)");
      for (int i = 0; i < 9; i++) q.update(-60, 10);
      CHECK(q.open, "hang holds 90 of 100 ms");
      CHECK(q.update(-53, 10), "back above close restarts the hang");
      for (int i = 0; i < 9; i++) q.update(-60, 10);
      CHECK(q.open, "hang restarted");
      q.update(-60, 10); q.update(-60, 10); CHECK(!q.open, "closes after the hang"); }
    // CTCSS
    { std::mt19937 rng(7); std::normal_distribution<float> nd(0.f, 0.1f);
      std::vector<float> x((size_t)R * 1), y = x;
      for (size_t i = 0; i < x.size(); i++) { const float v = nd(rng); x[i] = v + 0.05f * (float)std::sin(2 * kPi * 100.0 * i / R); y[i] = v + 0.05f * (float)std::sin(2 * kPi * 123.0 * i / R); }
      ToneDetector d; d.prepare(R, 100.f); d.process(x.data(), (int)x.size());
      CHECK(d.present(), "ctcss 100 Hz found");
      ToneDetector e; e.prepare(R, 100.f); e.process(y.data(), (int)y.size());
      CHECK(!e.present(), "other tone (123 Hz) is not 100 Hz");
      ToneDetector f; f.prepare(R, 100.f); std::vector<float> nn(x.size()); for (auto& v : nn) v = nd(rng); f.process(nn.data(), (int)nn.size());
      CHECK(!f.present(), "noise alone is not a tone"); }
    // DCS
    { CHECK(dcsWord(23) != dcsWord(25), "codes differ");
      const uint32_t w = dcsWord(23); CHECK((w & 0x1FF) == 023 && ((w >> 9) & 7) == 4, "word layout %x", w);
      CHECK(w == 0x763813u, "D023 is the word real radios use (#479), got %06x", w);
      // #479: what 3.9 transmitted for these codes was read by a handheld as 411, 412, 432, 053, 627. The words now decode as themselves.
      for (int code : {23, 25, 65, 226, 703}) { dect2::tones::DcsHit h; CHECK(dect2::tones::dcsDecode(dcsWord(code), h) && h.code == code && !h.inverted, "D%03dN reads back as itself", code); }
      uint32_t r = w & 0x7FFFFF;   // a codeword leaves no remainder
      for (int b = 22; b >= 11; b--) if (r & (1u << b)) r ^= 0xC75u << (b - 11);
      CHECK((r & 0x7FF) == 0, "golay remainder zero");
      std::mt19937 rng(3); std::normal_distribution<float> nd(0.f, 0.08f);
      auto synth = [&](int code, float sec) {
          std::vector<float> x((size_t)(R * sec)); const uint32_t cw = dcsWord(code);
          for (size_t i = 0; i < x.size(); i++) { const int bit = (int)((double)i * 134.4 / R); x[i] = (((cw >> (bit % 23)) & 1) ? 0.2f : -0.2f) + nd(rng); }
          return x; };
      DcsDetector d; d.prepare(R, 23); const auto a = synth(23, 1.5f); d.process(a.data(), (int)a.size());
      CHECK(d.present(), "dcs 023 found");
      DcsDetector e; e.prepare(R, 23); const auto b = synth(25, 1.5f); e.process(b.data(), (int)b.size());
      CHECK(!e.present(), "dcs 025 is not 023");
      DcsDetector f; f.prepare(R, 23); std::vector<float> nn((size_t)R * 3); for (auto& v : nn) v = nd(rng); f.process(nn.data(), (int)nn.size());
      CHECK(!f.present(), "noise is not dcs"); }
    // AGC
    { Agc a; a.prepare(R); a.setMode(AgcMode::Fast, -18.f);
      for (float amp : {0.02f, 0.5f}) {
          a.env_ = 0; a.gain_ = 1;
          std::vector<float> st = sineStereo(1000, amp, 1.5f); a.process(st.data(), (int)st.size() / 2);
          float pk = 0; for (size_t i = st.size() - 4800; i < st.size(); i++) pk = std::max(pk, std::fabs(st[i]));
          CHECK(std::fabs(20 * std::log10(pk) + 18.f) < 1.5f, "agc fast level from %.2f: %.1f dB", amp, 20 * std::log10(pk)); }
      a.setMode(AgcMode::Slow, -18.f); a.env_ = 0; a.gain_ = 1;
      std::vector<float> st = sineStereo(1000, 0.001f, 1.f); a.process(st.data(), (int)st.size() / 2);   // below the floor: not boosted
      CHECK(a.gain_ < 1.2f, "no boost of the noise floor (%.2f)", a.gain_); }
    // chain: off = same pointer; stereo modes
    { AudioControlConfig cfg; AudioControl ac; ac.setConfig(&cfg); ac.prepare(48000);
      std::vector<float> in = sineStereo(1000, 0.3f, 0.2f, 0.1f), sc(2048); bool act = true;
      CHECK(ac.process(in.data(), 512, sc.data(), &act) == in.data() && !act, "off returns the input itself");
      AudioControlSettings s; s.on = true; s.stereo = StereoMode::Swap; cfg.set(s);
      auto o = runChain(ac, sineStereo(1000, 0.3f, 0.3f, 0.1f));
      CHECK(std::fabs(toneDb(chan(o, 0), 1000, 9600) - 20 * std::log10(0.1f)) < 0.5f && std::fabs(toneDb(chan(o, 1), 1000, 9600) - 20 * std::log10(0.3f)) < 0.5f, "swap");
      s.stereo = StereoMode::Mono; cfg.set(s); ac.reset();
      o = runChain(ac, sineStereo(1000, 0.3f, 0.3f, 0.1f));
      CHECK(std::fabs(toneDb(chan(o, 0), 1000, 9600) - 20 * std::log10(0.2f)) < 0.5f, "mono is the mean");
      s.stereo = StereoMode::Left; cfg.set(s); ac.reset(); o = runChain(ac, sineStereo(1000, 0.3f, 0.3f, 0.1f));
      CHECK(std::fabs(toneDb(chan(o, 1), 1000, 9600) - 20 * std::log10(0.3f)) < 0.5f, "left only"); }
    // chain: band-limit, notch, eq
    { AudioControlConfig cfg; AudioControl ac; ac.setConfig(&cfg); ac.prepare(48000);
      AudioControlSettings s = audioControlDefaults(5); s.on = true; cfg.set(s);   // SSB 300..2700, 24 dB/oct
      auto o = chan(runChain(ac, sineStereo(100, 0.5f, 0.5f)), 0); CHECK(toneDb(o, 100, 12000) < -40.f, "ssb high-pass takes 100 Hz down: %.1f", toneDb(o, 100, 12000));
      ac.reset(); o = chan(runChain(ac, sineStereo(1000, 0.5f, 0.5f)), 0); CHECK(std::fabs(toneDb(o, 1000, 12000) + 6.f) < 0.7f, "1 kHz passes");
      ac.reset(); o = chan(runChain(ac, sineStereo(6000, 0.5f, 0.5f)), 0); CHECK(toneDb(o, 6000, 12000) < -30.f, "6 kHz cut: %.1f", toneDb(o, 6000, 12000));
      s = AudioControlSettings(); s.on = true; s.notch = true; s.notchHz = 1000; s.notchQ = 12; cfg.set(s); ac.reset();
      o = chan(runChain(ac, sineStereo(1000, 0.5f, 1.f)), 0); CHECK(toneDb(o, 1000, 24000) < -55.f, "chain notch depth %.1f", toneDb(o, 1000, 24000));
      s = AudioControlSettings(); s.on = true; s.eq = true; s.eqPreset = EqPreset::Custom; s.eqDb[2] = 12; cfg.set(s); ac.reset();
      o = chan(runChain(ac, sineStereo(1000, 0.1f, 0.5f)), 0); CHECK(std::fabs(toneDb(o, 1000, 12000) - (-20.f + 12.f)) < 1.f, "chain eq +12 dB: %.1f", toneDb(o, 1000, 12000)); }
    // auto notch: a steady whistle goes away, noise stays
    { AudioControlConfig cfg; AudioControl ac; ac.setConfig(&cfg); ac.prepare(48000);
      AudioControlSettings s; s.on = true; s.autoNotch = true; cfg.set(s);
      std::mt19937 rng(11); std::normal_distribution<float> nd(0.f, 0.05f);
      std::vector<float> in((size_t)R * 3 * 2);
      for (size_t i = 0; i < in.size() / 2; i++) { const float v = 0.2f * (float)std::sin(2 * kPi * 1234.0 * i / R) + nd(rng); in[2 * i] = in[2 * i + 1] = v; }
      auto o = chan(runChain(ac, in), 0), ic = chan(in, 0);
      const float before = toneDb(ic, 1234, 96000), after = toneDb(o, 1234, 96000);
      CHECK(before - after > 20.f, "auto notch depth %.1f dB", before - after);
      double pn = 0, pi = 0; for (size_t i = 96000; i < o.size(); i++) pn += o[i] * o[i]; for (size_t i = 96000; i < ic.size(); i++) pi += ic[i] * ic[i];
      CHECK(pn > 0.5 * 0.0025 * (double)(o.size() - 96000) && pi > pn, "noise is kept (%.4f)", pn / (double)(o.size() - 96000)); }
    // clarifier: 1000 Hz moves to 1300 Hz
    { AudioControlConfig cfg; AudioControl ac; ac.setConfig(&cfg); ac.prepare(48000);
      AudioControlSettings s; s.on = true; s.shiftHz = 300; cfg.set(s);
      auto o = chan(runChain(ac, sineStereo(1000, 0.4f, 1.f)), 0);
      const float at1300 = toneDb(o, 1300, 12000), at1000 = toneDb(o, 1000, 12000);
      CHECK(at1300 > -9.f && at1300 - at1000 > 25.f, "shift 1000->1300: %.1f vs %.1f", at1300, at1000);
      s.shiftHz = 0; s.cwToneHz = 900; cfg.set(s); ac.reset();   // 700 Hz receiver tone -> 900 Hz
      o = chan(runChain(ac, sineStereo(700, 0.4f, 1.f)), 0);
      CHECK(toneDb(o, 900, 12000) > -9.f, "cw pitch 700->900: %.1f", toneDb(o, 900, 12000)); }
    // squelch in the chain: SNR and level, tail delay
    { AudioControlConfig cfg; AudioControl ac; ac.setConfig(&cfg); ac.prepare(48000);
      std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 0.2f);
      std::vector<float> noise((size_t)R * 2 * 1), voice = noise;
      for (size_t i = 0; i < noise.size() / 2; i++) { noise[2 * i] = noise[2 * i + 1] = nd(rng); const float v = 0.3f * (float)(std::sin(2 * kPi * 500.0 * i / R) + 0.6 * std::sin(2 * kPi * 1500.0 * i / R)); voice[2 * i] = voice[2 * i + 1] = v + 0.01f * nd(rng); }
      AudioControlSettings s; s.on = true; s.sqKind = SquelchKind::Snr; s.sqOpenDb = 8; s.sqHangMs = 50; cfg.set(s);
      auto o = runChain(ac, noise); double pn = 0; for (size_t i = 24000; i < o.size(); i++) pn += o[i] * o[i];
      CHECK(!ac.squelchOpen() && pn / (double)(o.size() - 24000) < 1e-6, "snr squelch holds noise off");
      o = runChain(ac, voice); CHECK(ac.squelchOpen(), "snr squelch opens on a voice-like signal (measure %.1f)", cfg.statSqMeasure.load());
      s.sqKind = SquelchKind::Level; s.sqOpenDb = -30; s.sqHystDb = 3; cfg.set(s); ac.reset();
      std::vector<float> quiet = sineStereo(1000, 0.001f, 0.5f), loud = sineStereo(1000, 0.3f, 0.5f);
      runChain(ac, quiet); CHECK(!ac.squelchOpen(), "level squelch closed on quiet");
      runChain(ac, loud); CHECK(ac.squelchOpen(), "level squelch open on loud");
      // tail elimination: a voice-like signal, then noise at the carrier drop. The residual (everything but the voice tones) played just after
      // the drop is the noise burst that gets through before the squelch closes; the tail delay must remove most of it.
      auto residual = [&](const std::vector<float>& o) {   // power of the second difference (high-pass) over the 58 ms after the drop: noise, not voice
          double e = 0; const size_t a0 = 24000, a1 = 24000 + 2800;
          for (size_t i = a0; i < a1; i++) { const double d = o[2 * i] - 2.0 * o[2 * (i - 1)] + o[2 * (i - 2)]; e += d * d; }
          return e / (double)(a1 - a0); };
      std::vector<float> sig(voice.begin(), voice.begin() + 48000);   // 0.5 s of voice, then 0.5 s of noise
      sig.insert(sig.end(), noise.begin(), noise.begin() + 48000);
      s = AudioControlSettings(); s.on = true; s.sqKind = SquelchKind::Snr; s.sqOpenDb = 8; s.sqHangMs = 0; s.sqTailMs = 0; cfg.set(s); ac.reset();
      const double r0 = residual(runChain(ac, sig));
      s.sqTailMs = 60; cfg.set(s); ac.reset();
      const double r1 = residual(runChain(ac, sig));
      CHECK(r0 > 0.005 && r1 < 0.25 * r0, "tail elimination: residual %.5f -> %.5f", r0, r1); }
    // settings and devices
    { AudioControlSettings s; s.on = true; s.hp = true; s.hpHz = 250; s.eq = true; s.eqPreset = EqPreset::Custom; s.eqDb[3] = 4.5f; s.sqKind = SquelchKind::Ctcss; s.ctcssHz = 88.5f;
      s.volume = 0.37f; s.device = "USB Audio"; s.recordOnlyOpen = true; s.cwToneHz = 650; s.notchQ = 18;
      const AudioControlSettings t = audioControlFromString(audioControlToString(s), AudioControlSettings());
      CHECK(t.on && t.hp && t.hpHz == 250 && t.eqDb[3] == 4.5f && t.sqKind == SquelchKind::Ctcss && t.ctcssHz == 88.5f && std::fabs(t.volume - 0.37f) < 1e-6 && t.device == "USB Audio" &&
            t.recordOnlyOpen && t.cwToneHz == 650 && t.notchQ == 18, "settings round trip");
      const AudioControlSettings u = audioControlFromString("hpf=99999;vol=77;st=9;junk;=;sk=-3", AudioControlSettings());
      CHECK(u.hpHz <= 8000 && u.volume <= 2 && (int)u.stereo <= 4 && (int)u.sqKind >= 0, "bad values are clamped");
      const std::vector<std::string> dev = {"", "Speakers", "USB Audio"};
      CHECK(audioPickDevice(dev, "USB Audio") == "USB Audio" && audioPickDevice(dev, "Unplugged DAC").empty() && audioPickDevice(dev, "").empty(), "device fallback"); }
    // spectrum
    { std::vector<float> x(1024); for (size_t i = 0; i < x.size(); i++) x[i] = 0.5f * (float)std::sin(2 * kPi * 3000.0 * i / R);
      float db[512]; spectrumDb(x.data(), 1024, db); int pk = 0; for (int i = 1; i < 512; i++) if (db[i] > db[pk]) pk = i;
      CHECK(std::abs(pk - (int)(3000.0 / R * 1024)) <= 1 && db[pk] > -9.f && db[pk] < -4.f, "spectrum peak bin %d, %.1f dB", pk, db[pk]); }
    // meter
    { Meter m; std::vector<float> x = sineStereo(1000, 1.0f, 0.05f); m.process(x.data(), (int)x.size() / 2); CHECK(m.clipped > 0 && m.peakDb() > -0.1f, "clip counted");
      Meter q; std::vector<float> y = sineStereo(1000, 0.1f, 0.05f); q.process(y.data(), (int)y.size() / 2); CHECK(q.clipped == 0 && std::fabs(q.rmsDb() + 23.f) < 0.6f, "rms %.1f", q.rmsDb()); }
    if (fails) { printf("audio_control: %d FAILED\n", fails); return 1; }
    printf("audio_control: ok\n");
    return 0;
}
