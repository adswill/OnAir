#include "t2rx_impl.h"

namespace dect2 {

std::string t2rxProfile() {
    const double* v = StageClock::g();
    char b[300];
    snprintf(b, sizeof b, "receiver stages (s): resampler %.2f, P1 search %.2f, guard check %.2f, symbols %.2f (including FFT %.2f and data stage %.2f; CP/timing %.2f, display %.2f, P2 stage %.2f)", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
    return b;
}

void T2Receiver::Impl::stageLoop() {
    setThreadPriority(ThreadPriority::Realtime);
    for (;;) {
        std::pair<uint64_t, std::vector<cf32>> item;
        {
            std::unique_lock<std::mutex> lk(stage.mu);
            stage.cvIn.wait(lk, [&] { return stage.stop || !stage.in.empty(); });
            if (stage.stop) return;
            item = std::move(stage.in.front());
            stage.in.pop_front();
        }
        stage.cvSpace.notify_all();
        std::vector<cf32> res;
        {
            std::lock_guard<std::mutex> rl(stage.rsMu);
            StageClock sc(0);
            if (item.second.empty()) resampler.reset();   // a gap marker: the filter history from before the gap is of no use
            else resampler.process(item.second.data(), item.second.size(), res);
        }
        std::lock_guard<std::mutex> lk(stage.mu);
        if (item.first == stage.gen) stage.out.emplace_back(item.first, std::move(res));
    }
}

void T2Receiver::Impl::gapReset(size_t skippedIn) {
    frames.clear(); frameCells.clear(); p2cells.clear(); prevCells.clear();
    chValid = false;
    haveRejected = false;
    pendingGridOff = 0;
    // The skipped samples still happened on the transmitter's timeline: move the sample index on by their length (at the native rate) so that
    // the P1 cadence carries on, and look for the next P1 where the frame length puts it. Lost for good only if that guess is off by more
    // than the search window, in which case the windowed search falls back to searching everything.
    const double skippedNative = inRate > 0 ? (double)skippedIn * fn / inRate : 0.0;
    base += (int64_t)buf.size() + (int64_t)std::llround(skippedNative);
    buf.clear();
    scanPos = base; scanFirst = true;
    lastP1Abs = INT64_MIN / 2;
    if (state == 2 && frameLen > 0 && prevPos >= 0) {
        const double k = std::max(1.0, std::ceil(((double)base - prevPos) / frameLen));
        prevPos += (k - 1.0) * frameLen;   // a virtual previous P1, one frame before the expected one
        trackExpect = prevPos + frameLen;
        trackMiss = 0; trackFullUntil = 0; trackKeep = base;
    } else {
        prevPos = -1; trackMiss = 3; trackExpect = 0; trackFullUntil = 0; trackKeep = -1;
    }
}

void T2Receiver::Impl::resetAll() {
    stageFlush();
    { std::lock_guard<std::mutex> rl(stage.rsMu); resampler.reset(); }
    buf.clear();
    base = 0;
    scanPos = 0;
    scanFirst = true;
    lastP1Abs = INT64_MIN / 2;
    trackMiss = trackFrames = 0; trackExpect = 0; trackFullUntil = 0; trackKeep = -1;
    p1Evaluated = p1Rescans = 0;
    trace.clear(); coarse.clear();
    state = 0;
    p1 = P1Info();
    p1Count = 0;
    prevPos = -1;
    giIdx = -1;
    std::fill(std::begin(giScore), std::end(giScore), 0.f);
    giMargin = 0;
    fftN = guard = carriers = nP2 = 0;
    fftCode = curS1 = -1;
    frameSyms = 0;
    frameLen = sro = frameMsv = 0;
    frames.clear();
    cfoEst = cpCorrAvg = timingAvg = 0;
    lowCount = 0;
    gridOff = 0;
    symbols = symCounter = 0;
    prevCells.clear();
    prevSymAbs = -1;
    p1Const.clear(); diffCells.clear(); rawCells.clear();
    p2cells.clear(); chValid = false; chH.clear(); chMag.clear(); chPh.clear(); irDb.clear(); snrDbv.clear(); eqP2.clear();
    l1pre = L1Pre(); l1post = L1Post(); l1preOk = l1postOk = false; haveGoodL1 = false; l1Reuse = 0; l1preGood = l1preBad = l1postGood = l1postBad = 0;
    frameCells.clear(); dataValid = false; eqData.clear(); dataSnrCar.clear(); dataFrames = 0;
}

void T2Receiver::Impl::run() {
    for (int guardLoop = 0; guardLoop < 8; guardLoop++) {
        { StageClock sc(1); scanP1(); }
        if (state == 1) { StageClock sc(2); evaluateGi(); }
        if (state == 2) processFrames();
        if (state == 2 && frames.empty() && lastP1Seen > 0 && end() - lastP1Seen > (int64_t)(std::max(frameLen, 0.35 * fn) * 3 + 2 * fn * 0.05)) {
            state = 0; // no P1 for a long while
        }
        // limit memory
        int64_t need = scanPos;
        if (trackKeep >= 0 && state == 2 && frameLen > 0 && prevPos >= 0 && !trackDisabled()) need = std::min(need, trackKeep);
        if (state == 1) need = std::min(need, giAnchor);
        if (!frames.empty()) need = std::min(need, frames.front().anchor);
        int64_t drop = need - base - 4096;
        if (drop > (1 << 20)) {
            buf.erase(buf.begin(), buf.begin() + drop);
            base += drop;
        }
        break;
    }
}

void T2Receiver::Impl::publish() {
    std::lock_guard<std::mutex> lk(mu);
    RxTelemetry& t = tel;
    t.seq++;
    t.rateOk = rateOk;
    t.decimating = decimate;
    t.inputRate = inRate;
    t.nativeRate = fn;
    t.state = state;
    t.p1 = p1;
    t.p1Count = p1Count;
    t.p1Evaluated = p1Evaluated; t.p1Rescans = p1Rescans;
    t.secSinceP1 = lastP1Seen ? (double)(end() - lastP1Seen) / fn : 1e9;
    t.frameMs = frameMsv;
    t.symbolsPerFrame = frameSyms;
    t.sroPpm = sro * 1e6;
    std::copy(giScore, giScore + kNumGi, t.giScore);
    t.giIdx = giIdx;
    t.giMargin = giMargin;
    t.cfoHz = state >= 1 ? cfoEst : p1.cfoHz;
    t.cpCorr = (float)cpCorrAvg;
    double r = std::min(0.999, std::max(0.001, cpCorrAvg));
    t.cpSnrDb = (float)(10 * std::log10(r / (1 - r)));
    t.timingErr = (float)timingAvg;
    t.symbols = symbols;
    t.fftN = fftN;
    t.guard = guard;
    t.carriers = carriers;
    t.p1Trace = trace;
    t.p1Const = p1Const;
    t.cells = diffCells;
    t.rawCells = rawCells;
    t.chValid = chValid;
    t.extCarriers = extDetected;
    t.chCarriers = chK;
    t.chMagDb = chMag;
    t.chPhase = chPh;
    t.chDecim = chK > 0 ? std::max(1, chK / 4096) : 1;
    t.irDb = irDb;
    t.irTauMin = irMin;
    t.snrDb = snrDbv;
    t.snrStep = fftCode == 5 ? 6 : 3;
    t.p2SnrDb = p2Snr;
    t.eqCells = eqP2;
    t.l1preOk = l1preOk; t.l1postOk = l1postOk;
    t.l1pre = l1pre; t.l1post = l1post;
    t.plpSelectedId = selectedPlpId;
    t.plpList.clear(); t.unsupported.clear();
    if (l1postOk) {
        for (size_t i = 0; i < l1post.plps.size(); i++) {
            const L1PlpConf& c = l1post.plps[i];
            RxTelemetry::PlpInfo pi;
            pi.id = c.id; pi.type = c.type; pi.payloadType = c.payloadType; pi.mod = c.mod; pi.cod = c.cod; pi.rotation = c.rotation;
            pi.fecType = c.fecType; pi.tiType = c.timeIlType; pi.tiLength = c.timeIlLength;
            pi.blocks = i < l1post.dyn.size() ? l1post.dyn[i].numBlocks : 0;
            pi.supported = c.timeIlType == 0 && c.type != 2 && l1post.subSlices <= 1;
            t.plpList.push_back(pi);
            char b[160];
            if (c.timeIlType != 0) { snprintf(b, sizeof b, "PLP %d uses inter-frame time interleaving (TIME_IL_TYPE 1), which is not implemented", c.id); t.unsupported.push_back(b); }
            if (c.type == 2 || l1post.subSlices > 1) { snprintf(b, sizeof b, "PLP %d is sub-sliced (type 2 / %d sub-slices per frame), which is not implemented", c.id, l1post.subSlices); t.unsupported.push_back(b); }
        }
    }
    if (l1preOk) {
        if (l1pre.s1 == 1 || l1pre.s1 == 4) t.unsupported.insert(t.unsupported.begin(), "MISO transmission (two transmitters, Alamouti coding) is not implemented");
        if (l1pre.type != 0) t.unsupported.push_back("the multiplex carries generic streams (GSE/GS), not an MPEG transport stream");
    }
    if (l1postOk && l1post.fefLength > 0) t.unsupported.push_back("the signal contains FEF (future extension) frames; they are skipped");
    t.l1preGood = l1preGood; t.l1preBad = l1preBad; t.l1postGood = l1postGood; t.l1postBad = l1postBad;
    t.l1Iters = l1Iters;
    t.dataValid = dataValid;
    t.dataPp = dataPp; t.dataDx = dataDx; t.dataDy = dataDy;
    t.dataSnrDb = dataSnr;
    t.eqData = eqData;
    t.dataSnrCarrier = dataSnrCar;
    t.dataFrames = dataFrames;
    pollPlp();
    t.plpValid = plpValid; t.plpId = plpId; t.plpFec = plpFec; t.plpBlocks = plpBlocks;
    t.plpFrames = plpFrames; t.plpFramesDropped = plpDec.dropped();
    t.blocksOk = blocksOk; t.blocksBad = blocksBad; t.headersOk = headersOk; t.plpBchCorrected = plpBchCorr;
    t.plpMerDb = plpMer; t.plpPreBer = plpPre; t.plpIters = plpIters; t.plpDecodeMs = plpMs; t.plpOnGpu = plpGpu; t.gpuAvailable = plpDec.gpuAvailable(); t.computeMode = plpDec.mode();
    t.plpConst = plpConst; t.plpConstErr = plpConstErr; t.plpConstTx = plpConstTx; t.plpConstSeq = plpConstSeq;
    t.blockMap.assign(blockMaps.begin(), blockMaps.end());
    t.plpHeaderUpl = hUpl; t.plpHeaderDfl = hDfl; t.plpHeaderSyncd = hSyncd; t.plpSkipped = plpSkipped;
    if (dataValid) t.snrStep = dataDx;
}

T2Receiver::T2Receiver() : p_(new Impl) {}
T2Receiver::~T2Receiver() = default;

void T2Receiver::configure(double inputRateHz, double bandwidthMhz) {
    Impl& I = *p_;
    std::lock_guard<std::mutex> lk(I.mu);
    I.inRate = inputRateHz;
    I.fn = nativeRateHz(bandwidthMhz);
    {
        std::lock_guard<std::mutex> rl(I.stage.rsMu);
        I.rateOk = I.resampler.configure(inputRateHz, I.fn) && inputRateHz >= 7.9e6 * (bandwidthMhz / 8.0);
    }
    I.decimate = I.rateOk && !I.resampler.passthrough();
    I.gateOff = getenv("DECT2_NOP1GATE") != nullptr;
    I.resetAll();
}

void T2Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetAll();
}

void T2Receiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rateOk || I.fn <= 0) return;
    if (I.pipelined && I.decimate) {
        {
            std::unique_lock<std::mutex> lk(I.stage.mu);
            I.stage.cvSpace.wait(lk, [&] { return I.stage.in.size() < Impl::kStageMaxIn || I.stage.stop; });
            I.stage.in.emplace_back(I.stage.gen, std::vector<cf32>(x, x + n));
        }
        I.stage.cvIn.notify_one();
        for (;;) {   // process what the stage has finished, in order
            std::vector<cf32> res;
            {
                std::lock_guard<std::mutex> lk(I.stage.mu);
                if (I.stage.out.empty()) break;
                res = std::move(I.stage.out.front().second);
                I.stage.out.pop_front();
            }
            if (res.empty()) {   // a gap marker
                size_t skipped = 0;
                { std::lock_guard<std::mutex> lk(I.stage.mu); if (!I.stage.gaps.empty()) { skipped = I.stage.gaps.front(); I.stage.gaps.pop_front(); } }
                I.gapReset(skipped);
                continue;
            }
            I.buf.insert(I.buf.end(), res.begin(), res.end());
            I.run();
        }
        return;
    }
    if (I.decimate) { StageClock sc(0); I.rsOut.clear(); I.resampler.process(x, n, I.rsOut); I.buf.insert(I.buf.end(), I.rsOut.begin(), I.rsOut.end()); }
    else I.buf.insert(I.buf.end(), x, x + n);
    I.run();
}

void T2Receiver::markGap(size_t skippedSamples) {
    Impl& I = *p_;
    if (!I.rateOk || I.fn <= 0) return;
    if (I.pipelined && I.decimate) {   // goes through the resampler thread so that it lands between the right two chunks
        {
            std::unique_lock<std::mutex> lk(I.stage.mu);
            I.stage.cvSpace.wait(lk, [&] { return I.stage.in.size() < Impl::kStageMaxIn || I.stage.stop; });
            I.stage.in.emplace_back(I.stage.gen, std::vector<cf32>());
            I.stage.gaps.push_back(skippedSamples);
        }
        I.stage.cvIn.notify_one();
        return;
    }
    if (I.decimate) { std::lock_guard<std::mutex> rl(I.stage.rsMu); I.resampler.reset(); }
    I.gapReset(skippedSamples);
}

void T2Receiver::setPipelined(bool on) {
    Impl& I = *p_;
    if (on == I.pipelined) return;
    if (on) I.stageStart(); else I.stageStop();
    I.pipelined = on;
}

void T2Receiver::selectPlp(int id) { p_->plpSelect = id; }
void T2Receiver::setComputeMode(int m) { p_->plpDec.setMode(m); }
void T2Receiver::setPlpCallback(std::function<void(const PlpResult&)> cb) { p_->plpCb = std::move(cb); }

bool T2Receiver::telemetry(RxTelemetry& out, uint64_t lastSeq) {
    Impl& I = *p_;
    I.publish();
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.tel.seq <= lastSeq) return false;
    out = I.tel;
    return true;
}

} // namespace dect2
