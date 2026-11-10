// Bank of LoRa channels (see lora_bank.h).
#include "dect2/lorawan/lora_bank.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace lorabank {

namespace {
constexpr size_t kBlock = 4096;
}

struct Bank::Impl {
    struct Dec {
        std::unique_ptr<lora::Demod> dm;
        std::vector<size_t> stat;          // one row per sync word searched (one preamble search serves all of them)
        std::vector<int> syncs;
        int sf = 0, sync = 0;
        bool inverted = false;
        lora::DemodStats seen;
    };
    struct Chan {
        std::unique_ptr<lora::Channelizer> ch;   // one channel filter per channel ...
        size_t fdIndex = 0;                      // ... or the channel's slot in the shared frequency-domain channeliser
        double outRate = 0;
        int decim = 1;
        double delay = 0;
        lora::ChanBuf buf, bufInv;
        std::shared_ptr<lora::ChipStream> cs, csInv;     // chip-rate streams shared by the spreading factors of the channel           // bufInv: the conjugated samples (inverted IQ), only filled when a decoder wants them
        bool wantInv = false;
        std::vector<Dec> decs;
        size_t index = 0;
        double offsetHz = 0;
    };
    struct Recent { std::vector<uint8_t> payload; double sec; size_t chan; int sf; bool inv; };

    double rate = 0;
    std::vector<Channel> cfg;
    std::vector<Chan> chans;
    std::unique_ptr<lora::FdChannelizer> fd;   // used from 3 channels on: one FFT of the capture serves them all
    std::vector<lora::ChanBuf*> fdOut;
    std::vector<DecoderStat> stats;
    std::vector<cf32> pend;
    std::vector<lora::RxFrame> frames;
    std::vector<Recent> recent;
    int64_t nProc = 0;
    size_t running = 0, inBandChans = 0;

    void block(const cf32* x, size_t n, std::vector<Decoded>& out) {
        std::vector<int64_t> beforeFd;
        if (fd) {
            for (Chan& c : chans) { beforeFd.push_back(c.buf.end()); fdOut[c.fdIndex] = &c.buf; }
            fd->process(x, n, fdOut);
        }
        for (Chan& c : chans) {
            const int64_t before = fd ? beforeFd[c.fdIndex] : c.buf.end();
            if (!fd) c.ch->process(x, n, c.buf);
            if (c.wantInv) {
                std::vector<cf32> tmp((size_t)(c.buf.end() - before));
                for (size_t i = 0; i < tmp.size(); i++) tmp[i] = std::conj(c.buf.at(before + (int64_t)i)[0]);
                c.bufInv.append(tmp.data(), tmp.size());
            }
            c.cs->extend(c.buf);
            if (c.wantInv) c.csInv->extend(c.bufInv);
            int64_t keep = c.buf.end(), keepInv = c.bufInv.end();
            for (Dec& d : c.decs) {
                frames.clear();
                const lora::ChanBuf& b = d.inverted ? c.bufInv : c.buf;
                d.dm->process(b, frames);
                const lora::DemodStats& ds = d.dm->stats();
                for (size_t si : d.stat) {
                    DecoderStat& st = stats[si];
                    st.preambles = ds.preambles; st.headerBad = ds.headerBad; st.syncBad = ds.syncBad;
                }
                d.seen = ds;
                for (auto& f : frames) {
                    const double t = (f.startSample * c.decim - c.delay) / rate;   // channels start with the bank: sample 0
                    bool dup = false;
                    for (const auto& r : recent)
                        if (r.chan == c.index && (r.sf != d.sf || r.inv != d.inverted) && r.payload == f.payload && std::fabs(r.sec - t) < 0.25) dup = true;   // another setting locked the same frame
                    if (dup) continue;
                    recent.push_back({f.payload, t, c.index, d.sf, d.inverted});
                    if (recent.size() > 16) recent.erase(recent.begin());
                    size_t row = 0;
                    for (size_t si = 0; si < d.syncs.size(); si++) if (d.syncs[si] == f.syncWord) row = si;
                    stats[d.stat[row]].frames++;
                    if (f.hdr.crc && !f.crcOk) stats[d.stat[row]].crcBad++;
                    Decoded o;
                    o.chan = c.index; o.sf = d.sf; o.sync = d.syncs[row]; o.bwHz = cfg[c.index].bwHz; o.offsetHz = c.offsetHz; o.inverted = d.inverted;
                    o.sec = t; o.f = std::move(f);
                    out.push_back(std::move(o));
                }
                (d.inverted ? keepInv : keep) = std::min(d.inverted ? keepInv : keep, d.dm->oldestNeeded());
            }
            c.buf.trim(keep);
            c.cs->trimToSample(keep);
            if (c.wantInv) { c.bufInv.trim(keepInv); c.csInv->trimToSample(keepInv); }
        }
        nProc += (int64_t)n;
    }
};

Bank::Bank() : p_(std::make_unique<Impl>()) {}
Bank::~Bank() = default;

bool Bank::fits(double rate, const Channel& c) {
    const double fb = std::max(c.bwHz, c.filterBwHz);
    return rate >= 2.2 * fb && std::fabs(c.offsetHz) + 0.5 * fb + c.marginHz < 0.45 * rate;
}

void Bank::configure(double inputRate, const std::vector<Channel>& chans) {
    Impl& m = *p_;
    m.rate = inputRate;
    m.cfg = chans;
    m.chans.clear(); m.stats.clear(); m.pend.clear(); m.recent.clear();
    m.nProc = 0; m.running = 0; m.inBandChans = 0;
    m.fd.reset(); m.fdOut.clear();
    if (inputRate <= 0) return;
    {
        std::vector<lora::FdChannelizer::Spec> specs;
        for (const Channel& c : chans)
            if (fits(inputRate, c) && !c.sfs.empty() && !c.syncs.empty() && (c.normalIq || c.invertedIq)) specs.push_back({c.offsetHz, std::max(c.bwHz, c.filterBwHz)});
        if (lora::FdChannelizer::kEnabled && specs.size() >= 3) { m.fd = std::make_unique<lora::FdChannelizer>(inputRate, specs); m.fdOut.assign(specs.size(), nullptr); }
    }
    for (size_t ci = 0; ci < chans.size(); ci++) {
        const Channel& c = chans[ci];
        const bool inBand = fits(inputRate, c);
        const bool active = inBand && !c.sfs.empty() && !c.syncs.empty() && (c.normalIq || c.invertedIq);
        Impl::Chan* ch = nullptr;
        if (active) {
            m.chans.emplace_back();
            ch = &m.chans.back();
            if (m.fd) { ch->fdIndex = m.chans.size() - 1; ch->outRate = m.fd->outRate(ch->fdIndex); ch->decim = m.fd->decim(ch->fdIndex); ch->delay = 0; }
            else {
                ch->ch = std::make_unique<lora::Channelizer>(inputRate, c.offsetHz, std::max(c.bwHz, c.filterBwHz));
                ch->outRate = ch->ch->outRate(); ch->decim = ch->ch->decim(); ch->delay = ch->ch->delayIn();
            }
            ch->index = ci; ch->offsetHz = c.offsetHz;
            ch->buf.clear(0); ch->bufInv.clear(0);
            ch->wantInv = c.invertedIq;
            ch->cs = std::make_shared<lora::ChipStream>(ch->outRate / c.bwHz);
            ch->csInv = std::make_shared<lora::ChipStream>(ch->outRate / c.bwHz);
            m.inBandChans++;
        }
        for (int inv = 0; inv < 2; inv++) {
            if (inv ? !c.invertedIq : !c.normalIq) continue;
            if (c.syncs.empty()) continue;
            for (int sf : c.sfs) {
                Impl::Dec d;
                d.sf = sf; d.inverted = inv != 0; d.sync = c.syncs[0];
                for (int sw : c.syncs) {
                    DecoderStat st;
                    st.chan = ci; st.sf = sf; st.sync = sw; st.inverted = inv != 0; st.inBand = inBand;
                    m.stats.push_back(st);
                    d.stat.push_back(m.stats.size() - 1);
                    d.syncs.push_back(sw);
                }
                if (!active) continue;
                lora::Params p;
                p.sf = sf; p.bwHz = c.bwHz; p.syncWord = (uint8_t)c.syncs[0]; p.preamble = c.preamble;
                for (size_t si = 1; si < c.syncs.size(); si++) p.moreSync.push_back((uint8_t)c.syncs[si]);
                p.ldro = lora::autoLdro(sf, c.bwHz);
                p.cfoTrack = c.cfoTrack; p.carrierHz = c.carrierHz;
                if (c.implicitLen > 0) { p.implicitHdr = true; p.implicitLen = c.implicitLen; p.cr = 5; p.crc = false; }
                d.dm = std::make_unique<lora::Demod>(p, ch->outRate, 1, inv ? ch->csInv : ch->cs);
                d.dm->reset(0);
                ch->decs.push_back(std::move(d));
                m.running += c.syncs.size();
            }
        }
    }
}

void Bank::feed(const cf32* x, size_t n, std::vector<Decoded>& out) {
    Impl& m = *p_;
    if (m.rate <= 0 || n == 0) return;
    if (m.chans.empty()) { m.nProc += (int64_t)n; return; }
    size_t i = 0;
    if (!m.pend.empty()) {
        const size_t take = std::min(n, kBlock - m.pend.size());
        m.pend.insert(m.pend.end(), x, x + take);
        i = take;
        if (m.pend.size() == kBlock) { m.block(m.pend.data(), kBlock, out); m.pend.clear(); }
    }
    for (; i + kBlock <= n; i += kBlock) m.block(x + i, kBlock, out);
    if (i < n) m.pend.insert(m.pend.end(), x + i, x + n);
}

double Bank::timeSec() const { return p_->rate > 0 ? (double)(p_->nProc + (int64_t)p_->pend.size()) / p_->rate : 0; }
int64_t Bank::samples() const { return p_->nProc + (int64_t)p_->pend.size(); }
const std::vector<DecoderStat>& Bank::stats() const { return p_->stats; }
size_t Bank::decoders() const { return p_->running; }
size_t Bank::channelsInBand() const { return p_->inBandChans; }

} // namespace lorabank
} // namespace dect2
