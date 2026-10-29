// LoRaWAN FSK (DR7, 50 kbit/s GFSK, lorawan_fsk.h). Synthetic tests only; not checked against a real transmitter.
//  1. Known answers: the CRC against the CRC catalogue's check value of the same CRC, and against a transcription of the SX1276 datasheet's own
//     CRC routine (rev. 5, figure 54); the whitening against bytes an STM32WL (SX126x radio) sent with seed 0x1FF (ST community thread "STM32WL
//     (SX1262) vs S2-LP (CC1101) data whitening algorithm incompatible?", 2023: length 04 and data AA BB CC DD went out as FB 2D 03 95 6A) and
//     against a register-by-register transcription of the LFSR figure (SX1276 figure 37, SX1261/2 figure 6-5).
//  2. A frame encoded here equals what LoRaMac-node puts on the air: LoRaMac-node hands the PHY payload to Radio.Send() and the radio adds the
//     rest with the settings RegionEU868 / radio.c give it (variable length, CRC CCITT seed 0x1D0F inverted, whitening seed 0x1FF, sync C1 94 C1,
//     5 preamble bytes); the reference below builds those bytes the way the datasheets describe the packet handler, independently of encode().
//  3. Generator -> LoRaWAN (all channels) receiver: no noise, low SNR, +-25 kHz carrier offset, transmitter clock error, a 255-byte frame,
//     other sample rates, FSK next to the LoRa scene (an FSK frame overlapping a LoRa frame on another channel), AS923-1, noise alone, FSK off.
#include "dect2/lorawan/lorawan_fsk.h"
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/lorawan/lorawan_rx.h"
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
namespace fsk = dect2::lorawan::fsk;

namespace {

int fails = 0;
void fail(int line, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void fail(int line, const char* fmt, ...) {
    std::string b(600, '\0');
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(&b[0], b.size(), fmt, ap);
    va_end(ap);
    printf("FAIL line %d: %s\n", line, b.c_str());
    fails++;
}
#define EXPECT(c, ...) do { if (!(c)) fail(__LINE__, __VA_ARGS__); } while (0)

// ---- references, written from the datasheets and independent of lorawan_fsk.cpp

// SX1276/77/78/79 datasheet rev. 5, 7.5 figure 54: ComputeCrc / RadioPacketComputeCrc, CRC_TYPE_CCITT
uint16_t refComputeCrc(uint16_t crc, uint8_t data, uint16_t polynomial) {
    for (int i = 0; i < 8; i++) {
        if ((((crc & 0x8000) >> 8) ^ (data & 0x80)) != 0) { crc = (uint16_t)(crc << 1); crc ^= polynomial; }
        else crc = (uint16_t)(crc << 1);
        data = (uint8_t)(data << 1);
    }
    return crc;
}
uint16_t refPacketCrc(const uint8_t* buffer, size_t len) {
    uint16_t crc = 0x1D0F;   // CRC_CCITT_SEED
    for (size_t i = 0; i < len; i++) crc = refComputeCrc(crc, buffer[i], 0x1021);
    return (uint16_t)~crc;
}

// The whitening LFSR of the figure, one flip-flop per element: X0 is the output (XORed with the data, MSB first), X0 xor X5 goes into X8,
// every flip-flop moves one place towards X0 per bit. x[8] is the MSB of the 9-bit seed.
void refWhiten(std::vector<uint8_t>& d, uint16_t seed) {
    std::array<int, 9> x{};
    for (int i = 0; i < 9; i++) x[(size_t)i] = (seed >> i) & 1;
    for (auto& byte : d) {
        int o = 0;
        for (int b = 7; b >= 0; b--) {
            const int bit = ((byte >> b) & 1) ^ x[0];
            o |= bit << b;
            const int fb = x[0] ^ x[5];
            for (int i = 0; i < 8; i++) x[(size_t)i] = x[(size_t)i + 1];
            x[8] = fb;
        }
        byte = (uint8_t)o;
    }
}

// What the radio sends after the sync word for Radio.Send(phy, n) in LoRaMac-node's FSK set-up: length, payload, CRC MSB first, whitened
std::vector<uint8_t> refOnAir(const std::vector<uint8_t>& phy) {
    std::vector<uint8_t> v{(uint8_t)phy.size()};
    v.insert(v.end(), phy.begin(), phy.end());
    const uint16_t crc = refPacketCrc(v.data(), v.size());
    v.push_back((uint8_t)(crc >> 8)); v.push_back((uint8_t)crc);
    refWhiten(v, 0x01FF);
    return v;
}

std::vector<uint8_t> bytesOf(const char* s) { std::vector<uint8_t> v; for (const char* p = s; *p; p++) v.push_back((uint8_t)*p); return v; }

LorawanTelemetry run(const LorawanGenConfig& c, const LorawanSettings& s, double secs, double centreHz = 0) {
    LorawanGenerator g(c);
    LorawanReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);
    if (centreHz > 0) r.setCenterHz(centreHz);
    r.setSettings(s);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.3) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    LorawanTelemetry t;
    r.telemetry(t, 0);
    return t;
}

LorawanGenConfig fskOnly(int plan, double snr, double rate = 4e6) {
    LorawanGenConfig c;
    c.rate = rate;
    c.dialMhz = lorawanPlanCentreMhz(plan);
    c.dialOffsetHz = -lorawanTuning().tuneOffsetHz;
    c.snrDb = snr; c.loop = false;
    c.fsk = lorawanFskTestSchedule(plan);
    return c;
}

size_t fskFrames(const LorawanTelemetry& t) { size_t n = 0; for (const auto& f : t.log) n += f.fsk && f.lorawan; return n; }
const LwFrame* fskByType(const LorawanTelemetry& t, int mtype) {
    for (const auto& f : t.log) if (f.fsk && f.lorawan && f.mtype == mtype) return &f;
    return nullptr;
}

// every field of the three frames of the scene
void checkScene(const LorawanTelemetry& t, double mhz, double extraCfo, const char* what) {
    const LwFrame* up = fskByType(t, 2);
    EXPECT(up && up->hasDevAddr && up->devAddr == 0x26015A7E && up->fcnt == 812 && up->fport == 3 && up->frmLen == 24 && up->mic == 0x6C1E2F33 && up->flags == "ADR" &&
           up->uplink && up->len == 37, "%s: uplink fields", what);
    if (up) EXPECT(std::fabs(up->freqHz - mhz * 1e6) < 1 && std::fabs(up->cfoHz - (3000 + extraCfo)) < 800 && up->sf == 0 && up->type == "Unconfirmed up",
                   "%s: uplink %.0f Hz cfo %.0f (want %.0f)", what, up->freqHz, up->cfoHz, 3000 + extraCfo);
    const LwFrame* jr = fskByType(t, 0);
    EXPECT(jr && jr->devEui == 0x0004A30B001C0530ull && jr->joinEui == 0x70B3D57ED0000002ull && jr->devNonce == 0x1D2C && jr->mic == 0x7E57F00D && jr->len == 23,
           "%s: join request fields", what);
    if (jr) EXPECT(std::fabs(jr->cfoHz - (-4200 + extraCfo)) < 800, "%s: join request cfo %.0f", what, jr->cfoHz);
    const LwFrame* dn = fskByType(t, 3);
    EXPECT(dn && dn->hasDevAddr && dn->devAddr == 0x26015A7E && dn->fcnt == 77 && !dn->uplink && dn->fport == -1 && dn->mic == 0x0DDBA110 &&
           dn->flags.find("ACK") != std::string::npos && dn->mac.find("LinkADRReq") != std::string::npos, "%s: downlink fields (%s)", what, dn ? dn->mac.c_str() : "missing");
}

} // namespace

int main() {
    {   // 1. known answers
        const auto nine = bytesOf("123456789");
        // CRC-16/SPI-FUJITSU (alias CRC-16/AUG-CCITT, reveng catalogue: poly 0x1021 init 0x1D0F, no reflection, check 0xE5CC); the radios complement it
        EXPECT(fsk::crc16(nine.data(), nine.size()) == (uint16_t)~0xE5CC, "CRC of 123456789: %04X", fsk::crc16(nine.data(), nine.size()));
        EXPECT(refPacketCrc(nine.data(), nine.size()) == 0x1A33, "datasheet CRC routine of 123456789: %04X", refPacketCrc(nine.data(), nine.size()));
        std::vector<uint8_t> rnd(300);
        uint32_t x = 12345;
        for (auto& b : rnd) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
        for (size_t n : {1u, 2u, 13u, 64u, 255u, 300u})
            EXPECT(fsk::crc16(rnd.data(), n) == refPacketCrc(rnd.data(), n), "CRC against the datasheet routine, %zu bytes", n);
        // the STM32WL bytes (seed 0x1FF): 04 AA BB CC DD -> FB 2D 03 95 6A
        std::vector<uint8_t> w{0x04, 0xAA, 0xBB, 0xCC, 0xDD};
        fsk::whiten(w.data(), w.size());
        const std::vector<uint8_t> stm{0xFB, 0x2D, 0x03, 0x95, 0x6A};
        EXPECT(w == stm, "whitening of 04 AA BB CC DD: %02X %02X %02X %02X %02X", w[0], w[1], w[2], w[3], w[4]);
        fsk::whiten(w.data(), w.size());
        EXPECT(w[0] == 0x04 && w[1] == 0xAA && w[4] == 0xDD, "whitening twice gives the data back");
        for (uint16_t seed : {(uint16_t)0x01FF, (uint16_t)0x0100, (uint16_t)0x0155}) {
            std::vector<uint8_t> a = rnd, b = rnd;
            fsk::whiten(a.data(), a.size(), seed);
            refWhiten(b, seed);
            EXPECT(a == b, "whitening against the LFSR figure, seed %03X", seed);
        }
        // airtime: (5 + 3 + 1 + n + 2) bytes at 50 kbit/s
        EXPECT(fsk::frameBitCount(255) == 2128 && std::fabs(fsk::airSeconds(255) - 0.04256) < 1e-9, "airtime of 255 bytes");
        EXPECT(fsk::frameBitCount(23) == 272, "bit count of 23 bytes");
        // plans (RP002 DR7; the FSK channels of TTN's frequency plans)
        using namespace lorawan;
        EXPECT(planHasFsk(EU868) && planHasFsk(AS923_1) && planHasFsk(EU433) && !planHasFsk(US915) && !planHasFsk(AU915), "plans with DR7");
        EXPECT(planFskChannelsMhz(EU868).size() == 1 && std::fabs(planFskChannelsMhz(EU868)[0] - 868.8) < 1e-9, "EU868 FSK channel");
        EXPECT(planFskChannelsMhz(AS923_1).size() == 1 && std::fabs(planFskChannelsMhz(AS923_1)[0] - 924.8) < 1e-9 && planFskChannelsMhz(US915).empty(), "AS923-1 / US915 FSK channels");
    }
    {   // 2. frames encoded here against the radio's packet handler as the datasheets describe it
        using namespace lorawan;
        Build b; b.mtype = 2; b.devAddr = 0x26015A7E; b.adr = true; b.fcnt = 812; b.fport = 3; b.frm = {1, 2, 3, 4, 5, 6, 7, 8}; b.mic = 0x6C1E2F33;
        std::vector<std::vector<uint8_t>> frames{build(b), buildJoinRequest(0x70B3D57ED0000002ull, 0x0004A30B001C0530ull, 0x1D2C, 0x7E57F00D)};
        std::vector<uint8_t> big(255);
        for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7 + 1);
        big[0] = 0xE0;
        frames.push_back(big);
        for (const auto& phy : frames) {
            const std::vector<uint8_t> mine = fsk::encode(phy.data(), phy.size()), ref = refOnAir(phy);
            EXPECT(mine == ref, "%zu-byte frame: encode() differs from the packet handler reference", phy.size());
            const fsk::Unpacked u = fsk::decode(mine.data(), mine.size());
            EXPECT(u.lengthOk && u.crcOk && u.payload == phy, "%zu-byte frame: decode() round trip", phy.size());
            std::vector<uint8_t> bad = mine;
            bad[bad.size() / 2] ^= 0x10;
            EXPECT(!fsk::decode(bad.data(), bad.size()).crcOk, "%zu-byte frame: a flipped bit must fail the CRC", phy.size());
            const std::vector<uint8_t> bits = fsk::frameBits(phy.data(), phy.size());
            EXPECT(bits.size() == fsk::frameBitCount(phy.size()), "bit count");
            int pre = 0, sync = 0;
            for (int i = 0; i < 8; i++) pre = pre << 1 | bits[(size_t)i];
            for (int i = 40; i < 64; i++) sync = sync << 1 | bits[(size_t)i];
            EXPECT(pre == 0x55 && sync == 0xC194C1, "preamble %02X sync %06X", pre, sync);
        }
        std::vector<uint8_t> big256(256, 0);
        EXPECT(fsk::encode(big256.data(), big256.size()).empty(), "256 bytes refused");
    }
    LorawanSettings s;   // EU868, FSK on
    const double fskSecs = lorawanFskSceneSeconds(lorawanFskTestSchedule(lorawan::EU868));
    {   // 3a. no noise
        const LorawanTelemetry t = run(fskOnly(lorawan::EU868, 120), s, fskSecs);
        printf("  clean: %zu FSK frames, %llu FSK syncs, %llu CRC errors\n", fskFrames(t), (unsigned long long)t.fskSyncs, (unsigned long long)t.fskCrcBad);
        EXPECT(fskFrames(t) == 3 && t.fskFrames == 3 && t.fskCrcBad == 0, "clean: %zu FSK frames, %llu CRC errors", fskFrames(t), (unsigned long long)t.fskCrcBad);
        checkScene(t, 868.8, 0, "clean");
        const LwFrame* up = fskByType(t, 2);
        if (up) EXPECT(up->snrDb > 40 && std::fabs(up->levelDb - 20 * std::log10(0.3)) < 2, "clean: SNR %.1f level %.1f", up->snrDb, up->levelDb);
        bool ch = false;
        for (const auto& c : t.channels) if (c.fsk && std::fabs(c.freqHz - 868.8e6) < 1 && c.inBand && c.frames == 3) ch = true;
        EXPECT(ch, "channel list: 868.8 MHz FSK with 3 frames");
        bool dev = false;
        for (const auto& d : t.devices) if (d.hasDevAddr && d.devAddr == 0x26015A7E && d.uplinks == 1 && d.downlinks == 1) dev = true;
        EXPECT(dev, "device table: FSK uplink and downlink of 26015A7E");
    }
    {   // 3b. low SNR (in 125 kHz, as the LoRa scene: in the 100 kHz FSK bandwidth it is 1 dB more)
        for (double snr : {16.0, 14.0, 12.0, 10.0, 8.0, 6.0}) {
            const LorawanTelemetry t = run(fskOnly(lorawan::EU868, snr), s, fskSecs);
            const LwFrame* up = fskByType(t, 2);
            printf("  SNR %.0f dB: %zu of 3 FSK frames, reported SNR %.1f\n", snr, fskFrames(t), up ? up->snrDb : -99.0);
            if (snr >= 12) {
                EXPECT(fskFrames(t) == 3, "SNR %.0f dB: %zu of 3", snr, fskFrames(t));
                checkScene(t, 868.8, 0, "low SNR");
                if (up) EXPECT(std::fabs(up->snrDb - (snr + 10 * std::log10(1.25))) < 3, "SNR %.0f dB reported as %.1f", snr, up->snrDb);
            }
        }
    }
    {   // 3c. +-25 kHz crystal error (about 29 ppm at 868 MHz) and a transmitter clock off by +-100 ppm
        for (double cfo : {25e3, -25e3}) {
            LorawanGenConfig c = fskOnly(lorawan::EU868, 20);
            c.cfoHz = cfo;
            const LorawanTelemetry t = run(c, s, fskSecs);
            EXPECT(fskFrames(t) == 3, "offset %.0f Hz: %zu of 3", cfo, fskFrames(t));
            checkScene(t, 868.8, cfo, cfo > 0 ? "+25 kHz" : "-25 kHz");
        }
        for (double ppm : {100.0, -100.0}) {
            LorawanGenConfig c = fskOnly(lorawan::EU868, 20);
            c.sroPpm = ppm;
            const LorawanTelemetry t = run(c, s, fskSecs);
            EXPECT(fskFrames(t) == 3, "clock %.0f ppm: %zu of 3", ppm, fskFrames(t));
            checkScene(t, 868.8, 0, "clock error");
        }
    }
    {   // 3d. the longest frame: 255 bytes (a proprietary frame), with a clock error and an offset
        LorawanGenConfig c = fskOnly(lorawan::EU868, 20);
        c.fsk.resize(1);
        std::vector<uint8_t> big(255);
        for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 13 + 5);
        big[0] = 0xE0;
        c.fsk[0].payload = big; c.fsk[0].startSec = 0.05;
        c.sroPpm = 60; c.cfoHz = -12e3;
        const LorawanTelemetry t = run(c, s, lorawanFskSceneSeconds(c.fsk));
        const LwFrame* p = fskByType(t, 7);
        EXPECT(p && p->len == 255 && p->hex == lorawan::hex(big.data(), big.size()), "255-byte frame");
    }
    {   // 3e. other sample rates (an odd ratio at 2.4 Msps, 10 Msps); at 1 Msps (one decimation stage) 868.8 MHz lies outside the capture around
        // the plan's own centre: the radio is tuned to 868.8 MHz then (its centre known)
        for (double rate : {2.4e6, 10e6}) {
            const LorawanTelemetry t = run(fskOnly(lorawan::EU868, 20, rate), s, fskSecs);
            EXPECT(fskFrames(t) == 3, "%.1f Msps: %zu of 3", rate / 1e6, fskFrames(t));
        }
        const LorawanTelemetry out = run(fskOnly(lorawan::EU868, 20, 1e6), s, 0.5);
        bool listedOut = false;
        for (const auto& c : out.channels) listedOut |= c.fsk && !c.inBand;
        EXPECT(listedOut && fskFrames(out) == 0, "1 Msps around 868.3 MHz: the FSK channel is listed as outside the capture");
        LorawanGenConfig c = fskOnly(lorawan::EU868, 20, 1e6);
        c.dialMhz = 868.8;
        const LorawanTelemetry t = run(c, s, fskSecs, c.dialMhz * 1e6 - c.dialOffsetHz);
        EXPECT(fskFrames(t) == 3, "1 Msps at 868.8 MHz: %zu of 3", fskFrames(t));
        checkScene(t, 868.8, 0, "1 Msps");
    }
    {   // 3f. next to the LoRa scene: the FSK uplink overlaps the SF11 LoRa uplink on another channel; nothing lost on either side
        LorawanGenConfig c = fskOnly(lorawan::EU868, 20);
        c.frames = lorawanTestSchedule(lorawan::EU868);
        const LorawanTelemetry t = run(c, s, std::max(lorawanSceneSeconds(c.frames), fskSecs));
        size_t lora = 0;
        for (const auto& f : t.log) lora += !f.fsk && f.lorawan;
        printf("  with the LoRa scene: %zu LoRa + %zu FSK frames, %llu bad\n", lora, fskFrames(t), (unsigned long long)t.blocksBad);
        EXPECT(lora >= c.frames.size() && fskFrames(t) == 3 && t.blocksBad == 0, "LoRa %zu of %zu, FSK %zu of 3, %llu bad", lora, c.frames.size(), fskFrames(t), (unsigned long long)t.blocksBad);
        checkScene(t, 868.8, 0, "with LoRa");
        // the overlap the scene promises: the FSK uplink starts while the SF11 LoRa uplink is on the air
        const double sf11End = c.frames[1].startSec + lora::airSeconds(c.frames[1].p, c.frames[1].payload.size());
        EXPECT(c.fsk[0].startSec > c.frames[1].startSec && c.fsk[0].startSec < sf11End, "the scene's FSK uplink overlaps the SF11 frame");
    }
    {   // 3g. AS923-1: 924.8 MHz
        LorawanSettings a; a.plan = lorawan::AS923_1;
        const LorawanTelemetry t = run(fskOnly(lorawan::AS923_1, 20), a, fskSecs);
        EXPECT(fskFrames(t) == 3, "AS923-1: %zu of 3", fskFrames(t));
        checkScene(t, 924.8, 0, "AS923-1");
    }
    {   // 3i. the receiver alone on swapped tones (a 1 below the carrier: the conjugate of the scene): found, marked inverted, timed
        LorawanGenConfig c = fskOnly(lorawan::EU868, 25);
        LorawanGenerator g(c);
        fsk::Receiver r;
        const double off = (868.8 - c.dialMhz) * 1e6 + c.dialOffsetHz;
        r.configure(c.rate, {-off});
        std::vector<cf32> buf(16384);
        std::vector<fsk::RxFrame> got;
        for (size_t done = 0; done < (size_t)((fskSecs + 0.2) * c.rate); done += buf.size()) {
            g.generate(buf.data(), buf.size());
            for (auto& v : buf) v = std::conj(v);
            r.feed(buf.data(), buf.size(), got);
        }
        EXPECT(got.size() == 3, "swapped tones: %zu of 3", got.size());
        if (!got.empty()) EXPECT(got[0].inverted && std::fabs(got[0].sec - c.fsk[0].startSec) < 2e-4 && std::fabs(got[0].cfoHz + 3000) < 800 && got[0].devHz > 15e3 && got[0].devHz < 26e3,
                                 "swapped tones: inverted %d, start %.5f s (sent %.5f), cfo %.0f, deviation %.0f", (int)got[0].inverted, got[0].sec, c.fsk[0].startSec, got[0].cfoHz, got[0].devHz);
        EXPECT(r.stats().size() == 1 && r.stats()[0].frames == 3 && r.stats()[0].on, "receiver stats");
    }
    {   // 3h. noise alone; FSK switched off; a plan without FSK
        LorawanGenConfig c = fskOnly(lorawan::EU868, 10);
        c.fsk.clear();
        const LorawanTelemetry t = run(c, s, 3.0);
        printf("  noise alone: %llu syncs, %llu CRC errors\n", (unsigned long long)t.fskSyncs, (unsigned long long)t.fskCrcBad);
        EXPECT(fskFrames(t) == 0 && t.log.empty(), "noise alone: %zu frames", t.log.size());
        LorawanSettings off; off.fsk = false;
        const LorawanTelemetry t2 = run(fskOnly(lorawan::EU868, 30), off, fskSecs);
        EXPECT(fskFrames(t2) == 0, "FSK off: %zu frames", fskFrames(t2));
        LorawanSettings us; us.plan = lorawan::US915;
        const LorawanTelemetry t3 = run(fskOnly(lorawan::EU868, 30), us, 0.5);
        bool any = false;
        for (const auto& ch : t3.channels) any |= ch.fsk;
        EXPECT(!any, "US915 lists no FSK channel");
    }
    if (fails) printf("lorawan fsk: %d FAILED\n", fails);
    else printf("lorawan fsk: all passed\n");
    return fails ? 1 : 0;
}
