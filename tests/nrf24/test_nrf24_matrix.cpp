// nRF24 IQ receiver against the full parameter set the chip allows: data rate 250 k / 1 M / 2 M x address width 3 / 4 / 5 x CRC-8 / CRC-16 x payload length
// 0 / 1 / 32 x carrier offset -100 / +100 kHz x NO_ACK and PID. Each combination is a stream of one address; the address must be found with the right
// CRC size and the payload must come back byte for byte. Synthetic signal at 22 dB Eb/N0 (no real recording was available).
#include "dect2/nrf24/nrf24_gen.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/nrf24/nrf24_tel.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    int combos = 0, bad = 0;
    for (int rate : {250, 1000, 2000}) for (int aw = 3; aw <= 5; aw++) for (int crc : {8, 16}) for (int len : {0, 1, 32}) for (double cfo : {-100e3, 100e3}) {
        nrf24::Frame f;
        for (int i = 0; i < aw; i++) f.address.push_back((uint8_t)(0x37 + 0x41 * i + aw));
        f.pid = (len + aw) & 3; f.noAck = (len & 1) != 0; f.crcBits = crc;
        for (int i = 0; i < len; i++) f.payload.push_back((uint8_t)(0xA3 ^ (i * 29 + len)));
        Nrf24GenConfig c;
        c.rate = 8e6; c.rateKbps = rate; c.dialOffsetHz = -1.5e6; c.cfoHz = cfo; c.ebn0Db = 22; c.gapSec = 0.002; c.frames = {f};
        Nrf24Receiver rx; rx.configure(c.rate); rx.setSignalOffset(c.dialOffsetHz);
        Nrf24Generator g(c);
        std::vector<cf32> buf(1 << 14);
        const double sec = rate == 250 ? 0.6 : 0.3;
        for (size_t done = 0; done < (size_t)(sec * c.rate); done += buf.size()) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        rx.flush();
        Nrf24Telemetry t; rx.telemetry(t, 0);
        combos++;
        const std::string want = nrf24::hex(f.address), wp = nrf24::hex(f.payload);
        bool ok = false;
        for (const auto& fr : t.frames) if (fr.address == want && fr.payloadHex == wp && fr.crcBits == crc && fr.rateKbps == rate && fr.pid == f.pid && fr.noAck == f.noAck) ok = true;
        if (!ok) { bad++; printf("  MISS rate %d addr %d crc %d len %d cfo %+.0f k (%zu frames heard)\n", rate, aw, crc, len, cfo / 1e3, t.frames.size()); }
    }
    printf("  %d combinations, %d missed\n", combos, bad);
    CHECK(bad == 0, "%d of %d parameter combinations were not decoded", bad, combos);
    return testSummary("nrf24 matrix");
}
