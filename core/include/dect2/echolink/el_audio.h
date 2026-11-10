// EchoLink audio helpers: 48 kHz <-> 8 kHz rate conversion for the sound card, a VOX, and the rules of the optional RF bridge.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {
namespace echolink {

// 48 kHz mono float -> 8 kHz int16 (low-pass at 3.4 kHz, keep every 6th sample)
class Down6 {
public:
    Down6();
    void reset();
    void process(const float* in, int n, std::vector<int16_t>& out);   // appends
private:
    std::vector<float> h_, hist_;
    int pos_ = 0, phase_ = 0;
};

// 8 kHz int16 -> 48 kHz mono float (zero-stuffing by 6 and the same low-pass, gain 6)
class Up6 {
public:
    Up6();
    void reset();
    void process(const int16_t* in, int n, std::vector<float>& out);    // appends
private:
    std::vector<float> h_, hist_;
    int pos_ = 0;
};

// Voice-operated transmit: opens when a 20 ms block is louder than thresholdDbfs, closes holdMs after the last loud block.
struct VoxConfig {
    double thresholdDbfs = -30;
    int holdMs = 800;
};
class Vox {
public:
    void configure(const VoxConfig& c) { cfg_ = c; }
    // 8 kHz samples; returns the state after them
    bool process(const int16_t* pcm, int n);
    bool open() const { return open_; }
    double levelDbfs() const { return levelDb_; }
    void reset() { open_ = false; quietMs_ = 0; acc_ = 0; accN_ = 0; }
private:
    VoxConfig cfg_;
    bool open_ = false;
    int quietMs_ = 0;
    double acc_ = 0, levelDb_ = -120;
    int accN_ = 0;
};

// The RF bridge (a "link" node): what the radio receives goes to the EchoLink connection, and what EchoLink sends is transmitted on the
// radio. Off by default; it only acts after the user turned it on AND acknowledged the licensing note (an unattended link needs the right
// licence and, in many countries, a permit; the operator stays responsible for identification and for what is relayed). Half duplex: the
// side that started first keeps the channel until it is quiet. The radio transmit is limited to maxTxSec, then waits until EchoLink is quiet.
struct BridgeConfig {
    bool enabled = false;
    bool licenceAcknowledged = false;
    int tailMs = 500;          // keep the radio keyed this long after the EchoLink audio stops
    int maxTxSec = 180;        // transmit time-out (0 = none)
};
struct BridgeOut {
    bool forwardRfToEchoLink = false;   // send the radio's audio over EchoLink (transmit on the connection)
    bool keyRadio = false;              // key the radio and play the EchoLink audio into it
    bool timedOut = false;              // the transmit time-out cut the radio off
};
class RfBridge {
public:
    void configure(const BridgeConfig& c) { cfg_ = c; if (!active()) reset(); }
    bool active() const { return cfg_.enabled && cfg_.licenceAcknowledged; }
    // dtMs since the last call; rfSquelchOpen: the receiver hears a signal; echoLinkRx: EchoLink audio is arriving
    BridgeOut step(int dtMs, bool rfSquelchOpen, bool echoLinkRx);
    void reset() { dir_ = None; tailLeft_ = 0; txMs_ = 0; lockout_ = false; }
private:
    enum Dir { None, RfToNet, NetToRf };
    BridgeConfig cfg_;
    Dir dir_ = None;
    int tailLeft_ = 0;
    long txMs_ = 0;
    bool lockout_ = false;
};

} // namespace echolink
} // namespace dect2
