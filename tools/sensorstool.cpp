// Runs the Sub-GHz sensors receiver over a recording (.cu8 / .cs8 IQ from rtl_433's test set) or a bit code and prints the decoded
// messages as rtl_433 JSON lines, the way `rtl_433 -F json -r file` does (tools/sensors_vectors.py compares them with the expected files).
//   sensorstool [-R n[,n...]] [-Y classic|minmax|auto|level=dB|minlevel=dB] [-s rate] file.cu8
//   sensorstool [-R n[,n...]] -y "{N}hex..."
#include "dect2/sensors/sensors_rx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string file, code;
    std::vector<unsigned> only;
    int fskMode = 2;
    float level = 0, minlevel = -12.1442f;
    bool haveLevel = false, haveMin = false;
    uint32_t rate = 0;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-R" && i + 1 < argc) { std::stringstream ss(argv[++i]); std::string t; while (std::getline(ss, t, ',')) only.push_back((unsigned)std::atoi(t.c_str())); }
        else if (a == "-Y" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "classic") fskMode = 0; else if (v == "minmax") fskMode = 1; else if (v == "auto") fskMode = 2;
            else if (v.rfind("level=", 0) == 0) { level = (float)std::atof(v.c_str() + 6); haveLevel = true; }
            else if (v.rfind("minlevel=", 0) == 0) { minlevel = (float)std::atof(v.c_str() + 9); haveMin = true; }
        }
        else if (a == "-s" && i + 1 < argc) { std::string v = argv[++i]; double m = 1; if (!v.empty() && (v.back() == 'k' || v.back() == 'K')) { m = 1e3; v.pop_back(); } else if (!v.empty() && (v.back() == 'M')) { m = 1e6; v.pop_back(); } rate = (uint32_t)(std::atof(v.c_str()) * m); }
        else if (a == "-y" && i + 1 < argc) code = argv[++i];
        else if (a == "-l") list = true;
        else file = a;
    }
    dect2::SensorsReceiver rx;
    if (list) {
        for (const auto& p : dect2::SensorsReceiver::protocols()) std::printf("%u\t%d\t%s\n", p.num, p.defaultEnabled ? 1 : 0, p.name.c_str());
        return 0;
    }
    if (!only.empty()) rx.setProtocolsOnly(only);
    rx.setFskMode(fskMode);
    if (haveLevel) rx.setFixedLevelDb(level);
    if (haveMin) rx.setMinLevelDb(minlevel);
    rx.setJsonCallback([](const std::string& j) { std::printf("%s\n", j.c_str()); });
    if (!code.empty()) { std::vector<std::string> out; rx.decodeCode(code, &out); for (const auto& j : out) std::printf("%s\n", j.c_str()); return 0; }
    if (file.empty()) { std::fprintf(stderr, "usage: sensorstool [-R n] [-Y mode] [-s rate] file.cu8 | -y code | -l\n"); return 2; }
    if (!rate) {
        rate = 250000;
        const size_t us = file.rfind('_');
        // the last _NNNk / _NNNM in the name (rtl_433's file name convention)
        size_t pos = 0;
        while ((pos = file.find('_', pos)) != std::string::npos) {
            size_t e = pos + 1;
            while (e < file.size() && (std::isdigit((unsigned char)file[e]) || file[e] == '.')) e++;
            if (e > pos + 1 && e < file.size() && (file[e] == 'k' || file[e] == 'M') && (e + 1 == file.size() || file[e + 1] == '.' || file[e + 1] == '_')) {
                const double v = std::atof(file.substr(pos + 1, e - pos - 1).c_str());
                if (file[e] == 'k') rate = (uint32_t)(v * 1e3); else if (v < 40) { /* a frequency in MHz is written with M too; rates are written in k */ if (file[e] == 'M' && v >= 0.25 && v <= 3) rate = (uint32_t)(v * 1e6); }
            }
            pos = e;
        }
        (void)us;
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", file.c_str()); return 1; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string ext = file.substr(file.rfind('.') + 1);
    if (ext == "cs8") for (auto& b : d) b = (uint8_t)(b + 128);
    else if (ext == "cs16") { std::fprintf(stderr, "cs16 is not supported\n"); return 3; }
    const size_t block = 262144;   // bytes, as rtl_433 reads files
    for (size_t o = 0; o < d.size(); o += block) {
        const size_t n = std::min(block, d.size() - o) / 2;
        rx.feedCu8(d.data() + o, n, rate);
    }
    rx.flush();
    return 0;
}
