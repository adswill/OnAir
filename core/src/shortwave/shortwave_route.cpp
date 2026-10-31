// Shortwave mode routing (see shortwave_route.h).
#include "dect2/shortwave/shortwave_route.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace swr {

namespace {
// id, key, name, group, blurb, out, sideband, pitch, minRate, needs, sigId, defMhz
const DecoderInfo kTable[] = {
    {Dec::Rtty, "rtty", "RTTY", "Text", "Radioteletype, ITA2 Baudot: 45.45 / 50 / 75 / 100 baud, shifts 170 to 850 Hz found by itself. Press it on a signal and the text appears below.", Out::Text, +1, 1500, 24000, nullptr, "rtty", 10.100},
    {Dec::Cw, "cw", "CW", "Text", "Morse code, speed found by itself (a skimmer: every carrier within 7 kHz of the channel). Decodes the signal the marker is on.", Out::Text, +1, 0, 24000, nullptr, "cw", 7.030},
    {Dec::Psk, "psk", "PSK31", "Text", "PSK31 / PSK63 / QPSK keyboard chat. Every signal within 200 to 3800 Hz is decoded, text per signal.", Out::Text, +1, 1500, 24000, nullptr, "psk", 14.070},
    {Dec::Ft8, "ft8", "FT8", "Weak signal", "FT8 (15 s slots) on the channel; needs a clock within about a second.", Out::Lines, +1, 1500, 24000, nullptr, "ft8", 14.074},
    {Dec::Ft4, "ft4", "FT4", "Weak signal", "FT4 (7.5 s slots).", Out::Lines, +1, 1500, 24000, nullptr, "ft4", 14.080},
    {Dec::Wspr, "wspr", "WSPR", "Weak signal", "WSPR beacons (2 minute slots).", Out::Lines, +1, 1500, 24000, nullptr, "wspr", 14.0956},
    {Dec::Js8, "js8", "JS8", "Weak signal", "JS8Call messages and heartbeats.", Out::Lines, +1, 1500, 24000, nullptr, "js8", 14.078},
    {Dec::Sstv, "sstv", "SSTV", "Images", "Slow-scan TV (Robot, Martin, Scottie, PD): the picture builds line by line, the mode is read from the start tone.", Out::Picture, +1, 1500, 24000, nullptr, "sstv", 14.230},
    {Dec::Olivia, "olivia", "Olivia", "Text", "Olivia MFSK keyboard mode (8 to 64 tones, 125 to 2000 Hz); finds the signal in the audio.", Out::Text, +1, 1500, 24000, nullptr, "olivia", 14.1065},
    {Dec::Contestia, "contestia", "Contestia", "Text", "Contestia, the contest version of Olivia.", Out::Text, +1, 1500, 24000, nullptr, "contestia", 14.1095},
    {Dec::Mfsk, "mfsk", "MFSK", "Text", "MFSK16 / MFSK32 keyboard chat.", Out::Text, +1, 1500, 24000, nullptr, "mfsk", 14.0785},
    {Dec::Thor, "thor", "THOR", "Text", "THOR incremental frequency keying.", Out::Text, +1, 1500, 24000, nullptr, "thor", 14.077},
    {Dec::DominoEx, "dominoex", "DominoEX", "Text", "DominoEX incremental frequency keying.", Out::Text, +1, 1500, 24000, nullptr, "dominoex", 14.0715},
    {Dec::Hell, "hell", "Hell", "Images", "Hellschreiber: the text is drawn as a picture of the received pixels.", Out::Picture, +1, 1500, 24000, nullptr, "hell", 14.063},
    {Dec::HamDrm, "hamdrm", "HamDRM", "Images", "HamDRM / EasyPal pictures and files (a DRM-style OFDM signal in 2.5 kHz).", Out::Picture, +1, 1500, 24000, nullptr, "hamdrm", 14.233},
    {Dec::FreeDv, "freedv", "FreeDV", "Other", "FreeDV digital voice (the speech is played). Needs the Codec2 library, which OnAir does not ship.", Out::Status, +1, 1500, 24000, "the Codec2 library (libcodec2)", "freedv", 14.236},
    {Dec::Ale, "ale", "ALE", "Data links", "Automatic Link Establishment (MIL-STD-188-141): calls, soundings, addresses.", Out::Lines, +1, 1500, 24000, nullptr, "ale", 10.0},
    {Dec::Ardop, "ardop", "ARDOP", "Data links", "ARDOP frames heard (leader, connect requests, ID frames, data).", Out::Status, +1, 1500, 24000, nullptr, "ardop", 7.103},
    {Dec::Pactor, "pactor", "PACTOR", "Data links", "PACTOR-I (FSK 100 baud) monitor.", Out::Status, +1, 1500, 24000, nullptr, "pactor", 14.1},
    {Dec::Mt63, "mt63", "MT63", "Text", "MT63 (500 / 1000 / 2000 Hz) keyboard chat.", Out::Text, +1, 1500, 24000, nullptr, "mt63", 7.0},
    {Dec::Fsq, "fsq", "FSQ", "Text", "FSQCall directed chat (incremental frequency keying).", Out::Text, +1, 1500, 24000, nullptr, "fsq", 7.105},
    {Dec::Ifkp, "ifkp", "IFKP", "Text", "IFKP incremental frequency keying (33 tones).", Out::Text, +1, 1500, 24000, nullptr, "ifkp", 7.0},
    {Dec::Navtex, "navtex", "NAVTEX", "Other", "NAVTEX / SITOR-B maritime safety broadcasts (518, 490, 4209.5 kHz).", Out::Lines, +1, 1700, 24000, nullptr, "navtex", 4.2095},
    {Dec::Wefax, "wefax", "Weather fax", "Images", "HF weather fax (60 / 90 / 120 / 240 lines per minute): the chart builds as it arrives. Tune the carrier 1.9 kHz below the dial.", Out::Picture, +1, 1900, 24000, nullptr, "wefax", 7.8781},
    {Dec::TimeSig, "time", "Time", "Other", "Time signals (WWV, WWVH, BPM, CHU): the minute code.", Out::Status, +1, 0, 24000, nullptr, "time", 10.0},
    {Dec::HfId, "hfid", "Identify", "Other", "Waveform identifier for military / government HF (identification only; nothing is decoded).", Out::Status, +1, 1500, 24000, nullptr, "milhf", 0},
};
static_assert(sizeof kTable / sizeof *kTable == kDecCount, "one row per decoder");
}  // namespace

const DecoderInfo* decoderTable(int& count) { count = kDecCount; return kTable; }
const DecoderInfo& decoderInfo(Dec d) { return kTable[(int)d]; }

Dec decoderFromKey(const std::string& key, bool* ok) {
    for (const auto& r : kTable) if (key == r.key) { if (ok) *ok = true; return r.id; }
    if (ok) *ok = false;
    return Dec::Count;
}

Dec decoderForSignalId(const std::string& modeId, const std::string& subId) {
    for (const auto& r : kTable)
        if (r.sigIdMode && *r.sigIdMode && (subId == r.sigIdMode || (subId.empty() && modeId == r.sigIdMode))) return r.id;
    if (subId.empty() && modeId == "milhf") return Dec::HfId;
    return Dec::Count;
}

bool Router::start(Dec d) {
    if (d >= Dec::Count || on_[(int)d]) return false;
    on_[(int)d] = true; last_ = d; rev_++; moves_++;
    return true;
}
bool Router::stop(Dec d) {
    if (d >= Dec::Count || !on_[(int)d]) return false;
    on_[(int)d] = false; rev_++; moves_++;
    if (last_ == d) {   // the newest still running decoder becomes the one the listening follows
        last_ = Dec::Count;
        for (int i = kDecCount - 1; i >= 0; i--) if (on_[i]) { last_ = (Dec)i; break; }
    }
    return true;
}
bool Router::toggle(Dec d) { if (active(d)) { stop(d); return false; } start(d); return true; }
void Router::stopAll() {
    bool any = false;
    for (int i = 0; i < kDecCount; i++) { any = any || on_[i]; on_[i] = false; }
    last_ = Dec::Count;
    if (any) { rev_++; moves_++; }
}
int Router::count() const { int n = 0; for (int i = 0; i < kDecCount; i++) n += on_[i]; return n; }
std::vector<Dec> Router::running() const {
    std::vector<Dec> r;
    for (int i = 0; i < kDecCount; i++) if (on_[i]) r.push_back((Dec)i);
    return r;
}

void Router::setChannel(double offsetFromCentreHz) {
    if (offsetFromCentreHz == ch_) return;
    ch_ = offsetFromCentreHz; moves_++;
}

void Router::setOpts(Dec d, const Opts& o) {
    if (d >= Dec::Count || o == opts_[(int)d]) return;
    opts_[(int)d] = o; rev_++; moves_++;
}
int Router::effectiveSideband(Dec d) const { const int s = opts_[(int)d].sideband; return s < 0 ? -1 : s > 0 ? +1 : decoderInfo(d).sideband; }
double Router::effectivePitch(Dec d) const { const double p = opts_[(int)d].pitchHz; return p >= 0 ? p : decoderInfo(d).pitchHz; }

Plan Router::plan(Dec d) const {
    Plan p;
    p.id = d;
    p.active = on_[(int)d];
    p.opts = opts_[(int)d];
    p.pitchHz = effectivePitch(d);
    p.mirror = effectiveSideband(d) < 0;
    p.dialOffsetHz = (p.mirror ? -ch_ : ch_) - p.pitchHz;
    return p;
}
std::vector<Plan> Router::plans() const {
    std::vector<Plan> r;
    for (int i = 0; i < kDecCount; i++) if (on_[i]) r.push_back(plan((Dec)i));
    return r;
}

double Router::audioShiftHz(bool demodIsUsb, bool demodIsLsb) const {
    if (last_ == Dec::Count || (!demodIsUsb && !demodIsLsb)) return 0;
    const double pitch = effectivePitch(last_);
    // the decoder's signal is at the channel; a USB demodulator hears from 300 Hz above its channel: put the channel `pitch` below the signal
    return demodIsUsb ? -pitch : +pitch;
}
int Router::wantedSideband() const { return last_ == Dec::Count ? 0 : effectiveSideband(last_); }

void Router::setDetected(Dec d, float confidence) { detected_ = d; conf_ = d == Dec::Count ? 0 : confidence; }

std::string textDelta(const std::string& prev, const std::string& cur) {
    if (cur.size() >= prev.size() && cur.compare(0, prev.size(), prev) == 0) return cur.substr(prev.size());   // it only grew
    if (prev.empty()) return cur;
    // the window slid: find the longest tail of prev that starts cur's overlap
    const size_t maxK = std::min(prev.size(), cur.size());
    for (size_t k = maxK; k >= 8; k--) {
        if (prev.compare(prev.size() - k, k, cur, 0, k) == 0) return cur.substr(k);
        if (k == 8) break;
    }
    // a cleared or different text: everything is new
    return cur == prev ? std::string() : cur;
}

}  // namespace swr
}  // namespace dect2
