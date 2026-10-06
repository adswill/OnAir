// application state: preferences, device list, bandwidth, ingesting spectrum and receiver telemetry, channel list
#include "app.h"

void setFamily(App& a, int f) {
    a.family = f; a.atscMode = f == 1 || f == 3 || f == 4; a.atsc3Mode = f == 3; a.isdbtMode = f == 4; a.dabMode = f == 2; a.fmMode = f == 5;
    if (f == 2 && !(a.freqMhz >= 174 && a.freqMhz <= 240)) a.freqMhz = 218.640;
    if (f == 4 && !(a.freqMhz >= 170 && a.freqMhz <= 770)) a.freqMhz = 473.143;   // the centre of UHF channel 13/14 of the 6 MHz raster
    if (f == 5 && !(a.freqMhz >= 87.5 && a.freqMhz <= 108)) a.freqMhz = 100.0;     // FM band, default to 100 MHz
}

// what to tell the engine: 0 auto, 1 DVB-T2, 2 DVB-T, 3 ATSC, 4 DAB, 5 ATSC 3.0, 6 ISDB-T, 7 FM (Engine::start maps these to activeStandard())
int engineStd(const App& a) { return a.family == 1 ? 3 : a.family == 2 ? 4 : a.family == 3 ? 5 : a.family == 4 ? 6 : a.family == 5 ? 7 : a.stdMode; }

void refreshDevices(App& a) {
    a.devices.clear();
    DeviceInfo s; s.kind = DeviceInfo::Synthetic; s.name = "Synthetic test signal (DVB-T2 8K, 8 MHz)"; a.devices.push_back(s);
    DeviceInfo f; f.kind = DeviceInfo::File; f.name = "IQ recording file…"; a.devices.push_back(f);
    std::string err;
    // HackRF, the radios with a native driver (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP) and the rest through SoapySDR
    for (auto& d : listRadios(err)) a.devices.push_back(d);
    a.hackrfErr = err;
    size_t nHack = 0;
    for (const auto& d : a.devices) if (d.kind == DeviceInfo::HackRF) nHack++;
    a.engine.log("device scan: " + std::to_string(nHack) + " HackRF, " + std::to_string(a.devices.size() - 2 - nHack) + " other radio(s)" + (soapySupported() ? "" : " (built without SoapySDR)"));
}

void applyBandwidth(App& a) {
    // HackRF Pro: the tuned centre is only exact at <= 10 Msps and at 20 Msps, so use 10 Msps (8 for narrow channels)
    a.tune.sampleRate = kBw[a.bwIdx].mhz >= 7 ? 10e6 : 8e6;
    {   // a radio that cannot reach that rate runs as fast as it can (the source picks the nearest rate it offers)
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric() && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
        if (dv.isGeneric() && a.tune.gainDb > dv.gainMaxDb && dv.gainMaxDb > 0) a.tune.gainDb = dv.gainMaxDb;
    }
    a.tune.basebandFilterHz = 0;
    a.tune.bandwidthMhz = kBw[a.bwIdx].mhz;
    a.tune.synth.atsc = a.family == 1;
    a.tune.synth.dab = a.dabMode;
    if (a.dabMode) {   // a DAB ensemble is 1.536 MHz wide: 2.048 Msps is the natural rate (RTL-SDR dongles do it too)
        a.tune.bandwidthMhz = 1.7; a.tune.sampleRate = 2.048e6; a.tune.basebandFilterHz = 1.75e6;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric() && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
    } else if (a.atscMode) { a.tune.bandwidthMhz = 6; a.tune.sampleRate = 8e6; if (a.devices[a.devIdx].isGeneric() && a.devices[a.devIdx].maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, a.devices[a.devIdx].maxRateHz); }   // an ATSC channel is always 6 MHz wide
}

std::string openFileDialog() { return plat::openFileDialog(); }

std::string saveFileDialog(const char* name) { return plat::saveFileDialog(name); }

void loadPrefs(App& a) {
    plat::Prefs& d = plat::prefs();
    if (d.has("freqMhz")) a.freqMhz = d.getD("freqMhz", a.freqMhz);
    if (d.has("lna")) a.tune.lnaDb = (int)d.getI("lna", a.tune.lnaDb);
    if (d.has("vga")) a.tune.vgaDb = (int)d.getI("vga", a.tune.vgaDb);
    if (d.has("gain")) a.tune.gainDb = d.getD("gain", a.tune.gainDb);
    a.tune.ampOn = d.getB("amp", false);
    if (d.has("family")) { const int f = (int)d.getI("family", 0); a.family = f; a.atscMode = f == 1 || f == 3 || f == 4; a.atsc3Mode = f == 3; a.isdbtMode = f == 4; a.dabMode = f == 2; }
    if (d.has("compute")) a.computeMode = (int)d.getI("compute", a.computeMode);
    if (d.has("standard")) a.stdMode = (int)d.getI("standard", a.stdMode);
    a.bwIdx = std::max(0, std::min((int)(sizeof kBw / sizeof *kBw) - 1, (int)d.getI("bw", 0)));
    if (d.has("bwAuto")) a.bwAuto = d.getB("bwAuto", a.bwAuto);
    if (d.has("filePath")) a.file.path = d.getS("filePath", "");
    if (d.has("outPath")) snprintf(a.filePath, sizeof a.filePath, "%s", d.getS("outPath", "").c_str());
    if (d.has("udpHost")) snprintf(a.udpHost, sizeof a.udpHost, "%s", d.getS("udpHost", "").c_str());
    if (d.has("udpPort")) a.out.port = (int)d.getI("udpPort", a.out.port);
    a.out.rtp = d.getB("udpRtp", false);
    a.out.dropNull = d.getB("dropNull", false);
    a.updCheck = d.getB("updCheck", true); a.updAuto = d.getB("updAuto", true); a.updPre = d.getB("updPre", true);
    a.updLast = d.getD("updLast", 0); a.updSkip = d.getS("updSkip", "");
    a.channels = d.getChannels();
}

void savePrefs(const App& a) {
    plat::Prefs& d = plat::prefs();
    d.setD("freqMhz", a.freqMhz);
    d.setI("lna", a.tune.lnaDb);
    d.setI("vga", a.tune.vgaDb);
    d.setD("gain", a.tune.gainDb);
    d.setB("amp", a.tune.ampOn);
    d.setI("family", a.family);
    d.setChannels(a.channels);
    d.setI("compute", a.computeMode);
    d.setI("standard", a.stdMode);
    d.setI("bw", a.bwIdx);
    d.setB("bwAuto", a.bwAuto);
    d.setS("filePath", a.file.path);
    d.setS("outPath", a.filePath);
    d.setS("udpHost", a.udpHost);
    d.setI("udpPort", a.out.port);
    d.setB("udpRtp", a.out.rtp);
    d.setB("dropNull", a.out.dropNull);
    d.setB("updCheck", a.updCheck); d.setB("updAuto", a.updAuto); d.setB("updPre", a.updPre);
    d.setD("updLast", a.updLast); d.setS("updSkip", a.updSkip);
    d.flush();
}

void ingestSpectrum(App& a) {
    SpectrumFrame f;
    if (!a.engine.latestSpectrum(f, a.lastSeq)) return;
    a.lastSeq = f.seq;
    a.spec = f;
    size_t n = f.dbfs.size();
    if (a.smooth.size() != n || a.peak.size() != n) { a.smooth = f.dbfs; a.peak = f.dbfs; }   // both lines always have the frame size (a retune clears the peak line)
    for (size_t i = 0; i < n; i++) {
        a.smooth[i] += 0.35f * (f.dbfs[i] - a.smooth[i]);
        a.peak[i] = std::max(a.peak[i] - 0.15f, f.dbfs[i]);
    }
    a.wf.push(f.dbfs);
    a.wf.stamp(glfwGetTime());
}

void ingestRx(App& a) {
    double now = glfwGetTime();
    if (a.engine.running() && now - a.tsT > 0.25) { a.ts = a.engine.tsSnapshot(); a.bb = a.engine.bbStats(); a.tsT = now; }
    RxTelemetry t;
    if (!a.engine.latestRx(t, a.rxSeq)) return;
    a.rxSeq = t.seq;
    if (t.p1Count > 0) a.rxSeen = true;
    if (t.dataFrames < a.rx.dataFrames) { a.mpd.reset(); a.quality.reset(); a.cst.mod = -1; } // receiver was restarted or retuned
    a.rx = std::move(t);
    {   // statistics of the decoded constellation cells
        App::ConstStats& c = a.cst;
        const RxTelemetry& r = a.rx;
        if (r.plpConst.empty() || r.plpConstTx.size() != r.plpConst.size()) { c.seq = 0; c.mod = -1; }
        else if (r.plpConstSeq != c.seq) {
            const int nPts = 1 << (2 * (r.plpFec.mod + 1));
            if (c.mod != r.plpFec.mod || c.plp != r.plpId || (int)c.pts.size() != nPts || c.grid.empty()) {
                c.mod = r.plpFec.mod; c.plp = r.plpId;
                c.pts.assign(nPts, App::ConstStats::Pt());
                c.grid.assign(App::ConstStats::G * App::ConstStats::G, 0.f);
            }
            c.seq = r.plpConstSeq;
            for (float& v : c.grid) v *= 0.93f;
            for (auto& p : c.pts) { p.n *= 0.97; p.ei *= 0.97; p.eq *= 0.97; p.e2 *= 0.97; }
            const int G = App::ConstStats::G;
            const float lim = 1.4f;
            for (size_t i = 0; i < r.plpConst.size(); i++) {
                const cf32 o = r.plpConst[i];
                const int gx = (int)((o.real() + lim) / (2 * lim) * G), gy = (int)((lim - o.imag()) / (2 * lim) * G);
                if (gx >= 0 && gx < G && gy >= 0 && gy < G) c.grid[gy * G + gx] += 1.f;
                const unsigned l = r.plpConstTx[i];
                if (l < c.pts.size()) {
                    const cf32 e = o - qamPoint(c.mod, false, l);
                    auto& p = c.pts[l];
                    p.n += 1; p.ei += e.real(); p.eq += e.imag(); p.e2 += std::norm(e);
                }
            }
        }
    }
    a.mpd.update(a.rx);
    a.quality.update(a.rx);
    {
        const double nowT = glfwGetTime();
        if (a.engine.running() && nowT - a.histT >= 0.25) {
            App::HistSample h;
            h.t = (float)nowT;
            h.snr = a.rx.dataValid ? a.rx.dataSnrDb : NAN;
            h.mer = a.rx.plpMerDb > 0 && a.rx.plpMerDb < 90 ? (float)a.rx.plpMerDb : NAN;
            const uint64_t dOk = a.rx.blocksOk >= a.histOk ? a.rx.blocksOk - a.histOk : 0, dBad = a.rx.blocksBad >= a.histBad ? a.rx.blocksBad - a.histBad : 0;
            h.loss = (dOk + dBad) ? 100.f * (float)dBad / (float)(dOk + dBad) : NAN;
            a.histOk = a.rx.blocksOk; a.histBad = a.rx.blocksBad;
            h.cfo = a.rx.state == 2 ? (float)a.rx.cfoHz : NAN;
            h.sro = a.rx.state == 2 ? (float)a.rx.sroPpm : NAN;
            h.level = a.spec.stats.rmsDbfs; h.clip = a.spec.stats.clipFraction * 100.f;
            h.quality = a.quality.report().valid ? (float)a.quality.report().percent : NAN;
            a.hist.push_back(h);
            if (a.hist.size() > 3600) a.hist.pop_front();
            a.histT = nowT;
        }
    }
    auto push = [](std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); };
    if (a.rx.state == 2) {
        push(a.hCfo, (float)a.rx.cfoHz);
        push(a.hSnr, a.rx.cpSnrDb);
        push(a.hTiming, a.rx.timingErr);
    }
}

std::vector<double> xs(const App& a) {
    size_t n = a.smooth.size();
    std::vector<double> x(n);
    double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    double c = a.freqMhz;
    for (size_t i = 0; i < n; i++) x[i] = c + ((double)i / n - 0.5) * fs / 1e6;
    return x;
}

void applyOutputs(App& a) {
    a.out.host = a.udpHost;
    a.out.path = a.filePath;
    a.out.serviceId = a.selService;
    a.engine.setOutputs(a.out);
    savePrefs(a);
}

std::string channelLabel(const SavedChannel& c) {
    char b[200];
    snprintf(b, sizeof b, "%.3f MHz  %s%s", c.freqMhz, c.name.empty() ? "DVB-T2 mux" : c.name.c_str(), c.nServices > 1 ? (" +" + std::to_string(c.nServices - 1)).c_str() : "");
    return b;
}

// Merge DVB-T2 muxes found by the scanner into the remembered channel list.
void harvestScan(App& a) {
    ScanProgress pr = a.scanner.progress();
    const double now = glfwGetTime();
    const bool due = pr.running ? now - a.scanHarvestT > 0.5 : a.scanWas;
    if (!due) return;
    a.scanWas = pr.running;
    a.scanHarvestT = now;
    bool changed = false;
    for (auto& r : a.scanner.results()) {
        if (!r.t2) continue;
        SavedChannel sc;
        sc.freqMhz = r.freqMHz; sc.bwMhz = r.bwMhz; sc.mode = r.mode; sc.snrDb = r.snrDb; sc.nServices = (int)r.services.size();
        if (!r.services.empty()) { sc.name = r.services[0]; size_t br = sc.name.rfind(" ["); if (br != std::string::npos) sc.name.resize(br); }
        if (sc.name.empty()) sc.name = r.networkName;
        bool found = false;
        for (auto& c : a.channels)
            if (std::fabs(c.freqMhz - sc.freqMhz) < 0.01) {
                if (c.favourite) { sc.name = c.name; sc.favourite = true; }
                if (c.name != sc.name || c.nServices != sc.nServices || c.mode != sc.mode) { c = sc; changed = true; }
                found = true; break;
            }
        if (!found) { a.channels.push_back(sc); changed = true; }
    }
    if (changed) {
        std::sort(a.channels.begin(), a.channels.end(), [](const SavedChannel& x, const SavedChannel& y) { return x.freqMhz < y.freqMhz; });
        savePrefs(a);
    }
}

// Select a remembered channel: stop what is playing and tune the HackRF to it.
void tuneToChannel(App& a, const SavedChannel& c) {
    int hw = -1;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) hw = i;
    if (hw < 0) { a.engine.log("channel selector needs a radio"); return; }
    if (a.scanner.progress().running) a.scanner.stop();
    const double oldBw = a.tune.bandwidthMhz;
    a.freqMhz = c.freqMhz;
    for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == c.bwMhz) a.bwIdx = k;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    a.engine.player().select(-1);
    a.selService = -1;
    if (a.engine.running() && a.devIdx == hw && oldBw == a.tune.bandwidthMhz) {
        a.engine.log("channel: " + channelLabel(c));
        a.engine.retuneReset(a.tune);
    } else {
        if (a.engine.running()) a.engine.stop();
        a.devIdx = hw;
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[hw], a.tune, a.file);
    }
    a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
    a.mpd.reset(); a.quality.reset();
    savePrefs(a);
}

// Automatic bandwidth: follow what the engine detected; a change between 8 and 10 Msps (7/8 MHz vs narrower) needs a restart
void followBandwidth(App& a) {
    a.engine.setBandwidthAuto(a.bwAuto && !a.atscMode);
    if (!a.bwAuto || a.atscMode || !a.engine.running()) return;
    const bool hw = a.devices[a.devIdx].isRadio();
    const double want = a.engine.activeBandwidth();
    if (want == a.tune.bandwidthMhz) return;
    int idx = -1;
    for (int k = 0; k < (int)(sizeof kBw / sizeof *kBw); k++) if (kBw[k].mhz == want) idx = k;
    if (idx < 0) return;
    const double oldRate = a.tune.sampleRate;
    a.bwIdx = idx;
    applyBandwidth(a);
    savePrefs(a);
    if (hw && a.tune.sampleRate != oldRate) {
        a.engine.log(a.tune.sampleRate > oldRate ? "restarting at 10 Msps" : "restarting at 8 Msps");
        a.engine.stop();
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[a.devIdx], a.tune, a.file);
        a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
        a.mpd.reset(); a.quality.reset();
    }
}

