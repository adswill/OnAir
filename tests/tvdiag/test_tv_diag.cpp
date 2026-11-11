#include "dect2/t2/tv_diag.h"
#include <cstdio>
#include <random>
using namespace dect2::tvdiag;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
// a profile: noise floor around -60 dB with Gaussian jitter, peaks at given (sample, dB), tauMin offset
static std::vector<float> prof(int n, std::vector<std::pair<int, float>> pk, unsigned seed, float floorDb = -60) {
    std::mt19937 g(seed);
    std::normal_distribution<float> nd(0, 1.5f);
    std::vector<float> v((size_t)n);
    for (auto& x : v) x = floorDb + nd(g);
    for (auto& p : pk) { v[(size_t)p.first] = p.second; if (p.first > 0) v[(size_t)p.first - 1] = p.second - 8; if (p.first + 1 < n) v[(size_t)p.first + 1] = p.second - 8; }
    return v;
}
int main() {
    const double fs = 9.142857e6;   // 8 MHz DVB-T native rate
    const double us = 1e6 / fs;
    // clean
    auto r = findEchoes(prof(256, {{100, 0}}, 1), -50, fs, 64);
    check(r.echoes.empty() && r.verdict.find("clean") != std::string::npos, "clean channel");
    // harmless echo inside guard: 20 samples later, -12 dB
    for (unsigned seed = 1; seed <= 20; seed++) {
        r = findEchoes(prof(256, {{100, 0}, {120, -12}}, seed), -50, fs, 64);
        check(r.echoes.size() == 1 && r.echoes[0].kind == EchoKind::InsideGuard, "inside-guard echo found, across noise seeds");
        check(std::fabs(r.echoes[0].delayUs - 20 * us) < 1e-9 && std::fabs(r.echoes[0].levelDb + 12) < 1e-3, "delay and level");
        check(r.fix.empty(), "no fix needed inside the guard");
    }
    check(std::fabs(r.echoes[0].extraKm - 20 * us * 0.299792458) < 1e-6, "extra path km");
    // beyond guard: 80 samples > 64
    r = findEchoes(prof(256, {{60, 0}, {140, -14}}, 3), -50, fs, 64);
    check(r.echoes.size() == 1 && r.echoes[0].kind == EchoKind::BeyondGuard && r.verdict.find("beyond the guard") != std::string::npos && !r.fix.empty(), "beyond guard flagged");
    check(std::fabs(r.worstBeyondGuardDb + 14) < 1e-3, "worst beyond-guard level");
    // pre-echo: strong path before the main one
    r = findEchoes(prof(256, {{150, 0}, {110, -6}}, 4), -50, fs, 64);
    check(r.echoes.size() == 1 && r.echoes[0].kind == EchoKind::PreEcho && r.echoes[0].delayUs < 0 && r.verdict.find("Pre-echo") == 0, "pre-echo flagged");
    check(std::fabs(r.preEchoDb + 6) < 1e-3, "pre-echo level");
    // weak pre-echo (-30 dB) is listed but not the verdict
    r = findEchoes(prof(256, {{150, 0}, {110, -30}}, 5), -50, fs, 64);
    check(r.echoes.size() == 1 && r.verdict.find("Pre-echo") != 0, "weak pre-echo does not drive the verdict");
    // below the floor: -40 dB echo ignored with default minDb -35
    r = findEchoes(prof(256, {{100, 0}, {120, -40}}, 6), -50, fs, 64);
    check(r.echoes.empty(), "echo below the threshold ignored");
    // several echoes sorted strongest first, skirt of the main path not an echo
    r = findEchoes(prof(256, {{100, 0}, {130, -20}, {150, -9}, {170, -30}}, 7), -50, fs, 64);
    check(r.echoes.size() == 3 && r.echoes[0].levelDb > r.echoes[1].levelDb && r.echoes[1].levelDb > r.echoes[2].levelDb, "sorted strongest first, main skirt ignored");
    check(findEchoes({}, 0, fs, 64).verdict.find("No channel") == 0 && findEchoes(prof(256, {{1, 0}}, 1), 0, 0, 64).echoes.empty(), "degenerate input");
    // carrier dips
    std::mt19937 g(9);
    std::normal_distribution<float> nd(0, 0.6f);
    std::vector<float> snr(600);
    for (auto& x : snr) x = 24 + nd(g);
    for (int i = 300; i < 303; i++) snr[(size_t)i] -= 14;   // narrow interferer
    auto d = findCarrierDips(snr, 522.0, 0.0134);
    check(d.size() == 1 && d[0].narrow && d[0].depthDb > 10, "narrow interferer found alone");
    check(std::fabs(d[0].freqMhz - (522.0 + (301 - 299.5) * 0.0134)) < 0.03, "interferer frequency");
    for (int i = 100; i < 130; i++) snr[(size_t)i] -= 9;    // broad notch
    d = findCarrierDips(snr, 522.0, 0.0134);
    bool broad = false, nar = false;
    for (auto& x : d) { if (!x.narrow) broad = true; else nar = true; }
    check(broad && nar, "broad notch told apart from a narrow interferer");
    std::vector<float> flat(600, 20.f);
    check(findCarrierDips(flat, 522, 0.01).empty() && findCarrierDips({1, 2, 3}, 1, 1).empty(), "flat channel and tiny input give nothing");
    // tilt (slow slope) is not a dip
    std::vector<float> tilt(600);
    for (int i = 0; i < 600; i++) tilt[(size_t)i] = 30.f - 0.02f * (float)i;
    check(findCarrierDips(tilt, 522, 0.01).empty(), "a gentle tilt is not a dip");
    std::printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
