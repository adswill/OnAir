// Motorola Type II outbound control channel word and message layer (see smartnet_proto.h).
#include "dect2/smartnet/smartnet_proto.h"
#include <cmath>
#include <cstdio>

namespace dect2::smartnet {

namespace {
constexpr int kAddrMask = 0xCC38, kCmdMask = 0x0D5;   // what the on-air bits are XORed with
inline int bit(int v, int n) { return (v >> n) & 1; }
}

int crc(const uint8_t* info) {
    int accum = 0x0393, op = 0x036E;
    for (int j = 0; j < kDataBits; j++) {
        op = (op & 1) ? (op >> 1) ^ 0x0225 : op >> 1;
        if (info[j] & 1) accum ^= op;
    }
    return accum & 0x3FF;
}

void encodePayload(const Osw& o, uint8_t out[kPayloadBits]) {
    const int a = (o.addr & 0xFFFF) ^ kAddrMask, c = (o.cmd & 0x3FF) ^ kCmdMask;
    uint8_t info[kInfoBits] = {};
    for (int j = 0; j < 16; j++) info[j] = (uint8_t)bit(a, 15 - j);
    info[16] = o.group ? 0 : 1;
    for (int j = 0; j < 10; j++) info[17 + j] = (uint8_t)bit(c, 9 - j);
    const int k = crc(info);
    for (int j = 0; j < kCrcBits; j++) info[kDataBits + j] = (uint8_t)(bit(k, kCrcBits - 1 - j) ^ 1);   // the check bits go out inverted
    uint8_t raw[kPayloadBits];
    for (int i = 0; i < kInfoBits; i++) {
        raw[2 * i] = info[i];
        raw[2 * i + 1] = (uint8_t)(info[i] ^ (i ? info[i - 1] : 0));
    }
    for (int r = 0; r < kPayloadBits / 4; r++)
        for (int l = 0; l < 4; l++) out[r + l * 19] = raw[r * 4 + l];
}

bool decodePayload(const uint8_t bits[kPayloadBits], Osw& o, int* corrected) {
    uint8_t raw[kPayloadBits];
    for (int r = 0; r < kPayloadBits / 4; r++)
        for (int l = 0; l < 4; l++) raw[r * 4 + l] = bits[r + l * 19] & 1;
    uint8_t syn[kPayloadBits];
    syn[0] = 0;
    syn[1] = raw[1] ^ raw[0];
    for (int k = 2; k < kPayloadBits; k += 2) { syn[k] = 0; syn[k + 1] = raw[k + 1] ^ raw[k] ^ raw[k - 2]; }
    uint8_t info[kInfoBits - 1];
    int fixed = 0;
    for (int k = 0; k < kInfoBits - 1; k++) {
        // two neighbouring parity bits wrong: the information bit between them was
        if (syn[2 * k + 1] && syn[2 * k + 3]) { info[k] = raw[2 * k] ^ 1; fixed++; }
        else info[k] = raw[2 * k];
    }
    int given = 0;
    for (int j = 0; j < kCrcBits; j++) given = (given << 1) | ((info[kDataBits + j] ^ 1) & 1);
    if (given != crc(info)) return false;
    int a = 0, c = 0;
    for (int j = 0; j < 16; j++) a = (a << 1) | info[j];
    for (int j = 17; j < 27; j++) c = (c << 1) | info[j];
    o.addr = a ^ kAddrMask;
    o.group = !(info[16] & 1);
    o.cmd = c ^ kCmdMask;
    if (corrected) *corrected = fixed;
    return true;
}

std::vector<uint8_t> frameBits(const Osw& o) {
    std::vector<uint8_t> b(kFrameBits);
    for (int i = 0; i < kSyncBits; i++) b[(size_t)i] = (uint8_t)((kSync >> (kSyncBits - 1 - i)) & 1);
    encodePayload(o, b.data() + kSyncBits);
    return b;
}

// ------------------------------------------------------------------------------------------------ band plans

const char* bandName(Band b) {
    switch (b) {
    case Band::Auto: return "automatic";
    case Band::Domestic800: return "800 MHz domestic";
    case Band::Rebanded800: return "800 MHz rebanded";
    case Band::Splinter800: return "800 MHz domestic splinter";
    case Band::International800: return "800 MHz international";
    case Band::InternationalSplinter800: return "800 MHz international splinter";
    case Band::Us900: return "900 MHz";
    default: return "custom (OBT)";
    }
}

double channelHz(Band b, const CustomPlan& c, int ch) {
    if (ch < 0) return 0;
    const auto upper = [&]() -> double {   // the same on every US 800 MHz plan
        if (ch >= 0x2d0 && ch <= 0x2f7) return 866.0000e6 + 25000.0 * (ch - 0x2d0);
        if (ch >= 0x32f && ch <= 0x33f) return 867.0000e6 + 25000.0 * (ch - 0x32f);
        if (ch >= 0x3c1 && ch <= 0x3fe) return 867.4250e6 + 25000.0 * (ch - 0x3c1);
        if (ch == 0x3be) return 868.9750e6;
        return 0;
    };
    switch (b) {
    case Band::Domestic800:
        if (ch <= 0x2cf) return 851.0125e6 + 25000.0 * ch;
        return upper();
    case Band::Rebanded800:
        if (ch <= 0x1b7) return 851.0125e6 + 25000.0 * ch;
        if (ch >= 0x1b8 && ch <= 0x22f) return 851.0250e6 + 25000.0 * (ch - 0x1b8);
        return upper();
    case Band::Splinter800:
        if (ch <= 0x257) return 851.0000e6 + 25000.0 * ch;
        if (ch >= 0x258 && ch <= 0x2cf) return 866.0125e6 + 25000.0 * (ch - 0x258);
        return upper();
    case Band::International800: return ch <= 0x2f7 ? 851.0125e6 + 25000.0 * ch : 0;
    case Band::InternationalSplinter800: return ch <= 0x2f7 ? 851.0000e6 + 25000.0 * ch : 0;
    case Band::Us900: return ch <= 0x1de ? 935.0125e6 + 12500.0 * ch : 0;
    case Band::Custom: {
        if (c.baseHz <= 0) return 0;
        const double midHz = c.midHz > 0 ? c.midHz : c.baseHz, highHz = c.highHz > 0 ? c.highHz : midHz;
        const double midSp = c.midSpacingHz > 0 ? c.midSpacingHz : c.baseSpacingHz, highSp = c.highSpacingHz > 0 ? c.highSpacingHz : midSp;
        if (ch >= c.baseOffset && ch < c.midOffset) return c.baseHz + c.baseSpacingHz * (ch - c.baseOffset);
        if (ch >= c.midOffset && ch < c.highOffset) return midHz + midSp * (ch - c.midOffset);
        if (ch >= c.highOffset && ch < 760) return highHz + highSp * (ch - c.highOffset);
        return 0;
    }
    default: return 0;
    }
}

Band bandFromControl(int chan, double hz) {
    static const Band order[] = {Band::Domestic800, Band::Rebanded800, Band::Splinter800, Band::International800, Band::InternationalSplinter800, Band::Us900};
    if (chan >= 0 && hz > 0) {
        for (Band b : order) { const double f = channelHz(b, CustomPlan(), chan); if (f > 0 && std::fabs(f - hz) < 7000) return b; }
        return Band::Auto;
    }
    if (hz >= 851e6 && hz <= 870e6) return Band::Domestic800;
    if (hz >= 935e6 && hz <= 941e6) return Band::Us900;
    return Band::Auto;
}

// ------------------------------------------------------------------------------------------------ messages

const char* siteBandName(int b) {
    switch (b) {
    case 0: return "800 international splinter";
    case 1: return "800 international";
    case 2: return "800 domestic splinter";
    case 3: return "800 domestic";
    case 4: return "900";
    case 5: return "800 rebanded";
    case 7: return "OBT";
    default: return "?";
    }
}

bool isWideArea(int f) { return f >= 0 && (f & 0x10) != 0; }

std::string featuresText(int f) {
    if (f < 0) return "";
    std::string s;
    const auto add = [&](const char* t) { if (!s.empty()) s += ", "; s += t; };
    if ((f & 0x31) == 0) add("failsoft");
    else { if (f & 0x20) add("VOC"); add((f & 0x10) ? "wide area" : "site trunk"); }
    if (f & 0x08) add("digital");
    if (f & 0x04) add("analog");
    if (f & 0x02) add("CVSD");
    if (f & 0x01) add("active");
    return s;
}

void Parser::setBand(Band b, const CustomPlan& c) { band_ = b; custom_ = c; }
void Parser::feed(const Osw& o) { q_.push_back(o); }
void Parser::bad() { Osw o; o.cmd = kBadCmd; q_.push_back(o); }

void Parser::options(Msg& m, int addr) const {
    m.tg = addr & 0xFFF0;
    m.options = addr & 0xF;
    m.encrypted = (addr & 8) != 0;
    const int o = addr & 7;
    m.emergency = o == 2 || o == 4 || o == 5;
    m.patch = o == 3 || o == 4;
    m.multiselect = o == 5 || o == 7;
}

bool Parser::next(Msg& out) {
    while (q_.size() >= kNeed) {
        Msg m;
        bool afterBad = false;
        while (!q_.empty() && q_.front().cmd == kBadCmd) { q_.pop_front(); afterBad = true; }
        if (q_.size() < kNeed) { if (afterBad) { Osw o; o.cmd = kBadCmd; q_.push_front(o); } return false; }
        m.afterBad = afterBad;
        if (parseOne(m)) { out = m; return true; }
    }
    return false;
}

bool Parser::parseOne(Msg& m) {
    const Osw a = pop();
    if (isCustomTx(band_, custom_, a.cmd)) return parseObt(m, a);
    if (chan(a.cmd) && a.group) {            // voice channel update: the call goes on
        options(m, a.addr);
        m.kind = Kind::GroupUpdate; m.chan = a.cmd; m.freqHz = hz(a.cmd);
        return true;
    }
    if (chan(a.cmd) && !a.group && (a.addr & 0xFF00) == 0x1F00) {
        m.kind = Kind::ControlChannel; m.chan = a.cmd; m.freqHz = hz(a.cmd); m.data = a.addr & 0xFF;
        return true;
    }
    if (a.cmd == 0x2F8 && !a.group) { m.kind = Kind::Idle; m.data = a.addr; return true; }
    if (a.cmd == 0x300 && a.group) { m.kind = Kind::GroupBusy; options(m, a.addr); return true; }
    if (a.cmd == 0x303 && a.group) { m.kind = Kind::EmergencyBusy; options(m, a.addr); return true; }
    if (a.cmd == 0x308) return parse308(m, a);
    if (a.cmd == 0x321) return parse321(m, a);
    if (a.cmd == 0x324 && !a.group) { m.kind = Kind::InterconnectReject; m.src = a.addr; return true; }
    if (a.cmd == 0x32A && a.group) { m.kind = Kind::AffiliationRequest; m.data = a.addr; return true; }
    if (a.cmd == 0x32B && !a.group) { m.kind = Kind::SystemBeacon; m.system = a.addr; return true; }
    if (a.cmd == 0x32C && !a.group) { m.kind = Kind::Roaming; m.src = a.addr; return true; }
    if (a.cmd >= 0x360 && a.cmd <= 0x39F) { m.kind = Kind::Amss; m.site = a.cmd - 0x360 + 1; m.data = a.addr; return true; }
    if (a.cmd == 0x3A0 && a.group) { m.kind = Kind::Diagnostic; m.opcode = (a.addr & 0xF000) >> 12; m.data = a.addr & 0xFFF; return true; }
    if (a.cmd == 0x3BF || a.cmd == 0x3C0) {
        m.kind = Kind::SystemStatus; m.opcode = (a.addr & 0xE000) >> 13; m.data = a.addr & 0x1FFF;
        if (m.opcode == 1) m.typeII = (m.data & 0x1000) != 0;
        return true;
    }
    m.kind = Kind::Unknown; m.data = a.addr; m.opcode = a.cmd;
    return true;
}

bool Parser::parseObt(Msg& m, const Osw& a) {
    const Osw b = pop();
    if (b.cmd == 0x320 && a.group && b.group) {
        const Osw c = q_.front();
        if (c.cmd == 0x30B && (c.addr & 0xFC00) == 0x6000) {
            pop();
            m.kind = c.group ? Kind::AdjacentSite : Kind::AltSite;
            m.system = a.addr; m.site = ((b.addr & 0xFC00) >> 10) + 1; m.band = (b.addr & 0x380) >> 7; m.features = b.addr & 0x3F;
            m.chan = c.addr & 0x3FF; m.freqHz = hz(m.chan); m.words = 3;
            return true;
        }
        q_.push_front(b);
        m.kind = Kind::Unknown; m.opcode = a.cmd; m.data = a.addr;
        return true;
    }
    if (b.cmd == 0x2F8) { m.kind = Kind::Idle; m.src = a.addr; m.digital = !a.group; m.data = b.addr; m.words = 2; return true; }
    if (chan(b.cmd) && b.group && b.addr != 0 && a.addr != 0) {
        options(m, b.addr);
        m.kind = Kind::GroupGrant; m.src = a.addr; m.digital = !a.group; m.chan = b.cmd; m.freqHz = hz(b.cmd); m.words = 2;
        return true;
    }
    if (chan(b.cmd) && !b.group && b.addr != 0 && a.addr != 0) {
        m.kind = Kind::PrivateGrant; m.dst = a.addr; m.src = b.addr; m.group = false; m.encrypted = a.group; m.chan = b.cmd; m.freqHz = hz(b.cmd); m.words = 2;
        return true;
    }
    if (chan(b.cmd) && !b.group && b.addr != 0 && a.addr == 0 && !a.group) {
        m.kind = Kind::InterconnectGrant; m.src = b.addr; m.group = false; m.chan = b.cmd; m.freqHz = hz(b.cmd); m.words = 2;
        return true;
    }
    q_.push_front(b);
    m.kind = Kind::Unknown; m.opcode = a.cmd; m.data = a.addr;
    return true;
}

void Parser::parseExtended(Msg& m, const Osw& a, const Osw& b) {
    m.words = 2;
    if ((b.addr & 0xFC00) == 0x2800 && b.group) {
        m.kind = Kind::ControlChannel; m.system = a.addr; m.chan = b.addr & 0x3FF; m.freqHz = hz(m.chan);
    } else if ((b.addr & 0xFC00) == 0x6000) {
        m.kind = b.group ? Kind::AdjacentControl : Kind::AltControl; m.system = a.addr; m.chan = b.addr & 0x3FF; m.freqHz = hz(m.chan);
    } else if (b.group) {
        if (b.addr == 0x2021 && (((a.addr & 7) == 3 || (a.addr & 7) == 4) || ((a.addr & 7) == 5 || (a.addr & 7) == 7))) { m.kind = Kind::PatchCancel; options(m, a.addr); }
        else { m.kind = Kind::ExtendedFunction; options(m, a.addr); m.opcode = b.addr; }
    } else if (b.addr == 0x261C) { m.kind = Kind::Deaffiliation; m.src = a.addr; m.group = false; }
    else if (b.addr >= 0x26E0 && b.addr <= 0x26E7) { m.kind = Kind::StatusAck; m.src = a.addr; m.data = b.addr & 7; m.group = false; }
    else if (b.addr == 0x26E8) { m.kind = Kind::EmergencyAck; m.src = a.addr; m.group = false; }
    else if (b.addr >= 0x26F0 && b.addr <= 0x26FF) { m.kind = Kind::MessageAck; m.src = a.addr; m.data = b.addr & 0xF; m.group = false; }
    else if ((b.addr & 0xFC00) == 0x2C00) { m.kind = Kind::Denial; m.src = a.addr; m.opcode = b.addr; m.group = false; }
    else { m.kind = Kind::ExtendedFunction; m.src = a.addr; m.opcode = b.addr; m.group = false; }
}

bool Parser::parse308(Msg& m, const Osw& a) {
    Osw b = pop();
    m.words = 2;
    if (chan(b.cmd) && !b.group && (b.addr & 0xFF00) == 0x1F00) {
        m.kind = Kind::ControlChannel; m.system = a.addr; m.chan = b.cmd; m.freqHz = hz(b.cmd); m.data = b.addr & 0xFF;
        return true;
    }
    if (chan(b.cmd) && b.group && b.addr != 0 && a.addr != 0) {
        options(m, b.addr);
        m.kind = Kind::GroupGrant; m.src = a.addr; m.digital = false; m.chan = b.cmd; m.freqHz = hz(b.cmd);
        return true;
    }
    if (chan(b.cmd) && !b.group && b.addr != 0 && a.addr != 0) {
        m.kind = Kind::PrivateGrant; m.dst = a.addr; m.src = b.addr; m.group = false; m.chan = b.cmd; m.freqHz = hz(b.cmd);
        return true;
    }
    if (chan(b.cmd) && !b.group && b.addr != 0 && a.addr == 0) {
        m.kind = Kind::InterconnectGrant; m.src = b.addr; m.group = false; m.chan = b.cmd; m.freqHz = hz(b.cmd);
        return true;
    }
    if (b.cmd == 0x2F8) {
        const int c = q_.front().cmd;
        const bool continuation = c == 0x30A || c == 0x30B || c == 0x30D || c == 0x310 || c == 0x311 || c == 0x317 || c == 0x318 || c == 0x319 || c == 0x31A || c == 0x320 || c == 0x322 || c == 0x32E || c == 0x340;
        if (!continuation) { m.kind = Kind::Idle; m.src = a.addr; m.data = b.addr; return true; }
        m.words = 1; m.kind = Kind::Idle; m.data = b.addr;   // a lone idle that sits in the middle of this message: read the idle now and the message again
        q_.push_front(a);
        return true;
    }
    if (b.cmd == 0x300 && b.group) { options(m, b.addr); m.kind = Kind::GroupBusy; m.src = a.addr; return true; }
    if (b.cmd == 0x302 && !b.group) { m.kind = Kind::PrivateBusy; m.src = a.addr; m.dst = b.addr; m.group = false; return true; }
    if (b.cmd == 0x303 && b.group) { options(m, b.addr); m.kind = Kind::EmergencyBusy; m.src = a.addr; m.emergency = true; return true; }
    if (b.cmd == 0x308) {
        if (q_.front().cmd == 0x2F8) {   // a two-word idle interleaved with another message
            const Osw idle = pop();
            m.kind = Kind::Idle; m.src = a.addr; m.data = idle.addr;
            q_.push_front(b);
            return true;
        }
        q_.push_front(b);
        m.kind = Kind::Unknown; m.opcode = a.cmd; m.data = a.addr;
        return true;
    }
    if (b.cmd == 0x30A && !a.group && !b.group) { m.kind = Kind::Regroup; m.src = a.addr; options(m, b.addr); m.encrypted = false; return true; }
    if (b.cmd == 0x30B) {
        if (q_.front().cmd == 0x2F8 && !q_.front().group) {
            const Osw idle = pop();
            m.kind = Kind::Idle; m.data = idle.addr; m.words = 1;
            q_.push_front(b); q_.push_front(a);
            return true;
        }
        const Osw c = q_.front();
        if (b.group && !c.group && chan(c.cmd) && (c.addr & 0xFF00) == 0x1F00 && (b.addr & 0xFC00) == 0x2800 && (b.addr & 0x3FF) == c.cmd) {
            pop();
            m.kind = Kind::ControlChannel; m.system = a.addr; m.chan = c.cmd; m.freqHz = hz(c.cmd); m.data = c.addr & 0xFF; m.words = 3;
            return true;
        }
        parseExtended(m, a, b);
        return true;
    }
    if (b.cmd == 0x30D && !a.group && !b.group) {
        m.src = a.addr; options(m, b.addr); m.encrypted = false;
        const int op = b.addr & 0xF;
        m.kind = op < 8 ? Kind::StatusAck : op == 8 ? Kind::EmergencyAck : Kind::Regroup; m.data = op;
        return true;
    }
    if (b.cmd == 0x310 && !a.group && !b.group) { m.kind = Kind::Affiliation; m.src = a.addr; options(m, b.addr); m.encrypted = false; return true; }
    if (b.cmd == 0x311 && !a.group && !b.group) { m.kind = Kind::MessageAck; m.src = a.addr; options(m, b.addr); m.encrypted = false; m.data = b.addr & 0xF; return true; }
    if ((b.cmd == 0x315 || b.cmd == 0x317) && !a.group && !b.group) { m.kind = Kind::PrivateRing; m.dst = a.addr; m.src = b.addr; m.group = false; m.encrypted = b.cmd == 0x315; return true; }
    if (b.cmd == 0x318 && !a.group && !b.group) { m.kind = Kind::RingAck; m.dst = a.addr; m.src = b.addr; m.group = false; return true; }
    if (b.cmd == 0x319 && !a.group && !b.group) { m.kind = Kind::CallAlert; m.dst = a.addr; m.src = b.addr; m.group = false; return true; }
    if (b.cmd == 0x31A && !a.group && !b.group) { m.kind = Kind::CallAlertAck; m.dst = a.addr; m.src = b.addr; m.group = false; return true; }
    if (b.cmd == 0x31B && !a.group && !b.group) { m.kind = Kind::Roaming; m.src = a.addr; m.system = b.addr; return true; }
    if (b.cmd == 0x320) {
        const Osw c = q_.front();
        if (c.cmd == 0x2F8 && !c.group) {
            pop();
            m.kind = Kind::Idle; m.data = c.addr; m.words = 1;
            q_.push_front(b); q_.push_front(a);
            return true;
        }
        if (c.cmd == 0x30B && (c.addr & 0xFC00) == 0x6000) {
            pop();
            m.kind = c.group ? Kind::AdjacentSite : Kind::AltSite;
            m.system = a.addr; m.site = ((b.addr & 0xFC00) >> 10) + 1; m.band = (b.addr & 0x380) >> 7; m.features = b.addr & 0x3F;
            m.chan = c.addr & 0x3FF; m.freqHz = hz(m.chan); m.words = 3;
            return true;
        }
        q_.push_front(b);
        m.kind = Kind::Unknown; m.opcode = a.cmd; m.data = a.addr; m.words = 1;
        return true;
    }
    if (b.cmd == 0x322 && a.group && b.group) {
        m.kind = Kind::DateTime;
        m.year = ((a.addr & 0xFE00) >> 9) + 2000; m.month = (a.addr & 0x1E0) >> 5; m.day = a.addr & 0x1F;
        m.dow = (b.addr & 0xE000) >> 13; m.hour = (b.addr & 0x1F00) >> 8; m.minute = b.addr & 0xFF;
        return true;
    }
    if (b.cmd == 0x32E && a.group && b.group) { m.kind = Kind::EmergencyPtt; m.src = a.addr; options(m, b.addr); m.emergency = true; m.encrypted = false; return true; }
    if (b.cmd == 0x340 && a.group && b.group && (((a.addr & 7) == 3 || (a.addr & 7) == 4) || ((a.addr & 7) == 5 || (a.addr & 7) == 7))) {
        m.kind = Kind::Patch; options(m, a.addr); m.dst = (b.addr & 0xFFF) << 4; m.data = a.addr & 0xF;
        return true;
    }
    q_.push_front(b);
    m.kind = Kind::Unknown; m.opcode = a.cmd; m.data = a.addr; m.words = 1;
    return true;
}

bool Parser::parse321(Msg& m, const Osw& a) {
    const Osw b = pop();
    m.words = 2;
    if (chan(b.cmd) && a.group && b.group && b.addr != 0) {
        options(m, b.addr);
        m.kind = Kind::GroupGrant; m.src = a.addr; m.digital = true; m.chan = b.cmd; m.freqHz = hz(b.cmd);
        return true;
    }
    if (chan(b.cmd) && !b.group && b.addr != 0 && a.addr != 0) {
        m.kind = Kind::PrivateGrant; m.dst = a.addr; m.src = b.addr; m.group = false; m.digital = true; m.encrypted = a.group; m.chan = b.cmd; m.freqHz = hz(b.cmd);
        return true;
    }
    if (b.cmd == 0x2F8) {
        const int c = q_.front().cmd;
        if (c != 0x317 && c != 0x318) { m.kind = Kind::Idle; m.src = a.addr; m.data = b.addr; return true; }
        m.words = 1; m.kind = Kind::Idle; m.data = b.addr;
        q_.push_front(a);
        return true;
    }
    if ((b.cmd == 0x315 || b.cmd == 0x317) && !a.group && !b.group) { m.kind = Kind::PrivateRing; m.dst = a.addr; m.src = b.addr; m.group = false; m.digital = true; m.encrypted = b.cmd == 0x315; return true; }
    q_.push_front(b);
    m.kind = Kind::Unknown; m.opcode = a.cmd; m.data = a.addr; m.words = 1;
    return true;
}

std::string describe(const Msg& m) {
    char b[200];
    const char* mode = m.digital ? "digital" : "analog";
    const char* enc = m.encrypted ? " encrypted" : "";
    const char* em = m.emergency ? " EMERGENCY" : "";
    switch (m.kind) {
    case Kind::Idle: return "Idle";
    case Kind::GroupGrant: snprintf(b, sizeof b, "Group grant, %s%s%s: TG %d from %d on channel 0x%03x (%.4f MHz)", mode, enc, em, m.tg, m.src, m.chan, m.freqHz / 1e6); return b;
    case Kind::GroupUpdate: snprintf(b, sizeof b, "Call update%s%s: TG %d on channel 0x%03x (%.4f MHz)", enc, em, m.tg, m.chan, m.freqHz / 1e6); return b;
    case Kind::PrivateGrant: snprintf(b, sizeof b, "Private call, %s%s: %d to %d on channel 0x%03x (%.4f MHz)", mode, enc, m.src, m.dst, m.chan, m.freqHz / 1e6); return b;
    case Kind::InterconnectGrant: snprintf(b, sizeof b, "Interconnect call from %d on channel 0x%03x (%.4f MHz)", m.src, m.chan, m.freqHz / 1e6); return b;
    case Kind::ControlChannel: if (m.system >= 0) snprintf(b, sizeof b, "System 0x%04X, control channel 0x%03x (%.4f MHz)", m.system, m.chan, m.freqHz / 1e6); else snprintf(b, sizeof b, "Control channel 0x%03x (%.4f MHz)", m.chan, m.freqHz / 1e6); return b;
    case Kind::AltControl: snprintf(b, sizeof b, "System 0x%04X, alternate control channel %.4f MHz", m.system, m.freqHz / 1e6); return b;
    case Kind::AdjacentControl: snprintf(b, sizeof b, "System 0x%04X, adjacent control channel %.4f MHz", m.system, m.freqHz / 1e6); return b;
    case Kind::AdjacentSite: case Kind::AltSite: snprintf(b, sizeof b, "%s: system 0x%04X site %d, band %s, %s, control %.4f MHz", m.kind == Kind::AdjacentSite ? "Adjacent site" : "Site", m.system, m.site, siteBandName(m.band), featuresText(m.features).c_str(), m.freqHz / 1e6); return b;
    case Kind::SystemStatus: if (m.opcode == 1) snprintf(b, sizeof b, "System status: Type %s", m.typeII ? "II" : "I"); else snprintf(b, sizeof b, "System status opcode %d, data 0x%04x", m.opcode, m.data); return b;
    case Kind::SystemBeacon: snprintf(b, sizeof b, "System ID 0x%04X (Type II)", m.system); return b;
    case Kind::GroupBusy: snprintf(b, sizeof b, "Busy, queued: TG %d", m.tg); return b;
    case Kind::EmergencyBusy: snprintf(b, sizeof b, "Emergency busy, queued: TG %d", m.tg); return b;
    case Kind::PrivateBusy: snprintf(b, sizeof b, "Private call busy: %d to %d", m.src, m.dst); return b;
    case Kind::Regroup: snprintf(b, sizeof b, "Dynamic regroup: unit %d to TG %d", m.src, m.tg); return b;
    case Kind::Patch: snprintf(b, sizeof b, "%s: TG %d includes TG %d", m.multiselect ? "Multiselect" : "Patch", m.tg, m.dst); return b;
    case Kind::PatchCancel: snprintf(b, sizeof b, "Patch cancelled: TG %d", m.tg); return b;
    case Kind::Affiliation: snprintf(b, sizeof b, "Affiliation: unit %d to TG %d", m.src, m.tg); return b;
    case Kind::Deaffiliation: snprintf(b, sizeof b, "Deaffiliation: unit %d", m.src); return b;
    case Kind::EmergencyPtt: snprintf(b, sizeof b, "Emergency PTT: unit %d on TG %d", m.src, m.tg); return b;
    case Kind::StatusAck: snprintf(b, sizeof b, "Status %d from unit %d", m.data, m.src); return b;
    case Kind::EmergencyAck: snprintf(b, sizeof b, "Emergency acknowledged: unit %d", m.src); return b;
    case Kind::MessageAck: snprintf(b, sizeof b, "Message %d from unit %d", m.data, m.src); return b;
    case Kind::PrivateRing: snprintf(b, sizeof b, "Private call ring%s: %d to %d", enc, m.src, m.dst); return b;
    case Kind::RingAck: snprintf(b, sizeof b, "Ring acknowledged: %d to %d", m.src, m.dst); return b;
    case Kind::CallAlert: snprintf(b, sizeof b, "Call alert: %d to %d", m.src, m.dst); return b;
    case Kind::CallAlertAck: snprintf(b, sizeof b, "Call alert acknowledged: %d to %d", m.src, m.dst); return b;
    case Kind::Denial: snprintf(b, sizeof b, "Denied: unit %d, reason 0x%04x", m.src, m.opcode); return b;
    case Kind::DateTime: snprintf(b, sizeof b, "Date and time: %04d-%02d-%02d %02d:%02d", m.year, m.month, m.day, m.hour, m.minute); return b;
    case Kind::Amss: snprintf(b, sizeof b, "AMSS: site %d", m.site); return b;
    case Kind::Roaming: snprintf(b, sizeof b, "Roaming: unit %d", m.src); return b;
    case Kind::InterconnectReject: snprintf(b, sizeof b, "Interconnect rejected: unit %d", m.src); return b;
    case Kind::AffiliationRequest: return "Affiliation request";
    case Kind::Diagnostic: snprintf(b, sizeof b, "Diagnostic opcode %d, data 0x%03x", m.opcode, m.data); return b;
    case Kind::ExtendedFunction: snprintf(b, sizeof b, "Extended function 0x%04x", m.opcode); return b;
    case Kind::Unknown: snprintf(b, sizeof b, "Unknown OSW: command 0x%03x, address 0x%04x%s", m.opcode, m.data, m.afterBad ? " (after a bad word)" : ""); return b;
    default: return "";
    }
}

} // namespace dect2::smartnet
