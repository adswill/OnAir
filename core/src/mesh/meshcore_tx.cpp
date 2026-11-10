// MeshCore transmit: see meshcore_tx.h.
#include "dect2/mesh/meshcore_tx.h"
#include "dect2/mesh/meshcore_node.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/loraraw/loraraw_gen.h"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace dect2 {

std::string meshCoreTxPayload(const std::vector<std::vector<uint8_t>>& frames) {
    std::string s;
    for (const auto& f : frames) {
        const size_t n = std::min<size_t>(f.size(), 255);
        s.push_back((char)(uint8_t)n);
        s.append((const char*)f.data(), n);
    }
    return s;
}

bool meshCoreTxFrames(const std::string& p, std::vector<std::vector<uint8_t>>& frames) {
    frames.clear();
    size_t i = 0;
    while (i < p.size()) {
        const size_t n = (uint8_t)p[i++];
        if (n == 0 || i + n > p.size()) return false;
        frames.emplace_back((const uint8_t*)p.data() + i, (const uint8_t*)p.data() + i + n);
        i += n;
    }
    return !frames.empty();
}

namespace {

class Frames : public TxGenerator {
public:
    Frames(std::unique_ptr<LoraRawGenerator> g, double rate, size_t total) : g_(std::move(g)), rate_(rate), total_(total) {}
    double sampleRate() const override { return rate_; }
    bool finished() const override { return pos_ >= total_; }
    void generate(cf32* out, size_t n) override {
        g_->generate(out, n);
        for (size_t i = 0; i < n; i++) if (pos_ + i >= total_) out[i] = cf32(0, 0);
        pos_ += n;
    }
private:
    std::unique_ptr<LoraRawGenerator> g_;
    double rate_;
    size_t pos_ = 0, total_;
};

} // namespace

std::unique_ptr<TxGenerator> makeMeshCoreTxGenerator(const TxRequest& r, double rate, std::string& err) {
    meshcore::Radio radio;
    radio.sf = std::clamp((int)std::lround(r.o("meshcore.sf", 8)), 5, 12);
    radio.bwHz = std::max(1000.0, r.o("meshcore.bwHz", 62500));
    radio.cr = std::clamp((int)std::lround(r.o("meshcore.cr", 8)), 5, 8);
    radio.preamble = std::clamp((int)std::lround(r.o("meshcore.preamble", radio.sf <= 8 ? 32 : 16)), 6, 65535);
    const double ifHz = r.o("meshcore.ifHz", -meshTuning().tuneOffsetHz);   // default: where the Transmit window expects a mode-22 signal
    const double gap = std::max(0.0, r.o("meshcore.gapSec", 0.05));
    if (rate <= 0) { err = "no sample rate"; return nullptr; }
    if (std::fabs(ifHz) + radio.bwHz * 0.6 > 0.5 * rate) { err = "the MeshCore frame does not fit in the sample rate at this offset"; return nullptr; }
    std::vector<std::vector<uint8_t>> frames;
    if (!r.payload.empty()) {
        if (!meshCoreTxFrames(r.payload, frames)) { err = "the MeshCore frames are malformed"; return nullptr; }
    } else {
        const std::string text = r.text.empty() ? std::string("hello from OnAir") : r.text;
        meshcore::Packet p = meshcore::buildGroupText(meshcore::publicChannel(), (uint32_t)std::time(nullptr), r.name.empty() ? std::string("OnAir") : r.name, text);
        meshcore::floodOf(p, 1);
        frames.push_back(p.bytes());
    }
    LoraRawGenConfig c;
    c.rate = rate;
    c.dialOffsetHz = ifHz;
    c.snrDb = 99;
    c.amp = 0.7;
    c.loop = false;
    c.leadSec = 0.01;
    double total = c.leadSec;
    for (const auto& f : frames) {
        LoraRawGenFrame g;
        g.p.sf = radio.sf; g.p.bwHz = radio.bwHz; g.p.cr = radio.cr; g.p.preamble = radio.preamble; g.p.syncWord = 0x12;
        g.p.ldro = meshLoraLdro(radio.sf, radio.bwHz);
        g.p.crc = true;
        g.payload.assign(f.begin(), f.begin() + (long)std::min<size_t>(f.size(), 255));
        g.gapSec = gap;
        total += meshcore::airtimeSec(radio, g.payload.size()) + gap;
        c.frames.push_back(std::move(g));
    }
    total += 4 * std::ldexp(1.0, radio.sf) / radio.bwHz + 0.01;   // a few symbols to spare: the modulator's frame may be a little longer than the estimate
    return std::make_unique<Frames>(std::make_unique<LoraRawGenerator>(c), rate, (size_t)std::ceil(total * rate));
}

} // namespace dect2
