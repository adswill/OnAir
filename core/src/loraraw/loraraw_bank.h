// Internals shared by the Signal Lab auto search and the wideband survey (loraraw_lab.cpp, loraraw_survey.cpp): one channel filter and chip stream
// per bandwidth with a demodulator for every parameter set on it.
#pragma once
#include "dect2/loraraw/loraraw_lab.h"
#include <algorithm>
#include <memory>

namespace dect2 {
namespace lora {
namespace detail {


inline constexpr size_t kBlock = 4096;
inline const double kBws[] = {500000, 250000, 125000, 62500, 41700, 31250, 20800, 15600, 10400, 7800};

inline Params paramsFor(const LabSettings& s, uint8_t sync) {
    Params p;
    p.sf = std::clamp(s.sf, 5, 12);
    p.bwHz = s.bwHz;
    p.cr = std::clamp(s.cr, 5, 8);
    p.syncWord = sync;
    p.preamble = s.preamble;
    p.ldro = s.ldro < 0 ? autoLdro(p.sf, s.bwHz) : s.ldro == 1;
    p.crc = s.crc;
    p.implicitHdr = s.implicitHdr;
    p.implicitLen = s.implicitLen;
    return p;
}

// One bandwidth: the channel filter once, a demodulator per entry of `dec` on the shared chip stream.
struct Bank {
    struct One { std::unique_ptr<Demod> dm; Params p; std::vector<RxFrame> frames; };
    std::unique_ptr<Channelizer> ch;
    ChanBuf buf;
    std::shared_ptr<ChipStream> cs;
    std::vector<One> decs;
    double outRate = 0;
    int64_t nIn = 0;

    Bank(double fs, double offsetHz, double bw, const std::vector<Params>& ps) {
        ch = std::make_unique<Channelizer>(fs, offsetHz, bw);
        outRate = ch->outRate();
        buf.clear(0);
        cs = std::make_shared<ChipStream>(outRate / bw);
        for (const Params& p : ps) {
            One o;
            o.p = p;
            o.dm = std::make_unique<Demod>(p, outRate, 1, cs);
            o.dm->reset(0);
            decs.push_back(std::move(o));
        }
    }

    void feed(const cf32* x, size_t n) {
        ch->process(x, n, buf);
        cs->extend(buf);
        int64_t keep = buf.end();
        for (One& o : decs) {
            o.dm->process(buf, o.frames);   // appends
            keep = std::min(keep, o.dm->oldestNeeded());
        }
        buf.trim(keep);
        cs->trimToSample(keep);
        nIn += (int64_t)n;
    }

    // all samples, conjugated when asked, then a short silence so that the last frame completes
    void run(const cf32* x, size_t n, bool inv, double padSec) {
        std::vector<cf32> tmp;
        for (size_t i = 0; i < n; i += kBlock) {
            const size_t m = std::min(kBlock, n - i);
            if (inv) { tmp.assign(x + i, x + i + m); for (auto& v : tmp) v = std::conj(v); feed(tmp.data(), m); }
            else feed(x + i, m);
        }
        const size_t padN = (size_t)(padSec * ch->decim() * outRate);
        tmp.assign(std::min(padN, (size_t)kBlock), cf32(0, 0));
        for (size_t d = 0; d < padN; d += tmp.size()) feed(tmp.data(), std::min(tmp.size(), padN - d));
    }
};

inline LabFrame toLab(const RxFrame& f, const LabSettings& s, double outRate) {
    LabFrame l;
    l.payload = f.payload;
    l.crcPresent = s.implicitHdr ? s.crc : f.hdr.crc;
    l.crcOk = f.crcOk;
    l.cr = s.implicitHdr ? s.cr : f.hdr.cr;
    l.corrected = f.corrected;
    l.startSec = f.startSample / outRate;
    l.snrDb = f.snrDb;
    l.cfoHz = f.cfoHz;
    l.levelDb = f.levelDb;
    l.syncWord = f.syncWord;
    return l;
}

inline double padFor(int sf, double bw) { return std::min(4.0, 4.0 * symbolSeconds(sf, bw)) + 0.05; }


}  // namespace detail
}  // namespace lora
}  // namespace dect2
