// ATSC 3.0 test signal, see atsc3_synth.h.
#include "dect2/atsc3_synth.h"
#include "atsc3_synth_content.h"
#include "atsc3_synth_resampler.h"
#include "dect2/atsc3_alp.h"
#include "dect2/atsc3_bb.h"
#include "dect2/atsc3_frame.h"
#include "dect2/atsc3_ip.h"
#include "dect2/atsc3_route.h"
#include "dect2/atsc3_sync.h"
#include "dect2/exact_resampler.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {

namespace {

using namespace atsc3;

constexpr double kNativeRate = kBootstrapRate;   // 6.144 Msamples/s: the bootstrap and, with bsr_coefficient 0, the rest of the frame
constexpr double kLevel = 0.20;                  // rms of the output
constexpr uint32_t kSrcIp = 0x0A010105u, kDstIp = 0xEFFF0501u;   // 10.1.1.5 and 239.255.5.1: the addresses the service signalling names

class Atsc3Synth : public ModeSynth {
public:
    Atsc3Synth(const SynthConfig& c, double rate) : cfg_(c), rate_(rate), noise_(11) {}

    bool init() {
        // ---- the PLP
        plp_.id = 1;
        plp_.fecType = cfg_.modeOpt[2] == 1 ? 0 : 1;   // BCH + 16K / BCH + 64K LDPC
        plp_.mod = std::min(std::max(cfg_.modeOpt[0], 0), 3);
        plp_.cod = cfg_.modeOpt[1] >= 1 && cfg_.modeOpt[1] <= 12 ? cfg_.modeOpt[1] - 1 : 6;
        Bicm bicm(plpBicm(plp_));
        if (!bicm.ok()) { plp_.fecType = 1; plp_.mod = 0; plp_.cod = 6; bicm = Bicm(plpBicm(plp_)); }
        if (!bicm.ok()) return false;
        bytes_ = bicm.kPayload() / 8;
        cellsPerBlock_ = bicm.cells();

        // ---- the frame: 8K FFT at 6.144 Msamples/s, one subframe, one PLP
        fs_.bs.minorVersion = 0; fs_.bs.numSymbols = 4; fs_.bs.systemBandwidth = 0; fs_.bs.bsrCoefficient = 0;
        fs_.bs.preambleStructure = 10;   // 8K, guard 512, pilot spacing 6, L1-Basic mode 1
        fs_.bs.minTimeToNext = 1;        // the next frame comes after at least 100 ms
        fs_.fftCode = 0;
        fs_.guardCode = cfg_.modeOpt[3] >= 1 && cfg_.modeOpt[3] <= 12 ? cfg_.modeOpt[3] : 5;
        fs_.spPattern = 4;               // SP6_2
        fs_.numSymbols = numSymbolsFor(guardSamples());
        fs_.l1DetailMode = cfg_.modeOpt[5] >= 1 && cfg_.modeOpt[5] <= 7 ? cfg_.modeOpt[5] : 2; fs_.sbsNullCells = 16;
        frameRate_ = postBootstrapRate(fs_.bs);
        if (std::fabs(frameRate_ - kNativeRate) > 1) return false;
        boot_ = generateBootstrap(fs_.bs);

        // how many baseband packets fit into a frame
        int freeCells = 0;
        {
            FramePlp p = plp_;
            p.bbPackets.push_back(makeBbPacket(bytes_, {}, -1, -1));
            auto fr = buildFrame(fs_, {p}, nullptr, &freeCells);
            if (fr.empty()) return false;
            frameSamples_ = fr.size();
        }
        maxBlocks_ = (freeCells + cellsPerBlock_) / cellsPerBlock_;
        periodSamples_ = boot_.size() + frameSamples_;
        const double periodSec = (double)periodSamples_ / kNativeRate;
        const double capacityBps = (double)maxBlocks_ * bicm.kPayload() / periodSec;

        // ---- the programme: the video rate is chosen so that the PLP carries the whole service at least twice over
        int kbps = cfg_.modeVal[0] > 0 ? (int)(cfg_.modeVal[0] / 1000.0) : 700;
        if (cfg_.modeVal[0] <= 0) {
            const double room = capacityBps / 2.2 / 1.08 - 100000.0;
            if (room < kbps * 1000.0) kbps = std::max(60, (int)(room / 1000.0));
        }
        int pref = std::min(std::max(cfg_.modeOpt[4], 0), 3);
        content_ = atsc3synth::getContent(pref, kbps);
        if (!content_ && pref) content_ = atsc3synth::getContent(0, kbps);
        if (!content_) return false;

        // size of the traffic of a slot, to set the number of baseband packets per frame: the slot's data must be out before the next slot
        size_t maxSlot = 0;
        for (int s = 0; s < content_->slots; s++) { auto ip = slotAlp((uint64_t)s); size_t n = 0; for (auto& p : ip) n += p.size(); maxSlot = std::max(maxSlot, n); }
        ipId_ = 1;
        const double framesPerSlot = 1.0 / periodSec;
        cap_ = (int)std::ceil(1.5 * (double)maxSlot / framesPerSlot / (double)(bytes_ - 2));
        cap_ = std::max(1, std::min(cap_, maxBlocks_));
        capBytesPerSlot_ = (size_t)((double)cap_ * (bytes_ - 2) * framesPerSlot);

        // ---- the channel
        const double outRate = rate_ * (1.0 + cfg_.sroPpm * 1e-6);
        fast_ = fast_rs_.configure(kNativeRate, outRate);
        if (!fast_ && !resampler_.configure(kNativeRate, outRate)) return false;
        const double noisePow = cfg_.snrDb >= 150 ? 0.0 : std::pow(10.0, -cfg_.snrDb / 10.0) * rate_ / kNativeRate;   // signal power is 1
        sigma_ = (float)std::sqrt(noisePow / 2.0);
        echoAmp_ = cfg_.echoDb != 0 ? (float)std::pow(10.0, -std::fabs(cfg_.echoDb) / 20.0) : 0.f;
        echoDelay_ = (size_t)std::max(1, cfg_.echoDelay);
        echoRot_ = cf32(std::cos(0.8f), std::sin(0.8f));
        echoHist_.assign(echoDelay_, cf32(0, 0));
        scale_ = (float)(kLevel / std::sqrt(1.0 + (double)echoAmp_ * echoAmp_ + noisePow));
        dph_ = 2 * M_PI * cfg_.cfoHz / rate_;
        return true;
    }

    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        while (pending_.size() - pos_ < n) nextFrame();
        const cf32* src = &pending_[pos_];
        if (dph_ != 0) {   // carrier offset: an oscillator restarted from the exact phase every 128 samples, wherever the chunks end
            const cf32 stp((float)std::cos(dph_), (float)std::sin(dph_));
            size_t i = 0;
            while (i < n) {
                if (since_ == 0) {
                    const double ph = std::remainder(dph_ * (double)(count_ + i), 2 * M_PI);
                    rot_ = cf32((float)std::cos(ph), (float)std::sin(ph));
                }
                const size_t run = std::min(n - i, (size_t)(128 - since_));
                for (size_t k = 0; k < run; k++) { out[i + k] = src[i + k] * rot_; rot_ *= stp; }
                i += run;
                since_ = (since_ + (int)run) & 127;
            }
        } else for (size_t i = 0; i < n; i++) out[i] = src[i];
        count_ += n;
        if (sigma_ > 0) noise_.add(out, n, sigma_);
        for (size_t i = 0; i < n; i++) out[i] *= scale_;
        pos_ += n;
        if (pos_ > ((size_t)1 << 20) && pos_ >= pending_.size() / 2) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }

private:
    int guardSamples() const { return guardFromCode(cfg_.modeOpt[3] >= 1 && cfg_.modeOpt[3] <= 12 ? cfg_.modeOpt[3] : 5); }

    // data symbols of a frame: about 102 ms in all, a little more than the 100 ms the bootstrap promises as the least time to the next frame
    int numSymbolsFor(int guard) const {
        const double symSec = (8192.0 + guard) / kNativeRate;
        const double preSec = 8704.0 / kNativeRate;                     // the Preamble symbol (preamble structure 10: guard 512)
        const double bootSec = 4 * 3072.0 / kNativeRate;
        return std::max(8, (int)std::ceil((0.1015 - preSec - bootSec) / symSec));
    }

    // ---- the service: what one second of the programme puts on the air, as ALP packets
    uint32_t ipId_ = 1;
    void udp(std::vector<std::vector<uint8_t>>& ip, uint32_t dst, int port, const std::vector<uint8_t>& payload) {
        for (auto& p : makeUdpPackets(kSrcIp, dst, 4000, port, payload, (int)(ipId_++ & 0xFFFF))) ip.push_back(p);
    }
    void object(std::vector<std::vector<uint8_t>>& ip, uint32_t tsi, uint32_t toi, int cp, const std::vector<uint8_t>& data) {
        const int piece = 1100;
        for (size_t off = 0; off < data.size(); off += piece) {
            LctPacket p;
            p.tsi = tsi; p.toi = toi; p.codePoint = cp; p.startOffset = (uint32_t)off;
            const size_t n = std::min<size_t>(piece, data.size() - off);
            p.payload.assign(data.begin() + (long)off, data.begin() + (long)(off + n));
            p.transferLength = (int64_t)data.size();
            udp(ip, kDstIp, 3000, makeRoutePacket(p, true));
        }
    }

    // the ALP packets of the slot number `count` (counted from the start of the signal); the very first one has its own code point for the init segments
    std::vector<std::vector<uint8_t>> slotAlp(uint64_t count) {
        const int slot = (int)(count % (uint64_t)content_->slots);
        const uint64_t cycle = count / (uint64_t)content_->slots;
        static const std::string slt = "<SLT bsid=\"4660\"><Service serviceId=\"1001\" majorChannelNo=\"7\" minorChannelNo=\"1\" serviceCategory=\"1\" shortServiceName=\"ONAIR\" sltSvcSeqNum=\"0\">"
            "<BroadcastSvcSignaling slsProtocol=\"1\" slsDestinationIpAddress=\"239.255.5.1\" slsDestinationUdpPort=\"3000\" slsSourceIpAddress=\"10.1.1.5\"/></Service></SLT>";
        static const std::string stsid =
            "<S-TSID><RS dIpAddr=\"239.255.5.1\" dPort=\"3000\">"
            "<LS tsi=\"10\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"v1\" contentType=\"video\"/></ContentInfo></SrcFlow></LS>"
            "<LS tsi=\"20\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"a1\" contentType=\"audio\" lang=\"en\"/></ContentInfo></SrcFlow></LS></RS></S-TSID>";
        static const std::string mpd = "<MPD type=\"dynamic\"><Period/></MPD>";
        static const std::string usbd = "<BundleDescription/>";
        std::vector<MimePart> parts(3);
        parts[0].headers["content-type"] = "application/route-usd+xml";
        parts[0].body.assign(usbd.begin(), usbd.end());
        parts[1].headers["content-type"] = "application/route-s-tsid+xml";
        parts[1].body.assign(stsid.begin(), stsid.end());
        parts[2].headers["content-type"] = "application/dash+xml";
        parts[2].body.assign(mpd.begin(), mpd.end());
        std::vector<std::vector<uint8_t>> ip;
        const uint32_t toi = (uint32_t)(count + 1);
        udp(ip, kLlsAddress, kLlsPort, makeLls(1, 0, 1, slt));
        object(ip, 0, toi, 3, makeMultipart(parts, "b1"));
        object(ip, 10, 0, count == 0 ? 5 : 7, content_->vinit);
        object(ip, 20, 0, count == 0 ? 5 : 7, content_->ainit);
        object(ip, 10, toi, 8, atsc3synth::shiftedFragment(*content_, true, slot, cycle));
        object(ip, 20, toi, 8, atsc3synth::shiftedFragment(*content_, false, slot, cycle));
        std::vector<std::vector<uint8_t>> alp;
        for (auto& p : ip) { AlpPacket a; a.type = AlpIpv4; a.data = p; alp.push_back(alpSingle(a)); }
        return alp;
    }

    // ---- the queue of ALP bytes that becomes baseband packets
    std::vector<uint8_t> q_;
    size_t qOff_ = 0;
    uint64_t qBase_ = 0;
    std::deque<uint64_t> starts_;   // where the ALP packets begin (counted from the start of the signal)

    size_t queued() const { return q_.size() - qOff_; }

    void enqueueSlot() {
        // a slot that comes while a lot is still waiting is dropped: the receiver has to cope with a lost fragment anyway
        if (queued() < 2 * capBytesPerSlot_) {
            for (auto& a : slotAlp(slotCount_)) {
                starts_.push_back(qBase_ + q_.size());
                q_.insert(q_.end(), a.begin(), a.end());
            }
        } else ipId_ += 8;
        slotCount_++;
    }

    std::vector<std::vector<uint8_t>> takePackets(int limit) {
        std::vector<std::vector<uint8_t>> out;
        const int room = bytes_ - 2;
        while ((int)out.size() < limit && queued() > 0) {
            const uint64_t head = qBase_ + qOff_;
            while (!starts_.empty() && starts_.front() < head) starts_.pop_front();
            const int pointer = (!starts_.empty() && starts_.front() < head + (uint64_t)room) ? (int)(starts_.front() - head) : 8191;
            const size_t take = std::min<size_t>((size_t)room, queued());
            std::vector<uint8_t> chunk(q_.begin() + (long)qOff_, q_.begin() + (long)(qOff_ + take)), pk;
            if ((int)take == room) { BbHeader h; h.pointer = pointer; h.twoByteBase = true; pk = makeBbHeader(h); pk.insert(pk.end(), chunk.begin(), chunk.end()); }
            else pk = makeBbPacket(bytes_, chunk, pointer == 8191 ? -1 : pointer, -1);
            out.push_back(std::move(pk));
            qOff_ += take;
        }
        if (qOff_ > ((size_t)1 << 20) || (qOff_ == q_.size() && qOff_ > 0)) { qBase_ += qOff_; q_.erase(q_.begin(), q_.begin() + (long)qOff_); qOff_ = 0; }
        return out;
    }

    // ---- one frame: bootstrap + frame at the native rate, then into the channel
    void nextFrame() {
        const double start = (double)frameCount_ * (double)periodSamples_ / kNativeRate;
        while ((double)slotCount_ <= start + 1e-9) enqueueSlot();
        FramePlp fp = plp_;
        fp.bbPackets = takePackets(cap_);
        if (fp.bbPackets.empty()) fp.bbPackets.push_back(makeBbPacket(bytes_, {}, -1, -1));   // an all-padding packet
        auto fr = buildFrame(fs_, {fp});
        if (fr.empty()) { fp.bbPackets.resize(1); fr = buildFrame(fs_, {fp}); }
        native_.assign(boot_.begin(), boot_.end());
        native_.insert(native_.end(), fr.begin(), fr.end());
        native_.resize(periodSamples_, cf32(0, 0));
        const size_t old = pending_.size();
        if (fast_) fast_rs_.process(native_.data(), native_.size(), pending_);
        else resampler_.process(native_.data(), native_.size(), pending_);
        if (echoAmp_ > 0 && pending_.size() > old) {
            const size_t m = pending_.size() - old, d = echoDelay_;
            std::vector<cf32> tmp(d + m);
            std::copy(echoHist_.begin(), echoHist_.end(), tmp.begin());
            std::copy(pending_.begin() + (long)old, pending_.end(), tmp.begin() + (long)d);
            const cf32 g = echoRot_ * echoAmp_;
            for (size_t i = 0; i < m; i++) pending_[old + i] = tmp[d + i] + tmp[i] * g;
            std::copy(tmp.end() - (long)d, tmp.end(), echoHist_.begin());
        }
        frameCount_++;
    }

    SynthConfig cfg_;
    double rate_;
    FramePlp plp_;
    FrameSetup fs_;
    int bytes_ = 0, cellsPerBlock_ = 0, maxBlocks_ = 0, cap_ = 1;
    size_t capBytesPerSlot_ = 0;
    double frameRate_ = kNativeRate;
    size_t frameSamples_ = 0, periodSamples_ = 0;
    std::vector<cf32> boot_, native_;
    std::shared_ptr<const atsc3synth::Content> content_;
    uint64_t slotCount_ = 0, frameCount_ = 0;
    // channel
    atsc3synth::FastResampler fast_rs_;
    ExactResampler resampler_;   // for rates below the native one
    bool fast_ = true;
    genutil::NoiseSource noise_;
    float sigma_ = 0, scale_ = 1, echoAmp_ = 0;
    size_t echoDelay_ = 300;
    cf32 echoRot_;
    std::vector<cf32> echoHist_;
    double dph_ = 0;
    cf32 rot_{1.f, 0.f};
    int since_ = 0;   // samples since the oscillator was set to its exact phase
    uint64_t count_ = 0;
    std::vector<cf32> pending_;
    size_t pos_ = 0;
};

} // namespace

std::unique_ptr<ModeSynth> makeAtsc3Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    auto s = std::make_unique<Atsc3Synth>(cfg, sampleRate);
    if (!s->init()) return nullptr;
    return s;
}

} // namespace dect2
