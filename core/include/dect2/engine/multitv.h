// Multi-channel TV: two or three adjacent DVB-T / DVB-T2 channels decoded from ONE wide capture (a LimeSDR, USRP, BladeRF or PlutoSDR covers
// 2-3 channels of 7 / 8 MHz at once).
//
// Since whole-span TV (docs/WHOLESPAN-TV-DESIGN.md) this is SpanTv (spantv/spantv.h) configured for DVB-T2 or DVB-T on the channel raster:
//   feed(span) -> SpanScanner (which raster slots carry a signal)
//              -> per occupied slot, on the slot's own thread: ChannelFilter (mix, low-pass, resample) -> receiver -> BB unpack -> TsDemux
//
// Every instance keeps its own receiver, transport-stream demultiplexer, service list and EPG. One slot is "selected": its packets go to the
// sink (the engine's player, outputs and network tuner) while the others keep decoding.
#pragma once
#include "spantv/spantv.h"
#include "dect2/t2/t2rx.h"           // RxTelemetry (poll), as before the refactor
#include "dect2/dvbt/dvbt_rx.h"

namespace dect2 {

struct MultiTvOptions {
    bool dvbt = false;            // false DVB-T2, true DVB-T
    double spanRate = 0;          // sample rate of what feed() gets
    double widthMhz = 8;          // channel width (7 VHF, 8 UHF Europe, 6 Americas / Asia)
    double gridOffsetHz = 0;      // a channel centre on the raster, relative to the centre of the capture (0: the capture is centred on a channel)
    bool autoGrid = false;        // pick 0 or half a channel width, whichever finds more channels
    int maxChannels = 2;          // receivers running at once
    bool blocking = true;         // a slow receiver holds feed() back (files, tests); false: it loses blocks (a live radio)
    double thresholdDb = 6;       // a slot is occupied this far above the quietest part of the span
};

class MultiTv : public SpanTv {
public:
    using SpanTv::configure;
    // false when the span cannot hold even one channel of this width
    bool configure(const MultiTvOptions& o) {
        mo_ = o;
        SpanTvOptions s;
        s.stdMode = o.dvbt ? 2 : 1;
        s.spanRate = o.spanRate; s.widthMhz = o.widthMhz; s.gridOffsetHz = o.gridOffsetHz; s.autoGrid = o.autoGrid;
        s.maxChannels = o.maxChannels; s.blocking = o.blocking; s.thresholdDb = o.thresholdDb;
        return SpanTv::configure(s);
    }
    const MultiTvOptions& options() const { return mo_; }

private:
    MultiTvOptions mo_;
};

} // namespace dect2
