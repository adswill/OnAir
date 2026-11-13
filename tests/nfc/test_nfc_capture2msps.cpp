// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NFC at 2 Msps from a weak 8-bit capture (#271): a phone (reader) field of about 4 LSB sitting at DC, as a HackRF / RTL-SDR records it,
// with the tag's answer far below the quantisation step. The reader's frames must come out exactly (the 7-bit REQA / WUPA, SELECT,
// MIFARE Classic AUTH, HLTA, ending on a 0 or on a 1), the encrypted traffic after the AUTH is named and not decoded, and the quantisation
// noise and the pauses' ringing must not produce tag frames. Synthetic: no recording is kept in the repository.
#include "dect2/nfc/nfc_engine.h"
#include "dect2/nfc/nfc_proto.h"
#include "dect2/nfc/nfc_rx.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2e6;

// the field, 8-bit quantised, carrier on Q like a radio with the dial on 13.56 MHz and a DC offset: amplitude `amp` LSB
struct Cap {
    std::vector<std::complex<float>> x;
    uint32_t rng = 12345;
    float dither() { rng = rng * 1664525u + 1013904223u; return ((int)(rng >> 9) / 4194304.0f - 1.0f) * 0.35f; }
    void push(double env, double amp) { x.emplace_back(std::round(dither()) / 128.f, std::round(-amp * env + dither()) / 128.f); }
    void idle(double us, double amp) { for (int i = 0; i < (int)(us * 1e-6 * kFs); i++) push(1.0, amp); }
    void quiet(double us) { for (int i = 0; i < (int)(us * 1e-6 * kFs); i++) push(0.0, 0.0); }
    // a Type A reader frame: modified Miller, 100 % ASK, 3 us pauses
    void reader(const std::vector<uint8_t>& bytes, int bits, double amp) {
        const std::vector<uint8_t> seq = nfc::bytesToMillerSeq(bytes, bits);
        const double sp = kFs * nfc::kEtu;
        const size_t base = x.size(), total = (size_t)std::llround((double)seq.size() * sp);
        for (size_t i = 0; i < total; i++) push(1.0, amp);
        for (size_t k = 0; k < seq.size(); k++) {
            if (!seq[k]) continue;
            const double c = (k + (seq[k] == 1 ? 0.5 : 0.0)) * sp;   // X in the second half of its etu, Z at the start
            const size_t s = base + (size_t)std::llround(c);
            for (size_t i = 0; i < 6 && s + i < x.size(); i++) x[s + i] = std::complex<float>(std::round(dither()) / 128.f, std::round(dither()) / 128.f);
        }
    }
};

static std::vector<uint8_t> withCrc(std::vector<uint8_t> b) { const uint16_t c = nfc::crcA(b.data(), b.size()); b.push_back(c & 0xFF); b.push_back(c >> 8); return b; }

int main() {
    const double amp = 4.0;
    Cap c;
    c.quiet(2000);
    c.idle(1500, amp);
    c.reader({0x52}, 7, amp); c.idle(900, amp);                                   // WUPA
    c.reader({0x26}, 7, amp); c.idle(900, amp);                                   // REQA
    c.reader({0x93, 0x20}, 16, amp); c.idle(900, amp);                            // anticollision, last bit of the frame 0
    c.reader(withCrc({0x93, 0x70, 0x8E, 0x63, 0x3D, 0x3B, 0xEB}), 72, amp); c.idle(900, amp);   // SELECT
    c.reader(withCrc({0x60, 0x04}), 32, amp); c.idle(900, amp);                   // MIFARE Classic AUTH key A block 4
    c.reader({0x73, 0x44, 0x2F, 0xCC, 0x38, 0x8D, 0x28, 0x73}, 64, amp); c.idle(900, amp);       // encrypted nonces (parity not valid on purpose)
    c.reader(withCrc({0x50, 0x00}), 32, amp); c.idle(2000, amp);                  // HLTA
    c.quiet(3000);

    NfcReceiver rx;
    rx.configure(kFs);
    CHECK(rx.ready(), "2 Msps is accepted");
    CHECK(nfcTuning().minSampleRate <= 2.0e6, "minimum rate %.0f", nfcTuning().minSampleRate);
    for (size_t i = 0; i < c.x.size(); i += 4096) rx.feed(c.x.data() + i, std::min<size_t>(4096, c.x.size() - i));
    NfcTelemetry t;
    rx.telemetry(t, 0);
    int reader = 0, tag = 0; bool wupa = false, reqa = false, anti = false, sel = false, auth = false, hlta = false, enc = false;
    for (const auto& e : t.log) {
        printf("  %s %-40s | %s%s\n", e.fromReader ? "RDR" : "TAG", e.hex.c_str(), e.note.c_str(), e.parityOk ? "" : "  [parity]");
        if (e.fromReader) reader++; else tag++;
        wupa |= e.hex == "52"; reqa |= e.hex == "26"; anti |= e.hex == "93 20";
        sel |= e.hex == "93 70 8E 63 3D 3B EB 5E FC";
        auth |= e.hex.rfind("60 04", 0) == 0 && e.note.find("authentication") != std::string::npos;
        hlta |= e.hex == "50 00 57 CD";
        enc |= e.hex == "73 44 2F CC 38 8D 28 73" && e.note.find("encrypted") != std::string::npos;
    }
    CHECK(wupa && reqa, "WUPA / REQA as 7-bit frames");
    CHECK(anti, "anticollision 93 20");
    CHECK(sel, "SELECT with the UID");
    CHECK(auth, "AUTH named");
    CHECK(hlta, "HLTA");
    CHECK(enc, "encrypted frame named, not decoded");
    CHECK(reader == 7, "%d reader frames, want 7", reader);
    CHECK(tag == 0, "%d tag frames out of quantisation noise", tag);
    CHECK(t.tags.size() == 1 && t.tags[0].uid == "8E633D3B", "the tag table holds the UID of the reader's SELECT (%zu tags)", t.tags.size());

    // nothing but the 8-bit noise floor: no frame
    {
        Cap n; n.quiet(100000);
        NfcReceiver r2; r2.configure(kFs);
        for (size_t i = 0; i < n.x.size(); i += 4096) r2.feed(n.x.data() + i, std::min<size_t>(4096, n.x.size() - i));
        NfcTelemetry t2; r2.telemetry(t2, 0);
        CHECK(t2.log.empty(), "%zu frames from noise", t2.log.size());
        Cap m; m.idle(100000, amp);
        r2.reset();
        for (size_t i = 0; i < m.x.size(); i += 4096) r2.feed(m.x.data() + i, std::min<size_t>(4096, m.x.size() - i));
        r2.telemetry(t2, 0);
        CHECK(t2.log.empty(), "%zu frames from a bare carrier", t2.log.size());
    }
    return testSummary("nfc_capture2msps");
}
