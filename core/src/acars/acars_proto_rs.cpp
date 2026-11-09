// The ACARS block layer on top of the Rust crate rust/onair_acars: the functions of acars_proto.h; acars_proto.cpp keeps
// acarsBuildFrame (the transmitter of the test signal).
#include "dect2/acars/acars_proto.h"
#include "dect2/acars/acars_text.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/acars.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace dect2 {

namespace {
struct AbiCheck {
    AbiCheck() { dect2::rsAbiCheck("onair_acars", {onair_acars_block_size()}, {sizeof(OnairAcarsBlock)}); }
} abiCheck;
}

uint16_t acarsCrcUpdate(uint16_t crc, uint8_t b) { return onair_acars_crc_update(crc, b); }
uint16_t acarsCrc(const uint8_t* p, size_t n, uint16_t crc) { return onair_acars_crc(p, n, crc); }
uint16_t acarsSyndrome(int e, int bit) { return onair_acars_syndrome(e, bit); }

bool acarsRepairBlock(uint8_t* txt, int len, const uint8_t crc[2], int& fixedBits, int& parityErrors, int maxParityErrors) {
    int32_t fixed = 0, perr = 0;
    const bool ok = onair_acars_repair_block(txt, len < 0 ? 0 : (size_t)len, crc[0], crc[1], maxParityErrors, &fixed, &perr);
    fixedBits = fixed; parityErrors = perr;
    return ok;
}

const char* acarsLabelText(char a, char b) { return reinterpret_cast<const char*>(onair_acars_label_text((uint8_t)a, (uint8_t)b)); }

std::string acarsDecodeText(const std::string& label, const std::string& text) {
    std::string s(256, '\0');
    for (;;) {
        const size_t n = onair_acars_decode_text((const uint8_t*)label.data(), label.size(), (const uint8_t*)text.data(), text.size(), (uint8_t*)&s[0], s.size());
        if (n != SIZE_MAX) { s.resize(n); return n ? s : acarsInterpretLine(label, text); }
        s.assign(s.size() * 4, '\0');
    }
}

bool acarsParseBlock(const uint8_t* t, int len, AcarsMessage& m) {
    auto b = std::make_unique<OnairAcarsBlock>();   // 3.5 kB: not on the stack of a decoder thread
    const int stage = onair_acars_parse_block(t, len < 0 ? 0 : (size_t)len, b.get());
    if (stage == 0) return false;
    // the header, as far as the C++ parser gets before it can fail
    m.finalBlock = b->final_block != 0;
    m.mode = (char)b->mode;
    m.reg.assign((const char*)b->reg, b->reg_len);
    m.ack = (char)b->ack;
    m.label.assign((const char*)b->label, 2);
    m.labelText = acarsLabelText(m.label[0], m.label[1]);
    m.blockId = (char)b->block_id;
    m.downlink = b->downlink != 0;
    if (stage == 1) return false;
    if (stage == 2) { m.text.clear(); return true; }
    if (b->has_msg_num) {
        m.msgNum.assign((const char*)b->msg_num, 3);
        m.msgSeq = (char)b->msg_seq;
        m.flightId.assign((const char*)b->flight_id, 6);
    }
    m.text.assign((const char*)b->text, b->text_len);
    if (b->sublabel_len) m.sublabel.assign((const char*)b->sublabel, b->sublabel_len);
    if (b->mfi_len) m.mfi.assign((const char*)b->mfi, b->mfi_len);
    m.decoded.assign((const char*)b->decoded, b->decoded_len);
    return true;
}

} // namespace dect2
