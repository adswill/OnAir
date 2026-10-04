// The P2 stage of the DVB-T2 receiver: L1 signalling, extended carriers, integer frequency shift.
#include "t2rx_impl.h"

namespace dect2 {

bool T2Receiver::Impl::p2Hypothesis(bool ext, bool final) {
    const FftMode* fm = fftModeFromS2(fftCode);
    const int G = guard, N = fftN;
    PilotConfig pc; pc.fftCode = fftCode; pc.ext = ext; pc.pp = 0;
    PilotMap pm(pc);
    if (!pm.valid()) { pc.pp = 1; pm = PilotMap(pc); }
    if (!pm.valid()) return false;
    const int K = pm.carriers(), off = (kMax - K) / 2;
    const int S = fftCode == 5 ? 6 : 3;
    const int M = (K - 1) / S + 1;
    std::vector<cf32> hg(M, cf32(0, 0));
    std::vector<int> cnt(M, 0);
    std::vector<std::vector<cf32>> obs(nP2, std::vector<cf32>(M, cf32(0, 0)));
    std::vector<uint8_t> types;
    for (int l = 0; l < nP2; l++) {
        pm.symbolTypes(l, nP2 + 2, types);
        for (int n = 0; n < M; n++) {
            int kk = S * n;
            if (types[kk] != kCellP2Pilot) continue;
            cf32 v = p2cells[l][off + kk] / pm.pilot(l, kk, kCellP2Pilot).real();
            obs[l][n] = v;
            hg[n] += v;
            cnt[n]++;
        }
    }
    std::vector<char> have(M);
    for (int n = 0; n < M; n++) { have[n] = cnt[n] > 0; if (have[n]) hg[n] /= (float)cnt[n]; }
    for (int n = 0; n < M; n++) {
        if (have[n]) continue;
        int a = n - 1, b = n + 1;
        while (a >= 0 && !have[a]) a--;
        while (b < M && !have[b]) b++;
        if (a >= 0 && b < M) hg[n] = hg[a] + (hg[b] - hg[a]) * ((float)(n - a) / (float)(b - a));
        else if (a >= 0) hg[n] = hg[a];
        else if (b < M) hg[n] = hg[b];
    }
    const int back = std::min(G / 4, 32);
    std::vector<cf32> H;
    interp.run(hg, S, K, N, (double)back + G / 2.0, H);
    {
        // second pass: narrow the delay passband to where the channel actually has energy
        int tMin, tMax;
        int lim = std::min(N / 2 - 1, G * 3 / 2);
        bool spanOk = !getenv("DECT2_NONARROW") && delaySpan(H, N, -std::min(lim, std::max(G / 2, 64)), lim, back, tMin, tMax);
        if (getenv("DECT2_DEBUG") && spanOk) fprintf(stderr, "  [dbg] delay span %d .. %d samples (G=%d)\n", tMin, tMax, G);
        if (spanOk) {
            double centre = 0.5 * (tMin + tMax), half = 0.5 * (tMax - tMin) + 48;
            double cut = half / ((double)N / (2.0 * S));
            if (cut < 0.9) {
                interp.run(hg, S, K, N, (double)back + centre, H, cut);
                chDelayCentre = centre; chDelayHalf = half; chDelayKnown = true;
            } else { chDelayKnown = false; }
        } else chDelayKnown = false;
    }

    if (getenv("DECT2_DEBUG")) {
        std::vector<float> ir;
        impulseResponse(H, N, -N / 4, N / 2, back, ir);
        std::vector<std::pair<float,int>> pk;
        for (int i = 2; i + 2 < (int)ir.size(); i++) if (ir[i] > ir[i-1] && ir[i] >= ir[i+1] && ir[i] > -45) pk.push_back({ir[i], i - N / 4});
        std::sort(pk.rbegin(), pk.rend());
        fprintf(stderr, "  [dbg] P2 IR peaks (delay samples rel. main: dB):");
        for (size_t i = 0; i < pk.size() && i < 10; i++) fprintf(stderr, " %d:%.0f", pk[i].second, pk[i].first);
        fprintf(stderr, "\n");
    }
    // equalised P2 data cells, per symbol in carrier order, then frequency de-interleaved
    const int cP2 = pm.p2DataCells();
    std::vector<std::vector<cf32>> sym(nP2), gsym(nP2);
    std::vector<cf32> eqAll;
    for (int l = 0; l < nP2; l++) {
        pm.symbolTypes(l, nP2 + 2, types);
        std::vector<cf32> rx;
        rx.reserve(cP2);
        for (int kk = 0; kk < K; kk++)
            if (types[kk] == kCellData) rx.push_back(p2cells[l][off + kk] / H[kk]);
        std::vector<int> Hi;
        freqInterleaverSeq(fftCode, cP2, (l & 1) != 0, Hi);
        sym[l].assign(cP2, cf32(0, 0));
        gsym[l].assign(cP2, cf32(0, 0));
        {
            int jj = 0;
            for (int kk = 0; kk < K; kk++)
                if (types[kk] == kCellData && jj < cP2) { gsym[l][Hi[jj]] = cf32(std::norm(H[kk]), 0); jj++; }
        }
        for (int j = 0; j < cP2 && j < (int)rx.size(); j++) sym[l][Hi[j]] = rx[j];
        eqAll.insert(eqAll.end(), rx.begin(), rx.end());
    }
    std::vector<cf32> stream;
    p2Gather(sym, nP2, cP2, 1840, 0, stream);
    std::vector<cf32> preCells(stream.begin(), stream.begin() + 1840);
    float n0 = 0;
    for (auto& c : preCells) n0 += c.imag() * c.imag();
    n0 = std::max(1e-4f, n0 / 1840.f);
    if (getenv("DECT2_DEBUG")) {
        double mr = 0; for (auto& c : preCells) mr += std::fabs(c.real());
        int neg = 0; for (auto& c : preCells) neg += c.real() < 0;
        fprintf(stderr, "  [dbg] P2 hyp ext=%d: n0 %.4f mean|Re| %.3f neg %d/1840 K %d\n", (int)ext, n0, mr / 1840, neg, K);
    }
    L1Pre pre;
    L1Result r = decodeL1Pre(preCells, n0, pre);
    bool preOk = r.ok;
    l1Iters = r.ldpcIters;
    L1Post post;
    bool postOk = false;
    auto tryPost = [&](const L1Pre& pp, L1Post& out) {
        std::vector<cf32> st2;
        p2Gather(sym, nP2, cP2, 1840, pp.postSize, st2);
        std::vector<cf32> postCells(st2.begin() + 1840, st2.begin() + 1840 + std::min<size_t>(pp.postSize, st2.size() - 1840));
        return decodeL1Post(postCells, n0, pp, nP2, (pp.s2 & 1) != 0, out).ok;
    };
    if (preOk) postOk = tryPost(pre, post);
    if (!preOk && !final) return false;

    // The signalling (L1) hardly ever changes, and it is protected by a CRC that a weak frame sometimes fails: one frame in eight on a
    // marginal signal. Losing the L1 loses the whole 240 ms frame, which is a visible freeze, although its data would decode fine.
    // So when a block fails its CRC, the last good one is reused for a few frames; the data blocks have their own CRCs, which would
    // flag a real change of the configuration.
    bool reusedPre = false, reusedPost = false;
    {
        const int64_t anchorNow = frames.empty() ? 0 : frames.front().anchor;
        auto sameLayout = [](const L1Pre& a, const L1Pre& b) {
            return a.s1 == b.s1 && a.s2 == b.s2 && a.guardInterval == b.guardInterval && a.pilotPattern == b.pilotPattern && a.numDataSyms == b.numDataSyms &&
                   a.postSize == b.postSize && a.postInfoSize == b.postInfoSize && a.l1Mod == b.l1Mod && a.bwtExt == b.bwtExt && a.cellId == b.cellId &&
                   a.networkId == b.networkId && a.systemId == b.systemId;
        };
        if (haveGoodL1 && l1Reuse < 8 && !getenv("DECT2_NOL1REUSE")) {
            if (!preOk) { pre = goodPre; preOk = true; reusedPre = true; postOk = tryPost(pre, post); }
            if (preOk && !postOk && sameLayout(pre, goodPre)) {
                post = goodPost;
                const double fl = frameLen > 0 ? frameLen : 1.0;
                post.frameIdx = (goodPost.frameIdx + (int)std::llround((double)(anchorNow - goodAnchor) / fl)) & 0xFF;
                post.crcOk = true; postOk = true; reusedPost = true;
            }
        }
        if (preOk && postOk && !reusedPre && !reusedPost) { goodPre = pre; goodPost = post; goodAnchor = anchorNow; haveGoodL1 = true; l1Reuse = 0; }
        else if (reusedPre || reusedPost) { l1Reuse++; l1Reused++; }
    }

    // accept this hypothesis: publish channel data
    extDetected = ext;
    chH = H; chK = K; chValid = true;
    l1preOk = preOk; l1postOk = postOk;
    if (preOk) { l1pre = pre; if (reusedPre) l1preBad++; else l1preGood++; p2Sym = sym; p2G2.assign(nP2, {}); for (int l = 0; l < nP2; l++) { p2G2[l].resize(cP2); for (int j = 0; j < cP2; j++) p2G2[l][j] = gsym[l][j].real(); } } else l1preBad++;
    if (postOk) { l1post = post; if (reusedPost || reusedPre) l1postBad++; else l1postGood++; } else if (preOk) l1postBad++;
    l1N0 = n0;
    snrDbv.clear();
    float sigAll = 0, noiseAll = 0;
    if (nP2 >= 2) {
        std::vector<float> sig(M), nz(M);
        for (int n = 0; n < M; n++) {
            float v = 0, m2 = std::norm(hg[n]);
            int c = 0;
            for (int l = 0; l < nP2; l++) if (have[n]) { v += std::norm(obs[l][n] - hg[n]); c++; }
            sig[n] = m2;
            nz[n] = c > 1 ? v / (float)(c - 1) : 0.f; // per-observation noise variance (sample variance)
        }
        const int W = 12;
        for (int n = 0; n < M; n++) {
            double sa = 0, na = 0;
            for (int j = std::max(0, n - W); j <= std::min(M - 1, n + W); j++) { sa += sig[j]; na += nz[j]; }
            snrDbv.push_back((float)(10 * std::log10(std::max(1e-9, sa) / std::max(1e-12, na))));
        }
        for (int n = 0; n < M; n++) { sigAll += sig[n]; noiseAll += nz[n]; }
        p2Snr = (float)(10 * std::log10(std::max(1e-9f, sigAll) / std::max(1e-12f, noiseAll)));
    } else {
        // single P2 symbol: use the quadrature spread of the BPSK L1-pre cells
        p2Snr = (float)(10 * std::log10(0.5 / (2.0 * n0 > 1e-9 ? 2 * n0 : 1e-9)));
        (void)fm;
    }
    chMag.clear(); chPh.clear();
    int dec = std::max(1, K / 4096);
    for (int i = 0; i < K; i += dec) {
        chMag.push_back(20 * std::log10(std::max(1e-9f, std::abs(chH[i]))));
        chPh.push_back(std::arg(chH[i]));
    }
    irMin = -std::max(8, G / 4);
    impulseResponse(chH, N, irMin, G + std::max(8, G / 4), back, irDb);
    eqP2.clear();
    size_t step = std::max<size_t>(1, eqAll.size() / 4096);
    for (size_t i = 0; i < eqAll.size(); i += step) eqP2.push_back(eqAll[i]);
    if (preOk) {
        if (pre.guardInterval != giIdx && pre.guardInterval < kNumGi) {
            // the blind guard-interval guess was wrong: adopt the signalled value and resynchronise
            giIdx = pre.guardInterval;
            guard = guardSamples(fftN, giIdx);
            frames.clear();
            frameSyms = 0;
            frameLen = 0;
            prevPos = -1;
            dataValid = false;
            return preOk;
        }
        int total = nP2 + pre.numDataSyms;
        if (total != frameSyms) { frameSyms = total; frameMsv = (kP1Len + (double)total * (fftN + guard)) / fn * 1e3; }
        if (!frames.empty()) frames.front().maxSyms = std::max(frames.front().maxSyms, frameSyms);
    }
    return preOk;
}

int T2Receiver::Impl::p2IntegerShift() {
    PilotConfig pc; pc.fftCode = fftCode; pc.ext = false; pc.pp = 0;
    PilotMap pm(pc);
    for (int q = 1; !pm.valid() && q < 8; q++) { pc.pp = q; pm = PilotMap(pc); }
    if (!pm.valid() || p2cells.empty() || (int)p2cells[0].size() != kMax) return 0;
    const int K = pm.carriers(), off = (kMax - K) / 2;
    const int S = fftCode == 5 ? 6 : 3;
    std::vector<uint8_t> types;
    pm.symbolTypes(0, nP2 + 2, types);
    double best = -1, base = 0;
    int bestS = 0;
    for (int sh = -6; sh <= 6; sh++) {
        cd acc = 0;
        double pw = 0;
        for (int kk = 0; kk + S < K; kk += S) {
            if (types[kk] != kCellP2Pilot || types[kk + S] != kCellP2Pilot) continue;
            int i0 = off + kk + sh, i1 = off + kk + S + sh;
            if (i0 < 0 || i1 >= kMax) continue;
            const cf32 a = p2cells[0][i0], b = p2cells[0][i1];
            float p0 = pm.pilot(0, kk, kCellP2Pilot).real(), p1 = pm.pilot(0, kk + S, kCellP2Pilot).real();
            cf32 d = b * std::conj(a) * (p0 * p1);
            acc += cd(d.real(), d.imag());
            pw += std::abs(d);
        }
        double sc = pw > 0 ? std::abs(acc) / pw : 0;
        if (sh == 0) base = sc;
        if (sc > best) { best = sc; bestS = sh; }
    }
    if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] P2 integer-bin search: best shift %d score %.3f (shift 0: %.3f)\n", bestS, best, base);
    return (bestS != 0 && best > 0.35 && best > 1.8 * base) ? bestS : 0;
}

void T2Receiver::Impl::runP2Stage() {
    const FftMode* fm = fftModeFromS2(fftCode);
    if (!fm || p2cells.size() != (size_t)nP2) return;
    for (auto& c : p2cells) if ((int)c.size() != kMax) return;
    if (int sh = p2IntegerShift()) {
        double bin = fn / fftN;
        cfoEst += sh * bin;
        frameCfo = cfoEst;
        char b2[120]; snprintf(b2, sizeof b2, "corrected a %+d-carrier frequency offset (CFO now %+.1f Hz)", sh, cfoEst);
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] %s\n", b2);
        frames.clear(); frameCells.clear(); p2cells.clear();
        return;
    }
    bool extPossible = fm->kExt != 0;
    bool guess = false;
    if (extPossible) {
        int off = (kMax - fm->kNormal) / 2;
        double edge = 0, mid = 0;
        int ne = 0, nm = 0;
        for (auto& c : p2cells) {
            for (int i = 0; i < off; i++) { edge += std::norm(c[i]) + std::norm(c[kMax - 1 - i]); ne += 2; }
            for (int i = kMax / 2 - 200; i < kMax / 2 + 200; i++) { mid += std::norm(c[i]); nm++; }
        }
        guess = ne && nm && (edge / ne) > 0.25 * (mid / nm);
    }
    // try the more likely carrier mode first; L1-pre CRC decides
    if (!extPossible) { p2Hypothesis(false, true); return; }
    if (p2Hypothesis(guess, false)) return;
    p2Hypothesis(!guess, false) || p2Hypothesis(guess, true);
}

} // namespace dect2
