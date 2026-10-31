// Conversations: the state machine that turns detections into conversations (overs, speakers, nets, ended / active), the list with filters and sorting,
// the export, the timeline, the band labels, the auto-listen choice, the sweep plan for narrow radios and the audio clip files. See ssbconv.h.
#include "dect2/ssbconv/ssbconv.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

namespace dect2::ssbconv {

const char* sideName(Side s) { return s == Side::Usb ? "USB" : s == Side::Lsb ? "LSB" : "?"; }
const char* kindName(Kind k) {
    switch (k) { case Kind::Single: return "single call"; case Kind::Qso: return "QSO"; case Kind::RagChew: return "rag chew"; case Kind::Net: return "net"; }
    return "?";
}

// ------------------------------------------------------------------ bands
namespace {
struct Rng { double lo, hi; const char* band; const char* service; Side side; };
// amateur first (they win where a block overlaps another), then CB, maritime, aviation, utility
const Rng kBands[] = {
    {1.810e6, 2.000e6, "160 m", "Amateur", Side::Lsb}, {3.500e6, 3.800e6, "80 m", "Amateur", Side::Lsb}, {3.800e6, 4.000e6, "80 m", "Amateur", Side::Lsb},
    {5.3305e6, 5.4035e6, "60 m", "Amateur", Side::Usb}, {7.000e6, 7.300e6, "40 m", "Amateur", Side::Lsb}, {10.100e6, 10.150e6, "30 m", "Amateur", Side::Usb},
    {14.000e6, 14.350e6, "20 m", "Amateur", Side::Usb}, {18.068e6, 18.168e6, "17 m", "Amateur", Side::Usb}, {21.000e6, 21.450e6, "15 m", "Amateur", Side::Usb},
    {24.890e6, 24.990e6, "12 m", "Amateur", Side::Usb}, {28.000e6, 29.700e6, "10 m", "Amateur", Side::Usb}, {50.000e6, 54.000e6, "6 m", "Amateur", Side::Usb},
    {144.000e6, 146.000e6, "2 m", "Amateur", Side::Usb}, {430.000e6, 440.000e6, "70 cm", "Amateur", Side::Usb},
    {26.965e6, 27.405e6, "CB", "CB (27 MHz)", Side::Usb},
    {2.000e6, 2.850e6, "2 MHz", "Maritime SSB", Side::Usb}, {4.000e6, 4.438e6, "4 MHz", "Maritime SSB", Side::Usb}, {6.200e6, 6.525e6, "6 MHz", "Maritime SSB", Side::Usb},
    {8.100e6, 8.815e6, "8 MHz", "Maritime SSB", Side::Usb}, {12.230e6, 13.200e6, "12 MHz", "Maritime SSB", Side::Usb}, {16.360e6, 17.410e6, "16 MHz", "Maritime SSB", Side::Usb},
    {18.780e6, 18.900e6, "18 MHz", "Maritime SSB", Side::Usb}, {22.000e6, 22.855e6, "22 MHz", "Maritime SSB", Side::Usb}, {25.070e6, 25.210e6, "25 MHz", "Maritime SSB", Side::Usb},
    {2.850e6, 3.155e6, "3 MHz", "Aviation HF", Side::Usb}, {3.400e6, 3.500e6, "3 MHz", "Aviation HF", Side::Usb}, {4.650e6, 4.750e6, "4 MHz", "Aviation HF", Side::Usb},
    {5.450e6, 5.730e6, "5 MHz", "Aviation HF", Side::Usb}, {6.525e6, 6.765e6, "6 MHz", "Aviation HF", Side::Usb}, {8.815e6, 9.040e6, "8 MHz", "Aviation HF", Side::Usb},
    {10.005e6, 10.100e6, "10 MHz", "Aviation HF", Side::Usb}, {11.175e6, 11.400e6, "11 MHz", "Aviation HF", Side::Usb}, {13.200e6, 13.360e6, "13 MHz", "Aviation HF", Side::Usb},
    {15.010e6, 15.100e6, "15 MHz", "Aviation HF", Side::Usb}, {17.900e6, 18.030e6, "17 MHz", "Aviation HF", Side::Usb}, {21.870e6, 22.000e6, "21 MHz", "Aviation HF", Side::Usb},
};
}  // namespace

BandInfo bandInfo(double hz) {
    BandInfo b;
    for (const Rng& r : kBands) if (hz >= r.lo && hz <= r.hi) { b.band = r.band; b.service = r.service; b.expected = r.side; return b; }
    if (hz >= 1.6e6 && hz <= 30e6) { b.band = "HF"; b.service = "Utility / other"; b.expected = hz < 10e6 ? Side::Usb : Side::Usb; }
    else if (hz > 30e6) { b.band = "VHF/UHF"; b.expected = Side::Usb; }
    return b;
}
Side conventionSide(double hz) {
    const BandInfo b = bandInfo(hz);
    if (b.expected != Side::Unknown) return b.expected;
    return hz < 10e6 ? Side::Lsb : Side::Usb;
}
double estimateS(double snrDb) { return std::max(0.0, std::min(9.0, 1.0 + snrDb / 6.0)); }
double overS9Db(double snrDb) { return std::max(0.0, snrDb - 48.0); }
std::string sText(double snrDb) {
    char b[24];
    if (snrDb > 48.0 + 1.0) snprintf(b, sizeof b, "S9+%d", (int)std::lround(snrDb - 48.0));
    else snprintf(b, sizeof b, "S%d", (int)std::lround(estimateS(snrDb)));
    return b;
}

std::string Conversation::label() const {
    char b[48];
    snprintf(b, sizeof b, "%.4f MHz %s", dialHz / 1e6, sideName(side));
    return b;
}

// ------------------------------------------------------------------ callsigns
namespace {
bool isCall(const std::string& s) {
    if (s.size() < 3 || s.size() > 7) return false;
    size_t dpos = std::string::npos;
    for (size_t i = 0; i < s.size(); i++) if (std::isdigit((unsigned char)s[i])) dpos = i;   // the last digit separates prefix and suffix
    if (dpos == std::string::npos || dpos == 0 || dpos + 1 >= s.size() || dpos > 2) return false;
    int letters = 0;
    for (size_t i = 0; i < dpos; i++) { if (!std::isalnum((unsigned char)s[i])) return false; if (std::isalpha((unsigned char)s[i])) letters++; }
    if (letters == 0) return false;
    if (std::isdigit((unsigned char)s[0]) && dpos == 1 && !std::isalpha((unsigned char)s[0])) return false;
    const size_t suf = s.size() - dpos - 1;
    if (suf < 1 || suf > 3) return false;
    for (size_t i = dpos + 1; i < s.size(); i++) if (!std::isalpha((unsigned char)s[i])) return false;
    return true;
}
char phonetic(const std::string& w) {
    static const char* nato[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliett", "kilo", "lima", "mike", "november", "oscar", "papa", "quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey", "xray", "yankee", "zulu"};
    static const char* dig[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    for (int i = 0; i < 26; i++) if (w == nato[i]) return (char)('A' + i);
    if (w == "alfa") return 'A'; if (w == "juliet") return 'J'; if (w == "x-ray") return 'X'; if (w == "niner") return '9';
    for (int i = 0; i < 10; i++) if (w == dig[i]) return (char)('0' + i);
    return 0;
}
}  // namespace

std::vector<std::string> findAmateurCallsigns(const std::string& text) {
    std::vector<std::string> out;
    auto add = [&](const std::string& c) { if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); };
    std::vector<std::string> tok; std::string cur;
    for (char ch : text) { if (std::isalnum((unsigned char)ch) || ch == '-') cur += (char)std::tolower((unsigned char)ch); else { if (!cur.empty()) tok.push_back(cur); cur.clear(); } }
    if (!cur.empty()) tok.push_back(cur);
    for (size_t i = 0; i < tok.size();) {
        // a run of phonetic words / digit words / single characters
        std::string run; size_t j = i;
        while (j < tok.size()) {
            const char p = phonetic(tok[j]);
            if (p) run += p;
            else if (tok[j].size() == 1 && std::isalnum((unsigned char)tok[j][0])) run += (char)std::toupper((unsigned char)tok[j][0]);
            else break;
            j++;
        }
        if (j - i >= 4 && isCall(run)) { add(run); i = j; continue; }
        std::string up; for (char ch : tok[i]) up += (char)std::toupper((unsigned char)ch);
        if (isCall(up)) add(up);
        i = (j > i) ? j : i + 1;
    }
    return out;
}

// ------------------------------------------------------------------ the book
ConversationBook::ConversationBook(const BookOptions& o) : o_(o) {}
void ConversationBook::setOptions(const BookOptions& o) { o_ = o; }
uint32_t ConversationBook::convForTrack(uint32_t trackId) const {
    for (const Conversation& c : conv_) if (!c.ended && std::find(c.trackIds.begin(), c.trackIds.end(), trackId) != c.trackIds.end()) return c.id;
    return 0;
}
void ConversationBook::clear() { conv_.clear(); spk_.clear(); acc_.clear(); lastVoice_.clear(); }
const Conversation* ConversationBook::find(uint32_t id) const { for (const Conversation& c : conv_) if (c.id == id) return &c; return nullptr; }

bool ConversationBook::isIgnoredHz(double hz) const { for (double f : ignore_) if (std::fabs(f - hz) <= o_.freqTolHz) return true; return false; }
bool ConversationBook::isFavoriteHz(double hz) const { for (double f : fav_) if (std::fabs(f - hz) <= o_.freqTolHz) return true; return false; }
void ConversationBook::ignoreHz(double hz, bool on) {
    ignore_.erase(std::remove_if(ignore_.begin(), ignore_.end(), [&](double f) { return std::fabs(f - hz) <= o_.freqTolHz; }), ignore_.end());
    if (on) ignore_.push_back(hz);
}
void ConversationBook::favoriteHz(double hz, bool on) {
    fav_.erase(std::remove_if(fav_.begin(), fav_.end(), [&](double f) { return std::fabs(f - hz) <= o_.freqTolHz; }), fav_.end());
    if (on) fav_.push_back(hz);
}
void ConversationBook::setFavorite(uint32_t id, bool on) {
    for (Conversation& c : conv_) if (c.id == id) {
        c.favorite = on;
        fav_.erase(std::remove_if(fav_.begin(), fav_.end(), [&](double f) { return std::fabs(f - c.dialHz) <= o_.freqTolHz; }), fav_.end());
        if (on) fav_.push_back(c.dialHz);
    }
}
void ConversationBook::setMuted(uint32_t id, bool on) { for (Conversation& c : conv_) if (c.id == id) { c.muted = on; ignoreHz(c.dialHz, on); } }
void ConversationBook::setRecording(uint32_t id, bool on) { for (Conversation& c : conv_) if (c.id == id) c.recording = on; }
void ConversationBook::addClue(uint32_t id, const std::string& text) {
    for (Conversation& c : conv_) if (c.id == id)
        for (const std::string& k : findAmateurCallsigns(text)) if (std::find(c.clues.begin(), c.clues.end(), k) == c.clues.end()) c.clues.push_back(k);
}

void ConversationBook::classify(Conversation& c) const {
    const double meanOver = c.overs > 0 ? c.talkSec / c.overs : 0;
    if (c.overs < 2) c.kind = Kind::Single;
    else if (c.speakers >= 3 && c.overs >= 6 && meanOver < 10) c.kind = Kind::Net;
    else if (meanOver >= 20 || (c.lastVoiceT - c.startT) >= 600) c.kind = Kind::RagChew;
    else c.kind = Kind::Qso;
}

void ConversationBook::countOver(Conversation& c, Acc& a, double tEnd) {
    a.counted = true;
                // which speaker: median pitch and offset of this over against the speakers heard so far
                std::vector<float> pv; for (float v : a.pitches) if (v > 0) pv.push_back(v);
                std::sort(pv.begin(), pv.end());
                const float p = pv.empty() ? 0.f : pv[pv.size() / 2];
                std::vector<double> ov = a.offs; std::sort(ov.begin(), ov.end());
                const double off = ov.empty() ? 0 : ov[ov.size() / 2];
                std::vector<Speaker>& sp = spk_[c.id];
                int who = -1;
                for (size_t i = 0; i < sp.size(); i++) {
                    const double dp = (p > 0 && sp[i].pitch > 0) ? std::fabs(std::log(p / sp[i].pitch)) : 0.0;
                    const double dof = std::fabs(off - sp[i].off);
                    const bool different = dp > 0.25 || (dp > 0.15 && dof > 60) || dof > 110;
                    if (!different) { who = (int)i; break; }
                }
                if (who < 0 && !sp.empty() && pv.size() < 3) {   // too short to tell: the speaker with the nearest pitch
                    double bd = 1e9;
                    for (size_t i = 0; i < sp.size(); i++) { const double dd = (p > 0 && sp[i].pitch > 0) ? std::fabs(std::log(p / sp[i].pitch)) : 1.0; if (dd < bd) { bd = dd; who = (int)i; } }
                } else if (who < 0) { sp.push_back({p, off}); who = (int)sp.size() - 1; }
                else { if (p > 0) sp[(size_t)who].pitch = sp[(size_t)who].pitch > 0 ? 0.7f * sp[(size_t)who].pitch + 0.3f * p : p; sp[(size_t)who].off = 0.7 * sp[(size_t)who].off + 0.3 * off; }
                a.speaker = who;
                c.speakers = (int)sp.size();
                c.overs++;
                c.overList.push_back({a.t0, tEnd, who});
                if (c.overList.size() > 400) c.overList.erase(c.overList.begin());
                classify(c);
}

void ConversationBook::closeOver(Conversation& c, double t1) {
    auto it = acc_.find(c.id);
    if (it == acc_.end()) return;
    if (!it->second.counted && t1 - it->second.t0 >= o_.minOverSec) countOver(c, it->second, t1);
    if (it->second.counted && !c.overList.empty()) c.overList.back().t1 = t1;
    acc_.erase(it);
    classify(c);
}

std::vector<BookEvent> ConversationBook::update(double now, const std::vector<Detection>& dets) {
    std::vector<BookEvent> ev;
    for (const Detection& d : dets) {
        if (!d.confirmed) continue;
        if (isIgnoredHz(d.dialHz)) {   // a muted frequency: keep the row (if any) fresh but record nothing
            for (Conversation& c : conv_) if (!c.ended && std::fabs(c.dialHz - d.dialHz) <= o_.freqTolHz) c.lastSeenT = now;
            continue;
        }
        Conversation* cv = nullptr;
        double bestD = 1e18;
        for (Conversation& c : conv_) {   // the same track is the same station, even if its frequency estimate was corrected
            if (c.ended || std::find(c.trackIds.begin(), c.trackIds.end(), d.trackId) == c.trackIds.end()) continue;
            const double df = std::fabs(c.dialHz - d.dialHz);
            if (df < bestD) { bestD = df; cv = &c; }
        }
        if (cv && bestD > o_.freqTolHz) cv->dialHz = d.dialHz;   // the track's frequency was corrected (its band filled out): follow it
        if (!cv) {
            bestD = o_.freqTolHz + 1;
            for (Conversation& c : conv_) {
                if (c.ended) continue;
                const double df = std::fabs(c.dialHz - d.dialHz);
                if (df <= o_.freqTolHz && df < bestD) { bestD = df; cv = &c; }
            }
        }
        const double dur = !d.audio.empty() && d.audioRate > 0 ? (double)d.audio.size() / d.audioRate : 0.5;
        if (!cv) {
            if (!d.voiceNow) continue;
            Conversation c;
            c.id = nextId_++;
            c.dialHz = c.firstDialHz = d.dialHz;
            c.side = d.side;
            const BandInfo bi = bandInfo(d.dialHz);
            c.band = bi.band; c.service = bi.service;
            c.startT = std::min(d.sinceT > 0 ? d.sinceT : d.time, d.time - dur);
            c.lastVoiceT = c.lastSeenT = d.time;
            c.snrDb = c.snrMaxDb = d.snrDb;
            c.favorite = isFavoriteHz(d.dialHz);
            conv_.push_back(c);
            if (conv_.size() > o_.maxConversations) {
                auto old = std::find_if(conv_.begin(), conv_.end(), [](const Conversation& x) { return x.ended; });
                if (old != conv_.end()) { spk_.erase(old->id); conv_.erase(old); }
            }
            cv = &conv_.back();
        }
        Conversation& c = *cv;
        c.lastSeenT = d.time;
        if (std::find(c.trackIds.begin(), c.trackIds.end(), d.trackId) == c.trackIds.end()) { c.trackIds.push_back(d.trackId); if (c.trackIds.size() > 8) c.trackIds.erase(c.trackIds.begin()); }
        const double offNow = d.dialHz - c.dialHz;
        if (d.side != Side::Unknown) c.side = d.side;
        c.bwHz = d.bwHz; c.speech = d.speech;
        if (d.voiceNow) {
            c.snrDb = c.snrDb == 0 ? d.snrDb : 0.8 * c.snrDb + 0.2 * d.snrDb;
            c.snrMaxDb = std::max(c.snrMaxDb, (double)d.snrDb);
        }
        if (d.voiceNow && (c.snrHist.empty() || d.time - c.snrHist.back().first >= 5.0)) {
            c.snrHist.push_back({d.time, d.snrDb});
            while (!c.snrHist.empty() && d.time - c.snrHist.front().first > 600) c.snrHist.pop_front();
        }
        if (d.voiceNow) {
            c.talkSec += dur;
            const double v0 = d.time - dur;
            if (!c.segments.empty() && v0 - c.segments.back().second < 3.0) c.segments.back().second = d.time;
            else c.segments.push_back({v0, d.time});
            if (c.segments.size() > 1000) c.segments.erase(c.segments.begin(), c.segments.begin() + 100);
            Acc* a = nullptr;
            auto it = acc_.find(c.id);
            const bool newOver = it == acc_.end() || (d.time - lastVoice_[c.id]) >= o_.overGapSec;
            if (newOver) {
                if (it != acc_.end()) closeOver(c, lastVoice_[c.id]);
                Acc na; na.t0 = v0;
                acc_[c.id] = na;
                a = &acc_[c.id];
            } else {
                a = &it->second;
                // another voice without a pause between: the pitch (or the offset) changed for two windows in a row
                const size_t k = a->pitches.size();
                if (d.pitchHz > 0 && k >= 7 && a->counted && a->pitches[k - 1] > 0 && a->pitches[k - 2] > 0 && now - splitT_[c.id] > 3.0) {
                    std::vector<float> prior(a->pitches.begin(), a->pitches.end() - 2);
                    prior.erase(std::remove(prior.begin(), prior.end(), 0.f), prior.end());
                    if (prior.size() >= 4) {
                        std::sort(prior.begin(), prior.end());
                        const float med = prior[prior.size() / 2];
                        const float p3[3] = {a->pitches[k - 2], a->pitches[k - 1], d.pitchHz};
                        int farUp = 0, farDown = 0;
                        for (float p : p3) { const float lr = std::log(p / med); if (lr > 0.22f) farUp++; if (lr < -0.22f) farDown++; }
                        if (farUp == 3 || farDown == 3) {
                            const double ts = v0 - 2 * dur;
                            const std::vector<float> keep(p3, p3 + 2);
                            closeOver(c, ts);
                            Acc na; na.t0 = ts; na.pitches = keep;
                            acc_[c.id] = na;
                            a = &acc_[c.id];
                            splitT_[c.id] = now;
                        }
                    }
                }
            }
            a->pitches.push_back(d.pitchHz); a->offs.push_back(offNow);
            lastVoice_[c.id] = d.time;
            c.lastVoiceT = d.time;
            size_t voicedN = 0; for (float v : a->pitches) if (v > 0) voicedN++;
            if (!a->counted && d.time - a->t0 >= o_.minOverSec && (voicedN >= 3 || d.time - a->t0 >= 2.5)) {
                countOver(c, *a, d.time);
            } else if (a->counted && !c.overList.empty()) c.overList.back().t1 = d.time;
        }
        if (!c.announced && c.overs >= 1) { c.announced = true; BookEvent e; e.type = EventType::New; e.time = now; e.id = c.id; e.snapshot = c; ev.push_back(e); }
        // the frequency follows the signal while it drifts (a slow average, not a jump to the other station of the pair)
        c.dialHz = 0.9 * c.dialHz + 0.1 * d.dialHz;
    }
    std::vector<BookEvent> more = tick(now);
    ev.insert(ev.end(), more.begin(), more.end());
    return ev;
}

std::vector<BookEvent> ConversationBook::tick(double now) {
    std::vector<BookEvent> ev;
    for (Conversation& c : conv_) {
        if (c.ended) continue;
        c.active = (now - c.lastVoiceT) <= o_.activeSec;
        auto it = acc_.find(c.id);
        if (it != acc_.end() && now - lastVoice_[c.id] >= o_.overGapSec) closeOver(c, lastVoice_[c.id]);
        if (now - c.lastVoiceT > o_.conversationGapSec) {
            c.ended = true; c.active = false;
            closeOver(c, c.lastVoiceT);
            classify(c);
            if (c.announced) { BookEvent e; e.type = EventType::Ended; e.time = now; e.id = c.id; e.snapshot = c; ev.push_back(e); }
        }
        // strength trend: least squares over the last 2 minutes
        const double t1 = c.snrHist.empty() ? 0 : c.snrHist.back().first;
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
        for (const auto& p : c.snrHist) if (t1 - p.first <= 120) { const double x = p.first - t1; sx += x; sy += p.second; sxx += x * x; sxy += x * p.second; n++; }
        const double den = n * sxx - sx * sx;
        c.snrTrendDbPerMin = (n >= 3 && den > 1e-6) ? 60.0 * (n * sxy - sx * sy) / den : 0.0;
    }
    // forget old ended ones
    conv_.erase(std::remove_if(conv_.begin(), conv_.end(), [&](const Conversation& c) { return c.ended && now - c.lastVoiceT > o_.keepEndedSec; }), conv_.end());
    return ev;
}

std::vector<Conversation> ConversationBook::list(double now, const Filter& f, SortBy s) const {
    std::vector<Conversation> r;
    for (const Conversation& c : conv_) {
        if (c.overs == 0) continue;   // a blip, not a conversation
        if (c.muted && !f.showMuted) continue;
        if (!f.band.empty() && c.band != f.band) continue;
        if (c.snrDb < f.minSnrDb) continue;
        if (f.activeOnly && !(c.active && !c.ended)) continue;
        if (f.hideEnded && c.ended) continue;
        if (f.hideShortSec > 0 && c.talkSec < f.hideShortSec) continue;
        if (f.favoritesOnly && !c.favorite) continue;
        if (f.side != Side::Unknown && c.side != f.side) continue;
        if (!f.service.empty() && c.service != f.service) continue;
        r.push_back(c);
    }
    (void)now;
    auto act = [](const Conversation& c) { return c.active && !c.ended ? 1 : 0; };
    std::stable_sort(r.begin(), r.end(), [&](const Conversation& a, const Conversation& b) {
        switch (s) {
        case SortBy::Strength: if (act(a) != act(b)) return act(a) > act(b); return a.snrDb > b.snrDb;
        case SortBy::Start: return a.startT > b.startT;
        case SortBy::Frequency: return a.dialHz < b.dialHz;
        case SortBy::Activity: default:
            if (act(a) != act(b)) return act(a) > act(b);
            if (a.favorite != b.favorite && act(a)) return a.favorite;
            return a.lastVoiceT > b.lastVoiceT;
        }
    });
    return r;
}

uint32_t ConversationBook::bestActive(double now, const std::vector<uint32_t>& exclude) const {
    (void)now;
    uint32_t best = 0; double bs = -1e9;
    for (const Conversation& c : conv_) {
        if (c.ended || c.muted || !c.active) continue;
        if (std::find(exclude.begin(), exclude.end(), c.id) != exclude.end()) continue;
        const double sc = 100 + (c.favorite ? 40 : 0) + std::min(60.0, c.snrDb) + std::min(10, c.overs) + (c.lastVoiceT > 0 ? 0.001 * c.lastVoiceT : 0);
        if (sc > bs) { bs = sc; best = c.id; }
    }
    return best;
}

std::string ConversationBook::fmtTime(double t) {
    const time_t tt = (time_t)t;
    struct tm g;
#if defined(_WIN32)
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &g);
    return b;
}

namespace {
std::string jesc(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '"' || ch == '\\') { o += '\\'; o += ch; } else if ((unsigned char)ch < 0x20) o += ' '; else o += ch; }
    return o;
}
}  // namespace

std::string ConversationBook::toCsv(double now) const {
    std::ostringstream o;
    o << "id,frequency_mhz,sideband,band,service,start_utc,last_voice_utc,duration_s,talk_s,overs,speakers,kind,snr_db,snr_max_db,s_meter,trend_db_per_min,active,favorite,clues\n";
    for (const Conversation& c : list(now, [] { Filter f; f.showMuted = true; return f; }(), SortBy::Start)) {
        char b[64];
        snprintf(b, sizeof b, "%.4f", c.dialHz / 1e6);
        o << c.id << ',' << b << ',' << sideName(c.side) << ',' << c.band << ",\"" << c.service << "\"," << fmtTime(c.startT) << ',' << fmtTime(c.lastVoiceT) << ',';
        snprintf(b, sizeof b, "%.0f,%.0f", c.lastVoiceT - c.startT, c.talkSec);
        o << b << ',' << c.overs << ',' << c.speakers << ',' << kindName(c.kind) << ',';
        snprintf(b, sizeof b, "%.1f,%.1f", c.snrDb, c.snrMaxDb);
        o << b << ',' << sText(c.snrDb) << ',';
        snprintf(b, sizeof b, "%.1f", c.snrTrendDbPerMin);
        o << b << ',' << (c.active && !c.ended ? 1 : 0) << ',' << (c.favorite ? 1 : 0) << ",\"";
        for (size_t i = 0; i < c.clues.size(); i++) o << (i ? " " : "") << c.clues[i];
        o << "\"\n";
    }
    return o.str();
}

std::string ConversationBook::toJson(double now) const {
    std::ostringstream o;
    o << "{\"source\":\"OnAir\",\"kind\":\"ssb-conversations\",\"conversations\":[";
    bool first = true;
    for (const Conversation& c : list(now, [] { Filter f; f.showMuted = true; return f; }(), SortBy::Start)) {
        char b[256];
        snprintf(b, sizeof b, "{\"id\":%u,\"freq_hz\":%.0f,\"sideband\":\"%s\",\"band\":\"%s\",\"service\":\"%s\",\"start\":%.0f,\"last_voice\":%.0f,\"talk_s\":%.1f,\"overs\":%d,\"speakers\":%d,",
                 c.id, c.dialHz, sideName(c.side), jesc(c.band).c_str(), jesc(c.service).c_str(), c.startT, c.lastVoiceT, c.talkSec, c.overs, c.speakers);
        o << (first ? "" : ",") << b;
        first = false;
        snprintf(b, sizeof b, "\"kind\":\"%s\",\"snr_db\":%.1f,\"snr_max_db\":%.1f,\"s_meter\":\"%s\",\"trend_db_per_min\":%.1f,\"active\":%s,\"ended\":%s,\"favorite\":%s,\"clues\":[",
                 kindName(c.kind), c.snrDb, c.snrMaxDb, sText(c.snrDb).c_str(), c.snrTrendDbPerMin, c.active && !c.ended ? "true" : "false", c.ended ? "true" : "false", c.favorite ? "true" : "false");
        o << b;
        for (size_t i = 0; i < c.clues.size(); i++) o << (i ? "," : "") << '"' << jesc(c.clues[i]) << '"';
        o << "],\"overs_list\":[";
        for (size_t i = 0; i < c.overList.size(); i++) { snprintf(b, sizeof b, "%s{\"t0\":%.1f,\"t1\":%.1f,\"speaker\":%d}", i ? "," : "", c.overList[i].t0, c.overList[i].t1, c.overList[i].speaker); o << b; }
        o << "]}";
    }
    o << "]}\n";
    return o.str();
}

std::vector<ConversationBook::TimelineRow> ConversationBook::timeline(double now, double hours) const {
    std::vector<TimelineRow> rows;
    const double t0 = now - hours * 3600;
    for (const Conversation& c : conv_) {
        if (c.muted) continue;
        if (c.lastVoiceT < t0) continue;
        TimelineRow* row = nullptr;
        for (TimelineRow& r : rows) if (std::fabs(r.dialHz - c.dialHz) <= o_.freqTolHz) { row = &r; break; }
        if (!row) { rows.push_back({c.dialHz, c.label(), {}}); row = &rows.back(); }
        for (const auto& sg : c.segments) if (sg.second >= t0) row->bars.push_back({std::max(sg.first, t0), sg.second, c.id, c.side, c.favorite});
    }
    std::sort(rows.begin(), rows.end(), [](const TimelineRow& a, const TimelineRow& b) { return a.dialHz < b.dialHz; });
    return rows;
}

// ------------------------------------------------------------------ auto-listen
uint32_t AutoListen::update(const ConversationBook& b, double now) {
    skipped_.erase(std::remove_if(skipped_.begin(), skipped_.end(), [&](uint32_t id) { const Conversation* c = b.find(id); return !c || c->ended; }), skipped_.end());
    const Conversation* c = cur_ ? b.find(cur_) : nullptr;
    if (cur_ && (!c || c->ended)) { cur_ = 0; c = nullptr; quietSince_ = -1; }
    if (c) {
        if (hold) return cur_;
        if (c->active) { quietSince_ = -1; return cur_; }
        if (!enabled) return cur_;
        if (quietSince_ < 0) quietSince_ = now;
        if (now - quietSince_ >= switchDelaySec) {
            const uint32_t best = b.bestActive(now, skipped_);
            if (best && best != cur_) { cur_ = best; switchT_ = now; quietSince_ = -1; }
        }
        return cur_;
    }
    if (enabled) {
        const uint32_t best = b.bestActive(now, skipped_);
        if (best) { cur_ = best; switchT_ = now; quietSince_ = -1; }
    }
    return cur_;
}
void AutoListen::skip(uint32_t current) { if (current) skipped_.push_back(current); if (cur_ == current) { cur_ = 0; quietSince_ = -1; } }
void AutoListen::select(uint32_t id, double now) { cur_ = id; switchT_ = now; quietSince_ = -1; }
double AutoListen::gain(double now) const {
    double g = fadeSec > 0 ? std::min(1.0, std::max(0.0, (now - switchT_) / fadeSec)) : 1.0;
    if (quietSince_ >= 0 && enabled && !hold) {
        const double x = (now - (quietSince_ + switchDelaySec - fadeSec)) / std::max(0.05, fadeSec);
        g *= std::min(1.0, std::max(0.0, 1.0 - x));
    }
    return g;
}

// ------------------------------------------------------------------ sweep
std::vector<SweepRange> defaultSweepRanges() {
    return {{1.8e6, 2.0e6, "160 m"}, {3.5e6, 4.0e6, "80 m"}, {5.3e6, 5.41e6, "60 m"}, {7.0e6, 7.2e6, "40 m"}, {14.0e6, 14.35e6, "20 m"}, {18.068e6, 18.168e6, "17 m"},
            {21.0e6, 21.45e6, "15 m"}, {24.89e6, 24.99e6, "12 m"}, {26.965e6, 27.405e6, "CB"}, {28.0e6, 29.7e6, "10 m"}, {4.0e6, 4.438e6, "Maritime 4 MHz"},
            {6.2e6, 6.525e6, "Maritime 6 MHz"}, {8.1e6, 8.815e6, "Maritime 8 MHz"}, {5.45e6, 5.73e6, "Aviation 5 MHz"}, {8.815e6, 9.04e6, "Aviation 9 MHz"}};
}
SweepPlanner::SweepPlanner(std::vector<SweepRange> ranges, double spanHz, double dwellSec, double maxDwellSec) : ranges_(std::move(ranges)), dwell_(dwellSec), maxDwell_(maxDwellSec) {
    const double step = std::max(1000.0, spanHz * 0.8);
    for (size_t r = 0; r < ranges_.size(); r++) {
        const double lo = ranges_[r].loHz, hi = ranges_[r].hiHz;
        if (hi - lo <= spanHz) { centres_.push_back(0.5 * (lo + hi)); rangeOf_.push_back(r); continue; }
        for (double c = lo + spanHz / 2; c - spanHz / 2 < hi; c += step) { centres_.push_back(std::min(c, hi - spanHz / 2)); rangeOf_.push_back(r); if (c >= hi - spanHz / 2) break; }
    }
}
double SweepPlanner::centreHz() const { return centres_.empty() ? 0 : centres_[idx_]; }
const std::string& SweepPlanner::rangeName() const { static const std::string e; return centres_.empty() ? e : ranges_[rangeOf_[idx_]].name; }
bool SweepPlanner::update(double now, int activeSignals) {
    if (centres_.empty()) return false;
    if (stepStart_ < 0) { stepStart_ = now; return true; }
    const double lim = activeSignals > 0 ? maxDwell_ : dwell_;
    if (now - stepStart_ >= lim) { idx_ = (idx_ + 1) % centres_.size(); stepStart_ = now; return true; }
    return false;
}

// ------------------------------------------------------------------ clips
namespace {
void put32(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void wavHeader(FILE* f, uint32_t samples, uint32_t rate) {
    unsigned char h[44];
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + samples * 2); memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1);
    put32(h + 24, rate); put32(h + 28, rate * 2); put16(h + 32, 2); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, samples * 2);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f);
}
}  // namespace

std::string ClipRecorder::pathFor(uint32_t id) const { auto it = clips_.find(id); return it == clips_.end() ? std::string() : it->second.path; }

void ClipRecorder::append(const Conversation& c, const float* pcm, size_t n, double rateHz, double now) {
    if (n == 0) return;
    auto it = clips_.find(c.id);
    if (it == clips_.end()) {
        Clip cl; cl.rate = rateHz; cl.t0 = now;
        std::string ts = ConversationBook::fmtTime(now);
        for (char& ch : ts) if (ch == '-' || ch == ':') ch = '_'; else if (ch == ' ') ch = '-';
        char nm[160];
        snprintf(nm, sizeof nm, "ssb_%u_%.0fkHz_%s.wav", c.id, c.dialHz / 1e3, ts.c_str());
        cl.path = dir_ + "/" + nm;
        FILE* f = fopen(cl.path.c_str(), "wb");
        if (!f) return;
        unsigned char z[44] = {0};
        fwrite(z, 1, 44, f);
        cl.fp = f;
        it = clips_.emplace(c.id, cl).first;
    }
    Clip& cl = it->second;
    FILE* f = (FILE*)cl.fp;
    std::vector<unsigned char> b(n * 2);
    for (size_t i = 0; i < n; i++) {
        float v = pcm[i] * 0.25f;
        v = v > 1 ? 1 : v < -1 ? -1 : v;
        put16(&b[i * 2], (uint16_t)(int16_t)std::lrintf(v * 32767.f));
    }
    fwrite(b.data(), 1, b.size(), f);
    cl.samples += n;
}

std::string ClipRecorder::finish(const Conversation& c, double now) {
    auto it = clips_.find(c.id);
    if (it == clips_.end()) return {};
    Clip cl = it->second;
    clips_.erase(it);
    FILE* f = (FILE*)cl.fp;
    wavHeader(f, (uint32_t)cl.samples, (uint32_t)std::lround(cl.rate));
    fclose(f);
    std::string meta = cl.path.substr(0, cl.path.size() - 4) + ".json";
    if (FILE* m = fopen(meta.c_str(), "wb")) {
        char b[512];
        snprintf(b, sizeof b, "{\"source\":\"OnAir\",\"id\":%u,\"freq_hz\":%.0f,\"sideband\":\"%s\",\"band\":\"%s\",\"service\":\"%s\",\"start\":\"%s\",\"end\":\"%s\",\"seconds\":%.1f,\"overs\":%d,\"speakers\":%d,\"kind\":\"%s\",\"snr_db\":%.1f,\"audio_rate\":%.0f,\"clues\":[",
                 c.id, c.dialHz, sideName(c.side), jesc(c.band).c_str(), jesc(c.service).c_str(), ConversationBook::fmtTime(cl.t0).c_str(), ConversationBook::fmtTime(now).c_str(),
                 (double)cl.samples / cl.rate, c.overs, c.speakers, kindName(c.kind), c.snrDb, cl.rate);
        fputs(b, m);
        for (size_t i = 0; i < c.clues.size(); i++) fprintf(m, "%s\"%s\"", i ? "," : "", jesc(c.clues[i]).c_str());
        fputs("]}\n", m);
        fclose(m);
    }
    return cl.path;
}

}  // namespace dect2::ssbconv
