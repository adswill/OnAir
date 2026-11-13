// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker detector test signal (see bletrk_gen.h).
#include "dect2/bletrk/bletrk_gen.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/ble/ble_proto.h"

namespace dect2 {

std::vector<BleGenPacket> bletrkTestSchedule(bool only1, int channel) {
    using namespace ble;
    std::vector<BleGenPacket> v;
    int rot = 0;
    auto add = [&](std::vector<uint8_t> pdu, double gap, double amp, double cfo) {
        BleGenPacket p;
        p.pdu = std::move(pdu);
        p.channel = channel ? channel : 37 + (rot++ % 3);
        p.gapSec = gap; p.ampScale = amp; p.cfoHz = cfo;
        v.push_back(std::move(p));
    };
    // the addresses are random static (the two top bits of the first byte on the air set)
    const uint8_t airtag[6] = {0x5A, 0x31, 0xC4, 0x77, 0x2E, 0xE9};
    const uint8_t tile[6] = {0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB};
    const uint8_t chipolo[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xF6};
    const uint8_t tag[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0xD1};
    const uint8_t goog[6] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0xCF};
    const uint8_t phone[6] = {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    // Find My network frame: type 0x12, length 0x19, status, 22 key bytes, key bits, hint
    std::vector<uint8_t> fm = {0x12, 0x19, 0x10};
    for (int i = 0; i < 22; i++) fm.push_back((uint8_t)(0x40 + 7 * i));
    fm.push_back(0x01); fm.push_back(0x00);
    add(makePdu(AdvNonconnInd, true, advPayload(airtag, AdBuilder().mfr(0x004C, fm).b)), 0.004, 1.0, 5e3);
    if (only1) return v;
    add(makePdu(AdvInd, true, advPayload(tile, AdBuilder().flags(0x06).uuid16({0xFEED}).serviceData16(0xFEED, {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}).b)), 0.006, 0.6, -10e3);
    add(makePdu(AdvInd, true, advPayload(chipolo, AdBuilder().flags(0x06).uuid16({0xFE33}).b)), 0.007, 0.35, 15e3);
    add(makePdu(AdvNonconnInd, true, advPayload(tag, AdBuilder().flags(0x06).serviceData16(0xFD5A, {0x01, 0x40, 0x8A, 0x13, 0x77, 0x02, 0x55, 0x61}).b)), 0.006, 0.8, -5e3);
    add(makePdu(AdvNonconnInd, true, advPayload(goog, AdBuilder().serviceData16(0xFEAA, {0x41, 0xC2, 0x31, 0x77, 0x08, 0x19, 0xAB, 0x4E, 0x02}).b)), 0.008, 0.5, 8e3);
    add(makePdu(AdvInd, true, advPayload(phone, AdBuilder().flags(0x1A).mfr(0x004C, {0x10, 0x05, 0x01, 0x18, 0x44, 0x22, 0x11}).b)), 0.004, 0.9, 0);
    return v;
}

namespace {
class BletrkSynth : public ModeSynth {
public:
    BletrkSynth(const SynthConfig& s, double rate) : rate_(rate) {
        BleGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -bletrkTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.pkts = bletrkTestSchedule(s.modeOpt[0] == 1);
        gen_ = std::make_unique<BleGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<BleGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeBletrkSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<BletrkSynth>(cfg, sampleRate);
}

} // namespace dect2
