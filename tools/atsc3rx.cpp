// atsc3rx: decodes an ATSC 3.0 IQ recording to a transport stream and prints what it found (services, statistics). For checking recordings
// and the receiver without the GUI.
//   atsc3rx recording.cs8 --rate 10e6 [--format cs8|cu8|cf32] [--out out.ts] [--service id]
#include "dect2/atsc3_sync.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;

int main(int argc, char** argv) {
    std::string path, out, format = "cs8";
    double rate = 10e6;
    int service = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rate") rate = atof(val().c_str()); else if (a == "--format") format = val(); else if (a == "--out") out = val();
        else if (a == "--service") service = atoi(val().c_str()); else if (a[0] != '-') path = a;
        else { printf("unknown option %s\n", a.c_str()); return 1; }
    }
    if (path.empty()) { printf("usage: atsc3rx recording --rate 10e6 [--format cs8|cu8|cf32] [--out out.ts] [--service id]\n"); return 1; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { printf("cannot open %s\n", path.c_str()); return 1; }
    Atsc3Receiver rx;
    if (service >= 0) rx.selectService(service);
    std::vector<uint8_t> ts;
    std::thread reader([&] { uint8_t buf[8192]; for (;;) { int k = rx.readTs(buf, sizeof buf); if (k <= 0) break; ts.insert(ts.end(), buf, buf + k); } });
    Atsc3Sync sync(rate, &rx);
    sync.setThreads(getenv("ATSC3_THREADS") ? atoi(getenv("ATSC3_THREADS")) : 4);
    const size_t block = 1 << 18;
    std::vector<unsigned char> raw(block * (format == "cf32" ? 8 : 2));
    std::vector<cf32> x(block);
    long total = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool pickedService = service >= 0;
    while (f) {
        f.read((char*)raw.data(), (std::streamsize)raw.size());
        size_t got = (size_t)f.gcount() / (format == "cf32" ? 8 : 2);
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (format == "cf32") { float re, im; memcpy(&re, &raw[i * 8], 4); memcpy(&im, &raw[i * 8 + 4], 4); x[i] = cf32(re, im); }
            else if (format == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
            else x[i] = cf32((int8_t)raw[2 * i] / 127.f, (int8_t)raw[2 * i + 1] / 127.f);
        }
        sync.push(x.data(), got);
        total += (long)got;
        if (!pickedService) {   // the first video service
            for (auto& s : rx.services()) if (s.category == 1 && !s.hidden) { rx.selectService(s.serviceId); pickedService = true; break; }
        }
    }
    sync.flush();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (int i = 0; i < 60; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); size_t a = ts.size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (a == ts.size() && a > 0) break; }
    rx.stop();
    reader.join();
    auto ss = sync.stats();
    auto rs = rx.stats();
    printf("read %.2f s of signal in %.1f s (%.1fx real time)\n", total / rate, secs, total / rate / std::max(1e-9, secs));
    printf("synchronisation: %ld bootstraps, %ld frames decoded, carrier offset %.1f Hz\n", ss.bootstraps, ss.frames, ss.cfoHz);
    printf("CPU time: bootstrap search %.1f s, frame cut and resampling %.1f s (includes the decode below), frame decoding %.1f s\n", ss.secSearch, ss.secCut, ss.secDecode);
    printf("physical layer: %ld baseband packets (%ld bad), %ld frames failed\n", rs.bbPackets, rs.bbBad, rs.framesFailed);
    printf("link layer: %ld ALP packets (%ld IP, %ld signaling, %ld compressed IP), %ld UDP datagrams, %ld service list tables, %ld ROUTE objects\n",
           rs.alpPackets, rs.alpIp, rs.alpSignaling, rs.alpCompressedIp, rs.udp, rs.llsTables, rs.routeObjects);
    for (auto& s : rx.services())
        printf("service %d: %d.%d  %s  category %d  %s\n", s.serviceId, s.majorChannel, s.minorChannel, s.shortName.c_str(), s.category, s.slsProtocol == 1 ? "ROUTE" : "MMTP (not supported)");
    printf("transport stream: %zu bytes\n", ts.size());
    if (!out.empty()) { std::ofstream o(out, std::ios::binary); o.write((const char*)ts.data(), (std::streamsize)ts.size()); printf("written to %s\n", out.c_str()); }
    return ts.empty() ? 2 : 0;
}
