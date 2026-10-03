// Command-line channel scanner:  dect2scan [--from MHz] [--to MHz] [--step MHz] [--bw MHz] [--quick] [--lna dB] [--vga dB] [--noamp]
#include "dect2/scanner.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>
using namespace dect2;
int main(int argc, char** argv) {
    ScanConfig c;
    c.tune.lnaDb = 32; c.tune.vgaDb = 20; c.tune.ampOn = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--from") c.startMHz = nx(); else if (a == "--to") c.stopMHz = nx(); else if (a == "--step") c.stepMHz = nx();
        else if (a == "--bw") { c.bwMhz = nx(); c.autoBandwidth = false; } else if (a == "--quick") c.identifyServices = false;
        else if (a == "--lna") c.tune.lnaDb = (int)nx(); else if (a == "--vga") c.tune.vgaDb = (int)nx(); else if (a == "--noamp") c.tune.ampOn = false;
        else if (a == "--occ") c.occupancyDb = nx();
    }
    std::string err;
    auto list = listRadios(err);
    if (list.empty()) { fprintf(stderr, "no radio found (HackRF or SoapySDR)\n"); return 1; }
    if (list[0].kind == DeviceInfo::Soapy) {
        if (list[0].maxRateHz > 0) c.tune.sampleRate = std::min(c.tune.sampleRate, list[0].maxRateHz);
        c.tune.gainDb = std::max(list[0].gainMinDb, list[0].gainMaxDb * 0.6);
    }
    Scanner s;
    if (!s.start(list[0], c, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    size_t shown = 0;
    while (s.progress().running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto r = s.results();
        for (; shown < r.size(); shown++) {
            auto& x = r[shown];
            printf("%7.1f MHz  level %6.1f dBFS  occ %+5.1f dB  %-9s", x.freqMHz, x.levelDbfs, x.occupancyDb, x.t2 ? "DVB-T2" : x.occupied ? "signal" : "empty");
            if (x.t2) printf("  %s  SNR %.1f dB  %s  net 0x%04X \"%s\"", x.mode.c_str(), x.snrDb, x.plpInfo.c_str(), x.networkId, x.networkName.c_str());
            else if (x.occupied) printf("  %s", x.note.c_str());
            printf("\n");
            for (auto& sv : x.services) printf("            %s\n", sv.c_str());
            fflush(stdout);
        }
    }
    return 0;
}
