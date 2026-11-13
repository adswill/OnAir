// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The POCSAG / FLEX code word, text and phase layer on top of the Rust crate rust/onair_pager: the functions of pager_proto.h;
// pager_proto.cpp keeps the generators and the POCSAG message assembler.
#include "dect2/pager/pager_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/pager.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace dect2 {
namespace pager {

namespace {
struct AbiCheck {
    AbiCheck() { dect2::rsAbiCheck("onair_pager", {onair_pager_page_size()}, {sizeof(OnairPagerPage)}); }
} abiCheck;

// the speed table lives in Rust; this is its C++ view, read once
struct Modes {
    std::vector<FlexMode> m;
    Modes() {
        for (int i = 0; i < onair_pager_flex_mode_count(); i++) {
            FlexMode f{};
            int32_t baud = 0, levels = 0, speed = 0;
            onair_pager_flex_mode_info(i, &f.code, &baud, &levels, &speed);
            f.baud = baud; f.levels = levels; f.speed = speed;
            m.push_back(f);
        }
    }
};
const std::vector<FlexMode>& modes() { static const Modes t; return t.m; }
} // namespace

uint32_t rev21(uint32_t v) { return onair_pager_rev21(v); }
uint32_t encodeWord(uint32_t data21) { return onair_pager_encode_word(data21); }

Fixed correctWord(uint32_t w) {
    Fixed f;
    uint32_t d = 0;
    const int n = onair_pager_correct_word(w, &d);
    if (n < 0) return f;
    f.ok = true; f.bits = n; f.data = d;
    return f;
}

std::string alphaFromBits(const std::vector<uint8_t>& bits) {
    std::string s(bits.size() / 7 + 1, '\0');
    s.resize(onair_pager_alpha_from_bits(bits.data(), bits.size(), (uint8_t*)&s[0], s.size()));
    return s;
}

std::string numericFromBits(const std::vector<uint8_t>& bits, bool flex) {
    std::string s(bits.size() / 4 + 1, '\0');
    s.resize(onair_pager_numeric_from_bits(bits.data(), bits.size(), flex, (uint8_t*)&s[0], s.size()));
    return s;
}

bool pocsagLooksAlpha(const std::vector<uint8_t>& bits) { return onair_pager_looks_alpha(bits.data(), bits.size()); }

PocsagWord pocsagClassify(uint32_t raw) {
    PocsagWord w;
    uint32_t a = 0, b = 0;
    int32_t bits = 0, fixed = 0;
    w.kind = onair_pager_pocsag_classify(raw, &a, &b, &bits, &fixed);
    w.a = a; w.b = b; w.bits = bits; w.fixed = fixed != 0;
    return w;
}

const FlexMode* flexModeForCode(uint16_t code, int maxDistance) {
    const int i = onair_pager_flex_mode_for_code(code, maxDistance);
    return i < 0 ? nullptr : &modes()[(size_t)i];
}
const FlexMode* flexModeForSpeed(int speed) {
    const int i = onair_pager_flex_mode_for_speed(speed);
    return i < 0 ? nullptr : &modes()[(size_t)i];
}
uint32_t flexFiwData(int cycle, int frame) { return onair_pager_flex_fiw_data(cycle, frame); }
bool flexFiwCheck(uint32_t f, int& cycle, int& frame) {
    int32_t c = 0, fr = 0;
    if (!onair_pager_flex_fiw_check(f, &c, &fr)) return false;
    cycle = c; frame = fr;
    return true;
}

void flexDecodePhase(const uint8_t* bits, std::vector<PagerMessage>& out, FlexPhaseStat& st) {
    static thread_local OnairPagerPage pages[96];       // a phase holds 88 words: never more pages than that
    int32_t stat[4] = {0, 0, 0, 0};
    const size_t n = onair_pager_flex_decode_phase(bits, pages, 96, stat);
    st.ok += stat[0]; st.fixed += stat[1]; st.failed += stat[2];
    if (stat[3]) st.biwOk = true;
    const int64_t now = (int64_t)time(nullptr);
    for (size_t i = 0; i < n; i++) {
        PagerMessage m;
        m.wallTime = now;
        m.flex = true;
        m.address = pages[i].address;
        m.function = -1;
        m.type = pages[i].mtype;
        m.fixed = pages[i].fixed;
        m.damaged = pages[i].damaged != 0;
        m.text.assign((const char*)pages[i].text, pages[i].len);
        out.push_back(std::move(m));
    }
}

} // namespace pager
} // namespace dect2
