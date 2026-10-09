// The data stage of the DVB-T2 receiver: channel estimation from the scattered pilots and handing cells to the error correction.
#include "t2rx_impl.h"

namespace dect2 {

void T2Receiver::Impl::submitPlp(const std::vector<cf32>& dstream, const std::vector<float>& dn0, double sigma2) {
    const int cP2 = (int)p2Sym[0].size();
    const int nPre = 1840, nPost = l1pre.postSize;
    // pick the data PLP (first non-common PLP, else the first)
    int sel = -1;
    const int want = plpSelect.load();
    if (want >= 0) for (size_t i = 0; i < l1post.plps.size(); i++) if (l1post.plps[i].id == want) { sel = (int)i; break; }
    if (sel < 0) { sel = 0; for (size_t i = 0; i < l1post.plps.size(); i++) if (l1post.plps[i].type == 1) { sel = (int)i; break; } }
    if (sel < 0 || sel >= (int)l1post.plps.size()) return;
    const L1PlpConf& pc = l1post.plps[sel];
    if (sel >= (int)l1post.dyn.size()) return;
    const L1PlpDyn& dy = l1post.dyn[sel];
    selectedPlpId = pc.id;
    if (pc.timeIlType != 0 || pc.type == 2 || l1post.subSlices > 1) { plpSkipped++; return; } // inter-frame interleaving and sub-slicing are not implemented
    PlpJob job;
    job.frameNo = ++frameCounter;
    job.t2Frame = l1post.frameIdx;
    job.numT2Frames = l1pre.numFrames;
    job.frameInterval = std::max(1, pc.frameInterval);
    job.frameSec = frameMsv / 1e3;
    job.plpId = pc.id;
    job.fec.shortFrame = pc.fecType == 0;
    job.fec.rate = pc.cod;
    job.fec.mod = pc.mod;
    job.fec.rotation = pc.rotation != 0;
    job.numBlocks = dy.numBlocks;
    job.tiBlocks = pc.timeIlLength;
    FecDims d = fecDims(job.fec);
    if (!d.ok || job.numBlocks <= 0) { plpSkipped++; return; }
    std::vector<cf32> stream, g2s;
    p2Gather(p2Sym, nP2, cP2, nPre, nPost, stream);
    {
        std::vector<std::vector<cf32>> gs(nP2);
        for (int l = 0; l < nP2; l++) { gs[l].resize(cP2); for (int j = 0; j < cP2; j++) gs[l][j] = p2G2[l][j]; }
        p2Gather(gs, nP2, cP2, nPre, nPost, g2s);
    }
    std::vector<float> n0s(stream.size());
    for (size_t i = 0; i < stream.size(); i++) n0s[i] = (float)(sigma2 / (2.0 * std::max(1e-12f, g2s[i].real())));
    stream.insert(stream.end(), dstream.begin(), dstream.end());
    n0s.insert(n0s.end(), dn0.begin(), dn0.end());
    const size_t start = (size_t)nPre + nPost + dy.start;
    const size_t need = (size_t)job.numBlocks * d.cellsPerBlock;
    if (start + need > stream.size()) { plpSkipped++; return; }
    job.cells.assign(stream.begin() + start, stream.begin() + start + need);
    job.n0.assign(n0s.begin() + start, n0s.begin() + start + need);
    plpValid = true; plpId = pc.id; plpFec = job.fec; plpBlocks = job.numBlocks;
    plpDec.submit(std::move(job));
}

void T2Receiver::Impl::pollPlp() {
    PlpResult r;
    while (plpDec.poll(r)) {
        plpFrames++;
        blocksOk += r.blocksOk; blocksBad += r.bchFailed; headersOk += r.headerOk; plpBchCorr += r.bchCorrected;
        plpMer = r.blocksOk ? r.merDb : plpMer;
        plpPre = r.preBer; plpIters = r.avgLdpcIters; plpMs = r.decodeMs; plpGpu = r.usedGpu;
        {
            std::vector<uint8_t> bm(r.frames.size(), 0);
            for (size_t i = 0; i < r.frames.size(); i++) bm[i] = r.frames[i].bits.empty() ? 0 : 1;
            blockMaps.push_back(std::move(bm));
            if (blockMaps.size() > 90) blockMaps.pop_front();
        }
        if (!r.constellation.empty()) { plpConst = r.constellation; plpConstErr = r.constErr; plpConstTx = r.constTx; plpConstSeq++; }
        for (auto& f : r.frames) if (f.header.crcOk) { hUpl = f.header.upl; hDfl = f.header.dfl; hSyncd = f.header.syncd; break; }
        if (plpCb) plpCb(r);
    }
}

void T2Receiver::Impl::runDataStage() {
    StageClock sc(5);
    const FftMode* fm = fftModeFromS2(fftCode);
    if (!fm || !l1preOk) return;
    const int L = frameSyms, N = fftN, G = guard;
    for (int l = 0; l < L; l++) if ((int)frameCells[l].size() != kMax) return;
    PilotConfig pc;
    pc.fftCode = fftCode; pc.ext = l1pre.bwtExt != 0; pc.pp = l1pre.pilotPattern; pc.tr = (l1pre.papr & 2) != 0; pc.giIdx = giIdx;
    {
        const PilotConfig& c0 = dsCache.pc;
        if (!dsCache.pm || dsCache.L != L || c0.fftCode != pc.fftCode || c0.ext != pc.ext || c0.pp != pc.pp || c0.tr != pc.tr || c0.giIdx != pc.giIdx) {
            dsCache.pm.reset(new PilotMap(pc));
            dsCache.pc = pc; dsCache.L = L;
            dsCache.types.assign(L, {});
            if (dsCache.pm->valid()) for (int l = 0; l < L; l++) dsCache.pm->symbolTypes(l, L, dsCache.types[l]);
        }
    }
    const PilotMap& pm = *dsCache.pm;
    if (!pm.valid()) { dataValid = false; return; }
    const int K = pm.carriers(), off = (kMax - K) / 2, dx = pm.dx(), dy = pm.dy();
    const int M = (K - 1) / dx + 1;
    struct Obs { int l; cf32 v; uint8_t type; };
    std::vector<std::vector<Obs>> obs(M);
    const std::vector<std::vector<uint8_t>>& types = dsCache.types;
    for (int l = 0; l < L; l++) {
        for (int n = 0; n < M; n++) {
            int k = dx * n;
            uint8_t t = types[l][k];
            if (t != kCellP2Pilot && t != kCellScattered && t != kCellContinual) continue;
            cf32 pv = pm.pilot(l, k, t);
            obs[n].push_back({l, frameCells[l][off + k] / pv.real(), t});
        }
    }
    if (getenv("DECT2_DEBUG")) {
        // which pilot configuration explains the received cells? coherence of consecutive symbols on that config's continual pilots
        for (int e2 = 0; e2 < 2; e2++) for (int ppc = 0; ppc < 8; ppc++) {
            PilotConfig c2 = pc; c2.pp = ppc; c2.ext = e2;
            PilotMap m2(c2);
            if (!m2.valid()) continue;
            int K2 = m2.carriers(), off2 = (kMax - K2) / 2;
            cd tot = 0; double mag = 0; int n2 = 0;
            std::vector<uint8_t> ta, tb;
            for (int l = nP2 + 1; l < std::min(L - 1, nP2 + 12); l++) {
                m2.symbolTypes(l, L, ta); m2.symbolTypes(l - 1, L, tb);
                for (int kk = 0; kk < K2; kk++) {
                    if (ta[kk] != kCellContinual || tb[kk] != kCellContinual) continue;
                    cf32 za = frameCells[l][off2 + kk] / m2.pilot(l, kk, kCellContinual).real();
                    cf32 zb = frameCells[l - 1][off2 + kk] / m2.pilot(l - 1, kk, kCellContinual).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    tot += pr; mag += std::abs(pr); n2++;
                }
            }
            // scattered pilots, symbols 16 apart / dy apart, same carriers: coherence over dy symbols
            cd tot2 = 0; double mag2 = 0; int n3 = 0;
            int dy2 = m2.dy();
            for (int l = nP2; l + dy2 < std::min(L - 1, nP2 + 3 * dy2); l++) {
                m2.symbolTypes(l, L, ta); m2.symbolTypes(l + dy2, L, tb);
                for (int kk = 0; kk < K2; kk++) {
                    if (ta[kk] != kCellScattered || tb[kk] != kCellScattered) continue;
                    cf32 za = frameCells[l][off2 + kk] / m2.pilot(l, kk, kCellScattered).real();
                    cf32 zb = frameCells[l + dy2][off2 + kk] / m2.pilot(l + dy2, kk, kCellScattered).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    tot2 += pr; mag2 += std::abs(pr); n3++;
                }
            }
            fprintf(stderr, "  [dbg] ext %d PP%d: continual coherence %.3f (%d)  scattered(dy) coherence %.3f (%d) phase %.2f\n", e2, ppc + 1, mag > 0 ? std::abs(tot) / mag : 0.0, n2, mag2 > 0 ? std::abs(tot2) / mag2 : 0.0, n3, std::arg(tot2));
        }
    }
    // Common phase error and residual timing: compare each symbol with the previous one on the continual pilots.
    // The symbol grid has integer-sample resolution, so the sub-sample timing error shows up as a phase slope
    // across the carriers (up to ~1 rad at the band edge for 32K); it is estimated and removed here.
    {
        double cumA = 0, cumB = 0;
        std::vector<int> cont;
        std::vector<cd> prod;
        // raw (uncorrected) cells of the previous symbol, kept at its pilot carriers only (the only ones that are compared)
        std::vector<cf32> prevRaw(K), curRaw(K);
        auto isPilot = [](uint8_t t) { return t == kCellContinual || t == kCellScattered; };
        for (int kk = 0; kk < K; kk++) if (isPilot(types[nP2][kk])) prevRaw[kk] = frameCells[nP2][off + kk];
        for (int l = nP2 + 1; l < L; l++) {
            cont.clear(); prod.clear();
            for (int kk = 0; kk < K; kk++) if (isPilot(types[l][kk])) curRaw[kk] = frameCells[l][off + kk];
            // Continual pilots on both symbols as a rule. The frame-closing symbol has none: there the carriers where both symbols have a
            // pilot of any kind are used (every dx * dy-th). Skipping it left the closing symbol without the phase and timing that the
            // symbols before it had accumulated (several radians with a few tens of Hz of residual frequency offset): its pilots then
            // spoiled the frame-wide channel fit, and every cell of the frame with it (a real 32K PP4 multiplex: MER 10-15 dB, no PLP).
            for (int pass = 0; pass < 2 && cont.size() < 4; pass++) {
                cont.clear(); prod.clear();
                for (int kk = 0; kk < K; kk++) {
                    const uint8_t ta = types[l][kk], tb = types[l - 1][kk];
                    if (pass == 0 ? (ta != kCellContinual || tb != kCellContinual) : (!isPilot(ta) || !isPilot(tb))) continue;
                    cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, ta).real();
                    cf32 zb = prevRaw[kk] / pm.pilot(l - 1, kk, tb).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    cont.push_back(kk);
                    prod.push_back(pr);
                }
            }
            std::swap(prevRaw, curRaw);
            if (cont.size() < 4) continue;
            // slope from phase differences of neighbouring pilots
            cd sl = 0;
            double wsum = 0, dsum = 0;
            for (size_t i = 0; i + 1 < cont.size(); i++) {
                double w = std::min(std::abs(prod[i]), std::abs(prod[i + 1]));
                sl += prod[i + 1] * std::conj(prod[i]) / std::max(1e-12, std::abs(prod[i]) * std::abs(prod[i + 1])) * w;
                dsum += w * (cont[i + 1] - cont[i]);
                wsum += w;
            }
            double beta = (wsum > 0 && dsum > 0) ? std::arg(sl) / (dsum / wsum) : 0.0;
            cd a = 0;
            for (size_t i = 0; i < cont.size(); i++) a += prod[i] * std::polar(1.0, -beta * (cont[i] - (K - 1) / 2.0));
            double alpha = std::arg(a);
            cumA += alpha;
            cumB += beta;
            if (getenv("DECT2_DEBUG") && l < nP2 + 14) fprintf(stderr, "  [dbg] sym %d: alpha %+.3f beta*10k %+.3f (cum %+.2f, %+.2f) pilots %zu\n", l, alpha, beta * 1e4, cumA, cumB * 1e4, cont.size());
            // rotate by exp(-j (cumA + cumB (kk - (K-1)/2))): a phasor stepped along the carriers, re-anchored every 256 (a sin and cos per cell is slow)
            const cd stepR(std::cos(-cumB), std::sin(-cumB));
            cf32* row = &frameCells[l][off];
            for (int k0 = 0; k0 < K; k0 += 256) {
                const double ph = -(cumA + cumB * (k0 - (K - 1) / 2.0));
                cd z(std::cos(ph), std::sin(ph));
                const int k1 = std::min(K, k0 + 256);
                for (int kk = k0; kk < k1; kk++) { row[kk] *= cf32((float)z.real(), (float)z.imag()); z *= stepR; }
            }
        }
        if (getenv("DECT2_DEBUG")) {
            std::vector<uint8_t> ta2, tb2;
            cd tot2 = 0; double mag2 = 0; int n3 = 0;
            for (int l = nP2; l + dy < std::min(L - 1, nP2 + 3 * dy); l++) {
                for (int kk = 0; kk < K; kk++) {
                    if (types[l][kk] != kCellScattered || types[l + dy][kk] != kCellScattered) continue;
                    cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellScattered).real();
                    cf32 zb = frameCells[l + dy][off + kk] / pm.pilot(l + dy, kk, kCellScattered).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    tot2 += pr; mag2 += std::abs(pr); n3++;
                }
            }
            for (int lag : {1, 4, 16}) {
                cd tc = 0; double mc = 0; int nc = 0;
                for (int l = nP2; l + lag < std::min(L, nP2 + 40); l++)
                    for (int kk = 0; kk < K; kk++) {
                        if (types[l][kk] != kCellContinual || types[l + lag][kk] != kCellContinual) continue;
                        cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellContinual).real();
                        cf32 zb = frameCells[l + lag][off + kk] / pm.pilot(l + lag, kk, kCellContinual).real();
                        cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                        tc += pr; mc += std::abs(pr); nc++;
                    }
                fprintf(stderr, "  [dbg] continual coherence at lag %d: %.3f (%d)\n", lag, mc > 0 ? std::abs(tc) / mc : 0.0, nc);
            }
            fprintf(stderr, "  [dbg] AFTER correction: scattered(dy) coherence %.3f (%d) phase %.2f\n", mag2 > 0 ? std::abs(tot2) / mag2 : 0.0, n3, std::arg(tot2));
        }
        // rebuild the pilot observations from the phase-corrected cells
        for (int n = 0; n < M; n++) obs[n].clear();
        for (int l = 0; l < L; l++)
            for (int n = 0; n < M; n++) {
                int k = dx * n;
                uint8_t t = types[l][k];
                if (t != kCellP2Pilot && t != kCellScattered && t != kCellContinual) continue;
                obs[n].push_back({l, frameCells[l][off + k] / pm.pilot(l, k, t).real(), t});
            }
    }
    // per-grid-point noise from scattered-pilot second differences
    std::vector<float> varN(M, 0.f), pw(M, 0.f);
    double sumSig = 0, sumNoise = 0;
    for (int n = 0; n < M; n++) {
        std::vector<const Obs*> sc;
        for (auto& o : obs[n]) if (o.type == kCellScattered) sc.push_back(&o);
        double acc = 0, p = 0;
        int c = 0;
        for (size_t j = 1; j + 1 < sc.size(); j++) {
            double w = (double)(sc[j]->l - sc[j - 1]->l) / (double)(sc[j + 1]->l - sc[j - 1]->l);
            cf32 pred = sc[j - 1]->v + (sc[j + 1]->v - sc[j - 1]->v) * (float)w;
            double e = std::norm(sc[j]->v - pred) / (1.0 + w * w + (1 - w) * (1 - w));
            acc += e;
            p += std::norm(sc[j]->v);
            c++;
        }
        if (c) { varN[n] = (float)(acc / c); pw[n] = (float)(p / c); sumSig += pw[n]; sumNoise += varN[n]; }
    }
    const float amp2 = [&] { cf32 pv = pm.pilot(nP2, 0, kCellScattered); return pv.real() * pv.real(); }();
    const float cpAmp2 = [&] { for (int kk = 0; kk < K; kk++) if (types[nP2][kk] == kCellContinual) { cf32 pv = pm.pilot(nP2, kk, kCellContinual); return pv.real() * pv.real(); } return amp2; }();
    dataSnrCar.clear();
    const int Wd = 10;
    // noise power of the cells per grid point (over 2 Wd + 1 neighbours), for the LDPC input: the noise is not flat across the band on
    // real signals (a spur, an adjacent channel folded in, a filter edge), and a band-average would make the cells in the worst part
    // over-confident and those in the best part timid
    std::vector<float> nGrid(M);
    for (int n = 0; n < M; n++) {
        double sa = 0, na = 0; int c = 0;
        for (int j = std::max(0, n - Wd); j <= std::min(M - 1, n + Wd); j++) { sa += pw[j]; na += varN[j]; c += varN[j] > 0; }
        dataSnrCar.push_back((float)(10 * std::log10(std::max(1e-9, sa) / std::max(1e-12, na * amp2))));
        nGrid[n] = c ? (float)(na / c * amp2) : 0.f;
    }
    dataSnr = (float)(10 * std::log10(std::max(1e-9, sumSig) / std::max(1e-12, sumNoise * amp2)));
    if (getenv("DECT2_DEBUG")) {
        int cntN = 0; for (int n = 0; n < M; n++) cntN += varN[n] > 0;
        double ratioMed = 0; std::vector<double> rr; for (int n = 0; n < M; n++) if (varN[n] > 0 && pw[n] > 0) rr.push_back(10 * std::log10(pw[n] / varN[n]));
        std::sort(rr.begin(), rr.end()); if (!rr.empty()) ratioMed = rr[rr.size() / 2];
        fprintf(stderr, "  [dbg] data SNR: grid %d with noise est %d, sumSig %.3g sumNoise %.3g amp2 %.2f median pilot-SNR %.1f dB => %.1f dB\n", M, cntN, sumSig, sumNoise, amp2, ratioMed, dataSnr);
    }

    // channel per data symbol: time-interpolate each grid point, then frequency-interpolate
    std::vector<size_t> ptr(M, 0);
    std::vector<cf32> grid(M), H;
    const int back = std::min(G / 4, 32);
    std::vector<cf32> all;
    const int firstData = nP2;
    // noise power of the raw FFT cells: variance of the pilot observations times the pilot power
    double sigma2 = 0;
    {
        double sn = 0; int cn = 0;
        for (int n = 0; n < M; n++) if (varN[n] > 0) { sn += varN[n]; cn++; }
        sigma2 = cn ? sn / cn * amp2 : 0;
    }
    std::vector<cf32> dstream; std::vector<float> dn0;
    // per carrier, linear between the grid points (never below a tenth of the average: a lucky quiet stretch is no reason for certainty)
    std::vector<float> noiseCar(K, 0.f);
    for (int kk = 0; kk < K; kk++) {
        const int n0i = std::min(M - 1, kk / dx), n1i = std::min(M - 1, n0i + 1);
        const float w = (float)(kk - n0i * dx) / (float)dx;
        noiseCar[kk] = std::max((float)(0.1 * sigma2), nGrid[n0i] + w * (nGrid[n1i] - nGrid[n0i]));
    }
    const bool wantPlp = l1postOk && !l1post.plps.empty() && !p2Sym.empty() && sigma2 > 0;
    // Per grid carrier: weighted least-squares fit of H(l) = a + b (l - lbar) over every pilot observation in the frame
    // (P2 symbols excluded: they carry a different phase/timing reference). The slope is shrunk towards zero when it is
    // not significant, which averages the pilot noise over the whole frame for a quasi-static channel.
    std::vector<cf32> fa(M), fb(M);
    std::vector<float> flbar(M, 0.f);
    for (int n = 0; n < M; n++) {
        double sw = 0, sl = 0;
        cd sv = 0;
        int cnt = 0;
        for (auto& o : obs[n]) {
            if (o.type == kCellP2Pilot) continue;
            const double w = (o.type == kCellContinual ? cpAmp2 : amp2);
            sw += w; sl += w * o.l; sv += w * cd(o.v.real(), o.v.imag()); cnt++;
        }
        if (cnt == 0) { fa[n] = n ? fa[n - 1] : cf32(1, 0); fb[n] = 0; continue; }
        const double lbar = sl / sw;
        const cd a0 = sv / sw;
        double stt = 0;
        cd stv = 0;
        for (auto& o : obs[n]) {
            if (o.type == kCellP2Pilot) continue;
            const double w = (o.type == kCellContinual ? cpAmp2 : amp2), t = o.l - lbar;
            stt += w * t * t;
            stv += w * t * cd(o.v.real(), o.v.imag());
        }
        cd b0 = 0;
        if (cnt >= 3 && stt > 1e-9) {
            b0 = stv / stt;
            double snr = stt * std::norm(b0) / std::max(1e-18, (double)varN[n] * amp2 * 0 + 1.0 / 1.0 * 0 + (varN[n] > 0 ? varN[n] * amp2 : 1e-9));
            // James-Stein style shrinkage of the slope: keep it only if it stands out of the noise
            double shrink = std::max(0.0, 1.0 - 1.0 / std::max(1e-9, snr));
            b0 *= shrink;
        }
        fa[n] = cf32((float)a0.real(), (float)a0.imag());
        fb[n] = cf32((float)b0.real(), (float)b0.imag());
        flbar[n] = (float)lbar;
    }
    // Noise power of every data symbol: residual of the pilots against the fitted channel. Interference that comes and goes
    // (a few frames of raised noise, a burst) hits some symbols harder than the frame average that sigma2 describes; scaling the
    // cell noise variance per symbol keeps the LDPC input honest (over-confident wrong bits are far worse than weak ones).
    std::vector<double> symScale(L, 1.0);
    {
        std::vector<double> rp(L, 0.0); std::vector<int> rc(L, 0);
        for (int n = 0; n < M; n++)
            for (auto& o : obs[n]) {
                if (o.type == kCellP2Pilot || o.l < firstData) continue;
                const cf32 fit = fa[n] + fb[n] * ((float)o.l - flbar[n]);
                rp[(size_t)o.l] += std::norm(o.v - fit); rc[(size_t)o.l]++;
            }
        double tot = 0; long cnt = 0;
        for (int l = firstData; l < L; l++) { tot += rp[(size_t)l]; cnt += rc[(size_t)l]; }
        if (cnt > 0 && tot > 0) {
            const double mean = tot / (double)cnt;
            std::vector<double> raw(L, 1.0);
            for (int l = firstData; l < L; l++) raw[(size_t)l] = rc[(size_t)l] ? (rp[(size_t)l] / rc[(size_t)l]) / mean : 1.0;
            for (int l = firstData; l < L; l++) {   // light smoothing over neighbouring symbols, then limits
                const double a0 = raw[(size_t)std::max(firstData, l - 1)], a1 = raw[(size_t)l], a2 = raw[(size_t)std::min(L - 1, l + 1)];
                symScale[(size_t)l] = std::min(8.0, std::max(0.25, 0.25 * a0 + 0.5 * a1 + 0.25 * a2));
            }
        }
        if (getenv("DECT2_SYMNOISE")) {   // pilot residual against the frame's channel fit per data symbol, dB below the pilot power
            fprintf(stderr, "SYMNOISE frame %llu:", (unsigned long long)(frameCounter + 1));
            for (int l = firstData; l < L; l++) fprintf(stderr, " %.1f", rc[(size_t)l] ? 10 * std::log10(rp[(size_t)l] / rc[(size_t)l] / (amp2 * std::max(1e-12, (double)sumSig / M))) : 99.0);
            fprintf(stderr, "\n");
        }
    }
    // The grid is linear in the symbol index (a + b (l - lbar) per grid point) and the interpolator is linear, so it is run twice per
    // frame, on the grid at the middle data symbol and on the slopes, instead of once per symbol: H(l) = H(lc) + (l - lc) * Hslope.
    const int lc = firstData + (L - firstData) / 2;
    std::vector<cf32> Hmid, Hslope;
    {
        const double tau0 = (double)back + (chDelayKnown ? chDelayCentre : G / 2.0);
        const double cut = chDelayKnown ? std::min(1.0, chDelayHalf / ((double)N / (2.0 * dx))) : 1.0;
        for (int n = 0; n < M; n++) grid[n] = fa[n] + fb[n] * ((float)lc - flbar[n]);
        interp.run(grid, dx, K, N, tau0, Hmid, cut);
        interp.run(fb, dx, K, N, tau0, Hslope, cut);
    }
    // the equalised cells are only kept for the display, which thins them to about 6000: store every step-th one
    size_t totalData = 0;
    for (int l = firstData; l < L; l++) for (int kk = 0; kk < K; kk++) totalData += types[l][kk] == kCellData;
    const size_t dispStep = std::max<size_t>(1, totalData / 6000);
    size_t dispIdx = 0;
    H.resize(K);
    for (int l = firstData; l < L; l++) {
        const float dl = (float)(l - lc);
        for (int kk = 0; kk < K; kk++) H[kk] = Hmid[kk] + dl * Hslope[kk];
        for (int kk = 0; kk < K; kk++)
            if (types[l][kk] == kCellData) { if (dispIdx++ % dispStep == 0) all.push_back(frameCells[l][off + kk] / H[kk]); }
        if (wantPlp) {
            int cD = 0;
            for (int kk = 0; kk < K; kk++) cD += types[l][kk] == kCellData;
            std::vector<int> Hi;
            freqInterleaverSeq(fftCode, cD, (l & 1) != 0, Hi);
            std::vector<cf32> orig(cD);
            std::vector<float> on0(cD);
            int jj = 0;
            for (int kk = 0; kk < K; kk++)
                if (types[l][kk] == kCellData) {
                    cf32 hh = H[kk];
                    float g2 = std::max(1e-12f, std::norm(hh));
                    orig[Hi[jj]] = frameCells[l][off + kk] * std::conj(hh) / g2;
                    const float nk = noiseCar[kk] > 0 ? noiseCar[kk] : (float)sigma2;
                    on0[Hi[jj]] = (float)(nk * symScale[(size_t)l] / (2.0 * g2));
                    jj++;
                }
            dstream.insert(dstream.end(), orig.begin(), orig.end());
            dn0.insert(dn0.end(), on0.begin(), on0.end());
        }
        if (l == firstData + (L - firstData) / 2) { chH = H; chK = K; } // keep a mid-frame channel snapshot for display
    }
    if (wantPlp) submitPlp(dstream, dn0, sigma2);
    eqData.clear();
    eqData = std::move(all);
    dataValid = true;
    dataDx = dx; dataDy = dy; dataPp = pc.pp;
    dataFrames++;
    // refresh channel-derived displays from the data-symbol estimate
    chMag.clear(); chPh.clear();
    int dec = std::max(1, K / 4096);
    for (int i = 0; i < K; i += dec) {
        chMag.push_back(20 * std::log10(std::max(1e-9f, std::abs(chH[i]))));
        chPh.push_back(std::arg(chH[i]));
    }
    irMin = -std::max(8, G / 4);
    impulseResponse(chH, N, irMin, G + std::max(8, G / 4), back, irDb);
    snrDbv = dataSnrCar;
    p2Snr = dataSnr;
    (void)fm;
}

} // namespace dect2
