// Digital video generator: several instances, their outputs for the transmitter, and the free-text FFmpeg arguments (see videogen.h).
#include "dect2/tv/videogen.h"
#include "dect2/tv/ts_remux.h"
#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace dect2::vgen {

namespace {
struct Fifo { std::deque<uint8_t> bytes; };   // whole 188 byte packets, oldest first
std::mutex gMu;
std::map<int, Fifo> gFifo;
std::map<int, FrameSink> gSink;
std::map<int, double> gRate;                   // bit/s of each running generator with Output::Transmitter
constexpr size_t kMaxQueued = 188u * 4000;     // about 6 MB: at 10 Mbit/s a little over 4 s; the oldest packets go first when the transmitter is slower
}

std::string generatorSpec(int id) { return "generator:" + std::to_string(id); }

bool parseGeneratorSpec(const std::string& s, int& id) {
    if (s.compare(0, 10, "generator:") != 0 || s.size() == 10 || s.size() > 12) return false;
    int v = 0;
    for (size_t i = 10; i < s.size(); i++) { if (!isdigit((unsigned char)s[i])) return false; v = v * 10 + (s[i] - '0'); }
    if (v < 1 || v > kMaxInstances) return false;
    id = v;
    return true;
}

void generatorPush(int id, const uint8_t* packets, size_t n) {
    std::lock_guard<std::mutex> lk(gMu);
    Fifo& f = gFifo[id];
    f.bytes.insert(f.bytes.end(), packets, packets + n * 188);
    if (f.bytes.size() > kMaxQueued) f.bytes.erase(f.bytes.begin(), f.bytes.begin() + (std::ptrdiff_t)((f.bytes.size() - kMaxQueued) / 188 * 188));
}

void generatorClear(int id) { std::lock_guard<std::mutex> lk(gMu); gFifo[id].bytes.clear(); }

bool generatorPop(int id, uint8_t* pk) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gFifo.find(id);
    if (it == gFifo.end() || it->second.bytes.size() < 188) return false;
    std::copy(it->second.bytes.begin(), it->second.bytes.begin() + 188, pk);
    it->second.bytes.erase(it->second.bytes.begin(), it->second.bytes.begin() + 188);
    return true;
}

void generatorSetRate(int id, double bps) { std::lock_guard<std::mutex> lk(gMu); if (bps > 0) gRate[id] = bps; else gRate.erase(id); }

double generatorRate(int id) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gRate.find(id);
    return it == gRate.end() ? 0.0 : it->second;
}

std::function<void(uint8_t*)> generatorMuxSource(const std::vector<int>& ids, const std::vector<std::string>& names, const std::string& provider,
                                                 int firstServiceId, double outBitrate) {
    std::vector<TsRemuxInput> in;
    for (size_t k = 0; k < ids.size(); k++) {
        const int id = ids[k];
        if (id < 1 || id > kMaxInstances) return {};
        TsRemuxInput x;
        x.pop = [id](uint8_t* pk) { return generatorPop(id, pk); };
        x.rate = [id] { return generatorRate(id); };
        x.queued = [id] { return generatorQueued(id); };
        x.flush = [id] { generatorClear(id); };
        x.name = k < names.size() ? names[k] : std::string();
        x.provider = provider;
        in.push_back(std::move(x));
    }
    if (in.empty()) return {};
    TsRemuxConfig c;
    c.outBitrate = outBitrate;
    c.firstServiceId = firstServiceId > 0 ? firstServiceId : 1;
    auto mux = std::make_shared<TsRemux>(std::move(in), c);
    return [mux](uint8_t* pk) { mux->next(pk); };
}

size_t generatorQueued(int id) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gFifo.find(id);
    return it == gFifo.end() ? 0 : it->second.bytes.size() / 188;
}

bool generatorTsSource(int id, std::function<void(uint8_t*)>& out) {
    out = {};
    if (id < 1 || id > kMaxInstances) return false;
    out = [id](uint8_t* pk) {
        {
            std::lock_guard<std::mutex> lk(gMu);
            auto it = gFifo.find(id);
            if (it != gFifo.end() && it->second.bytes.size() >= 188) {
                std::copy(it->second.bytes.begin(), it->second.bytes.begin() + 188, pk);
                it->second.bytes.erase(it->second.bytes.begin(), it->second.bytes.begin() + 188);
                return;
            }
        }
        // nothing queued (the generator is not running, or slower than the transmitter): a null packet keeps the transmitter's rate
        pk[0] = 0x47; pk[1] = 0x1F; pk[2] = 0xFF; pk[3] = 0x10;
        std::fill(pk + 4, pk + 188, 0xFF);
    };
    return true;
}

void setGeneratorFrameSink(int id, FrameSink s) { std::lock_guard<std::mutex> lk(gMu); if (s) gSink[id] = std::move(s); else gSink.erase(id); }
bool generatorHasFrameSink(int id) { std::lock_guard<std::mutex> lk(gMu); return gSink.count(id) != 0; }
void generatorPublishFrame(int id, int w, int h, const uint8_t* rgb24) {
    std::lock_guard<std::mutex> lk(gMu);   // the sink is short (a copy); clearing waits for it, so it never runs after its owner has gone
    auto it = gSink.find(id);
    if (it != gSink.end()) it->second(w, h, rgb24);
}

// ---- FFmpeg arguments ----------------------------------------------------------------------------------------------------------

// Split like a shell would for quoting only: spaces separate, '...' and "..." group, \ escapes the next character outside single quotes.
// Nothing is expanded or run: no $, `, ;, |, & or globbing means anything here.
std::vector<std::string> splitArgs(const std::string& text, std::string& err) {
    err.clear();
    std::vector<std::string> out;
    std::string cur; bool have = false; char q = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) cur += text[++i];
            else cur += c;
        } else if (c == '\'' || c == '"') { q = c; have = true; }
        else if (c == '\\' && i + 1 < text.size()) { cur += text[++i]; have = true; }
        else if (isspace((unsigned char)c)) { if (have) { out.push_back(cur); cur.clear(); have = false; } }
        else { cur += c; have = true; }
        if (cur.size() > 400) { err = "an argument is too long"; return {}; }
    }
    if (q) { err = "a quote is not closed"; return {}; }
    if (have) out.push_back(cur);
    if (out.size() > 64) { err = "too many arguments"; return {}; }
    return out;
}

bool parseEncoderArgs(const std::string& text, std::vector<ArgOpt>& out, std::string& err) {
    out.clear();
    const std::vector<std::string> a = splitArgs(text, err);
    if (!err.empty()) return false;
    // what would change what the generator is (input, output, container, stream mapping, codec choice, filters, hardware set-up, files)
    static const char* const kDenied[] = {"i", "f", "y", "n", "c", "codec", "map", "filter", "filter_complex", "vf", "af", "lavfi", "hwaccel", "hwaccel_device", "init_hw_device",
                                          "filter_hw_device", "fs", "t", "to", "ss", "r", "s", "pix_fmt", "an", "vn", "sn", "dn", "protocol_whitelist", "safe", "metadata", "passlogfile",
                                          "pass", "stats_file", "dump", "report", "bsf", "attach", "dump_attachment", "override_ffserver", "headers", "user_agent", "cookies"};
    for (size_t i = 0; i < a.size(); i++) {
        std::string k = a[i];
        if (k.size() < 2 || k[0] != '-') { err = "\"" + k + "\" is not an option: options look like -preset veryfast"; return false; }
        k.erase(0, k[1] == '-' ? 2 : 1);
        if (k.size() >= 3 && k.compare(k.size() - 2, 2, ":v") == 0) k.resize(k.size() - 2);   // -b:v 800k is the video encoder's b
        if (k.empty() || k.find(':') != std::string::npos || k.find('=') != std::string::npos || k.find('/') != std::string::npos) { err = "\"" + a[i] + "\": only options of the video encoder are taken (no stream specifiers)"; return false; }
        for (char ch : k) if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') { err = "\"" + a[i] + "\" is not an encoder option"; return false; }
        for (const char* d : kDenied) if (k == d) { err = "-" + k + " is set by the generator itself (source, output and picture settings); it is not taken here"; return false; }
        if (i + 1 >= a.size()) { err = "-" + k + " needs a value"; return false; }
        const std::string& v = a[++i];
        if (v.size() > 300 || v.find('\n') != std::string::npos) { err = "the value of -" + k + " is not usable"; return false; }
        if (v.compare(0, 5, "pipe:") == 0 || v.compare(0, 7, "file://") == 0 || v.compare(0, 5, "file:") == 0) { err = "-" + k + ": a value that names a file or pipe is not taken"; return false; }
        out.push_back({k, v});
    }
    return true;
}

} // namespace dect2::vgen
