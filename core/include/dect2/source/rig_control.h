// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Rig control for transmitting with a classic transceiver: a CAT link that is watched and re-opened (RigKeeper), plain-words error hints,
// finding the rig on a port (name hints for Icom, Yaesu, Kenwood, Elecraft, Xiegu, QRP Labs, then a short CAT probe), pairing the rig's USB sound
// card, network rigs (rigctld, FlRig), and CatRigIo: the HamRigIo the ham transmitter keys (PTT by CAT, RTS, DTR or VOX; the frequency that passed
// the gate is set and read back first). Probing and watching only read the rig; only CatRigIo::ptt, called by HamSlotTx after the gate, keys it.
#pragma once
#include "dect2/ham/ham.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/hamlib_rig.h"
#include "dect2/source/rig_setup.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

const char* rigPttName(int pttType);                     // "CAT", "RTS", "DTR", "VOX"
// The Hamlib error with a sentence on what to check (cable, baud rate, CI-V address, the port held by another program, rigctld not running).
std::string hamlibErrorHint(const std::string& err);

// ---- network rigs ----
struct NetworkRigPreset { std::string label; int model = 0; std::string path; };
// rigctld (Hamlib NET rigctl, localhost:4532) and FlRig (localhost:12345); the model numbers are looked up in `all` by name when it has them.
std::vector<NetworkRigPreset> networkRigPresets(const std::vector<RigModelInfo>& all);

// ---- finding the rig ----
struct RigHint { std::string mfg, model; std::vector<int> bauds; };
// The rigs a port's name points to (the USB names of /dev/serial/by-id or the Windows friendly name: "IC-7300", "QDX", "Silicon Labs CP2105" ...),
// best first; empty when the name says nothing.
std::vector<RigHint> rigHintsForPort(const SerialPortInfo& p);
// The common rigs, in the order they are tried when the name gives no hint.
const std::vector<RigHint>& commonRigs();
// (model id, baud) attempts: the hints first, then the common rigs; models the linked Hamlib does not know are left out; at most maxTries.
std::vector<std::pair<int, int>> rigDetectPlan(const std::vector<RigModelInfo>& all, const std::vector<RigHint>& hints, int maxTries = 24);
struct RigDetectResult { bool found = false; int model = 0, baud = 0, tries = 0; std::string note; };
// Runs the plan with probe(model, baud) (true: the rig answered) and stops at the first answer. Kenwood / Yaesu style rigs share commands, so the
// note says a similar model may have answered.
RigDetectResult detectRig(const std::vector<std::pair<int, int>>& plan, const std::function<bool(int model, int baud)>& probe,
                          const std::vector<RigModelInfo>& all = {});
// One real probe: open (short timeout, no retry), read the frequency, close. Never keys PTT.
bool hamlibProbe(int model, const std::string& path, int baud, std::string& err);

// ---- audio pairing ----
// The index in `outputs` of the sound card that most likely belongs to the rig ("USB Audio CODEC" of Icom / Yaesu / Kenwood, "QDX", "IC-705" ...),
// -1 when none looks like it.
int pairRigAudio(const HamlibRig& r, const std::vector<RigModelInfo>& all, const std::vector<std::string>& outputs);

// ---- the CAT link, watched ----
struct RigCat {   // the calls RigKeeper makes; HamlibCat is the real one, tests use a fake
    virtual ~RigCat() = default;
    virtual bool open(const HamlibConn& c, std::string& err) = 0;
    virtual void close() = 0;
    virtual bool setFreq(double hz, std::string& err) = 0;
    virtual bool getFreq(double& hz, std::string& err) = 0;
    virtual bool setMode(const std::string& m, std::string& err) = 0;
    virtual bool setPtt(bool on, std::string& err) = 0;
    virtual bool setSplit(bool on, double txHz, std::string& err) = 0;
    virtual bool getSplitFreq(double& hz, std::string& err) = 0;
};
std::unique_ptr<RigCat> makeHamlibCat();

class RigKeeper {
public:
    explicit RigKeeper(std::unique_ptr<RigCat> cat = makeHamlibCat()) : cat_(std::move(cat)) {}
    enum State { Off, Up, Down };
    void setProfile(const HamlibRig& r);
    HamlibRig profile() const;
    bool connect(std::string& err);                     // open now (closes first)
    void disconnect();
    // Call often (any thread, e.g. every 200 ms): reads the dial every pingSec while up; when the rig stops answering the link is closed and
    // re-opened after 1, 2, 4 .. 30 s. Never while keyed: a keyed rig is not re-opened under the transmitter.
    void tick(double nowSec, double pingSec = 2.0);
    State state() const;
    std::string status() const;                          // "" when fine, else the error with its hint
    double dialHz() const;
    // Set the dial (or the TX VFO in split) and the mode, then read it back: false when the rig is not there within tolHz.
    bool tuneForTx(double hz, const std::string& mode, std::string& err, double tolHz = 20);
    bool ptt(bool on, std::string& err);                 // VOX: nothing is sent, true
    bool keyed() const;
    // While keyed: reads the dial every intervalSec; false (and the link marked down) when the rig stopped answering, so the burst stops.
    bool checkWhileKeyed(double nowSec, double intervalSec, std::string& err);
private:
    HamlibConn conn() const;
    void fail(const std::string& e, double now);
    std::unique_ptr<RigCat> cat_;
    mutable std::mutex mu_;
    HamlibRig prof_;
    State st_ = Off;
    std::string status_;
    double dial_ = 0, lastPing_ = -1e9, retryAt_ = 0, backoff_ = 1, now_ = 0, keyedPing_ = -1e9;
    bool keyed_ = false, wanted_ = false;
};

// ---- the TX audio of a CAT rig ----
struct RigAudio {   // SoundCardAudio is the real one (AudioOut); tests use a fake
    virtual ~RigAudio() = default;
    virtual bool start(const std::string& device, int rate, std::string& err) = 0;
    virtual int write(const float* mono, int n) = 0;
    virtual int buffered() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
};
std::unique_ptr<RigAudio> makeSoundCardAudio();

class CatRigIo : public ham::HamRigIo {
public:
    CatRigIo(RigKeeper& rig, std::unique_ptr<RigAudio> audio = makeSoundCardAudio()) : rig_(rig), audio_(std::move(audio)) {}
    bool prepare(const ham::HamTxCheck& gate, std::string& err) override;   // tunes the rig to gate.dialHz in the data mode and checks it
    bool ptt(bool on, std::string& err) override;
    bool beginAudio(int rate, std::string& err) override;
    int write(const float* mono, int frames) override;
    int pending() override;
    void flushAudio() override;
    void endAudio() override;
    double latencySec() const override { return 0.06; }   // CAT PTT and the sound card buffer
    const char* kind() const override { return "rig"; }
    std::string lastError() const override { std::lock_guard<std::mutex> l(mu_); return err_; }
    void setCheckInterval(double sec) { checkSec_ = sec; }   // how often the rig is read during a burst (default 2 s)
private:
    RigKeeper& rig_;
    double checkSec_ = 2.0;
    std::unique_ptr<RigAudio> audio_;
    mutable std::mutex mu_;
    bool prepared_ = false, drained_ = false;
    std::string err_;
};

} // namespace dect2
