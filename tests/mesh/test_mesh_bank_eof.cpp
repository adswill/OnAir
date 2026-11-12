// A complete LoRa frame must be recoverable from the Bank's last partial input block.
#include "../data/mesh/testutil.h"
#include "dect2/lorawan/lora_bank.h"
#include <algorithm>

using namespace dect2;
using namespace meshtest;

static void finalBlock(bool wideband) {
    const double rate = wideband ? 500e3 : 250e3;
    std::mt19937 g(9);
    const auto payload = randomBytes(g, 37);
    lora::TxFrame f;
    f.p.sf = 8; f.p.bwHz = 62.5e3; f.p.cr = 8; f.p.preamble = 32;
    f.p.syncWord = 0x12; f.p.crc = true;
    f.data = lora::encode(f.p, payload.data(), payload.size());
    // Put the end of the frame near a bank block boundary. Keep real silence after
    // every transmitted sample so this tests bank staging, not filter lookahead.
    f.startSec = 16024.0 / 250e3; f.amp = 0.3f;
    const auto iq = render({f}, f.endSec() + 2 * lora::symbolSeconds(f.p.sf, f.p.bwHz), rate, 0);

    lorabank::Channel ch;
    ch.bwHz = f.p.bwHz; ch.sfs = {f.p.sf}; ch.syncs = {f.p.syncWord}; ch.preamble = f.p.preamble;
    std::vector<lorabank::Channel> channels{ch};
    if (wideband) {       // Three active channels select the shared FFT channelizer.
        ch.offsetHz = -125e3; channels.push_back(ch);
        ch.offsetHz = 125e3; channels.push_back(ch);
    }

    double firstTime = -1;
    for (size_t chunk : {277, 4096}) {
        lorabank::Bank bank;
        bank.configure(rate, channels);
        std::vector<lorabank::Decoded> out;
        bank.flushPending(out);
        CHECK(out.empty(), "empty input, wideband %d", wideband);
        for (size_t i = 0; i < iq.size(); i += chunk)
            bank.feed(iq.data() + i, std::min(chunk, iq.size() - i), out);
        const auto samples = bank.samples();
        const auto time = bank.timeSec();
        bank.flushPending(out);
        CHECK(bank.samples() == (int64_t)iq.size() && bank.samples() == samples &&
              bank.timeSec() == time, "flush changed input accounting, wideband %d, chunk %zu", wideband, chunk);
        CHECK(out.size() == 1 && out[0].chan == 0 && out[0].f.hdr.crc &&
              out[0].f.crcOk && out[0].f.payload == payload,
              "expected one exact frame, got %zu, wideband %d, chunk %zu", out.size(), wideband, chunk);
        if (out.size() == 1) {
            if (firstTime < 0) firstTime = out[0].sec;
            else CHECK(out[0].sec == firstTime, "chunking changed frame time, wideband %d", wideband);
        }
        const auto frames = out.size();
        bank.flushPending(out);
        CHECK(out.size() == frames && bank.samples() == samples && bank.timeSec() == time,
              "second flush changed the result, wideband %d, chunk %zu", wideband, chunk);
    }
}

int main() {
    finalBlock(false);
    finalBlock(true);
    return fails ? 1 : 0;
}
