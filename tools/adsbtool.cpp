// ADS-B: write the test signal to a file, or decode a recording.
//   adsbtool gen out.cs8 [--rate 2e6] [--seconds 10] [--snr 30] [--aircraft 12] [--seed 1] [--cfo 0] [--sro 0] [--mult 1] [--filter 0] [--noreplies]
//                        [--emergency] [--ref lat lon] [--format cs8|cu8|cf32]
//   adsbtool rx recording.cs8 [--rate 2e6] [--format cs8|cu8|cf32] [--fix 0|1|2] [--ref lat lon] [--messages] [--table]
//   adsbtool sim ...    the generator straight into the receiver, with the list of what was sent checked off
//   adsbtool sweep ...  detection rate against signal-to-noise ratio at one sample rate
//   adsbtool decode 8D4840D6202CC371C32CE0576098 [...]   one message (14 or 7 bytes in hex) taken apart: CRC remainder, address, every field
// A recording that is named like capture_2Msps.cs8 gives its own rate. Output of rx: the table of aircraft, the statistics, and with --messages
// every good message as one line (time, hex, address, level, what it is).
#include "dect2/adsb_gen.h"
#include "dect2/adsb_rx.h"
#include "dect2/adsb_sim.h"
#include "dect2/adsb_track.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;

static void usage() {
    fprintf(stderr,
            "usage:\n"
            "  adsbtool gen out.cs8 [--rate 2e6] [--seconds 10] [--snr 30] [--aircraft 12] [--seed 1] [--cfo 0] [--sro 0] [--mult 1] [--filter 0]\n"
            "                       [--noreplies] [--emergency] [--ref lat lon] [--format cs8|cu8|cf32]\n"
            "  adsbtool rx recording.cs8 [--rate 2e6] [--format cs8|cu8|cf32] [--fix 0|1|2] [--ref lat lon] [--messages] [--table]\n"
            "  adsbtool sim [--rate 2e6] [--seconds 10] [--snr 30] [--aircraft 12] [--seed 1] [--filter 0] [--cfo 0] [--sro 0] [--dc 0] [--mult 1] [--noquant] [--fix 1]\n"
            "  adsbtool sweep [--rate 2e6] [--filter 0] [--from 6] [--to 20] [--step 1] [--frames 1000] [--noquant] [--fix 1] [--cfo 0] [--sro 0] [--short]\n"
            "  adsbtool decode HEX [HEX ...]\n");
}

static std::string formatOf(const std::string& path, std::string fmt) {
    if (!fmt.empty()) return fmt;
    const size_t d = path.rfind('.');
    const std::string ext = d == std::string::npos ? "" : path.substr(d + 1);
    return (ext == "cu8" || ext == "cf32") ? ext : "cs8";
}

static int gen(int argc, char** argv) {
    std::string path, fmt;
    double rate = 2e6, secs = 10;
    SynthConfig c;
    c.snrDb = 30;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--rate") rate = next();
        else if (a == "--seconds") secs = next();
        else if (a == "--snr") c.snrDb = next();
        else if (a == "--aircraft") c.modeOpt[0] = (int)next();
        else if (a == "--seed") c.modeOpt[2] = (int)next();
        else if (a == "--cfo") c.cfoHz = next();
        else if (a == "--sro") c.sroPpm = next();
        else if (a == "--mult") c.modeVal[0] = next();
        else if (a == "--filter") c.modeVal[1] = next();
        else if (a == "--noreplies") c.modeOpt[1] = 1;
        else if (a == "--emergency") c.modeOpt[3] = 1;
        else if (a == "--ref" && i + 2 < argc) { c.modeVal[2] = atof(argv[i + 1]); c.modeVal[3] = atof(argv[i + 2]); i += 2; }
        else if (a == "--format" && i + 1 < argc) fmt = argv[++i];
        else if (path.empty()) path = a;
        else { usage(); return 1; }
    }
    if (path.empty()) { usage(); return 1; }
    fmt = formatOf(path, fmt);
    c.mode = 13;
    auto synth = makeAdsbSynth(c, rate);
    if (!synth) { fprintf(stderr, "the sample rate must be at least 2 Msps\n"); return 1; }
    std::ofstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    const size_t total = (size_t)(secs * rate), block = 1 << 16;
    std::vector<cf32> x(block);
    std::vector<unsigned char> raw(block * 8);
    for (size_t done = 0; done < total; done += block) {
        const size_t n = std::min(block, total - done);
        synth->generate(x.data(), n);
        size_t bytes = 0;
        for (size_t i = 0; i < n; i++) {
            if (fmt == "cf32") { const float re = x[i].real(), im = x[i].imag(); memcpy(&raw[i * 8], &re, 4); memcpy(&raw[i * 8 + 4], &im, 4); bytes = n * 8; }
            else if (fmt == "cu8") { raw[2 * i] = (unsigned char)std::lround(std::min(255.f, std::max(0.f, x[i].real() * 127.5f + 127.5f))); raw[2 * i + 1] = (unsigned char)std::lround(std::min(255.f, std::max(0.f, x[i].imag() * 127.5f + 127.5f))); bytes = n * 2; }
            else { raw[2 * i] = (unsigned char)(int8_t)std::lround(std::min(127.f, std::max(-128.f, x[i].real() * 128.f))); raw[2 * i + 1] = (unsigned char)(int8_t)std::lround(std::min(127.f, std::max(-128.f, x[i].imag() * 128.f))); bytes = n * 2; }
        }
        f.write((const char*)raw.data(), (std::streamsize)bytes);
    }
    printf("wrote %.1f s at %.3f Msps (%s) to %s\n", secs, rate / 1e6, fmt.c_str(), path.c_str());
    return 0;
}

static double rateFromName(const std::string& p) {
    // capture_2Msps.cs8, x_2.4msps.cu8
    std::string l = p;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const size_t k = l.find("msps");
    if (k == std::string::npos) return 0;
    size_t s = k;
    while (s > 0 && (isdigit((unsigned char)l[s - 1]) || l[s - 1] == '.')) s--;
    return s < k ? atof(l.substr(s, k - s).c_str()) * 1e6 : 0;
}

static int rxMain(int argc, char** argv) {
    std::string path, fmt;
    double rate = 0;
    int fix = 1;
    bool messages = false, table = true, haveRef = false;
    double refLat = 0, refLon = 0;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) fmt = argv[++i];
        else if (a == "--fix" && i + 1 < argc) fix = atoi(argv[++i]);
        else if (a == "--messages") messages = true;
        else if (a == "--table") table = true;
        else if (a == "--ref" && i + 2 < argc) { refLat = atof(argv[i + 1]); refLon = atof(argv[i + 2]); haveRef = true; i += 2; }
        else if (path.empty()) path = a;
        else { usage(); return 1; }
    }
    if (path.empty()) { usage(); return 1; }
    if (rate <= 0) rate = rateFromName(path);
    if (rate <= 0) rate = 2e6;
    fmt = formatOf(path, fmt);
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    AdsbReceiver rx;
    rx.configure(rate);
    if (!rx.ready()) { fprintf(stderr, "the sample rate %.3f Msps is below the 2 Msps this receiver needs\n", rate / 1e6); return 1; }
    rx.setCorrection(fix);
    if (haveRef) rx.setReference(refLat, refLon);
    if (messages) rx.setFrameCallback([](const AdsbFrame& fr) {
        printf("%9.4f  %s  %06X  %6.1f dBFS%s  %s\n", fr.timeSec, adsb::toHex(fr.bytes, fr.bits).c_str(), fr.icao, fr.levelDbfs, fr.corrected ? "  fixed" : "       ", adsbDescribe(fr.msg).c_str());
    });
    rx.setLogCallback([](const std::string& s) { fprintf(stderr, "[log] %s\n", s.c_str()); });
    const size_t bps = fmt == "cf32" ? 8 : 2, block = 1 << 16;
    std::vector<unsigned char> raw(block * bps);
    std::vector<cf32> x(block);
    size_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (f) {
        f.read((char*)raw.data(), (std::streamsize)raw.size());
        const size_t got = (size_t)f.gcount() / bps;
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (fmt == "cf32") { float re, im; memcpy(&re, &raw[i * 8], 4); memcpy(&im, &raw[i * 8 + 4], 4); x[i] = cf32(re, im); }
            else if (fmt == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
            else x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
        }
        rx.feed(x.data(), got);
        total += got;
    }
    // a little silence so that the last message is not left waiting in the look-ahead
    std::vector<cf32> tail((size_t)(rate * 200e-6) + 64, cf32(0, 0));
    rx.feed(tail.data(), tail.size());
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    AdsbTelemetry t;
    for (int k = 0; k < 2; k++) rx.telemetry(t, 0);
    printf("read %.2f s of signal at %.3f Msps in %.2f s (%.1fx real time)\n", total / rate, rate / 1e6, secs, total / rate / std::max(1e-9, secs));
    printf("%s\n", adsbSummary(t).c_str());
    printf("messages %llu good (%llu repaired), %llu failed the CRC, %llu preambles, noise floor %.1f dBFS, mean level %.1f dBFS\n", (unsigned long long)t.blocksOk, (unsigned long long)t.corrected,
           (unsigned long long)t.blocksBad, (unsigned long long)t.preambles, t.noiseDbfs, t.levelDbfs);
    printf("by format:");
    for (int i = 0; i < 32; i++) if (t.dfCount[i]) printf("  DF%d %llu", i, (unsigned long long)t.dfCount[i]);
    printf("\n");
    if (table) {
        printf("%-6s %-8s %-4s %5s %6s %4s %5s %5s  %-19s %6s %5s %4s %7s\n", "ICAO", "callsign", "cat", "squawk", "alt ft", "kt", "hdg", "vs", "position", "dist", "brg", "msgs", "level");
        for (const auto& a : t.aircraft) {
            char pos[48] = "", alt[16] = "", spd[16] = "", hdg[16] = "", vs[16] = "", sq[8] = "", dist[16] = "", brg[16] = "";
            if (a.hasPos) snprintf(pos, sizeof pos, "%8.4f %9.4f %d", a.lat, a.lon, a.posKind);
            if (a.hasAlt) snprintf(alt, sizeof alt, "%d", a.altFt);
            if (a.hasSpeed) snprintf(spd, sizeof spd, "%.0f", a.speedKt);
            if (a.hasHeading) snprintf(hdg, sizeof hdg, "%.0f", a.headingDeg);
            if (a.hasVrate) snprintf(vs, sizeof vs, "%d", a.vrateFpm);
            if (a.hasSquawk) snprintf(sq, sizeof sq, "%04d", a.squawk);
            if (a.hasRange) { snprintf(dist, sizeof dist, "%.1f", a.distNm); snprintf(brg, sizeof brg, "%.0f", a.bearingDeg); }
            printf("%06X %-8s %-4s %5s %6s %4s %5s %5s  %-19s %6s %5s %4u %6.1f%s\n", a.icao & 0xFFFFFF, a.callsign.c_str(), a.category.c_str(), sq, alt, spd, hdg, vs, pos, dist, brg, a.messages, a.levelDbfs,
                   a.ground ? "  ground" : (a.emergency ? "  EMERGENCY" : ""));
        }
    }
    return t.blocksOk ? 0 : 2;
}

// adsbtool sim [--rate 2e6] [--seconds 10] [--snr 30] [--aircraft 12] [--seed 1] [--filter 0] [--cfo 0] [--sro 0] [--dc 0] [--noquant]:
// the generator straight into the receiver, with the list of what was sent checked off
static int sim(int argc, char** argv) {
    AdsbSimConfig c;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--rate") c.rate = next();
        else if (a == "--seconds") c.seconds = next();
        else if (a == "--snr") c.snrDb = next();
        else if (a == "--aircraft") c.aircraft = (int)next();
        else if (a == "--seed") c.seed = (uint32_t)next();
        else if (a == "--filter") c.filterMHz = next();
        else if (a == "--cfo") c.cfoHz = next();
        else if (a == "--sro") c.sroPpm = next();
        else if (a == "--dc") c.dcOffset = next();
        else if (a == "--mult") c.mult = next();
        else if (a == "--noquant") c.quantise = false;
        else if (a == "--fix") c.fixBits = (int)next();
        else { usage(); return 1; }
    }
    AdsbReceiver rx;
    AdsbSimResult r = adsbSimulate(c, rx);
    printf("sent %zu, decoded %zu (%.1f %%), phantom messages %zu; feed() took %.2f s for %.1f s of signal (%.1fx real time)\n", r.sent.size(), r.decoded,
           r.sent.empty() ? 0.0 : 100.0 * r.decoded / r.sent.size(), r.phantom, r.cpuSec, r.signalSec, r.signalSec / std::max(1e-9, r.cpuSec));
    printf("%s\n  preambles %llu, good %llu (repaired %llu), failed %llu\n", adsbSummary(r.tel).c_str(), (unsigned long long)r.tel.preambles, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.corrected, (unsigned long long)r.tel.blocksBad);
    for (double lo = -10; lo < 60; lo += 3) {
        size_t n1, n2;
        const double a = adsbDetectionRate(r, lo, lo + 3, false, &n1), b = adsbDetectionRate(r, lo, lo + 3, true, &n2);
        if (n2) printf("  SNR %3.0f to %3.0f dB: %5zu frames, alone %5.1f %% (%zu), with overlap %5.1f %%\n", lo, lo + 3, n2, 100 * a, n1, 100 * b);
    }
    return 0;
}

// adsbtool sweep [--rate 2e6] [--filter 0] [--from 6] [--to 20] [--step 1] [--frames 1000] [--noquant] [--fix 1] [--short]:
// one frame every millisecond at a fixed signal-to-noise ratio (noise in 2 MHz), random content, carrier offset and phase: the fraction decoded
static int sweep(int argc, char** argv) {
    AdsbSimConfig base;
    double from = 6, to = 20, step = 1;
    int frames = 1000;
    bool shortFrames = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--rate") base.rate = next();
        else if (a == "--filter") base.filterMHz = next();
        else if (a == "--from") from = next();
        else if (a == "--to") to = next();
        else if (a == "--step") step = next();
        else if (a == "--frames") frames = (int)next();
        else if (a == "--noquant") base.quantise = false;
        else if (a == "--fix") base.fixBits = (int)next();
        else if (a == "--cfo") base.cfoHz = next();
        else if (a == "--sro") base.sroPpm = next();
        else if (a == "--short") shortFrames = true;
        else if (a == "--kpulse") base.kPulse = (float)next();
        else if (a == "--kgap") base.kGap = (float)next();
        else { usage(); return 1; }
    }
    printf("rate %.3f Msps, filter %.2f MHz, %d frames per point, %s, fix %d\n", base.rate / 1e6, base.filterMHz > 0 ? base.filterMHz : std::min(5.0, std::max(1.75, 0.875 * base.rate / 1e6)), frames, base.quantise ? "8 bit" : "float", base.fixBits);
    for (double snr = from; snr <= to + 1e-9; snr += step) {
        AdsbSimConfig c = base;
        c.snrDb = 30;
        c.seconds = frames * 1e-3 + 0.01;
        uint32_t x = 12345 + (uint32_t)(snr * 100);
        auto rnd = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
        for (int k = 0; k < frames; k++) {
            AdsbTx t;
            t.t = 0.001 * (k + 1) + (rnd() % 100000) * 1e-11;   // a random fraction of a microsecond: the phase against the sample clock
            const uint32_t icao = 0x400000 + rnd() % 0x100000;
            if (shortFrames) t.frame = adsb::encodeAllCall(icao, 5, 0);
            else t.frame = adsb::encodeAirbornePosition(icao, 5, 11, 20000 + 25 * (int)(rnd() % 800), false, rnd() & 1, 25 + (rnd() % 1000) * 1e-3, 55 + (rnd() % 1000) * 1e-3);
            t.amp = (float)(0.1 * std::pow(10.0, (snr - 30.0) / 20.0));
            t.cfoHz = ((int)(rnd() % 400) - 200) * 1e3;
            t.phase = (float)((rnd() % 6283) * 1e-3);
            c.custom.push_back(t);
        }
        AdsbReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        AdsbSimResult r = adsbSimulate(c, rx);
        printf("  SNR %5.1f dB: %6.1f %% decoded (%zu of %zu), repaired %llu, phantom %zu, bad %llu\n", snr, 100.0 * r.decoded / std::max<size_t>(1, r.sent.size()), r.decoded, r.sent.size(),
               (unsigned long long)r.tel.corrected, r.phantom, (unsigned long long)r.tel.blocksBad);
        fflush(stdout);
    }
    return 0;
}

// adsbtool decode HEX...: the fields of single messages, for looking at a message from a log or a book
static int decodeMain(int argc, char** argv) {
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        std::string h = argv[i];
        std::string clean;
        for (char c : h) if (isxdigit((unsigned char)c)) clean += c;
        if (clean.size() != 14 && clean.size() != 28) { printf("%s: a message is 14 or 28 hex digits\n", h.c_str()); bad++; continue; }
        uint8_t b[14] = {};
        const int nbits = (int)clean.size() * 4;
        for (int k = 0; k < nbits / 8; k++) { unsigned v; sscanf(clean.c_str() + 2 * k, "%2x", &v); b[k] = (uint8_t)v; }
        adsb::Msg m;
        printf("%s\n", clean.c_str());
        const uint32_t rem = adsb::crc24(b, nbits);
        if (!adsb::decodeMsg(b, nbits, m)) { printf("  DF %u: not a format with %d bits\n", adsb::getBits(b, 1, 5), nbits); bad++; continue; }
        printf("  DF %d, %d bits, CRC remainder %06X", m.df, nbits, rem);
        if (m.df == 17 || m.df == 18 || m.df == 19) printf(rem == 0 ? " (good)" : " (not zero: damaged%s)", adsb::singleBitPosition(nbits, rem) >= 0 ? ", one bit flip would explain it" : "");
        else if (m.df == 11) printf(rem == 0 ? " (good)" : rem < 128 ? " (interrogator code)" : " (damaged)");
        else printf(" = the address of the aircraft (address / parity format)");
        printf("\n");
        if (m.df == 11 || m.df == 17 || m.df == 18 || m.df == 19) printf("  address %06X, %s %d\n", m.icao, m.df == 18 ? "control field" : "capability", m.ca);
        if (m.fs >= 0) printf("  flight status %d (ground %d, alert %d, SPI %d)\n", m.fs, m.ground, m.alert, m.spi);
        if (m.vs >= 0) printf("  vertical status %d\n", m.vs);
        if (m.hasAlt) printf("  %s %d ft\n", m.altGnss ? "GNSS height" : "altitude", m.altFt);
        if (m.hasSquawk) printf("  squawk %04d\n", m.squawk);
        if (m.tc >= 0) printf("  type code %d%s\n", m.tc, m.st >= 0 ? (" subtype " + std::to_string(m.st)).c_str() : "");
        if (m.hasIdent) printf("  callsign '%s', category %c%d\n", m.callsign.c_str(), m.catSet, m.catCode);
        if (m.hasCpr) printf("  CPR %s %s, latitude %d, longitude %d\n", m.surface ? "surface" : "airborne", m.cprOdd ? "odd" : "even", m.cprLat, m.cprLon);
        if (m.hasMove) printf("  movement %.3f kt%s\n", m.moveKt, m.hasTrack ? "" : "");
        if (m.hasTrack) printf("  ground track %.2f deg\n", m.trackDeg);
        if (m.hasVel) printf("  %s speed %.1f kt\n", m.speedKind == 0 ? "ground" : m.speedKind == 1 ? "indicated air" : "true air", m.speedKt);
        if (m.hasHeading && m.tc == 19) printf("  %s %.2f deg\n", m.headingIsTrack ? "track" : "heading", m.headingDeg);
        if (m.hasVrate) printf("  vertical rate %d ft/min (%s)\n", m.vrateFpm, m.vrateBaro ? "barometric" : "GNSS");
        if (m.hasGnssDiff) printf("  GNSS height minus barometric altitude %d ft\n", m.gnssDiffFt);
        if (m.emergency >= 0) printf("  emergency state %d: %s\n", m.emergency, adsb::emergencyName(m.emergency));
        if (m.hasSelAlt) printf("  selected altitude %d ft (%s)\n", m.selAltFt, m.selAltFms ? "FMS" : "MCP/FCU");
        if (m.hasBaro) printf("  barometric setting %.1f mb\n", m.baroMb);
        if (m.hasSelHdg) printf("  selected heading %.1f deg\n", m.selHdgDeg);
        if (m.adsbVersion >= 0) printf("  ADS-B version %d, NACp %d, SIL %d\n", m.adsbVersion, m.nacp, m.sil);
        const adsb::CommB& c = m.commb;
        if (c.bds) {
            printf("  Comm-B BDS %X,%X by inference\n", c.bds >> 4, c.bds & 15);
            if (c.bds == 0x20) printf("    callsign '%s'\n", c.callsign.c_str());
            if (c.hasSelAlt) printf("    MCP/FCU altitude %d ft\n", c.selAltMcpFt);
            if (c.hasSelAltFms) printf("    FMS altitude %d ft\n", c.selAltFmsFt);
            if (c.hasBaro) printf("    baro setting %.1f mb\n", c.baroMb);
            if (c.hasRoll) printf("    roll %.2f deg\n", c.rollDeg);
            if (c.hasTrack) printf("    track %.2f deg\n", c.trackDeg);
            if (c.hasGs) printf("    ground speed %d kt\n", c.gsKt);
            if (c.hasTrackRate) printf("    track rate %.3f deg/s\n", c.trackRateDps);
            if (c.hasTas) printf("    true airspeed %d kt\n", c.tasKt);
            if (c.hasHeading) printf("    heading %.2f deg\n", c.headingDeg);
            if (c.hasIas) printf("    indicated airspeed %d kt\n", c.iasKt);
            if (c.hasMach) printf("    Mach %.3f\n", c.mach);
            if (c.hasBaroRate) printf("    barometric rate %d ft/min\n", c.baroRateFpm);
            if (c.hasInertialRate) printf("    inertial rate %d ft/min\n", c.inertialRateFpm);
        } else if (m.df == 20 || m.df == 21) printf("  Comm-B: not one of BDS 2,0 / 4,0 / 5,0 / 6,0 (or fits more than one)\n");
        printf("  %s\n", adsbDescribe(m).c_str());
    }
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "sweep")) return sweep(argc, argv);
    if (!strcmp(argv[1], "sim")) return sim(argc, argv);
    if (!strcmp(argv[1], "gen")) return gen(argc, argv);
    if (!strcmp(argv[1], "rx")) return rxMain(argc, argv);
    if (!strcmp(argv[1], "decode")) return decodeMain(argc, argv);
    usage();
    return 1;
}
