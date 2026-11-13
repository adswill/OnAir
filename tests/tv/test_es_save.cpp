// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Save stream": a synthetic DRA-like elementary stream is wrapped into PES / transport-stream packets (adaptation fields, a second PID
// as noise, PES headers with optional fields); what the saver writes must be the stream, byte for byte. The CDR path writes units one
// after the other through the same sink.
#include "dect2/tv/es_save.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> readAll(const char* p) {
    std::vector<uint8_t> v; FILE* f = fopen(p, "rb"); if (!f) return v;
    uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
    fclose(f); return v;
}

int main() {
    std::vector<uint8_t> es;   // frames that start with the DRA sync word 7F FF
    for (int f = 0; f < 40; f++) { es.push_back(0x7F); es.push_back(0xFF); for (int i = 0; i < 300 + f; i++) es.push_back((uint8_t)(f * 7 + i)); }
    std::vector<uint8_t> ts;
    int cc = 0, cc2 = 0;
    auto packet = [&](int pid, bool pusi, const uint8_t* d, int n, int& c, int stuff) {
        uint8_t p[188]; memset(p, 0xFF, sizeof p);
        p[0] = 0x47; p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid;
        const int room = 184 - (stuff ? stuff : 0);
        const int take = n < room ? n : room;
        int off = 4;
        if (take < 184) {   // an adaptation field takes the slack
            const int af = 184 - take;
            p[3] = (uint8_t)(0x30 | (c & 15));
            p[4] = (uint8_t)(af - 1); if (af > 1) p[5] = 0;
            off = 4 + af;
        } else p[3] = (uint8_t)(0x10 | (c & 15));
        c++;
        memcpy(p + off, d, take);
        ts.insert(ts.end(), p, p + 188);
        return take;
    };
    size_t pos = 0; int k = 0;
    while (pos < es.size()) {   // PES packets of up to 700 bytes with a 5-byte PTS
        const size_t chunk = std::min<size_t>(700, es.size() - pos);
        std::vector<uint8_t> pes = {0, 0, 1, 0xC0, 0, 0, 0x80, 0x80, 5, 0x21, 0, 1, 0, 1};
        pes.insert(pes.end(), es.begin() + pos, es.begin() + pos + chunk);
        pos += chunk;
        int o = 0; bool first = true;
        while (o < (int)pes.size()) { o += packet(0x101, first, pes.data() + o, (int)pes.size() - o, cc, (k++ % 3) == 0 && !first ? 4 : 0); first = false; }
        uint8_t junk[40]; memset(junk, 0xAB, sizeof junk);
        packet(0x102, true, junk, sizeof junk, cc2, 0);   // another PID between the packets
    }
    const char* out = "es_save_test.dra";
    EsSink sink; TsEsExtractor ex;
    CHECK(sink.open(out), "open");
    ex.setPid(0x101);
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) ex.feed(ts.data() + i, sink);
    sink.close();
    const std::vector<uint8_t> got = readAll(out);
    CHECK(got == es, "TS path: got %zu bytes, expected %zu", got.size(), es.size());
    CHECK(sink.bytes() == es.size(), "byte counter %llu", (unsigned long long)sink.bytes());
    CHECK(got.size() > 2 && got[0] == 0x7F && got[1] == 0xFF, "DRA sync word at the start");
    EsSink s2; CHECK(s2.open(out), "reopen");
    size_t q = 0; while (q < es.size()) { const size_t n = std::min<size_t>(321, es.size() - q); s2.write(es.data() + q, n); q += n; }
    s2.close();
    CHECK(readAll(out) == es, "unit path");
    CHECK(std::strcmp(esExtension("DRA"), "dra") == 0 && std::strcmp(esExtension("MPEG audio"), "mp2") == 0, "extensions");
    remove(out);
    printf(fails ? "FAILED\n" : "es_save OK\n");
    return fails ? 1 : 0;
}
