// TETRAPOL receiver (tetrapol_rx.h): GMSK 8 kbit/s discriminator, symbol timing, frame sync with automatic polarity, scrambling
// constant and band detection, data blocks and signalling, and clear voice frames to the user's codec plug-in.
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_codec.h"
#include "dect2/tetrapol/tetrapol_link.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/tetrapol/tetrapol_voice.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

ModeTuning tetrapolTuning() {
    ModeTuning t;
    t.stdMode = 81; t.id = "tetrapol"; t.name = "TETRAPOL";
    t.minMhz = 68; t.maxMhz = 520; t.defMhz = 392.0;
    t.sampleRate = 250000;
    t.basebandHz = 200000;
    t.bandwidthMhz = 0.2;
    t.minSampleRate = 64000;
    return t;
}

namespace {
using dect2::math::kPi;
namespace tp = tetrapol;
constexpr double kBaud = 8000.0;
constexpr int kSps = 8;
constexpr double kWork = kBaud * kSps;   // 64 kHz
constexpr int kFirTaps = 31;
constexpr int kVoiceMaxErr = 6;           // coded bits in error allowed in the 52 protected bits of a voice frame
constexpr int kDataMaxErr = 20;          // ... in the 152 coded bits of a data frame
constexpr int kCandidateFrames = 12;     // frames a sync candidate has to lock a scrambling constant in (by its CRCs)
constexpr int kLostFrames = 25;          // frames in a row that fail every check: the scrambling constant is looked for again
constexpr int kPendingMax = 50;          // frames kept while the scrambling constant is looked for, decoded once it is known

// Searching the scrambling constant (256 hypotheses a frame) needs the strict limits; once it is locked the soft decoder's frames are
// trusted up to the wider ones (the CRC, the zero bits and for data the HDLC FCS still check them)
constexpr int kVoiceMaxErrSearch = 3, kDataMaxErrSearch = 10;
bool frameGood(const tp::Frame& f, bool search = false) {
    if (!f.ok) return false;
    if (search) return f.type == tp::kVoice ? f.metric <= kVoiceMaxErrSearch : f.metric <= kDataMaxErrSearch;
    return f.type == tp::kVoice ? f.metric <= kVoiceMaxErr : f.metric <= kDataMaxErr;
}

std::string wallStamp() {
    const time_t tt = std::time(nullptr);
    struct tm g{};
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &g);
    return b;
}
} // namespace

struct TetrapolReceiver::Impl {
    std::mutex mu;
    TetrapolTelemetry tel;
    std::function<void(const std::string&)> log;
    std::function<void(const int16_t*, size_t, int)> pcmOut;
    std::unique_ptr<codec::SpeechCodec> codec;
    bool codecFromUser = false;
    bool listen = true;
    std::string recFolder;
    double fs = 0, offHz = 0, centreHz = 0;
    int spacing = 12500;
    int polMode = 0, bandMode = 0;
    bool rateSet = false;

    // front end
    cf32 rot{1.f, 0.f}, step{1.f, 0.f};
    // Frequency control: a radio's carrier error (20 ppm at 400 MHz is 8 kHz) sits in the channel filter's skirt and costs 7 dB at 3 kHz; the
    // discriminator's slow mean is the error, and the mixer is steered by it (a loop of about 50 ms, much slower than the average itself)
    double afcAcc = 0, afcW = 0;      // fast mean of the discriminator output (rad per work sample), accumulated mixer correction (rad per input sample)
    int afcCnt = 0;
    cf32 boxSum{0.f, 0.f};
    int boxN = 0;
    double hold = 0;
    std::vector<float> fir;    // channel filter at kWork: the mode's own (31 taps, about +-6.5 kHz) or the user's (Baseband filter "tetrapol")
    std::vector<cf32> firBuf;
    int firPos = 0;
    cf32 prevZ{0.f, 0.f};
    double dc = 0;
    uint32_t dcN = 0;          // samples in the frequency-offset average so far (a running mean until it is a slow average)
    std::vector<float> dq;     // discriminator output (DC removed)
    double tsym = 8;           // centre of the next symbol in dq
    float softAbs = 0, softVar = 0;
    uint64_t samp = 0;

    // bits and frames
    std::vector<uint8_t> raw;
    size_t scan = 0;           // hunting position in raw
    int sync = 0;              // 0 hunting, 1 candidate, 2 locked
    size_t frameAt = 0;
    bool invert = false;
    int candFrames = 0, misses = 0, lostRun = 0;
    int band = 0, scr = -1;    // locked coding
    int stat[2][128] = {};
    std::deque<std::vector<uint8_t>> pending;   // 152-bit frames before the scrambling constant is known
    std::deque<std::vector<float>> pendingRel;  // ... and the reliability of their bits
    std::vector<float> rawRel;                  // |soft value| of each bit in raw
    uint64_t frameNo = 0;
    uint64_t telSeq = 0;       // report number: only ever counts up, also over reset() (the engine ignores a report numbered below the last one it took)
    double lastVoiceSec = -1e9;

    tp::BlockAssembler asmMain, asmVch, asmBch, asmPch, asmRch;
    int sfn = -1;              // frame number in the control channel's 200-frame superframe (from the D_SYSTEM_INFO of the BCH), -1 unknown
    int muxType = 0;           // cell_config mux type: 1 adds a PCH in frames 48, 49
    bool sawVoice = false;     // a voice frame since the frame sync: a traffic channel
    double talkerSec = -1e9;   // when the talking party was last sent
    tp::CallGate gate;
    tp::WavWriter wav;
    int callCount = 0;
    std::string statusText;

    void retune() {
        const double w = -2 * kPi * offHz / std::max(fs, 1.0) - afcW;
        step = cf32((float)std::cos(w), (float)std::sin(w));
    }

    void designFir() {
        const double fc = 6500.0 / kWork;
        std::vector<float> def(kFirTaps);
        double sum = 0;
        for (int i = 0; i < kFirTaps; i++) {
            const double m = i - (kFirTaps - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (kFirTaps - 1));
            def[i] = (float)(s * w);
            sum += s * w;
        }
        for (float& v : def) v = (float)(v / sum);
        fir = bbf::overrideTaps("tetrapol", def, kWork, 13000.0, 5, 401);   // the user's channel filter (#408), else the mode's own
        double g = 0;
        for (float v : fir) g += v;
        if (g > 1e-6) for (float& v : fir) v = (float)(v / g);
        firBuf.assign(fir.size(), cf32(0.f, 0.f));
        firPos = 0;
    }

    void event(const std::string& text) {
        if (tel.events.size() >= kTetrapolLogMax) tel.events.pop_back();
        tel.events.insert(tel.events.begin(), TetrapolEvent{(int64_t)std::time(nullptr), tel.timeSec, text});
        if (log) log("TETRAPOL: " + text);
    }

    TetrapolCell& cell() {
        for (auto& c : tel.cells) if (c.freqHz == centreHz && (c.scramble == scr || c.scramble < 0)) return c;
        if (tel.cells.size() >= kTetrapolMax) tel.cells.pop_back();
        tel.cells.insert(tel.cells.begin(), TetrapolCell());
        TetrapolCell& c = tel.cells.front();
        c.freqHz = centreHz;
        c.firstWall = (int64_t)std::time(nullptr);
        return c;
    }

    // ---- signalling
    enum Chan { kSdch, kBch, kPch, kRch, kVch };
    static const char* chanName(Chan ch, bool tch) {
        switch (ch) {
        case kBch: return "BCH";
        case kPch: return "PCH";
        case kRch: return "RCH";
        case kVch: return "VCH";
        default: return tch ? "SCH" : "SDCH";
        }
    }

    void addMsg(Chan ch, const std::string& text) {
        if (text.empty()) return;
        const std::string cn = chanName(ch, tel.channelKind == 2);
        for (size_t i = 0; i < tel.msgs.size(); i++) {
            if (tel.msgs[i].text != text || tel.msgs[i].channel != cn) continue;
            TetrapolMsg m = tel.msgs[i];
            m.count++;
            m.lastWall = (int64_t)std::time(nullptr);
            m.lastSec = tel.timeSec;
            tel.msgs.erase(tel.msgs.begin() + (std::ptrdiff_t)i);
            tel.msgs.insert(tel.msgs.begin(), m);
            return;
        }
        if (tel.msgs.size() >= kTetrapolMsgsMax) tel.msgs.pop_back();
        TetrapolMsg m;
        m.lastWall = (int64_t)std::time(nullptr);
        m.lastSec = tel.timeSec;
        m.channel = cn;
        m.text = text;
        m.count = 1;
        tel.msgs.insert(tel.msgs.begin(), m);
        if (log) log("TETRAPOL " + cn + ": " + text);
    }

    void onSysinfo(const tp::SysInfo& s) {
        TetrapolCell& c = cell();
        const bool fresh = !c.hasSysinfo || c.country != s.country || c.systemId != s.systemId || c.cellId != s.bsId || c.locArea != s.locArea;
        c.hasSysinfo = true;
        if (!s.disconnected) {
            c.country = s.country; c.systemId = s.systemId; c.locArea = s.locArea; c.cellId = s.bsId; c.rswId = s.rswId;
            muxType = s.muxType();
        } else {
            c.channelId = s.channelId;
        }
        c.bnId = s.bnId;
        if (fresh) {
            char line[200];
            if (!s.disconnected)
                snprintf(line, sizeof line, "D_SYSTEM_INFO country %d system %d BN %d cell %d (RSW %d) LA %d scramble %d",
                         s.country, s.systemId, s.bnId, s.bsId, s.rswId, s.locArea, scr);
            else
                snprintf(line, sizeof line, "D_SYSTEM_INFO (cell disconnected) BN %d channel %d scramble %d", s.bnId, s.channelId, scr);
            event(line);
        }
    }

    void onActivation(const tp::CallSignal& s, int coverage) {
        const int v = tp::keyRefClear(s.keyRef) ? 1 : 2;
        TetrapolActivation* a = nullptr;
        for (auto& e : tel.activations)
            if (e.groupId == s.groupId && e.channelId == s.channelId && e.dScramble == s.dScramble) { a = &e; break; }
        if (!a) {
            if (tel.activations.size() >= kTetrapolCallsMax) tel.activations.pop_back();
            tel.activations.insert(tel.activations.begin(), TetrapolActivation());
            a = &tel.activations.front();
            a->groupId = s.groupId; a->channelId = s.channelId; a->dScramble = s.dScramble;
            char line[160];
            snprintf(line, sizeof line, "group %d call on channel %d (D scramble %d): %s", s.groupId, s.channelId, s.dScramble,
                     v == 1 ? "clear" : "encrypted, never decoded");
            event(line);
        }
        a->verdict = v;
        a->keyRef = s.keyRef;
        a->coverage = coverage;
        a->heard++;
        a->lastWall = (int64_t)std::time(nullptr);
        a->lastSec = tel.timeSec;
    }

    void onTsdu(const std::vector<uint8_t>& t, Chan ch) {
        tel.tsdus++;
        addMsg(ch, tp::describeTsdu(t.data(), (int)t.size()));
        tp::SysInfo si;
        if (tp::parseSysInfo(t.data(), (int)t.size(), si)) {
            onSysinfo(si);
            return;
        }
        if (!t.empty() && t[0] == 0x94) {
            std::vector<tp::Neighbour> v;
            if (tp::parseNeighbours(t.data(), (int)t.size(), v)) {
                tel.neighbours.clear();
                for (const auto& n : v) tel.neighbours.push_back({n.channelId, n.bnNb, n.bsId, n.rswId, n.param});
            }
            return;
        }
        tp::CallSignal cs;
        if (!tp::parseCallSignal(t.data(), (int)t.size(), cs)) return;
        if (cs.kind == tp::CallSignal::GroupActivation) { onActivation(cs, t.size() > 3 ? t[3] : -1); return; }
        gate.onSignal(cs, tel.timeSec);
        if (cs.kind != tp::CallSignal::CallEnd && gate.active() && !tel.calls.empty() && tel.calls.front().active)
            tel.calls.front().verdict = (int)gate.verdict();
    }

    void onBlock(const std::vector<uint8_t>& bytes, Chan ch) {
        tel.dataBlocks++;
        if (ch == kPch) { addMsg(ch, tp::describePch(bytes)); return; }
        if (ch == kRch) { std::string t; if (tp::describeRch(bytes, t)) addMsg(ch, t); return; }
        tp::Hdlc h;
        if (!tp::parseHdlc(bytes, h)) {
            if (tp::isStuffing(bytes)) tel.stuffing++;
            return;
        }
        if (h.cmd == tp::kCmdUiVch) { addMsg(kVch, tp::describeVch(h.info.data(), (int)h.info.size())); return; }
        if (h.cmd == tp::kCmdUiCd) {
            std::string who;
            addMsg(kVch, tp::describeCd(h.info.data(), (int)h.info.size(), &who));
            if (!who.empty()) {
                if (who != tel.talker) event("talking party " + who);
                tel.talker = who;
                talkerSec = tel.timeSec;
                if (!tel.calls.empty() && tel.calls.front().active) tel.calls.front().talker = who;
            }
            return;
        }
        std::vector<uint8_t> t;
        if (!tp::tsduOf(h, t)) return;
        // the frame number of a control channel: the D_SYSTEM_INFO of the BCH ends in frame 3 or 103 (PAS 0001-3-3 5.1.3)
        tp::SysInfo si;
        if ((ch == kBch || sfn < 0) && bytes.size() == 24 && tp::parseSysInfo(t.data(), (int)t.size(), si) && !si.disconnected) {
            sfn = 100 * si.bch + 3;   // three data frames and the parity frame: frames 0..3 or 100..103
            tel.superframePos = sfn;
            tel.channelKind = 1;
            onTsdu(t, kBch);
            return;
        }
        onTsdu(t, ch);
    }

    // ---- voice
    void endCall() {
        if (!tel.calls.empty() && tel.calls.front().active) {
            TetrapolCall& c = tel.calls.front();
            c.active = false;
            char line[160];
            snprintf(line, sizeof line, "call ended after %.1f s (%s)", c.lastSec - c.startSec, tp::verdictText((tp::Verdict)c.verdict));
            event(line);
        }
        wav.close();
        tel.wavPath.clear();
        tel.callVerdict = -1;
    }

    void onVoice(const tp::VoiceFrame& v, bool damaged) {
        tel.voiceFrames++;
        lastVoiceSec = tel.timeSec;
        bool started = false;
        // a call announced on a control channel heard earlier (D_GROUP_ACTIVATION with this downlink scrambling constant) tells the
        // encryption state of the call on this traffic channel: encrypted always counts, clear only when announced in the last 30 s
        const TetrapolActivation* ann = nullptr;
        for (const auto& a : tel.activations) if (a.dScramble == scr && scr > 0) { ann = &a; break; }
        const bool usable = ann && ann->keyRef >= 0 && (ann->verdict != 1 || tel.timeSec - ann->lastSec <= 30);
        gate.setChannelKey(usable ? ann->keyRef : -1);
        const tp::Verdict verdict = gate.onVoice(tel.timeSec, started);
        if (started) {
            if (!tel.calls.empty() && tel.calls.front().active) endCall();
            if (tel.calls.size() >= kTetrapolCallsMax) tel.calls.pop_back();
            tel.calls.insert(tel.calls.begin(), TetrapolCall());
            TetrapolCall& c = tel.calls.front();
            c.startWall = (int64_t)std::time(nullptr);
            c.startSec = tel.timeSec;
            c.active = true;
            c.groupId = ann ? ann->groupId : -1;
            // a TP_ADDRESS sent just before the voice (the speech set-up) belongs to this call
            if (tel.timeSec - talkerSec <= 2.0) c.talker = tel.talker;
            else tel.talker.clear();
            callCount++;
            char line[96] = "";
            if (ann) snprintf(line, sizeof line, " (group %d)", ann->groupId);
            event(std::string("call started: ") + tp::verdictText(verdict) + line);
        }
        TetrapolCall& c = tel.calls.front();
        c.verdict = (int)verdict;
        c.lastSec = tel.timeSec;
        c.voiceFrames++;
        if (damaged) c.damaged++;
        tel.callVerdict = (int)verdict;
        if (verdict != tp::Verdict::Clear) {
            tel.voiceRefused++;
            statusText = tp::verdictText(verdict);
            return;   // never handed to the codec
        }
        if (!listen) { statusText = "clear call, listening is off"; return; }
        if (!codec) codec = codec::pluginVocoder("tetrapol");
        if (!codec) { statusText = "clear call: speech codec plug-in not installed, no audio"; return; }
        uint8_t fb[tp::kPluginFrameBytes];
        tp::packPluginFrame(v, damaged, fb);
        tel.voicePassed++;
        const std::vector<int16_t> pcm = codec->decode(std::vector<uint8_t>(fb, fb + sizeof fb));
        if (pcm.empty()) { statusText = "clear call: the codec returned no audio"; return; }
        tel.voiceDecoded++;
        c.decoded++;
        statusText = "clear call: decoding";
        const int rate = codec->sampleRate();
        if (pcmOut) pcmOut(pcm.data(), pcm.size(), rate);
        if (!recFolder.empty()) {
            if (!wav.isOpen()) {
                char nm[64];
                snprintf(nm, sizeof nm, "/tetrapol_call_%s_%d.wav", wallStamp().c_str(), callCount);
                if (wav.open(recFolder + nm, rate)) { c.wavPath = wav.path(); tel.wavPath = wav.path(); }
            }
            wav.write(pcm.data(), pcm.size());
        }
    }

    // ---- frames
    // Where a data frame goes (PAS 0001-3-3 5.1.3): on a control channel whose superframe position is known, frames 0..3 / 100..103 are the
    // BCH, 98, 99 (and 48, 49 with mux type 1) the PCH, 14, 39, 64, 89 the RCH and the rest the SDCH; on a traffic channel asb[0] = 1 is the
    // VCH (LSDUs) and asb[0] = 0 the SCH. Until either is known every frame goes to one assembler (that is how the BCH is found).
    Chan route(const tp::Frame* f) const {
        if (sfn >= 0) {
            const int m = sfn % 100;
            if (m <= 3) return kBch;
            if (m == 98 || m == 99 || (muxType == 1 && (m == 48 || m == 49))) return kPch;
            if (m % 25 == 14) return kRch;
            return kSdch;
        }
        if (sawVoice && f && f->data.asb[0]) return kVch;
        return kSdch;
    }
    tp::BlockAssembler& assembler(Chan ch) {
        switch (ch) {
        case kBch: return asmBch;
        case kPch: return asmPch;
        case kRch: return asmRch;
        case kVch: return asmVch;
        default: return asmMain;
        }
    }

    void dispatch(const tp::Frame& f, const uint8_t* bits152) {
        if (sfn >= 0) sfn = (sfn + 1) % 200;
        tel.superframePos = sfn;
        if (frameGood(f)) {
            lostRun = 0;
            tel.blocksOk++;
            tel.dataValid = true;
            tel.state = 2;
            TetrapolCell& c = cell();
            c.scramble = scr;
            c.band = band;
            c.framesOk++;
            c.levelDb = tel.levelDb;
            c.snrDb = tel.snrDb;
            c.cfoHz = tel.cfoHz;
            c.lastWall = (int64_t)std::time(nullptr);
            c.lastSec = tel.timeSec;
            if (f.type == tp::kVoice) {
                if (!sawVoice) { sawVoice = true; if (sfn < 0) tel.channelKind = 2; }
                onVoice(f.voice, false);
                return;
            }
            tel.dataFrames++;
            const Chan ch = route(&f);
            std::vector<uint8_t> bytes;
            if (assembler(ch).push(f.data, true, bytes)) onBlock(bytes, ch);
            return;
        }
        tel.blocksBad++;
        lostRun++;
        if (!tel.cells.empty()) tel.cells.front().framesBad++;
        tp::DataFrame none;
        assembler(route(nullptr)).push(none, false, bytes_);
        if (gate.active()) {
            // a damaged frame inside a call: handed on as a damaged voice frame (the codec conceals it)
            tp::Frame v;
            decodeFrameForced(bits152, v);
            onVoice(v.voice, true);
        }
    }
    std::vector<uint8_t> bytes_;

    void decodeFrameForced(const uint8_t* bits152, tp::Frame& v) {
        tp::decodeFrame(bits152, band, scr, v, tp::kVoice);
        v.type = tp::kVoice;
    }

    // tries every scrambling constant and band on one frame; returns true when one passed
    bool detect(const uint8_t* bits152, const float* rel) {
        bool any = false;
        for (int b = 0; b < 2; b++) {
            const int bd = b == 0 ? tp::kUhf : tp::kVhf;
            if (bandMode && bandMode != bd) continue;
            for (int s = 0; s < 128; s++) {
                tp::Frame f;
                tp::decodeFrame(bits152, bd, s, f, -1, rel);
                if (frameGood(f, true)) { stat[b][s] += 2; any = true; }
                else if (stat[b][s] > 0) stat[b][s]--;
            }
        }
        int bb = 0, bs = 0, best = -1, second = -1;
        for (int b = 0; b < 2; b++)
            for (int s = 0; s < 128; s++) {
                const int v = stat[b][s];
                if (v > best) { second = best; best = v; bb = b; bs = s; }
                else if (v > second) second = v;
            }
        if (best >= 4 && best >= second + 4) {
            band = bb == 0 ? tp::kUhf : tp::kVhf;
            scr = bs;
            tel.band = band;
            tel.scramble = scr;
            memset(stat, 0, sizeof stat);
            char line[96];
            snprintf(line, sizeof line, "frames found: %s coding, scramble %d, %s polarity", band == tp::kUhf ? "UHF" : "VHF", scr,
                     invert ? "inverted" : "normal");
            event(line);
        }
        return any;
    }

    void onFrame(const uint8_t* raw160, const float* rel160) {
        const float* rel = rel160 + tp::kHdrBits;
        frameNo++;
        uint8_t bits[tp::kDataBits];
        tp::frameBits152(raw160, invert, bits);
        if (sync == 1) {
            // a sync candidate: kept only when one of its frames passes a CRC (that also confirms the polarity)
            candFrames++;
            detect(bits, rel);
            if (scr >= 0) {
                sync = 2;
                tel.synced = true;
                tel.polarity = invert ? 2 : 1;
            } else if (candFrames >= kCandidateFrames) {
                loseSync();
                return;
            }
        } else if (scr < 0) {
            detect(bits, rel);
        }
        if (scr < 0) {
            pending.emplace_back(bits, bits + tp::kDataBits);
            pendingRel.emplace_back(rel, rel + tp::kDataBits);
            if (pending.size() > (size_t)kPendingMax) { pending.pop_front(); pendingRel.pop_front(); }
            return;
        }
        while (!pending.empty()) {   // the frames heard before the constant was known, in order
            tp::Frame f;
            tp::decodeFrame(pending.front().data(), band, scr, f, -1, pendingRel.front().data());
            dispatch(f, pending.front().data());
            pending.pop_front();
            pendingRel.pop_front();
        }
        tp::Frame f;
        tp::decodeFrame(bits, band, scr, f, -1, rel);
        dispatch(f, bits);
        if (lostRun >= kLostFrames) {
            // the constant changed (a new call on a traffic channel) or the carrier is gone: look again
            scr = -1;
            tel.scramble = -1;
            lostRun = 0;
            sfn = -1;
            sawVoice = false;
            memset(stat, 0, sizeof stat);
        }
    }

    void loseSync() {
        sync = 0;
        tel.synced = false;
        scan = frameAt + 1;
        scr = -1;
        tel.scramble = -1;
        pending.clear();
        pendingRel.clear();
        asmMain.reset();
        asmVch.reset();
        asmBch.reset(); asmPch.reset(); asmRch.reset();
        sfn = -1;
        sawVoice = false;
        memset(stat, 0, sizeof stat);
    }

    void frames() {
        const size_t F = tp::kFrameBits;
        for (;;) {
            if (sync == 0) {
                if (raw.size() < scan + 3 * F + 8) break;
                bool found = false;
                for (int k = 0; k < 2 && !found; k++) {
                    const bool inv = k == 1;
                    if (polMode == 1 && inv) continue;
                    if (polMode == 2 && !inv) continue;
                    // three frame headers in a row (one bit wrong at most) before the CRCs are asked
                    if (tp::syncErrors(&raw[scan], inv) + tp::syncErrors(&raw[scan + F], inv) + tp::syncErrors(&raw[scan + 2 * F], inv) <= 1) {
                        found = true;
                        invert = inv;
                    }
                }
                if (!found) { scan++; continue; }
                sync = 1;
                frameAt = scan;
                candFrames = 0;
                misses = 0;
                scr = -1;
                memset(stat, 0, sizeof stat);
                continue;
            }
            if (raw.size() < frameAt + F + 8) break;
            int e = tp::syncErrors(&raw[frameAt], invert);
            if (e > 1) {
                // a slip of a bit or two: look around the expected place
                for (int d = 1; d <= 2 && e > 0; d++) {
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        const long at = (long)frameAt + sgn * d;
                        if (at < 0) continue;
                        if (tp::syncErrors(&raw[(size_t)at], invert) == 0) { frameAt = (size_t)at; e = 0; break; }
                    }
                }
            }
            if (e > 1) {
                if (++misses >= 6) { loseSync(); continue; }
            } else if (misses > 0) {
                misses--;
            }
            onFrame(&raw[frameAt], &rawRel[frameAt]);
            if (sync == 0) continue;
            frameAt += F;
        }
        // drop what is behind us
        const size_t keep = sync == 0 ? scan : (frameAt > 4 ? frameAt - 4 : 0);
        if (keep > 4096) {
            raw.erase(raw.begin(), raw.begin() + (std::ptrdiff_t)keep);
            rawRel.erase(rawRel.begin(), rawRel.begin() + (std::ptrdiff_t)keep);
            if (sync == 0) scan -= keep; else frameAt -= keep;
            if (sync == 0) frameAt = 0;
        }
    }

    void symbols() {
        // soft value of a symbol centred at c: the discriminator summed over the symbol
        auto soft = [&](double c) {
            const long i0 = (long)std::lround(c) - kSps / 2;
            float s = 0;
            for (int k = 0; k < kSps; k++) s += dq[(size_t)(i0 + k)];
            return s;
        };
        while (tsym + kSps / 2 + 3 < (double)dq.size()) {
            if (tsym < kSps / 2 + 3) { tsym += kSps; continue; }
            const float s = soft(tsym), se = soft(tsym - 2), sl = soft(tsym + 2);
            const float ae = std::fabs(se), al = std::fabs(sl);
            float err = (al - ae) / (ae + al + 1e-9f);
            if (!std::isfinite(err)) err = 0;   // a NaN here reached the cast of tsym below (UBSan, fuzz_engine_modes)
            raw.push_back(s < 0.f ? 1 : 0);
            rawRel.push_back(std::fabs(s));
            const float a = std::fabs(s);
            softAbs += 0.002f * (a - softAbs);
            softVar += 0.002f * ((a - softAbs) * (a - softAbs) - softVar);
            tsym += kSps + 0.4 * err;
        }
        // keep the last few samples for the next symbols
        const long drop = (long)tsym - kSps * 2;
        if (drop > 4096) {
            dq.erase(dq.begin(), dq.begin() + drop);
            tsym -= (double)drop;
        }
    }

    void front(const cf32* x, size_t n) {
        const double adv = kWork / fs;
        double pow = 0;
        for (size_t i = 0; i < n; i++) {
            const cf32 y = x[i] * rot;
            rot *= step;
            pow += (double)std::norm(x[i]);
            boxSum += y;
            boxN++;
            hold += adv;
            if (hold < 1.0) continue;
            hold -= 1.0;
            const cf32 z0 = boxSum / (float)boxN;
            boxSum = cf32(0.f, 0.f);
            boxN = 0;
            const int nt = (int)fir.size();
            firBuf[(size_t)firPos] = z0;
            firPos = (firPos + 1) % nt;
            cf32 z(0.f, 0.f);
            for (int k = 0; k < nt; k++) z += firBuf[(size_t)((firPos + k) % nt)] * fir[(size_t)k];
            const bool first = prevZ == cf32(0.f, 0.f);
            const float d = first ? 0.f : std::arg(z * std::conj(prevZ));
            prevZ = z;
            if (first) continue;
            if (dcN < 8000) dcN++;
            dc += (d - dc) / (double)dcN;
            dq.push_back((float)(d - dc));
            afcAcc += 0.002 * (d - afcAcc);
            if (++afcCnt >= 64) {
                afcCnt = 0;
                // dead band: a small error stays with the symbol loop (it tracks it), only a skirt-sized one is steered out
                const double db = 2 * kPi * 1000.0 / kWork;
                const double e = afcAcc > db ? afcAcc - db : (afcAcc < -db ? afcAcc + db : 0.0);
                afcW += 0.02 * e * adv;
                const double lim = 2 * kPi * 7000.0 / std::max(fs, 1.0);
                afcW = std::max(-lim, std::min(lim, afcW));
                retune();
            }
        }
        rot /= std::abs(rot);
        if (n) {
            const double p = pow / (double)n;
            tel.levelDb = p > 1e-20 ? (float)(10.0 * std::log10(p)) : -200.f;
        }
        tel.cfoHz = offHz + (afcW * fs + 0.0) / (2 * kPi) + dc * kWork / (2 * kPi);
        if (softVar > 0) tel.snrDb = (float)(10.0 * std::log10(std::max(1e-6, (double)softAbs * softAbs / softVar)));
    }

    void process(const cf32* x, size_t n) {
        front(x, n);
        symbols();
        frames();
        if (gate.tick(tel.timeSec)) endCall();
        tel.voiceStatus = gate.active() ? statusText : (codec ? "speech codec ready, waiting for a clear call" : "no call");
        tel.codecReady = codec != nullptr || codec::pluginLoaded("tetrapol");
        if (tel.state == 2 && tel.timeSec - (tel.cells.empty() ? 0 : tel.cells.front().lastSec) > 30) tel.state = 1;
        if (tel.carriers.empty()) tel.carriers.push_back({centreHz, tel.levelDb, tel.synced});
        else { tel.carriers[0].freqHz = centreHz; tel.carriers[0].levelDb = tel.levelDb; tel.carriers[0].synced = tel.synced; }
        tel.polarityAuto = polMode == 0;
        tel.seq = ++telSeq;
        tel.blocks++;
        tel.spacingHz = spacing;
        tel.centerHz = centreHz;
        tel.inputRate = fs;
    }

    void resetDsp() {
        rot = cf32(1.f, 0.f);
        boxSum = cf32(0.f, 0.f);
        boxN = 0;
        hold = 0;
        designFir();   // a changed channel filter applies from here (a start, a retune)
        prevZ = cf32(0.f, 0.f);
        dc = 0;
        dcN = 0;
        afcAcc = 0; afcW = 0; afcCnt = 0; retune();
        dq.clear();
        tsym = kSps;
        softAbs = softVar = 0;
        raw.clear();
        rawRel.clear();
        scan = 0;
        sync = 0;
        frameAt = 0;
        scr = -1;
        band = 0;
        pending.clear();
        pendingRel.clear();
        asmMain.reset();
        asmVch.reset();
        asmBch.reset(); asmPch.reset(); asmRch.reset();
        sfn = -1;
        muxType = 0;
        sawVoice = false;
        memset(stat, 0, sizeof stat);
        lostRun = misses = candFrames = 0;
        if (gate.active()) endCall();
        gate.reset();
        lastVoiceSec = -1e9;
    }
};

TetrapolReceiver::TetrapolReceiver() : p_(std::make_unique<Impl>()) { p_->designFir(); }
TetrapolReceiver::~TetrapolReceiver() = default;

void TetrapolReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= tetrapolTuning().minSampleRate - 1;
    p_->retune();
    p_->tel.inputRate = inputRateHz;
}
void TetrapolReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offHz = hz;
    p_->retune();
}
void TetrapolReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->centreHz = hz;
    p_->tel.centerHz = hz;
}
void TetrapolReceiver::setSpacingHz(int hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->spacing = (hz == 10000) ? 10000 : 12500;
}
void TetrapolReceiver::setPolarity(int mode) {
    std::lock_guard<std::mutex> lk(p_->mu);
    mode = std::clamp(mode, 0, 2);
    if (mode == p_->polMode) return;
    p_->polMode = mode;
    if (p_->sync && ((mode == 1 && p_->invert) || (mode == 2 && !p_->invert))) p_->loseSync();
    p_->tel.polarityAuto = mode == 0;
}
void TetrapolReceiver::setBand(int mode) {
    std::lock_guard<std::mutex> lk(p_->mu);
    mode = std::clamp(mode, 0, 2);
    if (mode == p_->bandMode) return;
    p_->bandMode = mode;
    p_->scr = -1;
    p_->tel.scramble = -1;
    memset(p_->stat, 0, sizeof p_->stat);
}
void TetrapolReceiver::setCodec(std::unique_ptr<codec::SpeechCodec> c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->codec = std::move(c);
    p_->codecFromUser = p_->codec != nullptr;
}
bool TetrapolReceiver::hasCodec() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->codec != nullptr;
}
void TetrapolReceiver::setPcmOut(std::function<void(const int16_t*, size_t, int)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pcmOut = std::move(cb);
}
void TetrapolReceiver::setListen(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->listen = on;
}
void TetrapolReceiver::setRecordFolder(const std::string& folder) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->recFolder = folder;
    if (folder.empty()) { p_->wav.close(); p_->tel.wavPath.clear(); }
}
bool TetrapolReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet;
}
void TetrapolReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetDsp();
    p_->samp = 0;
    // a retune restarts the search for the carrier, the cells and the log stay (cells are kept by frequency)
    TetrapolTelemetry keep = std::move(p_->tel);
    p_->tel = TetrapolTelemetry();
    p_->tel.cells = std::move(keep.cells);
    p_->tel.events = std::move(keep.events);
    // the group calls a control channel announced stay: they tell the group and encryption state of a traffic channel tuned to next
    p_->tel.activations = std::move(keep.activations);
    for (auto& a : p_->tel.activations) a.lastSec -= keep.timeSec;   // the receiver's clock starts again from 0
    p_->tel.inputRate = p_->fs;
    p_->tel.centerHz = p_->centreHz;
    p_->tel.spacingHz = p_->spacing;
    p_->tel.polarityAuto = p_->polMode == 0;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq = std::max(keep.seq, p_->telSeq) + 1;
    p_->telSeq = p_->tel.seq;
}
void TetrapolReceiver::clearCells() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.cells.clear();
    p_->tel.events.clear();
    p_->tel.carriers.clear();
    p_->tel.activations.clear();
    p_->tel.msgs.clear();
    p_->tel.neighbours.clear();
    if (!p_->gate.active()) p_->tel.calls.clear();
    p_->tel.seq = ++p_->telSeq;
}
void TetrapolReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    if (p_->tel.state == 0) p_->tel.state = 1;
    p_->samp += n;
    p_->tel.timeSec = p_->fs > 0 ? (double)p_->samp / p_->fs : 0;
    p_->process(x, n);
}
bool TetrapolReceiver::telemetry(TetrapolTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void TetrapolReceiver::clearResults() { clearCells(); }
std::string TetrapolReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    // the cells and the event log only: the call table is never saved
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool TetrapolReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    TetrapolTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.cells = std::move(l.cells);
    p_->tel.events = std::move(l.events);
    p_->tel.seq = ++p_->telSeq;
    return true;
}
void TetrapolReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}
bool TetrapolReceiver::busy() const { return false; }

} // namespace dect2
