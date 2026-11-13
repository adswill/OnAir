// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink (1030 MHz) message layer: the uplink formats UF0, 4, 5, 11, 16, 20, 21 and 24, their fields, and the address / parity field.
// Source: ICAO Annex 10 Vol. IV 3.1.2 (formats, fields, codes) and 4.3.8.4 (ACAS resolution messages in UF16). Bit numbers are the standard's
// (bit 1 is the first, most significant bit). The AP field is the 24 parity bits XOR the address (all ones for the UF11 all-call), so the
// address comes back as AP XOR the CRC of the rest; uplinkOverlay() is the one place that maps an address to the overlaid sequence.
#pragma once
#include <cstdint>
#include <string>

namespace dect2::uplink {

struct Frame { uint8_t b[14] = {}; int nbits = 56; };

struct Uplink {
    int uf = -1, nbits = 0;
    uint8_t b[14] = {};
    uint32_t ap = 0, addr = 0;   // addr: AP XOR parity (UF11: 0xFFFFFF for a good all-call)
    bool allCallOk = false;      // UF11 whose address came back as all ones
    // UF11
    int pr = 0, ic = 0, cl = 0;
    int ii = -1, si = -1;        // the interrogator code: II 0-15 or SI 1-63, from UF11 IC/CL or the IIS / SIS of SD
    // UF4 / 5 / 20 / 21
    int pc = 0, rr = 0, di = 0, sd = 0;
    int rrs = 0, lss = 0;
    int bds1 = -1, bds2 = -1;    // the Comm-B register asked for (RR >= 16); bds1 0: air-initiated Comm-B
    // UF0 / 16
    int rl = 0, aq = 0, ds = 0;
    // UF16 resolution message (UDS 0x30)
    bool ra = false;
    int uds = 0, mtb = 0, cvc = 0, vrc = 0, chc = 0, hrc = 0, hsb = 0, vsb = 0;
    uint32_t mid = 0;            // the address of the ACAS aircraft sending it
    // UF24
    int rc = 0, nc = 0;
};

uint32_t uplinkOverlay(uint32_t addr);   // the sequence XORed into the parity for this address
uint32_t uplinkAddress(uint32_t overlay);
int ufLength(int uf);                    // 56, 112, or 0 for a format that is not defined
int ufOf(const uint8_t* b);              // UF24 and up: 24
bool decode(const uint8_t* b, int nbits, Uplink& out);   // false: length does not fit the format
std::string describe(const Uplink& u);   // one line for the log
std::string bdsText(int bds1, int bds2); // "4,0"
const char* vrcText(int vrc);
const char* cvcText(int cvc);

// encoders (tests, test signal)
void finish(Frame& f, uint32_t addr);    // fills AP
Frame uf0(uint32_t addr, int rl, int aq, int ds);
Frame uf45(int uf, uint32_t addr, int pc, int rr, int di, int sd);
Frame uf11(int pr, int ic, int cl);
Frame uf16(uint32_t addr, int rl, int aq, const uint8_t mu[7]);
Frame uf2021(int uf, uint32_t addr, int pc, int rr, int di, int sd, const uint8_t ma[7]);
Frame uf24(uint32_t addr, int rc, int nc, const uint8_t mc[10]);
int sdIis(int iis, int rrs);              // SD for DI 7 (and 0 / 1 with rrs 0)
int sdSis(int sis, int lss, int rrs);     // SD for DI 3
void raMu(uint8_t mu[7], uint32_t mid, int mtb, int cvc, int vrc, int chc, int hrc, int hsb, int vsb);

} // namespace dect2::uplink
