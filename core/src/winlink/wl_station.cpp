// Winlink station: mailbox, gateway list, Telnet link, and the session runner. See wl_station.h.
#include "dect2/winlink/wl_station.h"
#include "../net/tcp_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace dect2 {
namespace winlink {

// ================================================================ mailbox

const char* folderName(Folder f) {
    switch (f) {
    case Folder::Inbox: return "inbox";
    case Folder::Outbox: return "outbox";
    case Folder::Sent: return "sent";
    case Folder::Archive: return "archive";
    }
    return "inbox";
}

namespace {
bool safeMid(const std::string& mid) {
    if (mid.empty() || mid.size() > 32) return false;
    for (char c : mid) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
}
bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
} // namespace

Mailbox::Mailbox(std::string dir) : dir_(std::move(dir)) {}
void Mailbox::setDir(const std::string& d) { dir_ = d; }

std::string Mailbox::path(Folder f, const std::string& mid) const {
    return (fs::path(dir_) / folderName(f) / (mid + ".b2f")).string();
}

bool Mailbox::save(Folder f, const B2Message& m, std::string* err) {
    const std::string mid = m.mid();
    if (dir_.empty() || !safeMid(mid)) { if (err) *err = "no mailbox folder or a bad message ID"; return false; }
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / folderName(f), ec);
    const std::vector<uint8_t> b = encodeMessage(m);
    const std::string p = path(f, mid), tmp = p + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { if (err) *err = "cannot write " + tmp; return false; }
        o.write((const char*)b.data(), (std::streamsize)b.size());
        if (!o) { if (err) *err = "cannot write " + tmp; return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    return true;
}

bool Mailbox::load(Folder f, const std::string& mid, B2Message& m) const {
    if (!safeMid(mid)) return false;
    std::vector<uint8_t> b;
    if (!readFile(path(f, mid), b)) return false;
    return parseMessage(b.data(), b.size(), m);
}

std::vector<MailEntry> Mailbox::list(Folder f) const {
    std::vector<std::pair<std::string, MailEntry>> v;
    std::error_code ec;
    const fs::path d = fs::path(dir_) / folderName(f);
    if (dir_.empty() || !fs::is_directory(d, ec)) return {};
    for (const auto& e : fs::directory_iterator(d, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".b2f") continue;
        std::vector<uint8_t> b;
        B2Message m;
        if (!readFile(e.path(), b) || !parseMessage(b.data(), b.size(), m)) continue;
        MailEntry me;
        me.mid = m.mid(); me.subject = m.subject(); me.from = m.from(); me.to = m.to(); me.date = m.date();
        me.bytes = b.size(); me.files = (int)m.files.size();
        v.emplace_back(me.date + me.mid, me);
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<MailEntry> o;
    for (auto& x : v) o.push_back(std::move(x.second));
    return o;
}

bool Mailbox::move(const std::string& mid, Folder from, Folder to) {
    if (!safeMid(mid)) return false;
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / folderName(to), ec);
    fs::rename(path(from, mid), path(to, mid), ec);
    return !ec;
}

bool Mailbox::remove(Folder f, const std::string& mid) {
    if (!safeMid(mid)) return false;
    std::error_code ec;
    return fs::remove(path(f, mid), ec);
}

bool Mailbox::savePartial(const B2Proposal& p, const std::vector<uint8_t>& held) {
    if (dir_.empty() || !safeMid(p.mid) || held.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / "partial", ec);
    const fs::path f = fs::path(dir_) / "partial" / (p.mid + ".part");
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << "OnAirPartial1 " << p.code << ' ' << p.size << ' ' << p.csize << '\n';
    o.write((const char*)held.data(), (std::streamsize)held.size());
    return (bool)o;
}

std::vector<uint8_t> Mailbox::loadPartial(const B2Proposal& p) {
    std::vector<uint8_t> b;
    if (dir_.empty() || !safeMid(p.mid)) return b;
    const fs::path f = fs::path(dir_) / "partial" / (p.mid + ".part");
    if (!readFile(f, b)) return {};
    const auto nl = std::find(b.begin(), b.end(), (uint8_t)'\n');
    char code = 0;
    int size = -1, csize = -1;
    const std::string head(b.begin(), nl);
    if (nl == b.end() || std::sscanf(head.c_str(), "OnAirPartial1 %c %d %d", &code, &size, &csize) != 3 || code != p.code || size != p.size ||
        csize != p.csize || (int)(b.end() - nl - 1) >= csize) {
        removePartial(p.mid);   // another message with this ID, or a stale file
        return {};
    }
    return std::vector<uint8_t>(nl + 1, b.end());
}

void Mailbox::removePartial(const std::string& mid) {
    if (dir_.empty() || !safeMid(mid)) return;
    std::error_code ec;
    fs::remove(fs::path(dir_) / "partial" / (mid + ".part"), ec);
}

std::vector<std::string> Mailbox::partials() const {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(dir_) / "partial", ec))
        if (e.path().extension() == ".part") v.push_back(e.path().stem().string());
    std::sort(v.begin(), v.end());
    return v;
}

namespace {
std::string baseCall(std::string s) {
    const size_t at = s.find('@');
    if (at != std::string::npos) {
        std::string dom = s.substr(at + 1);
        for (auto& c : dom) c = (char)tolower((unsigned char)c);
        if (dom != "winlink.org") return std::string();   // an internet address: not a station
        s.resize(at);
    }
    const size_t dash = s.find('-');
    if (dash != std::string::npos) s.resize(dash);
    std::string o;
    for (char c : s) if (!isspace((unsigned char)c)) o += (char)toupper((unsigned char)c);
    return o;
}
} // namespace

bool addressedTo(const B2Message& m, const std::string& call) {
    const std::string want = baseCall(call);
    if (want.empty()) return false;
    for (const char* k : {"To", "Cc"})
        for (const std::string& v : m.getAll(k)) if (baseCall(v) == want) return true;
    return false;
}

bool Mailbox::has(const std::string& mid) const {
    if (!safeMid(mid)) return false;
    std::error_code ec;
    for (Folder f : {Folder::Inbox, Folder::Outbox, Folder::Sent, Folder::Archive})
        if (fs::exists(path(f, mid), ec)) return true;
    return false;
}

// ================================================================ gateways

const char* transportName(Transport t) {
    switch (t) {
    case Transport::Packet1200: return "Packet 1200";
    case Transport::Packet9600: return "Packet 9600";
    case Transport::Ardop: return "ARDOP";
    case Transport::Telnet: return "Telnet";
    case Transport::Modem: return "External modem";
    }
    return "?";
}

namespace {
std::string clean(std::string s) {
    for (auto& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}
}

std::string saveGateways(const std::vector<Gateway>& g) {
    std::string o;
    char b[32];
    for (const auto& x : g) {
        snprintf(b, sizeof b, "%.6f", x.mhz);
        o += clean(x.call) + "|" + std::to_string((int)x.transport) + "|" + b + "|" + clean(x.via) + "|" + clean(x.host) + "|" + std::to_string(x.port) + "|" + clean(x.note) + "|" + (x.peer ? "1" : "") + "|" + clean(x.modem) + "\n";
    }
    return o;
}

std::vector<Gateway> loadGateways(const std::string& text) {
    std::vector<Gateway> v;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> p;
        size_t i = 0;
        for (;;) { const size_t e = line.find('|', i); p.push_back(line.substr(i, e == std::string::npos ? std::string::npos : e - i)); if (e == std::string::npos) break; i = e + 1; }
        if (p.size() < 2 || p[0].empty()) continue;
        Gateway g;
        g.call = p[0];
        const int t = atoi(p[1].c_str());
        g.transport = (Transport)std::max(0, std::min(4, t));
        if (p.size() > 2) g.mhz = atof(p[2].c_str());
        if (p.size() > 3) g.via = p[3];
        if (p.size() > 4) g.host = p[4];
        if (p.size() > 5) g.port = std::max(1, std::min(65535, atoi(p[5].c_str())));
        if (p.size() > 6) g.note = p[6];
        if (p.size() > 7) g.peer = p[7] == "1";
        if (p.size() > 8) g.modem = p[8];
        v.push_back(g);
    }
    return v;
}

// ================================================================ Telnet

TelnetLink::TelnetLink(std::string host, int port, std::string mycall, std::string pw)
    : host_(std::move(host)), mycall_(std::move(mycall)), loginPw_(std::move(pw)), port_(port) {}

TelnetLink::~TelnetLink() { abort(); }

void TelnetLink::connect(double) {
    if (th_.joinable()) return;
    if (host_.empty()) { setState(LinkState::Failed, "no Telnet host"); return; }
    setState(LinkState::Connecting);
    stop_ = false;
    th_ = std::thread([this] { run(); });
}

void TelnetLink::run() {
    std::string err;
    sock_t s = tcpConnect(host_, port_, 15000, err);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (s == kBadSock) { thErr_ = err; thState_ = 4; return; }
        sock_ = (long long)s;
        thState_ = 1;
    }
    sockRecvTimeout(s, 200);
    char buf[2048];
    while (!stop_) {
        const long long k = sockRecv(s, buf, sizeof buf);
        if (k > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            rx_.insert(rx_.end(), buf, buf + k);
            continue;
        }
        if (k == 0) break;
#ifdef _WIN32
        const int e = WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
        break;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (thState_ != 4) thState_ = 3;
}

void TelnetLink::write(const uint8_t* d, size_t n) {
    long long s;
    { std::lock_guard<std::mutex> lk(mu_); s = sock_; }
    if (s < 0 || st_ != LinkState::Connected) return;
    if (!sockSendAll((sock_t)s, d, n)) setState(LinkState::Failed, "the Telnet connection broke");
}

void TelnetLink::disconnect(double) { abort(); }

void TelnetLink::abort() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (sock_ >= 0) sockShutdown((sock_t)sock_);
    }
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    if (sock_ >= 0) { sockClose((sock_t)sock_); sock_ = -1; }
    if (st_ != LinkState::Failed && st_ != LinkState::Closed) setState(LinkState::Closed);
}

void TelnetLink::tick(double) {
    std::vector<uint8_t> got;
    int ts;
    std::string terr;
    long long s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        got.swap(rx_);
        ts = thState_; terr = thErr_; s = sock_;
    }
    if (ts == 4 && st_ != LinkState::Failed) { setState(LinkState::Failed, terr.empty() ? std::string("cannot connect") : terr); return; }
    if (!got.empty()) {
        if (st_ == LinkState::Connecting) {
            // the CMS login: "Callsign :" then "Password :"; everything after it belongs to B2F
            loginBuf_.append(got.begin(), got.end());
            got.clear();
            auto lower = loginBuf_;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            size_t pc = lower.find("callsign");
            size_t pp = lower.find("password");
            if (pc != std::string::npos && lower.find(':', pc) != std::string::npos && loginBuf_.find('\x01') == std::string::npos && mycall_.size()) {
                const std::string l = mycall_ + "\r";
                sockSendAll((sock_t)s, l.data(), l.size());
                loginBuf_.erase(0, lower.find(':', pc) + 1);
                lower.erase(0, lower.find(':', pc) + 1);
                pp = lower.find("password");
                mycall_.clear();   // sent once
            }
            if (pp != std::string::npos && lower.find(':', pp) != std::string::npos) {
                const std::string l = loginPw_ + "\r";
                sockSendAll((sock_t)s, l.data(), l.size());
                const size_t cut = lower.find(':', pp) + 1;
                std::vector<uint8_t> rest(loginBuf_.begin() + (long)cut, loginBuf_.end());
                loginBuf_.clear();
                setState(LinkState::Connected);
                while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\r' || rest[0] == '\n')) rest.erase(rest.begin());
                if (!rest.empty() && onData) onData(rest.data(), rest.size());
            }
        } else if (onData) onData(got.data(), got.size());
    }
    if (ts == 3 && st_ != LinkState::Closed && st_ != LinkState::Failed) setState(LinkState::Closed);
}

// ================================================================ station

WinlinkStation::WinlinkStation(Mailbox& mb) : mb_(mb) {}
WinlinkStation::~WinlinkStation() { if (link_) link_->abort(); }

bool WinlinkStation::running() const { return link_ != nullptr; }

bool WinlinkStation::makeLink(const StationConfig& c, std::string& err) {
    ax_ = nullptr; ar_ = nullptr;
    const Transport t = c.gateway.transport;
    if (t == Transport::Packet1200 || t == Transport::Packet9600) {
        Ax25LinkConfig lc;
        lc.mycall = c.mycall; lc.peer = c.gateway.call;
        std::string v = c.gateway.via;
        for (size_t i = 0; i < v.size();) {
            size_t e = v.find(',', i);
            if (e == std::string::npos) e = v.size();
            std::string one = v.substr(i, e - i);
            while (!one.empty() && one.front() == ' ') one.erase(0, 1);
            while (!one.empty() && one.back() == ' ') one.pop_back();
            if (!one.empty()) lc.via.push_back(one);
            i = e + 1;
        }
        lc.paclen = c.paclen; lc.window = c.window;
        if (c.t3Sec > 0) lc.t3Sec = c.t3Sec;
        lc.t1Sec = c.t1Sec > 0 ? c.t1Sec : (t == Transport::Packet9600 ? 3.0 : 6.0) + 2.0 * (double)lc.via.size();
        auto l = std::make_unique<Ax25Link>(lc);
        l->sendFrame = [this](const ax25::Frame& f) { outAx_.push_back(f); };
        ax_ = l.get();
        link_ = std::move(l);
    } else if (t == Transport::Ardop) {
        ArdopLinkConfig lc;
        lc.mycall = c.mycall; lc.peer = c.gateway.call;
        auto l = std::make_unique<ArdopLink>(lc);
        l->sendFrame = [this](const ArdopLinkFrame& f) { outAr_.push_back(f); };
        ar_ = l.get();
        link_ = std::move(l);
    } else if (t == Transport::Modem) {
        ExtModemConfig mc;
        mc.profile = c.modem;
        mc.mycall = c.mycall; mc.peer = c.gateway.call;
        mc.p2p = c.p2p;
        const std::string v = c.gateway.via;
        for (size_t i = 0; i < v.size();) {
            size_t e = v.find(',', i);
            if (e == std::string::npos) e = v.size();
            std::string one = v.substr(i, e - i);
            while (!one.empty() && one.front() == ' ') one.erase(0, 1);
            while (!one.empty() && one.back() == ' ') one.pop_back();
            if (!one.empty()) mc.via.push_back(one);
            i = e + 1;
        }
        link_ = std::make_unique<ExtModemLink>(mc);
    } else {
        if (c.p2p) { err = "peer-to-peer sessions run over packet, ARDOP or an external modem, not Telnet"; return false; }
        link_ = std::make_unique<TelnetLink>(c.gateway.host, c.gateway.port, c.mycall);
    }
    return true;
}

B2fSession::Hooks WinlinkStation::hooks() {
    B2fSession::Hooks h;
    h.send = [this](const uint8_t* d, size_t n) { if (link_) link_->write(d, n); };
    h.outbound = [this]() {
        std::vector<B2Proposal> v;
        const char code = session_ && session_->gzipAgreed() ? 'D' : 'C';
        std::string peer = peer_;
        if (c_.p2p && peer.empty() && link_) peer = link_->remoteCall();
        for (const MailEntry& e : mb_.list(Folder::Outbox)) {
            B2Message m;
            if (!mb_.load(Folder::Outbox, e.mid, m)) continue;
            if (c_.p2p && !addressedTo(m, peer)) continue;   // peer to peer: only what is for that station
            v.push_back(makeProposal(m, code));
            if (v.size() >= 5) break;
        }
        std::reverse(v.begin(), v.end());   // oldest first
        return v;
    };
    h.inboundAnswer = [this](const B2Proposal& p) { return mb_.has(p.mid) ? '-' : '+'; };
    h.received = [this](const B2Message& m, const B2Proposal& p) {
        std::string e;
        mb_.removePartial(p.mid);
        if (!mb_.save(Folder::Inbox, m, &e)) log_.push_back("could not store " + m.mid() + ": " + e);
        else log_.push_back("received " + m.mid() + " \"" + m.subject() + "\" from " + m.from() + (p.offset > 0 ? " (resumed from byte " + std::to_string(p.offset) + ")" : ""));
    };
    h.sent = [this](const std::string& mid, bool rejected) {
        mb_.move(mid, Folder::Outbox, Folder::Sent);
        log_.push_back(rejected ? (c_.p2p ? "the other station already had " : "the gateway already had ") + mid : "sent " + mid);
    };
    if (c_.resume) {
        h.resumeData = [this](const B2Proposal& p) { return mb_.loadPartial(p); };
        h.partial = [this](const B2Proposal& p, const std::vector<uint8_t>& held) {
            if (held.empty()) mb_.removePartial(p.mid);
            else if (mb_.savePartial(p, held)) log_.push_back("kept " + std::to_string(held.size()) + " of " + std::to_string(p.csize) + " bytes of " + p.mid + ": the rest comes at the next connection");
        };
    }
    h.log = [this](const std::string& l) { log_.push_back(l); };
    return h;
}

bool WinlinkStation::start(const StationConfig& c, double now, std::string& err) {
    if (link_) { err = "a Winlink session is already running"; return false; }
    if (c.mycall.empty()) { err = "enter your call sign first"; return false; }
    if (c.gateway.call.empty() && (c.gateway.transport != Transport::Telnet || c.p2p)) { err = c.p2p ? "enter the other station's call sign" : "pick a gateway (its call sign)"; return false; }
    if (c.gateway.transport == Transport::Telnet && !c.telnetEnabled && !c.p2p) { err = "Telnet to the CMS is off: turn it on in the Winlink settings"; return false; }
    if (c.gateway.transport == Transport::Telnet && c.gateway.host.empty() && !c.p2p) { err = "enter the Telnet host"; return false; }
    if (c.gateway.transport == Transport::Modem && c.modem.host.empty()) { err = "pick a modem profile (Winlink > Modems)"; return false; }
    c_ = c;
    stopping_ = false; sessionEnded_ = false;
    lastOk_ = false; lastErr_.clear();
    startedAt_ = now;
    outAx_.clear(); outAr_.clear();
    peer_ = c.gateway.call;
    if (!makeLink(c, err)) return false;
    B2fSession::Config sc;
    sc.role = B2fSession::Role::Client;
    sc.mycall = c.mycall; sc.targetCall = c.gateway.call.empty() ? std::string("CMS") : c.gateway.call;
    sc.locator = c.locator; sc.password = c.p2p ? std::string() : c.password;   // a peer gets no Winlink password
    sc.gzip = c.gzip;
    session_ = std::make_unique<B2fSession>(sc, hooks());
    link_->log = [this](const std::string& l) { log_.push_back(l); };
    link_->onData = [this](const uint8_t* d, size_t n) { if (session_) session_->onBytes(d, n); };
    link_->onState = [this](LinkState s) {
        if (s == LinkState::Connected && session_ && session_->state() == B2fSession::State::Idle) session_->start();
    };
    log_.push_back(std::string("calling ") + sc.targetCall + " (" + transportName(c.gateway.transport) + (c.p2p ? ", peer to peer" : "") + ")");
    link_->connect(now);
    return true;
}

bool WinlinkStation::listen(const StationConfig& cfg, double now, std::string& err) {
    if (link_) { err = "a Winlink session is already running"; return false; }
    if (cfg.mycall.empty()) { err = "enter your call sign first"; return false; }
    StationConfig c = cfg;
    c.p2p = true;
    if (c.gateway.transport == Transport::Modem && c.modem.host.empty()) { err = "pick a modem profile (Winlink > Modems)"; return false; }
    c.gateway.call.clear();
    c_ = c;
    stopping_ = false; sessionEnded_ = false;
    lastOk_ = false; lastErr_.clear();
    startedAt_ = now;
    outAx_.clear(); outAr_.clear();
    peer_.clear();
    if (!makeLink(c, err)) return false;
    B2fSession::Config sc;
    sc.role = B2fSession::Role::Gateway;
    sc.p2p = true;
    sc.mycall = c.mycall;
    sc.locator = c.locator;
    sc.gzip = c.gzip;
    session_ = std::make_unique<B2fSession>(sc, hooks());
    link_->log = [this](const std::string& l) { log_.push_back(l); };
    link_->onData = [this](const uint8_t* d, size_t n) { if (session_) session_->onBytes(d, n); };
    link_->onState = [this](LinkState s) {
        if (s == LinkState::Connected && session_ && session_->state() == B2fSession::State::Idle) {
            peer_ = link_->remoteCall();
            log_.push_back("connected by " + (peer_.empty() ? std::string("another station") : peer_) + " (peer to peer)");
            session_->start();
        }
    };
    log_.push_back(std::string("listening for a peer-to-peer call (") + transportName(c.gateway.transport) + ")");
    listenAgain_ = true;
    link_->listen();
    return true;
}

void WinlinkStation::stop(double now) {
    listenAgain_ = false;
    if (!link_) return;
    if (link_->state() == LinkState::Listening) { link_->abort(); finish(now); return; }
    if (stopping_) { link_->abort(); finish(now); return; }
    stopping_ = true;
    link_->disconnect(now);
}

void WinlinkStation::rxAx25(const ax25::Frame& f, double now) { if (ax_) ax_->onFrame(f, now); }
void WinlinkStation::rxArdop(const ArdopRxFrameIn& f, double now) { if (ar_) ar_->onFrame(f, now); }

std::vector<ax25::Frame> WinlinkStation::takeAx25() { std::vector<ax25::Frame> v; v.swap(outAx_); return v; }
std::vector<ArdopLinkFrame> WinlinkStation::takeArdop() { std::vector<ArdopLinkFrame> v; v.swap(outAr_); return v; }
std::vector<std::string> WinlinkStation::takeLog() { std::vector<std::string> v; v.swap(log_); return v; }

void WinlinkStation::tick(double now) {
    if (!link_) return;
    link_->tick(now);
    if (session_ && !sessionEnded_ && (session_->done() || session_->failed())) {
        sessionEnded_ = true;
        lastOk_ = session_->done();
        if (session_->failed()) lastErr_ = session_->error();
        link_->disconnect(now);
    }
    const LinkState s = link_->state();
    if (s == LinkState::Closed || s == LinkState::Failed) {
        if (s == LinkState::Failed && lastErr_.empty()) lastErr_ = link_->error();
        if (session_ && !sessionEnded_) { session_->linkClosed(); lastOk_ = false; if (lastErr_.empty()) lastErr_ = session_->error(); }
        finish(now);
        if (listenAgain_) {
            const StationConfig again = c_;
            std::string err;
            if (!listen(again, now, err)) { listenAgain_ = false; log_.push_back("cannot listen again: " + err); }
        }
    }
}

void WinlinkStation::finish(double) {
    if (session_) { lastRx_ = session_->messagesReceived(); lastTx_ = session_->messagesSent(); }
    log_.push_back(lastOk_ ? "session complete: " + std::to_string(lastRx_) + " received, " + std::to_string(lastTx_) + " sent"
                           : "session ended: " + (lastErr_.empty() ? std::string("stopped") : lastErr_));
    // called from tick() or stop(), never from inside a link callback
    link_.reset();
    session_.reset();
    ax_ = nullptr; ar_ = nullptr;
}

std::string WinlinkStation::status() const {
    if (!link_) return lastOk_ ? "done" : (lastErr_.empty() ? "idle" : "failed: " + lastErr_);
    std::string s = linkStateName(link_->state());
    if (session_) {
        switch (session_->state()) {
        case B2fSession::State::Handshake: s += ", handshake"; break;
        case B2fSession::State::WaitAnswer: s += ", offering messages"; break;
        case B2fSession::State::TheirTurn: s += ", gateway's turn"; break;
        case B2fSession::State::Receiving: s += ", receiving " + std::to_string(session_->rxBytes()) + " / " + std::to_string(session_->rxTotal()) + " bytes"; break;
        case B2fSession::State::Done: s += ", done"; break;
        case B2fSession::State::Failed: s += ", failed"; break;
        default: break;
        }
    }
    const std::string dt = link_->detail();
    if (!dt.empty()) s += " (" + dt + ")";
    return s;
}

} // namespace winlink
} // namespace dect2
