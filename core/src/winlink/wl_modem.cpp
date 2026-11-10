// Winlink link frames as IQ on the packet and ARDOP generators, and the "winlink" transmit generator. See wl_modem.h.
#include "dect2/winlink/wl_modem.h"
#include "dect2/ardop/ardop_gen.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/tx/tx.h"

#include <algorithm>
#include <cmath>

namespace dect2 {
namespace winlink {

namespace {
std::vector<PacketGenFrame> genFrames(const std::vector<ax25::Frame>& frames, int baud) {
    std::vector<PacketGenFrame> g;
    for (const auto& f : frames) { PacketGenFrame p; p.frame = f; p.baud = baud == 9600 ? 9600 : 1200; g.push_back(p); }
    return g;
}

class BurstGen : public TxGenerator {
public:
    BurstGen(std::vector<cf32> s, double rate) : s_(std::move(s)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) out[i] = pos_ < s_.size() ? s_[pos_++] : cf32(0.f, 0.f);
    }
    bool finished() const override { return pos_ >= s_.size(); }
private:
    std::vector<cf32> s_;
    double rate_;
    size_t pos_ = 0;
};
} // namespace

double ax25BurstSec(const std::vector<ax25::Frame>& frames, int baud, double gapSec) {
    return packetSynthBurstSec(genFrames(frames, baud), gapSec);
}

std::vector<cf32> renderAx25Burst(const std::vector<ax25::Frame>& frames, int baud, double rate, double snrDb, double cfoHz, double gapSec) {
    if (frames.empty()) return {};
    const std::vector<PacketGenFrame> g = genFrames(frames, baud);
    SynthConfig cfg;
    cfg.snrDb = snrDb;
    cfg.cfoHz = cfoHz;
    std::unique_ptr<ModeSynth> syn = makePacketSynthBurst(cfg, rate, g, gapSec);
    if (!syn) return {};
    std::vector<cf32> o((size_t)std::ceil(packetSynthBurstSec(g, gapSec) * rate));
    syn->generate(o.data(), o.size());
    return o;
}

std::vector<cf32> renderArdopBurst(const std::vector<ArdopLinkFrame>& frames, double rate, double snrDb, double cfoHz, double gapSec, uint32_t seed) {
    if (frames.empty()) return {};
    ArdopGenConfig c;
    c.rate = rate;
    c.dialOffsetHz = -ardopTuning().tuneOffsetHz;
    c.cfoHz = cfoHz;
    c.snrDb = snrDb;
    c.repeat = false;
    c.pauseSec = 0;
    c.startSec = 0.1;
    c.seed = seed;
    for (const auto& f : frames) {
        ArdopGenFrame g;
        g.code = f.code; g.sid = f.sid; g.body = f.body; g.gapSec = gapSec;
        g.leaderMs = 240;
        c.frames.push_back(std::move(g));
    }
    ArdopGenerator gen(c);
    std::vector<cf32> o((size_t)std::ceil((gen.scriptSeconds() + 0.2) * rate));
    gen.generate(o.data(), o.size());
    return o;
}

std::string winlinkTxPayloadAx25(int modem, const std::vector<ax25::Frame>& frames) {
    std::string s(1, (char)modem);
    for (const auto& f : frames) {
        std::vector<uint8_t> b = ax25::buildFrame(f);
        if (b.size() >= 2) b.resize(b.size() - 2);   // without the FCS
        s += (char)(b.size() >> 8); s += (char)(b.size() & 0xFF);
        s.append(b.begin(), b.end());
    }
    return s;
}

std::string winlinkTxPayloadArdop(const std::vector<ArdopLinkFrame>& frames) {
    std::string s(1, (char)kModemArdop);
    for (const auto& f : frames) {
        const size_t n = f.body.size() + 2;
        s += (char)(n >> 8); s += (char)(n & 0xFF);
        s += (char)f.code; s += (char)f.sid;
        s.append(f.body.begin(), f.body.end());
    }
    return s;
}

bool parseWinlinkTxPayload(const std::string& p, int& modem, std::vector<ax25::Frame>& ax, std::vector<ArdopLinkFrame>& ar) {
    ax.clear(); ar.clear();
    if (p.empty()) return false;
    modem = (uint8_t)p[0];
    if (modem < kModemPacket1200 || modem > kModemArdop) return false;
    size_t i = 1;
    while (i + 2 <= p.size()) {
        const size_t n = ((size_t)(uint8_t)p[i] << 8) | (uint8_t)p[i + 1];
        i += 2;
        if (i + n > p.size()) return false;
        const uint8_t* d = (const uint8_t*)p.data() + i;
        if (modem == kModemArdop) {
            if (n < 2) return false;
            ArdopLinkFrame f;
            f.code = d[0]; f.sid = d[1];
            f.body.assign(d + 2, d + n);
            ar.push_back(std::move(f));
        } else {
            ax25::Frame f;
            if (!ax25::parseFrame(d, n, f)) return false;
            ax.push_back(std::move(f));
        }
        i += n;
    }
    return i == p.size() && (!ax.empty() || !ar.empty());
}

std::unique_ptr<TxGenerator> makeWinlinkTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    int modem = 0;
    std::vector<ax25::Frame> ax;
    std::vector<ArdopLinkFrame> ar;
    if (!parseWinlinkTxPayload(r.payload, modem, ax, ar)) { err = "Winlink: nothing to send (the Winlink screen fills this transmission)"; return nullptr; }
    const double rate = rateHz > 0 ? rateHz : 2e6;
    std::vector<cf32> s = modem == kModemArdop ? renderArdopBurst(ar, rate) : renderAx25Burst(ax, modem == kModemPacket9600 ? 9600 : 1200, rate);
    if (s.empty()) { err = "Winlink: this sample rate is too low for the modem"; return nullptr; }
    return std::make_unique<BurstGen>(std::move(s), rate);
}

} // namespace winlink
} // namespace dect2
