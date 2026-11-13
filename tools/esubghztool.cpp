// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat tool: read a recording, or write one message as a file a radio's own tool can send.
//   esubghztool rx <file.cs8|file.cf32> [--rate Hz] [--offset Hz] [--password P | --hex KEY] [--packets]
//       .cs8 is what hackrf_transfer -r writes (interleaved int8), .cf32 interleaved float; --offset: where the chat sits from the recording's
//       centre (default 0). Prints every chat line (and with --packets every packet, CRC good or bad)
//   esubghztool tx <out.cs8> "<line>" [--rate Hz] [--if Hz] [--password P | --hex KEY]
//       the line as a Flipper would send it ("Name: text"; encrypted with the key), at --if Hz from the file's centre (default 300 kHz):
//       hackrf_transfer -t out.cs8 -f <chat Hz minus the IF> -s <rate> -x <TX gain>
#include "dect2/esubghz/esubghz_gen.h"
#include "dect2/esubghz/esubghz_proto.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/esubghz/esubghz_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

static int usage() {
    fprintf(stderr, "esubghztool rx <file.cs8|file.cf32> [--rate Hz] [--offset Hz] [--password P | --hex KEY] [--packets]\n"
                    "esubghztool tx <out.cs8> \"<line>\" [--rate Hz] [--if Hz] [--password P | --hex KEY]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1], path = argv[2];
    double rate = 2e6, offset = 0, ifHz = 300e3;
    bool packets = false;
    esubghz::Key key;
    std::string line;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "--rate") rate = atof(next());
        else if (a == "--offset") offset = atof(next());
        else if (a == "--if") ifHz = atof(next());
        else if (a == "--password") key = esubghz::keyFromPassword(next());
        else if (a == "--hex") { if (!esubghz::keyFromHex(next(), key)) { fprintf(stderr, "the key must be 64 hex digits\n"); return 2; } }
        else if (a == "--packets") packets = true;
        else if (cmd == "tx" && line.empty() && a[0] != '-') line = a;
        else return usage();
    }
    if (cmd == "rx") {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { perror(path.c_str()); return 1; }
        const bool isFloat = path.ends_with(".cf32") || path.ends_with(".fc32");
        EsubghzReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(offset);
        rx.setKey(key);
        std::vector<cf32> blk(1 << 16);
        std::vector<int8_t> raw8(blk.size() * 2);
        std::vector<float> rawf(blk.size() * 2);
        double secs = 0;
        for (;;) {
            size_t n;
            if (isFloat) { n = fread(rawf.data(), 8, blk.size(), f); for (size_t i = 0; i < n; i++) blk[i] = cf32(rawf[2 * i], rawf[2 * i + 1]); }
            else { n = fread(raw8.data(), 2, blk.size(), f); for (size_t i = 0; i < n; i++) blk[i] = cf32(raw8[2 * i] / 128.f, raw8[2 * i + 1] / 128.f); }
            if (n == 0) break;
            rx.feed(blk.data(), n);
            secs += (double)n / rate;
        }
        // silence past the message gap (kMessageGapSec) so the last message is complete
        std::fill(blk.begin(), blk.end(), cf32(0, 0));
        for (size_t left = (size_t)std::ceil((esubghz::kMessageGapSec + 0.2) * rate); left > 0;) {
            const size_t k = std::min(left, blk.size());
            rx.feed(blk.data(), k);
            left -= k;
        }
        fclose(f);
        EsubghzTelemetry t;
        rx.telemetry(t, 0);
        printf("%.1f s, %llu bursts, %llu packets ok, %llu bad, %llu messages\n", secs, (unsigned long long)t.bursts, (unsigned long long)t.blocksOk,
               (unsigned long long)t.blocksBad, (unsigned long long)t.messages);
        if (packets)
            for (size_t i = t.packets.size(); i-- > 0;) {
                const EsgPacket& p = t.packets[i];
                printf("packet %8.3f s  %2d octets  CRC %s%s  %6.1f dBFS  SNR %5.1f dB  offset %+7.0f Hz  %s\n", p.sec, p.len, p.crcOk ? "ok " : "BAD",
                       p.inverted ? " inverted" : "", p.levelDb, p.snrDb, p.cfoHz, p.hex.c_str());
            }
        static const char* kinds[] = {"plain", "decrypted", "encrypted, not readable with this key", "repeat dropped", "not text"};
        for (const EsgChatLine& c : t.chat)
            printf("%8.3f s  [%s, %d packet%s, %+.0f Hz, %.1f dB]  %s\n", c.sec, kinds[c.kind], c.packets, c.packets == 1 ? "" : "s", c.cfoHz, c.snrDb,
                   c.kind <= 1 ? esubghz::displayText(c.text).c_str() : c.hex.c_str());
        return 0;
    }
    if (cmd == "tx") {
        if (line.empty()) return usage();
        esubghz::Sender s;
        const std::vector<uint8_t> msg = s.encode(&key, line);
        const std::vector<cf32> iq = esubghzMessageIq(msg, rate, ifHz, 0.8f, 0.01, 0.02);
        std::vector<int8_t> raw(iq.size() * 2);
        for (size_t i = 0; i < iq.size(); i++) { raw[2 * i] = (int8_t)std::lround(iq[i].real() * 127); raw[2 * i + 1] = (int8_t)std::lround(iq[i].imag() * 127); }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { perror(path.c_str()); return 1; }
        fwrite(raw.data(), 1, raw.size(), f);
        fclose(f);
        printf("%zu octets (%s), %zu packets, %.3f s at %.3f Msps, %.0f Hz from the centre\n", msg.size(), key.on ? "encrypted" : "plain",
               esubghz::splitMessage(msg).size(), (double)iq.size() / rate, rate / 1e6, ifHz);
        return 0;
    }
    return usage();
}
