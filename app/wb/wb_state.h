// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench screens: the state shared by app/wb_ui.cpp (loading, the waveform and spectrogram) and app/wb_tabs.cpp (measure, demodulate,
// bits, export). Everything here is the interface thread's; the background jobs hand their results over through the flags.
#pragma once
#include "app.h"
#include "dect2/wb/wb_demod.h"
#include "dect2/wb/wb_export.h"
#include "dect2/wb/wb_gen.h"
#include "dect2/wb/wb_measure.h"
#include "dect2/wb/wb_view.h"
#include "dect2/audio/audioout.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wbui {

struct Job {   // one background computation: start with run(), the result is picked up when done() says so
    std::thread th;
    std::atomic<bool> running{false}, finished{false}, cancel{false};
    std::atomic<double> prog{0};
    void join() { if (th.joinable()) th.join(); }
    void stop() { cancel = true; join(); running = false; finished = false; cancel = false; }
    ~Job() { cancel = true; join(); }
};

struct State {
    bool loaded = false;                       // preferences read
    // the recording
    std::shared_ptr<dect2::wb::Source> src;
    std::unique_ptr<dect2::wb::Overview> ov;
    std::unique_ptr<dect2::wb::SpecEngine> spec;
    std::string err, pendingPath, path;
    char pathBuf[1024] = "";
    char rateBuf[48] = "2000000", cfBuf[48] = "0";
    int fmtSel = 0;                            // 0 automatic, 1 cs8, 2 cu8, 3 cf32, 4 ci16, 5 cu16, 6 audio
    bool stereoIq = false, bigEndian = false;
    // the view: samples and Hz from the recording's DC
    double v0 = 0, v1 = 1, vf0 = 0, vf1 = 1;
    int fftIdx = 4, win = dect2::wb::WinHann, overlapPct = 50, cmap = 0;
    float dbMin = -110, dbMax = -20;
    bool autoRange = true;
    // selection and cursors
    bool selT = false, selF = false;
    double sT0 = 0, sT1 = 0, sF0 = 0, sF1 = 0;     // samples, Hz
    bool curOn = false;
    double curT = 0, curF = 0;
    int dragKind = 0;                          // 0 none, 1 selecting, 2 panning, 3 overview drag, 4 waveform selecting
    ImVec2 dragP0;
    double dragT0 = 0, dragF0 = 0;
    // the spectrogram picture
    gfx::Image* tex = nullptr;
    int texW = 0, texH = 0, texCmap = -1;
    float texMin = 0, texMax = 0;
    uint64_t texSeq = 0, specSeq = 0;
    dect2::wb::SpecImage img;
    bool imgFresh = false;
    std::vector<uint32_t> rgba;
    dect2::wb::WaveColumns wave;
    double waveV0 = -1, waveV1 = -1; int waveCols = 0; bool waveOvDone = false;
    // measure
    Job mJob;
    std::shared_ptr<dect2::wb::Measurement> mRes;
    dect2::wb::Measurement meas;
    bool haveMeas = false;
    double mT0 = 0, mT1 = 0, mF0 = 0, mF1 = 0;     // what was measured
    // demodulate
    Job dJob;
    std::shared_ptr<dect2::wb::DemodResult> dRes;
    dect2::wb::DemodResult dr;
    bool haveDemod = false;
    dect2::wb::DemodParams dp;
    int afskPreset = 0;
    bool bwFromSel = true;
    // bits
    dect2::wb::BitOpts bo;
    int bview = dect2::wb::ViewHex, bpl = 16, bpage = 0, bskip = 0;
    bool blsb = false, bInvertAlso = false;
    char pat[160] = "0x7E";
    int patMis = 0;
    std::vector<size_t> hits, hitsInv;
    std::string bitsText;
    dect2::wb::Bits shown;
    bool bitsDirty = true;
    size_t manchErrors = 0;
    // audio
    std::unique_ptr<dect2::AudioOut> aud;
    bool playing = false;
    size_t playPos = 0;
    std::string note;                          // the last thing that was exported / failed
    int tab = 0;
};

State& S();

// helpers shared by the two files
std::string fmtFreq(double hz, int decimals = 3);       // "12.500 kHz"
std::string fmtTime(double sec);                        // "1.250 s", "340 ms"
void loadFile(const std::string& path);
void savePrefs();
void startMeasure();
void startDemod();
void drawMeasure(App& a);
void drawDemod(App& a);
void drawBits(App& a);
void drawExport(App& a);
void audioTick(App& a);
void stopAudio();
void recomputeBits();
double rateHz();
bool haveSource();
// the selection as a sample range / band (the whole recording and the whole band when none is made)
void selection(uint64_t& t0, uint64_t& t1, double& f0, double& f1);
void noSource(const char* what);
void setSelection(double t0Sample, double t1Sample, bool band, double f0, double f1);

} // namespace wbui
