#include "dect2/voice/conversation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace dect2::voice {

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string srcId(const CallInfo& c) { return c.srcText.empty() ? (c.src ? std::to_string(c.src) : std::string()) : c.srcText; }
std::string dstId(const CallInfo& c) { return c.dstText.empty() ? (c.dst ? std::to_string(c.dst) : std::string()) : c.dstText; }

int64_t endWall(const CallEntry& c) { return c.wallEnd >= c.wallStart ? c.wallEnd : c.wallStart; }

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c;
        }
    }
    return o;
}

std::string csvCell(const std::string& s) {
    std::string v = s;
    if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@')) v = "'" + v;
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string o = "\"";
    for (char c : v) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string num(double v, int dec = 1) { char b[40]; std::snprintf(b, sizeof b, "%.*f", dec, v); return b; }

std::string stateText(const CallEntry& c) { return c.played ? "played" : verdictText(c.verdict); }

} // namespace

const char* signalLabel(float e) {
    if (e < 0) return "-";
    if (e < 0.02f) return "good";
    if (e < 0.08f) return "fair";
    return "poor";
}

std::string partiesKey(const CallInfo& c) {
    if (c.group) return "G:" + (dstId(c).empty() ? std::string("?") : dstId(c));
    std::string a = srcId(c), b = dstId(c);
    if (b < a) std::swap(a, b);
    return "P:" + a + "<>" + b;
}

std::string utcText(int64_t t) {
    if (t <= 0) return "-";
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

bool callMatches(const CallEntry& c, const ConvFilter& f) {
    if (f.minSeconds > 0 && c.seconds() < f.minSeconds) return false;
    if (f.maxErrorRate >= 0 && c.errorRate >= 0 && c.errorRate > f.maxErrorRate) return false;
    if (f.playedOnly && !c.played) return false;
    if (f.withAudioOnly && c.file.empty()) return false;
    if (f.hideEncrypted && (c.info.encrypted || c.verdict == Verdict::Encrypted)) return false;
    if (f.slot > 0 && c.info.slot != f.slot) return false;
    if (f.sinceWall > 0 && c.wallStart < f.sinceWall) return false;
    if (!f.query.empty()) {
        const std::string q = lower(f.query);
        const std::string hay = lower(targetText(c.info) + "\n" + sourceText(c.info) + "\n" + c.info.alias + "\n" + c.label + "\n" + c.info.access
                                      + "\n" + stateText(c));
        if (hay.find(q) == std::string::npos) return false;
    }
    return true;
}

std::vector<Conversation> groupConversations(const std::vector<CallEntry>& in, double gapSec, const ConvFilter& f) {
    std::vector<CallEntry> calls;
    for (const auto& c : in) if (callMatches(c, f)) calls.push_back(c);
    std::stable_sort(calls.begin(), calls.end(), [](const CallEntry& a, const CallEntry& b) { return a.wallStart != b.wallStart ? a.wallStart < b.wallStart : a.id < b.id; });
    std::vector<Conversation> out;
    std::map<std::string, size_t> open;   // key -> index of the conversation still open
    for (const auto& c : calls) {
        char kb[64];
        std::snprintf(kb, sizeof kb, "%.0f|%d|", c.info.carrierHz, c.info.slot);
        const std::string key = kb + partiesKey(c.info);
        auto it = open.find(key);
        if (it == open.end() || double(c.wallStart - out[it->second].lastWall) > gapSec) {
            Conversation v;
            v.key = key;
            v.group = c.info.group;
            v.slot = c.info.slot;
            v.carrierHz = c.info.carrierHz;
            if (c.info.group) v.title = targetText(c.info);
            else { std::string a = srcId(c.info), b = dstId(c.info); if (b < a) std::swap(a, b); v.title = (a.empty() ? "?" : a) + " <-> " + (b.empty() ? "?" : b); }
            v.firstWall = c.wallStart;
            out.push_back(v);
            open[key] = out.size() - 1;
            it = open.find(key);
        }
        Conversation& v = out[it->second];
        v.calls.push_back(c);
        v.lastWall = std::max(v.lastWall, endWall(c));
        if (v.label.empty()) v.label = c.label;
        if (v.access.empty()) v.access = c.info.access;
        v.talkSeconds += c.seconds();
        if (c.played) ++v.played;
        if (!c.file.empty()) ++v.withAudio;
        if (c.info.encrypted || c.verdict == Verdict::Encrypted) ++v.encrypted;
        if (c.info.emergency) ++v.emergency;
        if (c.active) v.active = true;
    }
    for (auto& v : out) {
        std::map<std::string, Talker> t;
        double sum = 0; size_t n = 0;
        for (const auto& c : v.calls) {
            const std::string who = sourceText(c.info);
            Talker& k = t[who];
            k.who = who; ++k.calls; k.seconds += c.seconds();
            if (c.errorRate >= 0) { sum += c.errorRate; ++n; v.worstError = std::max(v.worstError, c.errorRate); }
        }
        if (n) v.avgError = float(sum / double(n));
        for (auto& kv : t) v.talkers.push_back(kv.second);
        std::sort(v.talkers.begin(), v.talkers.end(), [](const Talker& a, const Talker& b) { return a.seconds != b.seconds ? a.seconds > b.seconds : a.who < b.who; });
    }
    std::stable_sort(out.begin(), out.end(), [](const Conversation& a, const Conversation& b) { return a.lastWall > b.lastWall; });
    return out;
}

std::string conversationsCsv(const std::vector<Conversation>& conv) {
    std::string s = "conversation,start_utc,end_utc,seconds,target,source,alias,slot,access,bit_error_percent,signal,state,recording\n";
    // numbered oldest first so the numbers do not change when a new conversation appears
    std::vector<const Conversation*> order;
    for (const auto& c : conv) order.push_back(&c);
    std::sort(order.begin(), order.end(), [](const Conversation* a, const Conversation* b) { return a->firstWall < b->firstWall; });
    int n = 0;
    for (const Conversation* v : order) {
        ++n;
        for (const auto& c : v->calls) {
            s += std::to_string(n) + "," + utcText(c.wallStart) + "," + utcText(endWall(c)) + "," + num(c.seconds()) + ","
               + csvCell(targetText(c.info)) + "," + csvCell(sourceText(c.info)) + "," + csvCell(c.info.alias) + "," + std::to_string(c.info.slot) + ","
               + csvCell(c.info.access) + "," + (c.errorRate >= 0 ? num(c.errorRate * 100.0) : std::string()) + "," + signalLabel(c.errorRate) + ","
               + stateText(c) + "," + csvCell(c.file) + "\n";
        }
    }
    return s;
}

std::string conversationsJson(const std::vector<Conversation>& conv, const std::string& mode) {
    std::string s = "{\n  \"mode\": \"" + jsonEsc(mode) + "\",\n  \"conversations\": [";
    bool firstV = true;
    for (const auto& v : conv) {
        s += firstV ? "\n" : ",\n"; firstV = false;
        s += "    {\"title\": \"" + jsonEsc(v.title) + "\", \"label\": \"" + jsonEsc(v.label) + "\", \"access\": \"" + jsonEsc(v.access) + "\", \"slot\": " + std::to_string(v.slot)
           + ", \"start_utc\": \"" + utcText(v.firstWall) + "\", \"end_utc\": \"" + utcText(v.lastWall) + "\", \"calls\": " + std::to_string(v.calls.size())
           + ", \"talk_seconds\": " + num(v.talkSeconds) + ", \"recordings\": " + std::to_string(v.withAudio) + ", \"encrypted_calls\": " + std::to_string(v.encrypted)
           + ", \"talkers\": [";
        for (size_t i = 0; i < v.talkers.size(); ++i)
            s += std::string(i ? ", " : "") + "{\"who\": \"" + jsonEsc(v.talkers[i].who) + "\", \"calls\": " + std::to_string(v.talkers[i].calls) + ", \"seconds\": " + num(v.talkers[i].seconds) + "}";
        s += "], \"call_list\": [";
        for (size_t i = 0; i < v.calls.size(); ++i) {
            const auto& c = v.calls[i];
            s += std::string(i ? ", " : "") + "{\"start_utc\": \"" + utcText(c.wallStart) + "\", \"seconds\": " + num(c.seconds()) + ", \"source\": \"" + jsonEsc(sourceText(c.info))
               + "\", \"alias\": \"" + jsonEsc(c.info.alias) + "\", \"bit_error\": " + (c.errorRate >= 0 ? num(c.errorRate, 4) : std::string("null")) + ", \"state\": \"" + jsonEsc(stateText(c))
               + "\", \"recording\": \"" + jsonEsc(c.file) + "\"}";
        }
        s += "]}";
    }
    s += "\n  ]\n}\n";
    return s;
}

} // namespace dect2::voice
