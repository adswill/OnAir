// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The hand-off to a platform xHE-AAC decoder (drm_usac_ext.h; Android's MediaCodec registers itself there) with a fake decoder, and the
// AudioSpecificConfig built from a DRM static config, compared with bytes written out by hand from ISO/IEC 14496-3 (ASC, audio object type 42)
// and ISO/IEC 23003-3 (UsacConfig). No real USAC bitstream and no real decoder is used, so the audio itself is NOT verified by this test.
#include "dect2/drm/drm_audio.h"
#include "dect2/drm/drm_fec.h"
#include "dect2/drm/drm_usac_ext.h"
#include "dect2/drm/drm_usac_lib.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::drm;

#include "../common/test_util.h"

struct FakeExt : UsacExternalDecoder {
    std::string reason;                 // unavailableReason()
    int opens = 0, decodes = 0, flushes = 0, closes = 0;
    std::vector<uint8_t> lastAsc; int lastRate = 0, lastCh = 0;
    int primeFrames = 0;                // the first N frames give kNoOutput
    bool failOpen = false;
    const char* name() const override { return "Fake codec"; }
    std::string unavailableReason() override { return reason; }
    bool open(const std::vector<uint8_t>& asc, int rate, int ch, std::string& why) override {
        if (failOpen) { why = "refused"; return false; }
        opens++; lastAsc = asc; lastRate = rate; lastCh = ch; return true;
    }
    Result decode(const std::vector<uint8_t>& au, std::vector<int16_t>& pcm, int& rate, int& ch) override {
        decodes++;
        if (decodes <= primeFrames) return Result::kNoOutput;
        rate = 48000; ch = 2; pcm.assign(2 * 1024, (int16_t)(au.size() > 1 ? au[1] * 100 : 0));
        return Result::kPcm;
    }
    void flush() override { flushes++; }
    void close() override { closes++; }
};

static std::vector<uint8_t> crcFrame(std::vector<uint8_t> au, bool damage = false) {
    const uint32_t c = crc16Bytes(au.data(), au.size());
    au.push_back((uint8_t)(c >> 8)); au.push_back((uint8_t)(damage ? ~c : c));
    return au;
}
static std::vector<uint8_t> superFrame(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> sf = {(uint8_t)(frames.size() << 4)};
    sf.push_back((uint8_t)crc8Bytes(sf.data(), 1));
    std::vector<int> borders; int at = 0;
    for (const auto& f : frames) { borders.push_back(at); at += (int)f.size(); sf.insert(sf.end(), f.begin(), f.end()); }
    for (auto b = borders.rbegin(); b != borders.rend(); ++b) { const int e = (*b << 4) | (int)frames.size(); sf.push_back((uint8_t)(e >> 8)); sf.push_back((uint8_t)e); }
    return sf;
}

// bytes written by hand (see the bit layouts in the comments of the test below)
static void testAscVectors() {
    // DRM static config, 24 kHz mono: coreSbrFrameLengthIndexDrm 0 (2 bits), noiseFilling 1 (1), numExt 0 (2) = 00 1 00
    SdcAudio m; m.coding = 3; m.mode = 0; m.rateCode = 4; m.config = {0x20};
    // ASC: 11111 001010 (object type 42), 0110 (24 kHz), 0001 (mono); UsacConfig: 00110 (24 kHz) 001 (coreSbrFrameLengthIndex 1, no SBR) 00001 (mono)
    //      0000 (one element) 00 (SCE) 0 (tw_mdct) 1 (noiseFilling) 0 (no config extension)
    const std::vector<uint8_t> wantMono = {0xF9, 0x4C, 0x26, 0x21, 0x01, 0x00};
    CHECK(xheAscFromSdc(m) == wantMono, "24 kHz mono ASC matches the hand-written bytes");

    // 48 kHz stereo, SBR (coreSbrFrameLengthIndexDrm 1), default SBR header start 5 stop 9, no MPS212 (stereoConfigIndex 0), no extensions
    SdcAudio s; s.coding = 3; s.mode = 2; s.rateCode = 7; s.config = {0x41, 0x64, 0x00};
    // ASC: 11111 001010, 0011 (48 kHz), 0010 (stereo); UsacConfig: 00011 (48 kHz) 010 (coreSbrFrameLengthIndex 2) 00010 (2 channels) 0000 (one element)
    //      01 (CPE) 0 (tw_mdct) 0 (noiseFilling) 000 0101 1001 00 (SBR: 3 flags, start 5, stop 9, no header extras) 00 (stereoConfigIndex) 0 (no extension)
    const std::vector<uint8_t> wantStereo = {0xF9, 0x46, 0x43, 0x42, 0x04, 0x0B, 0x20};
    CHECK(xheAscFromSdc(s) == wantStereo, "48 kHz stereo SBR ASC matches the hand-written bytes");

    SdcAudio bad = m; bad.mode = 1;                          // parametric stereo is not an xHE-AAC mode
    CHECK(xheAscFromSdc(bad).empty(), "an invalid audio mode gives no ASC");
    bad = m; bad.config = {0x20}; bad.coding = 0;
    CHECK(xheAscFromSdc(bad).empty(), "AAC is not xHE-AAC: no ASC");
}

static void testHandOff() {
    auto fake = std::make_shared<FakeExt>();
    usac_ext::setShared(fake);
    UsacLib::setSharedForTest(UsacLib());                    // no libfdk-aac
    SdcAudio a; a.coding = 3; a.mode = 0; a.rateCode = 4; a.config = {0x20};
    {
        AudioDecoder d;
        CHECK(d.configure(a, false) && d.decodable(), "the external decoder opens the stream");
        CHECK(fake->opens == 1 && fake->lastAsc == xheAscFromSdc(a) && fake->lastRate == 24000 && fake->lastCh == 1, "it got the ASC, rate %d, channels %d", fake->lastRate, fake->lastCh);
        CHECK(d.info().find("Fake codec") != std::string::npos, "the status names the decoder: %s", d.info().c_str());
        const std::vector<std::vector<uint8_t>> frames = {crcFrame({0x80, 1, 2}), crcFrame({0x80, 3, 4}, true), crcFrame({0x80, 5, 6}), crcFrame({0x80, 7, 8})};
        const std::vector<uint8_t> sf = superFrame(frames);
        std::vector<float> out;
        d.superFrame(sf.data(), (int)sf.size(), (int)sf.size(), out);
        CHECK(d.framesOk() == 2 && d.framesBad() == 1, "two good frames and one lost (%llu, %llu)", (unsigned long long)d.framesOk(), (unsigned long long)d.framesBad());
        CHECK(fake->decodes == 2, "a lost frame makes the next frames wait for an independent one: %d decoded", fake->decodes);   // frames 1 and 3 (frame 2 lost; the 4th is still in progress)
        CHECK(fake->flushes >= 1, "the lost frame flushed the decoder");
        CHECK(!out.empty() && d.state() == 2 && d.samples() > 0, "PCM came out (%zu floats)", out.size());
    }
    CHECK(fake->closes >= 1, "the decoder is closed with the stream");

    // priming: no output yet is not an error
    fake->primeFrames = 1000; fake->decodes = 0;
    {
        AudioDecoder d; d.configure(a, false);
        const std::vector<uint8_t> sf = superFrame({crcFrame({0x80, 1, 2}), crcFrame({0x80, 3, 4})});
        std::vector<float> out;
        d.superFrame(sf.data(), (int)sf.size(), (int)sf.size(), out);
        CHECK(fake->decodes == 1 && out.empty() && d.state() != 2, "priming frames give no sound and no failure");
    }

    // a phone without a decoder: the plain line, no silence without a reason
    fake->reason = usacNoDecoderText(); fake->opens = 0;
    {
        AudioDecoder d;
        CHECK(!d.configure(a, false) && !d.decodable(), "no decoder: the stream is not decodable");
        CHECK(d.info() == "This phone has no xHE-AAC decoder; DRM audio of this service cannot play", "the status line says so: %s", d.info().c_str());
        CHECK(fake->opens == 0, "open is not called when unavailable (%d)", fake->opens);
        const std::vector<uint8_t> sf = superFrame({crcFrame({0x80, 1, 2}), crcFrame({0x80, 3, 4})});
        std::vector<float> out;
        d.superFrame(sf.data(), (int)sf.size(), (int)sf.size(), out);   // must not crash
        CHECK(out.empty() && d.state() == 1 && d.info() == usacNoDecoderText(), "the line stays while frames arrive");
    }
    // the decoder refuses the configuration
    fake->reason.clear(); fake->failOpen = true;
    { AudioDecoder d; CHECK(!d.configure(a, false) && d.info() == "refused", "a refusal is shown: %s", d.info().c_str()); }
    usac_ext::setShared(nullptr);
}

int main() {
    testAscVectors();
    testHandOff();
    return testSummary("drm_xhe_ext");
}
