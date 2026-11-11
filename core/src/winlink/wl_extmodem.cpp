// External modem transport (ARDOP / VARA HF / VARA FM / Mercury over TCP). See wl_extmodem.h and docs/HFMODEMS-3.9.md.
// Written from the published command descriptions only; no code from Pat, ardopcf, VARA or Mercury is used.
#include "dect2/winlink/wl_extmodem.h"
#include "../net/tcp_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace winlink {

namespace {

std::string up(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

// only letters, digits, '-' and '/': nothing that could end a command line or add a second one
bool safeToken(const std::string& s, size_t maxLen = 24) {
    if (s.empty() || s.size() > maxLen) return false;
    for (char c : s) if (!isalnum((unsigned char)c) && c != '-' && c != '/') return false;
    return true;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string w;
    while (in >> w) v.push_back(w);
    return v;
}

std::string clean(std::string s) {
    for (auto& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

// wait for something to read on either socket (ms); returns bit 1 for a, bit 2 for b
int waitReadable(sock_t a, sock_t b, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    int mx = 0;
    if (a != kBadSock) { FD_SET(a, &rd); mx = std::max(mx, (int)a); }
    if (b != kBadSock) { FD_SET(b, &rd); mx = std::max(mx, (int)b); }
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int n = select(mx + 1, &rd, nullptr, nullptr, &tv);
    if (n <= 0) return 0;
    return (a != kBadSock && FD_ISSET(a, &rd) ? 1 : 0) | (b != kBadSock && FD_ISSET(b, &rd) ? 2 : 0);
}

} // namespace

const char* modemTypeName(ModemType t) {
    switch (t) {
    case ModemType::Ardop: return "ARDOP";
    case ModemType::VaraHF: return "VARA HF";
    case ModemType::VaraFM: return "VARA FM";
    case ModemType::Mercury: return "Mercury";
    }
    return "?";
}

ModemProfile defaultModemProfile(ModemType t) {
    ModemProfile p;
    p.type = t;
    p.name = modemTypeName(t);
    if (t == ModemType::Ardop) { p.cmdPort = 8515; p.dataPort = 8516; }
    else { p.cmdPort = 8300; p.dataPort = 8301; }
    return p;
}

std::vector<std::string> modemBwChoices(ModemType t) {
    switch (t) {
    case ModemType::Ardop: return {"200MAX", "500MAX", "1000MAX", "2000MAX", "200FORCE", "500FORCE", "1000FORCE", "2000FORCE"};
    case ModemType::VaraHF: case ModemType::Mercury: return {"500", "2300", "2750"};
    case ModemType::VaraFM: return {};
    }
    return {};
}

bool modemBwValid(ModemType t, const std::string& bw) {
    if (bw.empty()) return true;
    if (t == ModemType::VaraFM) return false;
    size_t i = 0;
    while (i < bw.size() && isdigit((unsigned char)bw[i])) i++;
    if (i < 3 || i > 4) return false;
    const std::string rest = up(bw.substr(i));
    if (t == ModemType::Ardop) return rest.empty() || rest == "MAX" || rest == "FORCE";
    return rest.empty();
}

std::string modemBwCommand(ModemType t, const std::string& bw) {
    if (bw.empty() || !modemBwValid(t, bw)) return std::string();
    if (t == ModemType::Ardop) return "ARQBW " + up(bw);
    return "BW" + bw;
}

ModemHelp modemHelp(ModemType t) {
    ModemHelp h;
    const std::string common =
        "OnAir does not include, start or bundle this modem: it is a separate program that OnAir connects to over TCP.\n"
        "The modem program owns the sound card and the radio's PTT. Set the audio device and PTT in the modem program, not in OnAir.\n"
        "Use Test connection: it opens the two ports and asks for the version; nothing is sent over the air.\n";
    switch (t) {
    case ModemType::Ardop:
        h.steps = common +
            "1. Install ardopcf (open source) and start it: ardopcf 8515 <capture device> <playback device> (see its USAGE files).\n"
            "2. In OnAir add an ARDOP profile: host 127.0.0.1, command port 8515, data port 8516.\n"
            "3. Choose the BW (ARQBW), for example 500MAX, or leave it empty for the modem's own setting.\n"
            "4. Add a gateway with link type External modem, pick the profile, enter the gateway's call sign.\n"
            "ardopcf listens on all network interfaces by default: keep the ports behind a firewall.";
        h.installUrl = "https://github.com/pflarue/ardop";
        h.licence = "ardopcf is open source (MIT licence). ARDOP's specification is public.";
        h.source = "ardopcf docs/Host_Interface_Commands.md; the ARDOP TNC interface specification; wl2k-go transport/ardop (MIT)";
        break;
    case ModemType::VaraHF:
        h.steps = common +
            "1. Install VARA HF from its author's site (EA5HVK) and start it. It is Windows software.\n"
            "2. Linux and macOS: people run it under Wine in its own prefix (see the Pat wiki page on VARA and the DF3FY guide, linked in docs/HFMODEMS-3.9.md). OnAir cannot check that setup.\n"
            "3. In OnAir add a VARA HF profile: host 127.0.0.1, command port 8300, data port 8301 (the defaults; check VARA's Settings > TCP ports).\n"
            "4. BW: 500, 2300 (the default) or 2750. Add a gateway with link type External modem and pick the profile.\n"
            "5. For a peer-to-peer session mark the gateway entry as another station: OnAir then sends P2P SESSION.\n"
            "VARA's free version is speed limited; a licence key (bought from the author) removes the limit. OnAir does not read or change it.";
        h.installUrl = "https://rosmodem.wordpress.com/";
        h.licence = "VARA is closed, proprietary, Windows-only software by EA5HVK. Its licence and any registration are between you and its author. OnAir does not contain or decode VARA.";
        h.source = "VARA Protocol Native TNC Commands (EA5HVK, 13 Feb 2022, as published with Pat-Vara, MIT); Pat-Vara (MIT)";
        break;
    case ModemType::VaraFM:
        h.steps = common +
            "1. Install VARA FM from its author's site and start it. It is Windows software.\n"
            "2. In OnAir add a VARA FM profile: host 127.0.0.1, command port 8300, data port 8301 (VARA FM and HF both default to these, so run one at a time or change one program's ports).\n"
            "3. VARA FM has no BW command. Digipeaters go in the gateway's Via field and are sent as: CONNECT <mycall> <call> via <digi1> <digi2>.\n"
            "4. Add a gateway with link type External modem and pick the profile.";
        h.installUrl = "https://rosmodem.wordpress.com/";
        h.licence = "VARA FM is closed, proprietary, Windows-only software by EA5HVK. OnAir does not contain or decode it.";
        h.source = "VARA Protocol Native TNC Commands (EA5HVK, 2022)";
        break;
    case ModemType::Mercury:
        h.steps = common +
            "1. Install Mercury (Rhizomatica, open source) and start it; it exposes a VARA-style TCP interface.\n"
            "2. In OnAir add a Mercury profile: host 127.0.0.1, command port 8300, data port 8301 (Mercury: -p <base_port>, data = base + 1).\n"
            "3. BW: 500, 2300 or 2750 (2750 behaves as 2300 today). Add a gateway with link type External modem and pick the profile.\n"
            "A Mercury call can last up to 300 s before it gives up: the connect timeout of the profile is 330 s unless you set one.";
        h.installUrl = "https://github.com/Rhizomatica/mercury";
        h.licence = "Mercury is open source (GPL-3.0-or-later; some bundled parts under LGPL, MIT, Unlicense).";
        h.source = "Mercury docs/TNC.md and README (Rhizomatica)";
        break;
    }
    return h;
}

std::string saveModemProfiles(const std::vector<ModemProfile>& p) {
    std::string o;
    for (const auto& x : p)
        o += clean(x.name) + "|" + std::to_string((int)x.type) + "|" + clean(x.host) + "|" + std::to_string(x.cmdPort) + "|" + std::to_string(x.dataPort) +
             "|" + clean(x.bw) + "|" + std::to_string(x.connectTimeoutSec) + "|" + clean(x.pttNote) + "|" + clean(x.audioNote) + "\n";
    return o;
}

std::vector<ModemProfile> loadModemProfiles(const std::string& text) {
    std::vector<ModemProfile> v;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> p;
        size_t i = 0;
        for (;;) { const size_t e = line.find('|', i); p.push_back(line.substr(i, e == std::string::npos ? std::string::npos : e - i)); if (e == std::string::npos) break; i = e + 1; }
        if (p.size() < 2 || p[0].empty()) continue;
        ModemProfile m;
        m.name = p[0];
        m.type = (ModemType)std::max(0, std::min(3, atoi(p[1].c_str())));
        const ModemProfile d = defaultModemProfile(m.type);
        m.cmdPort = d.cmdPort; m.dataPort = d.dataPort;
        if (p.size() > 2 && !p[2].empty()) m.host = p[2];
        if (p.size() > 3) m.cmdPort = std::max(1, std::min(65535, atoi(p[3].c_str())));
        if (p.size() > 4) m.dataPort = std::max(1, std::min(65535, atoi(p[4].c_str())));
        if (p.size() > 5) m.bw = p[5];
        if (p.size() > 6) m.connectTimeoutSec = std::max(0, std::min(3600, atoi(p[6].c_str())));
        if (p.size() > 7) m.pttNote = p[7];
        if (p.size() > 8) m.audioNote = p[8];
        v.push_back(m);
    }
    return v;
}

// ================================================================ test connection

ModemTestResult testModemProfile(const ModemProfile& p, int timeoutMs) {
    ModemTestResult r;
    netInit();
    const std::string who = std::string(modemTypeName(p.type)) + " at " + p.host + ":" + std::to_string(p.cmdPort);
    std::string err;
    sock_t cs = tcpConnect(p.host, p.cmdPort, timeoutMs, err);
    if (cs == kBadSock) {
        r.message = "cannot open the command port of " + who + " (" + err + "): is the modem program running, and are the host and port right?";
        return r;
    }
    r.cmdOpen = true;
    const std::string q = "VERSION\r";
    sockSendAll(cs, q.data(), q.size());
    std::string buf;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeoutMs)) {
        if (!waitReadable(cs, kBadSock, 100)) continue;
        char b[512];
        const long long k = sockRecv(cs, b, sizeof b);
        if (k <= 0) break;
        buf.append(b, (size_t)k);
        size_t pos;
        bool done = false;
        while ((pos = buf.find_first_of("\r\n")) != std::string::npos) {
            std::string l = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (l.empty()) continue;
            if (l.rfind("VERSION", 0) == 0) { r.answered = true; r.version = l.size() > 8 ? l.substr(8) : std::string(); done = true; break; }
            if (l == "WRONG") { r.answered = true; done = true; break; }
        }
        if (done) break;
    }
    sockClose(cs);
    sock_t ds = tcpConnect(p.host, p.dataPort, timeoutMs, err);
    if (ds != kBadSock) { r.dataOpen = true; sockClose(ds); }
    if (r.answered && !r.version.empty()) r.message = std::string(modemTypeName(p.type)) + " answered: " + r.version;
    else if (r.answered) r.message = std::string(modemTypeName(p.type)) + " answered, but it does not report a version";
    else r.message = "the command port is open but the modem did not answer VERSION within " + std::to_string(timeoutMs / 1000) + " s (some VARA versions do not)";
    if (!r.dataOpen) r.message += "; the data port " + std::to_string(p.dataPort) + " could not be opened";
    return r;
}

// ================================================================ the link

ExtModemLink::ExtModemLink(const ExtModemConfig& c) : c_(c) {
    if (c_.profile.connectTimeoutSec <= 0) c_.profile.connectTimeoutSec = c_.profile.type == ModemType::Mercury ? 330 : 120;
    c_.arqRepeats = std::max(2, std::min(15, c_.arqRepeats));
}

ExtModemLink::~ExtModemLink() { closeSockets(); }

void ExtModemLink::connect(double now) {
    now_ = now;
    wantListen_ = false;
    if (!safeToken(c_.mycall)) { setState(LinkState::Failed, "my call sign is empty or has characters a modem command cannot carry"); return; }
    if (!safeToken(c_.peer)) { setState(LinkState::Failed, "the station to call is empty or has characters a modem command cannot carry"); return; }
    for (const auto& v : c_.via) if (!safeToken(v)) { setState(LinkState::Failed, "a digipeater call sign is not valid"); return; }
    if (!c_.profile.bw.empty() && !modemBwValid(c_.profile.type, c_.profile.bw)) {
        setState(LinkState::Failed, std::string("the BW \"") + c_.profile.bw + "\" is not one " + modemTypeName(c_.profile.type) + " documents; edit the modem profile");
        return;
    }
    setState(LinkState::Connecting);
    start();
}

void ExtModemLink::listen() {
    wantListen_ = true;
    if (!safeToken(c_.mycall)) { setState(LinkState::Failed, "my call sign is empty or has characters a modem command cannot carry"); return; }
    if (!c_.profile.bw.empty() && !modemBwValid(c_.profile.type, c_.profile.bw)) {
        setState(LinkState::Failed, std::string("the BW \"") + c_.profile.bw + "\" is not one " + modemTypeName(c_.profile.type) + " documents; edit the modem profile");
        return;
    }
    setState(LinkState::Listening);
    start();
}

void ExtModemLink::start() {
    if (th_.joinable()) return;
    stop_ = false;
    started_ = true;
    heardAt_ = now_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        thState_ = 1;
        thErr_.clear();
    }
    th_ = std::thread([this] { run(); });
}

void ExtModemLink::run() {
    netInit();
    const std::string who = std::string(modemTypeName(c_.profile.type));
    auto open = [&](int port, const char* what, sock_t& out) -> bool {
        std::string err;
        const int tries = std::max(1, c_.profile.tcpAttempts);
        for (int a = 0; a < tries && !stop_; a++) {
            out = tcpConnect(c_.profile.host, port, 3000, err);
            if (out != kBadSock) return true;
            for (int i = 0; i < 10 && !stop_ && a + 1 < tries; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::lock_guard<std::mutex> lk(mu_);
        thErr_ = "cannot open the " + std::string(what) + " port of " + who + " at " + c_.profile.host + ":" + std::to_string(port) + " (" + err +
                 "): is the modem program running, and are the host and port right?";
        thState_ = 4;
        return false;
    };
    sock_t cs = kBadSock, ds = kBadSock;
    if (!open(c_.profile.cmdPort, "command", cs)) return;
    if (!open(c_.profile.dataPort, "data", ds)) { if (cs != kBadSock) sockClose(cs); return; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        cs_ = (long long)cs; ds_ = (long long)ds;
        thState_ = 2;
    }
    char buf[4096];
    bool lost = false;
    while (!stop_ && !lost) {
        const int r = waitReadable(cs, ds, 100);
        if (r & 1) {
            const long long k = sockRecv(cs, buf, sizeof buf);
            if (k <= 0) { lost = true; break; }
            std::lock_guard<std::mutex> lk(mu_);
            lineBuf_.append(buf, (size_t)k);
            size_t pos;
            while ((pos = lineBuf_.find_first_of("\r\n")) != std::string::npos) {
                std::string l = lineBuf_.substr(0, pos);
                lineBuf_.erase(0, pos + 1);
                if (!l.empty()) lines_.push_back(l);
            }
            if (lineBuf_.size() > 65536) lineBuf_.clear();
        }
        if (r & 2) {
            const long long k = sockRecv(ds, buf, sizeof buf);
            if (k <= 0) { lost = true; break; }
            std::lock_guard<std::mutex> lk(mu_);
            if (!arqStyle()) data_.insert(data_.end(), buf, buf + k);
            else {
                // ARDOP: 2-byte length (counts the tag and the bytes), a 3-letter tag, the bytes. Only "ARQ" is session data.
                frameBuf_.insert(frameBuf_.end(), buf, buf + k);
                for (;;) {
                    if (frameBuf_.size() < 2) break;
                    const size_t len = ((size_t)frameBuf_[0] << 8) | frameBuf_[1];
                    if (frameBuf_.size() < 2 + len) break;
                    if (len >= 3) {
                        const std::string tag((const char*)&frameBuf_[2], 3);
                        if (tag == "ARQ") data_.insert(data_.end(), frameBuf_.begin() + 5, frameBuf_.begin() + 2 + (long)len);
                        else if (tag == "ERR" || tag == "IDF") lines_.push_back("INFO " + tag + " frame from the modem");
                    }
                    frameBuf_.erase(frameBuf_.begin(), frameBuf_.begin() + 2 + (long)len);
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (thState_ != 4) thState_ = 3;
}

void ExtModemLink::closeSockets() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cs_ >= 0) sockShutdown((sock_t)cs_);
        if (ds_ >= 0) sockShutdown((sock_t)ds_);
    }
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    if (cs_ >= 0) { sockClose((sock_t)cs_); cs_ = -1; }
    if (ds_ >= 0) { sockClose((sock_t)ds_); ds_ = -1; }
}

void ExtModemLink::sendCmd(const std::string& s, bool critical) {
    long long cs;
    { std::lock_guard<std::mutex> lk(mu_); cs = cs_; }
    if (cs < 0) return;
    const std::string line = s + "\r";
    if (log) log("modem <- " + s);
    if (!sockSendAll((sock_t)cs, line.data(), line.size())) { failNow("the connection to the modem's command port broke"); return; }
    if (!arqStyle()) sentCmds_.push_back(critical ? "!" + s : s);
}

void ExtModemLink::sendInit() {
    initDone_ = true;
    const ModemType t = c_.profile.type;
    const std::string bwCmd = modemBwCommand(t, c_.profile.bw);
    if (t == ModemType::Ardop) {
        sendCmd("INITIALIZE");
        sendCmd("MYCALL " + c_.mycall);
        sendCmd("PROTOCOLMODE ARQ");
        if (!bwCmd.empty()) sendCmd(bwCmd);
        sendCmd(wantListen_ ? "LISTEN TRUE" : "LISTEN FALSE");
        if (!wantListen_) {
            sendCmd("ARQCALL " + c_.peer + " " + std::to_string(c_.arqRepeats));
            callAt_ = now_;
        }
    } else {
        sendCmd("MYCALL " + c_.mycall, true);
        if (t == ModemType::VaraHF) {
            sendCmd(c_.p2p ? "P2P SESSION" : "WINLINK SESSION");
            sendCmd("CHAT OFF");
        }
        if (t != ModemType::Mercury) sendCmd("COMPRESSION TEXT");
        if (!bwCmd.empty()) sendCmd(bwCmd);
        if (wantListen_) sendCmd("LISTEN ON", true);
        else {
            sendCmd("LISTEN OFF");
            std::string call = "CONNECT " + c_.mycall + " " + c_.peer;
            if (t == ModemType::VaraFM && !c_.via.empty()) {
                call += " via";
                for (const auto& v : c_.via) call += " " + v;
            }
            sendCmd(call, true);
            callAt_ = now_;
        }
    }
}

void ExtModemLink::failNow(const std::string& why) {
    if (st_ == LinkState::Failed || st_ == LinkState::Closed) return;
    setState(LinkState::Failed, why);
}

void ExtModemLink::handleLine(const std::string& l, double now) {
    heardAt_ = now;
    if (log) log("modem -> " + l);
    const std::vector<std::string> w = words(l);
    if (w.empty()) return;
    const std::string k = up(w[0]);
    const ModemType t = c_.profile.type;
    auto rest = [&](size_t from) { std::string o; for (size_t i = from; i < w.size(); i++) o += (o.empty() ? "" : " ") + w[i]; return o; };

    if (k == "OK" || k == "WRONG") {
        // VARA and Mercury answer each command in order; ARDOP answers with its own text
        if (!sentCmds_.empty()) {
            const std::string c = sentCmds_.front();
            sentCmds_.erase(sentCmds_.begin());
            if (k == "WRONG") {
                const bool critical = !c.empty() && c[0] == '!';
                const std::string name = critical ? c.substr(1) : c;
                if (critical && (st_ == LinkState::Connecting || st_ == LinkState::Listening)) failNow("the modem rejected \"" + name + "\" (WRONG)");
                else if (log) log("the modem did not accept \"" + name + "\" (WRONG); continuing");
            }
        }
        return;
    }
    if (k == "CONNECTED") {
        pending_ = false;
        if (t == ModemType::Ardop) {
            // CONNECTED <call> <bw>
            if (w.size() >= 2) remote_ = w[1];
            if (w.size() >= 3) bw_ = w[2];
        } else {
            // CONNECTED <source> <destination> [via <digis>] [<bw>]
            if (w.size() >= 3) {
                const bool mine = up(w[1]) == up(c_.mycall);
                remote_ = mine ? w[2] : w[1];
            }
            if (w.size() >= 4) {
                const std::string last = w.back();
                if (!last.empty() && isdigit((unsigned char)last[0])) bw_ = last;
            }
        }
        if (st_ == LinkState::Connecting || st_ == LinkState::Listening) {
            sawLinkUp_ = true;
            setState(LinkState::Connected);
            if (!rxHold_.empty()) { std::vector<uint8_t> h; h.swap(rxHold_); if (onData) onData(h.data(), h.size()); }
            flushTx();
        }
        return;
    }
    if (k == "DISCONNECTED") {
        pending_ = false;
        buffer_ = 0;
        if (st_ == LinkState::Connecting) failNow("the call to " + c_.peer + " did not connect");
        else if (st_ == LinkState::Connected || st_ == LinkState::Disconnecting) setState(LinkState::Closed);
        return;
    }
    if (k == "BUFFER") { if (w.size() >= 2) buffer_ = std::max(0, atoi(w[1].c_str())); return; }
    if (k == "BUSY") {
        if (w.size() >= 2) { const std::string v = up(w[1]); busy_ = v == "ON" || v == "TRUE"; }
        return;
    }
    if (k == "PTT") {
        if (w.size() >= 2) { const std::string v = up(w[1]); ptt_ = v == "ON" || v == "TRUE"; }
        return;
    }
    if (k == "PENDING") { pending_ = true; return; }
    if (k == "CANCELPENDING") { pending_ = false; return; }
    if (k == "SN") {
        if (w.size() >= 2) { snr_ = atof(w[1].c_str()); snrKnown_ = true; }
        return;
    }
    if (k == "BITRATE") { bitrate_ = rest(1); return; }
    if (k == "VERSION") { version_ = rest(1); return; }
    if (k == "IAMALIVE") return;
    if (k == "MISSING") {
        failNow("the modem reports a missing sound card (" + l + "): restart the modem program");
        return;
    }
    if (k == "REJECTEDBW" || k == "REJECTEDBUSY") {
        if (st_ == LinkState::Connecting) failNow(k == "REJECTEDBW" ? "the station rejected the bandwidth (" + l + ")" : "the station was busy (" + l + ")");
        return;
    }
    if (k == "FAULT") {
        if (log) log("modem fault: " + rest(1));
        if (st_ == LinkState::Connecting && !sawLinkUp_ && !initDone_) failNow("the modem reported a fault: " + rest(1));
        return;
    }
    if (k == "STATUS") {
        // ardopcf: "STATUS CONNECT TO <call> FAILED!" ends a call that got no answer
        const std::string r = up(rest(1));
        if (st_ == LinkState::Connecting && r.find("FAILED") != std::string::npos) failNow(rest(1));
        return;
    }
    // NEWSTATE, TARGET, REGISTERED, LINK ..., ENCRYPT..., PING..., INFO ...: logged above, nothing to do
}

void ExtModemLink::deliverData(const uint8_t* d, size_t n) {
    if (st_ == LinkState::Connected) { if (onData) onData(d, n); }
    else if (st_ == LinkState::Connecting || st_ == LinkState::Listening) rxHold_.insert(rxHold_.end(), d, d + n);   // data ahead of CONNECTED
    else if (st_ == LinkState::Disconnecting) { if (onData) onData(d, n); }
}

void ExtModemLink::flushTx() {
    if (txq_.empty() || (st_ != LinkState::Connected)) return;
    long long ds;
    { std::lock_guard<std::mutex> lk(mu_); ds = ds_; }
    if (ds < 0) return;
    std::vector<uint8_t> q;
    q.swap(txq_);
    size_t off = 0;
    while (off < q.size()) {
        const size_t n = std::min<size_t>(q.size() - off, 1024);
        bool ok;
        if (arqStyle()) {
            std::vector<uint8_t> f(2 + n);
            f[0] = (uint8_t)(n >> 8); f[1] = (uint8_t)(n & 0xFF);
            std::copy(q.begin() + (long)off, q.begin() + (long)(off + n), f.begin() + 2);
            ok = sockSendAll((sock_t)ds, f.data(), f.size());
        } else ok = sockSendAll((sock_t)ds, q.data() + off, n);
        if (!ok) { failNow("the connection to the modem's data port broke"); return; }
        off += n;
    }
}

void ExtModemLink::write(const uint8_t* d, size_t n) {
    txq_.insert(txq_.end(), d, d + n);
    flushTx();
}

size_t ExtModemLink::pendingTx() const { return txq_.size() + (size_t)std::max(0, buffer_); }

void ExtModemLink::disconnect(double now) {
    now_ = now;
    if (st_ == LinkState::Closed || st_ == LinkState::Failed) return;
    if (st_ == LinkState::Listening || (st_ == LinkState::Connecting && !initDone_)) { abort(); return; }
    discSent_ = true;
    discAt_ = now;
    if (st_ == LinkState::Connecting) sendCmd(arqStyle() ? "ABORT" : "DISCONNECT");
    else sendCmd("DISCONNECT");
    if (st_ == LinkState::Connected || st_ == LinkState::Connecting) setState(LinkState::Disconnecting);
}

void ExtModemLink::abort() {
    if (st_ == LinkState::Closed || st_ == LinkState::Failed) { closeSockets(); return; }
    long long cs;
    { std::lock_guard<std::mutex> lk(mu_); cs = cs_; }
    if (cs >= 0 && (initDone_ || sawLinkUp_)) {
        sendCmd("ABORT");
        sendCmd(arqStyle() ? "LISTEN FALSE" : "LISTEN OFF");
    }
    closeSockets();
    setState(LinkState::Closed);
}

void ExtModemLink::tick(double now) {
    now_ = now;
    if (!started_ || st_ == LinkState::Closed || st_ == LinkState::Failed) return;
    std::vector<std::string> ls;
    std::vector<uint8_t> dt;
    int ts;
    std::string terr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ls.swap(lines_);
        dt.swap(data_);
        ts = thState_; terr = thErr_;
    }
    if (ts == 4) { failNow(terr.empty() ? std::string("cannot reach the modem") : terr); closeSockets(); return; }
    if (ts == 2 && !initDone_) { heardAt_ = now; sendInit(); if (st_ == LinkState::Failed) { closeSockets(); return; } }
    for (const std::string& l : ls) {
        handleLine(l, now);
        if (st_ == LinkState::Failed || st_ == LinkState::Closed) break;
    }
    if (!dt.empty() && st_ != LinkState::Failed && st_ != LinkState::Closed) deliverData(dt.data(), dt.size());
    if (st_ == LinkState::Failed) { closeSockets(); return; }
    if (st_ == LinkState::Closed) {
        // tell the modem to stop answering calls, then let go of it
        sendCmd(arqStyle() ? "LISTEN FALSE" : "LISTEN OFF");
        closeSockets();
        return;
    }
    if (ts == 3) {
        if (st_ == LinkState::Disconnecting) setState(LinkState::Closed);
        else failNow("the modem closed the connection (was the modem program stopped?)");
        closeSockets();
        return;
    }
    // timers
    if (st_ == LinkState::Connecting && callAt_ >= 0 && now - callAt_ > c_.profile.connectTimeoutSec) {
        sendCmd(arqStyle() ? "ABORT" : "DISCONNECT");
        failNow("no answer from " + c_.peer + " within " + std::to_string(c_.profile.connectTimeoutSec) + " s");
        closeSockets();
        return;
    }
    if (st_ == LinkState::Disconnecting && discAt_ >= 0 && now - discAt_ > (sawLinkUp_ ? c_.disconnectWaitSec : std::min(c_.disconnectWaitSec, 10.0))) {
        sendCmd("ABORT");
        setState(LinkState::Closed);
        return;
    }
    if (c_.profile.type == ModemType::VaraHF || c_.profile.type == ModemType::VaraFM) {
        if (initDone_ && c_.idleWatchdogSec > 0 && now - heardAt_ > c_.idleWatchdogSec) {
            failNow("the modem has been silent for " + std::to_string((int)c_.idleWatchdogSec) + " s (VARA sends IAMALIVE every 60 s): is it still running?");
            closeSockets();
        }
    }
}

std::string ExtModemLink::detail() const {
    std::string s = modemTypeName(c_.profile.type);
    if (!bw_.empty()) s += " " + bw_;
    if (!remote_.empty() && (st_ == LinkState::Connected || st_ == LinkState::Disconnecting)) s += " with " + remote_;
    if (st_ == LinkState::Connected || st_ == LinkState::Disconnecting) s += ", buffer " + std::to_string(buffer_) + " B";
    if (snrKnown_) { char b[32]; snprintf(b, sizeof b, ", S/N %.1f dB", snr_); s += b; }
    if (busy_) s += ", channel busy";
    if (ptt_) s += ", transmitting";
    if (pending_) s += ", call heard";
    return s;
}

} // namespace winlink
} // namespace dect2
