// OnAir Web safety rules, checked three ways:
//  1. compile time: the web classes have no transmit / arm / key / record member (detection idiom);
//  2. the protocol: every transmit / key / decrypt / record message type is refused with "forbidden" by default (transmit: unless the host sets
//     web_allow_tx=1, see test_web_tx_guard.cpp), before and after hello;
//  3. a scan of server/web: no engine (it has the recorder), no voice key store, no file writing, and the
//     restricted decoder classes are off in a fresh policy.
#include "../../server/web/web_config.h"
#include "../../server/web/web_hub.h"
#include "../../server/web/web_server.h"
#include "server_config.h"
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <memory>
#include <sstream>
#include <type_traits>
using namespace onair_web;
#include "../common/test_util.h"

#ifndef ONAIR_SOURCE_DIR
#define ONAIR_SOURCE_DIR "."
#endif

// ---- 1. no such member can be called on the web classes
#define HAS_MEMBER(name)                                                                                   \
    template <class T, class = void> struct has_##name : std::false_type {};                               \
    template <class T> struct has_##name<T, std::void_t<decltype(&T::name)>> : std::true_type {};
HAS_MEMBER(transmit)
HAS_MEMBER(startTx)
HAS_MEMBER(stopTx)
HAS_MEMBER(setTx)
HAS_MEMBER(arm)
HAS_MEMBER(ptt)
HAS_MEMBER(txSink)
HAS_MEMBER(importKey)
HAS_MEMBER(setKey)
HAS_MEMBER(decrypt)
HAS_MEMBER(startRecording)
HAS_MEMBER(engine)
template <class T> constexpr bool noTxSurface() {
    return !has_transmit<T>::value && !has_startTx<T>::value && !has_stopTx<T>::value && !has_setTx<T>::value && !has_arm<T>::value && !has_ptt<T>::value &&
           !has_txSink<T>::value && !has_importKey<T>::value && !has_setKey<T>::value && !has_decrypt<T>::value && !has_startRecording<T>::value && !has_engine<T>::value;
}
static_assert(noTxSurface<WebHub>(), "WebHub must not offer transmit, keys, recording or the engine");
static_assert(noTxSurface<WebServer>(), "WebServer must not offer transmit, keys, recording or the engine");
static_assert(noTxSurface<VirtualRx>(), "VirtualRx must not offer transmit");
static_assert(noTxSurface<SliceIq>(), "SliceIq must not offer transmit");
static_assert(noTxSurface<WebDecoder>(), "decoders must not offer transmit or keys");

static std::string readFile(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

static void testProtocol() {
    WebHub h;
    std::string c, w;
    AdmitRequest a; a.ip = "10.0.0.1";
    const int id = h.admit(a, c, w);
    for (const char* t : {"tx", "transmit", "ptt", "arm", "tx_arm", "carrier", "key", "keys", "import_key", "key_import", "voicekeys", "decrypt", "record", "recording", "save", "upload", "exec", "shell", "config", "admin"}) {
        ClientMsg m; std::string code, why;
        CHECK(!parseClientMsg(std::string("{\"type\":\"") + t + "\"}", m, code, why) && code == err::Forbidden, "%s must be forbidden (%s)", t, code.c_str());
    }
    // what a session can send at all: none of it reaches a transmitter, a key or the disk
    for (const auto& t : clientTypes()) CHECK(!isForbiddenType(t), "%s", t.c_str());
    CHECK(clientTypes().size() == 10, "the protocol has exactly the ten documented client types");
    h.leave(id);
}

static void testSourceScan() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(ONAIR_SOURCE_DIR);
    const fs::path dir = root / "server" / "web";
    int files = 0;
    const char* banned[] = {"voicekey", "VoiceKey", "voice_keys", "dect2/engine/engine.h", "startRecording",
                            "std::ofstream", "ofstream(", "fopen(", "fwrite(", "std::filesystem::rename", "fs::rename", "fs::remove", "std::remove(\"", "system(", "popen("};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        files++;
        const std::string raw = readFile(e.path());
        // the code without comments and string literals (the forbidden message names are strings), plus every #include line as it is
        std::string code, includes;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw.compare(i, 2, "//") == 0) { while (i < raw.size() && raw[i] != '\n') i++; code += '\n'; continue; }
            if (raw.compare(i, 2, "/*") == 0) { const size_t e2 = raw.find("*/", i + 2); i = e2 == std::string::npos ? raw.size() : e2 + 1; continue; }
            if (raw[i] == '"') { i++; while (i < raw.size() && raw[i] != '"') { if (raw[i] == '\\') i++; i++; } code += "\"\""; continue; }
            if (raw[i] == '\'') { i++; while (i < raw.size() && raw[i] != '\'') { if (raw[i] == '\\') i++; i++; } code += "''"; continue; }
            code += raw[i];
        }
        std::istringstream ls(raw);
        std::string line;
        while (std::getline(ls, line)) if (line.compare(0, 8, "#include") == 0) includes += line + "\n";
        const std::string name = e.path().filename().string();
        for (const char* b : banned) {
            CHECK(code.find(b) == std::string::npos, "%s uses \"%s\"", name.c_str(), b);
            CHECK(includes.find(b) == std::string::npos, "%s includes \"%s\"", name.c_str(), b);
        }
    }
    CHECK(files >= 12, "found the web sources (%d files in %s)", files, dir.string().c_str());
}

static void testDefaults() {
    WebPolicy p;
    for (const auto& d : decoderCatalogue()) {
        if (d.cls == DecoderClass::Data) CHECK(p.decoderAllowed(d.kind), "%s on", d.kind);
        else CHECK(!p.decoderAllowed(d.kind), "%s must be off by default", d.kind);
    }
    CHECK(!p.legalAccepted(), "the notice is not accepted by default");
    onair_server::ConfigFile cf;
    cf.parse("web=1\n");
    bool made;
    WebSettings s = loadWebSettings(cf, made);
    CHECK(s.server.bind == "127.0.0.1" && !s.logIp && !s.directory.on && !s.server.adminLan && !s.server.plainInternet, "safe defaults: loopback, no IP logging, no directory, no admin from the network");
    {   // the directory heartbeat is built in but refuses to start (and so sends nothing) without a URL, a key place and the host's confirmation
        std::unique_ptr<DirectoryHeartbeat> d(makeDirectoryHeartbeat());
        std::string err;
        CHECK(d && !d->start(s.directory, [] { return std::string("{}"); }, err) && !err.empty(), "directory refuses to start by default");
    }
}

// ---- 4. the web policy is an allow-list: only the approved modes are on; every mode of the registry is classified; a new mode is blocked
static std::set<std::string> registryModeIds() {
    namespace fs = std::filesystem;
    const fs::path root = ONAIR_SOURCE_DIR;
    std::set<std::string> ids = {"dvb", "t2", "t", "atsc", "atsc3", "dab", "isdbt", "fm"};   // the built-in ones (ServerHost::buildModes)
    // the sub-modes (core/src/engine/modes.cpp kSubModes): {"id", "Name", family, ...
    {
        const std::string s = readFile(root / "core/src/engine/modes.cpp");
        size_t p = 0;
        while ((p = s.find("\n    {\"", p)) != std::string::npos) {
            const size_t a = p + 7, b = s.find('"', a);
            if (b == std::string::npos) break;
            const std::string id = s.substr(a, b - a);
            const size_t c = s.find(", \"", b), nl = s.find('\n', b);
            if (c != std::string::npos && c < nl && !id.empty() && id.size() < 20) ids.insert(id);
            p = b;
        }
    }
    // every XxxTuning() definition: its id (a literal, or a ternary of two literals)
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / "core/src", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".cpp") continue;
        const std::string s = readFile(it->path());
        size_t p = 0;
        while ((p = s.find("ModeTuning ", p)) != std::string::npos) {
            size_t q = p + 11;
            while (q < s.size() && (std::isalnum((unsigned char)s[q]) || s[q] == '_')) q++;
            const bool def = s.compare(q, 3, "() ") == 0 && s.compare(q - 6, 6, "Tuning") == 0 && s.find('{', q) < s.find(';', q);
            p = q;
            if (!def) continue;
            const size_t fin = s.find("\n}\n", q);
            const std::string body = s.substr(q, std::min<size_t>(1500, fin == std::string::npos ? 1500 : fin - q));
            const size_t k = body.find(".id = ");
            if (k == std::string::npos) continue;
            const size_t end2 = body.find(';', k);
            const std::string expr = body.substr(k, end2 - k);
            size_t u = 0;
            while ((u = expr.find('"', u)) != std::string::npos) {
                const size_t v = expr.find('"', u + 1);
                if (v == std::string::npos) break;
                ids.insert(expr.substr(u + 1, v - u - 1));
                u = v + 1;
            }
        }
    }
    return ids;
}

static void testAllowList() {
    // the approved web-safe modes are data / on
    for (const char* k : {"fm", "am", "general", "airband", "specan", "specx", "adsb", "ais", "packet", "aprs", "acars", "vdl2", "ft8", "ft4", "wspr", "sstv", "wefax", "goes", "meteor", "hrpt", "elektro"})
        CHECK(decoderClass(k) == DecoderClass::Data, "%s is on the web-safe list", k);
    WebPolicy p;
    for (const char* k : {"adsb", "ais", "packet", "acars", "vdl2", "ft8", "wspr", "sstv", "wefax", "fm", "specan"}) CHECK(p.decoderAllowed(k), "%s on by default", k);
    // everything else is off by default, including the ones the old name list did not know
    for (const char* k : {"dmr", "p25", "nxdn", "tetra", "dpmr", "dstar", "fusion", "m17", "pocsag", "flex", "freedv", "hamdrm", "meshtastic", "meshcore", "meshcom", "lorawan", "loraaprs", "loraraw",
                          "cw", "rtty", "hfdl", "dsc", "navtex", "ale", "dab", "dvbs", "drm", "ble", "rid", "dji", "mac", "kaal", "iridium", "inmc", "aero", "a_mode_added_next_year", ""}) {
        CHECK(decoderClass(k) != DecoderClass::Data, "%s must not be web-safe", k);
        CHECK(!p.decoderAllowed(k), "%s off by default", k);
    }
    // turning one on needs the legal notice
    std::string err;
    CHECK(!p.setDecoder("meshtastic", true, "", err) && !p.decoderAllowed("meshtastic"), "a restricted mode needs the notice");
    CHECK(p.setDecoder("meshtastic", true, kLegalNoticeVersion, err) && p.decoderAllowed("meshtastic"), "and is then on");
    // a host mode id that is blocked keeps the host decoder's output away (the hub checks the kind and the mode)
    CHECK(!p.decoderAllowed("lorawan") && p.decoderAllowed("host"), "the host kind is on, its mode decides");
    // the data kinds can be turned off by the host
    CHECK(p.setDecoder("ais", false, "", err) && !p.decoderAllowed("ais"), "a data kind can be turned off");

    // every mode id of the registry is classified (a new mode must be added to web_policy.cpp: safe, voice, pager or restricted)
    const std::set<std::string> ids = registryModeIds();
    CHECK(ids.size() >= 100, "found the mode registry (%d ids)", (int)ids.size());
    for (const std::string& id : ids) CHECK(decoderKindClassified(id), "mode \"%s\" is not classified in server/web/web_policy.cpp", id.c_str());
    // and only the approved ones are on
    int on = 0;
    for (const std::string& id : ids) if (decoderClass(id) == DecoderClass::Data) on++;
    CHECK(on > 0 && on < 30, "only a few modes are web-safe (%d)", on);
    CHECK(decoderClass("dmr") == DecoderClass::Voice && decoderClass("pocsag") == DecoderClass::Pager, "voice and pager keep their names");
}

int main() {
    testAllowList();
    testProtocol();
    testSourceScan();
    testDefaults();
    return testSummary("web_safety");
}
