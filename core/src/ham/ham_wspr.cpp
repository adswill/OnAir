// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSPR beacon schedule: which 2 minute slot to transmit in, and the message. Transmit only; the burst goes through HamSlotTx (the gate).
#include "dect2/ham/ham.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dect2::ham {

bool wsprDbmValid(int dbm) { return dbm >= 0 && dbm <= 60 && (dbm % 10 == 0 || dbm % 10 == 3 || dbm % 10 == 7); }

double WsprBeacon::audioHz() const { return std::max(1400.0, std::min(1600.0, cfg.audioHz)); }

std::string WsprBeacon::message() const {
    if (!hamCallValid(cfg.myCall) || !hamGridValid(cfg.myGrid) || !wsprDbmValid(cfg.dbm)) return "";
    std::string c = cfg.myCall, g = cfg.myGrid.substr(0, 4);
    for (auto& ch : c) ch = (char)toupper((unsigned char)ch);
    for (auto& ch : g) ch = (char)toupper((unsigned char)ch);
    char b[48];
    snprintf(b, sizeof b, "%s %s %d", c.c_str(), g.c_str(), cfg.dbm);
    return b;
}

std::string WsprBeacon::messageForSlot(double slotUtc) {
    if (!on_) return "";
    const int n = std::max(1, cfg.everyNth);               // the operator's choice: 1 = every slot (a long duty cycle can overheat the radio)
    if (sent_ >= std::max(1, cfg.maxTx)) { on_ = false; stopped_ = "maxtx"; return ""; }
    const long idx = slotIndex(slotUtc, slotPeriod(3));
    const long off = ((cfg.offset % n) + n) % n;
    if (((idx % n) + n) % n != off) return "";
    return message();
}

}  // namespace dect2::ham
