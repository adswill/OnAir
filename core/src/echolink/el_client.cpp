// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EchoLink client: see el_client.h. Connection behaviour (SDES connect / keep-alive every 10 s, 5 tries, 50 s time-out, BYE, info text on
// connect) follows SvxLink's EchoLinkQso.cpp (GPL-2.0-or-later, Tobias Blomberg SM0SVX); the code is OnAir's own.
#include "dect2/echolink/el_client.h"
#include "dect2/echolink/el_conference.h"
#include "../net/tcp_client.h"
#include <chrono>
#include <cstring>
#include <ctime>

namespace dect2 {
namespace echolink {

int64_t monoMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* qsoStateName(QsoState s) {
    switch (s) {
    case QsoState::Connecting: return "connecting";
    case QsoState::Connected: return "connected";
    default: return "not connected";
    }
}

// ---- Session ------------------------------------------------------------------------------------------------------------------------------
void Session::setLocal(const std::string& call, const std::string& name, const std::string& info) {
    call_ = upperCall(call); name_ = name; info_ = info.empty() ? "Running OnAir" : info;
}

void Session::setInfo(const std::string& info) {
    info_ = info.empty() ? "Running OnAir" : info;
    if (state_ == QsoState::Connected && send_) send_(false, peerIp_, peerAudio_, makeInfoPacket(info_));
}

void Session::sendSdes() {
    if (send_ && !peerIp_.empty()) send_(true, peerIp_, peerCtrl_, makeSdes(call_, name_));
}
void Session::sendBye(const std::string& ip, int ctrlPort) {
    if (send_) send_(true, ip, ctrlPort, makeBye());
}

bool Session::connect(const std::string& ip, int remoteAudioPort, int64_t nowMs) {
    if (state_ != QsoState::Idle || ip.empty() || call_.empty()) return false;
    peerIp_ = ip; peerAudio_ = remoteAudioPort; peerCtrl_ = remoteAudioPort + 1;
    peerCall_.clear(); peerName_.clear(); peerInfo_.clear();
    state_ = QsoState::Connecting;
    retries_ = 0; haveRxSeq_ = false; lastAudioMs_ = -1;
    lastSdesOut_ = nowMs; lastHeard_ = nowMs;
    sendSdes();
    events_.push_back("connecting to " + ip);
    return true;
}

void Session::setConnected(int64_t nowMs) {
    state_ = QsoState::Connected;
    lastHeard_ = nowMs;
    events_.push_back("connected to " + (peerCall_.empty() ? peerIp_ : peerCall_));
    if (send_) send_(false, peerIp_, peerAudio_, makeInfoPacket(info_));
}

void Session::toIdle(const std::string& why) {
    if (state_ != QsoState::Idle && !why.empty()) events_.push_back(why);
    state_ = QsoState::Idle;
    peerIp_.clear(); peerCtrl_ = peerAudio_ = 0;
    lastAudioMs_ = -1;
}

void Session::disconnect() {
    if (state_ == QsoState::Idle) return;
    sendBye(peerIp_, peerCtrl_);
    toIdle("disconnected from " + (peerCall_.empty() ? peerIp_ : peerCall_));
}

void Session::onCtrl(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs) {
    // the peer: its address, and while connected also its control port (two stations can share one public address)
    const bool fromPeer = state_ != QsoState::Idle && ip == peerIp_ && (state_ == QsoState::Connecting || srcPort == peerCtrl_);
    if (isRtcpBye(p, n)) {
        if (fromPeer) toIdle((peerCall_.empty() ? peerIp_ : peerCall_) + " disconnected");
        return;
    }
    if (!isRtcpSdes(p, n)) return;
    std::string nameItem, call, name;
    const bool named = sdesItem(p, n, 2, nameItem) && splitSdesName(nameItem, call, name);
    if (!fromPeer) {
        if (state_ != QsoState::Idle || !accept_ || !named) {   // busy, or not taking calls: tell the caller
            sendBye(ip, srcPort);
            if (named) events_.push_back("refused a call from " + call);
            return;
        }
        // an incoming connection: accepted at once (as EchoLink does), answered from the port the request came from
        peerIp_ = ip; peerCtrl_ = srcPort; peerAudio_ = srcPort - 1;
        peerCall_ = call; peerName_ = name; peerInfo_.clear();
        haveRxSeq_ = false; lastAudioMs_ = -1; retries_ = 0;
        events_.push_back("incoming call from " + call);
        lastSdesOut_ = nowMs;
        sendSdes();
        setConnected(nowMs);
        return;
    }
    if (named) { peerCall_ = call; peerName_ = name; }
    peerCtrl_ = srcPort;   // answer where the peer really sends from (NAT)
    lastHeard_ = nowMs;
    if (state_ == QsoState::Connecting) setConnected(nowMs);
}

void Session::onAudio(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs) {
    if (state_ == QsoState::Idle || ip != peerIp_) return;
    const AudioPortKind kind = classifyAudioPort(p, n);
    if (state_ != QsoState::Connected && kind == AudioPortKind::Audio) return;   // info may arrive before the SDES answer; audio may not
    if (state_ == QsoState::Connected) peerAudio_ = srcPort;
    switch (kind) {
    case AudioPortKind::Audio: {
        uint16_t seq; uint8_t pt; const uint8_t* fr; int nf;
        if (!parseAudioPacket(p, n, seq, pt, fr, nf)) return;
        pktIn_++;
        if (haveRxSeq_) {
            const int16_t d = (int16_t)(uint16_t)(seq - rxSeq_);
            if (d <= 0) { late_++; return; }
            if (d > 1) lost_ += (uint64_t)(d - 1);
        }
        haveRxSeq_ = true; rxSeq_ = seq;
        lastAudioMs_ = nowMs;
        RxPacket r;
        r.seq = seq; r.nframes = nf;
        std::memcpy(r.frames, fr, (size_t)nf * kGsmFrameBytes);
        if (rx_.size() < 64) rx_.push_back(r);
        break;
    }
    case AudioPortKind::Info: {
        std::string s;
        if (parseInfoPacket(p, n, s)) { if (s.size() > 4096) s.resize(4096); peerInfo_ = s; }
        break;
    }
    case AudioPortKind::Chat: {
        std::string s;
        if (parseChatPacket(p, n, s) && chat_.size() < 256) { if (s.size() > 1024) s.resize(1024); chat_.push_back(s); }
        break;
    }
    default: break;
    }
}

void Session::tick(int64_t nowMs) {
    if (state_ == QsoState::Idle) return;
    if (state_ == QsoState::Connected && nowMs - lastHeard_ >= kConnectTimeoutMs) {
        sendBye(peerIp_, peerCtrl_);
        toIdle("connection to " + (peerCall_.empty() ? peerIp_ : peerCall_) + " timed out");
        return;
    }
    if (nowMs - lastSdesOut_ >= kKeepAliveMs) {
        if (state_ == QsoState::Connecting && ++retries_ >= kConnectRetries) {
            toIdle("no answer from " + peerIp_);
            return;
        }
        lastSdesOut_ = nowMs;
        sendSdes();
    }
}

bool Session::sendAudio(const uint8_t frames[kFramesPerPacket][kGsmFrameBytes]) {
    if (state_ != QsoState::Connected || !send_) return false;
    send_(false, peerIp_, peerAudio_, makeAudioPacket(txSeq_++, frames));
    pktOut_++;
    return true;
}

bool Session::sendChat(const std::string& message) {
    if (state_ != QsoState::Connected || !send_ || message.empty()) return false;
    send_(false, peerIp_, peerAudio_, makeChatPacket(call_, message));
    return true;
}

std::vector<RxPacket> Session::takeAudio() { std::vector<RxPacket> v; v.swap(rx_); return v; }
std::vector<std::string> Session::takeChat() { std::vector<std::string> v; v.swap(chat_); return v; }
std::vector<std::string> Session::takeEvents() { std::vector<std::string> v; v.swap(events_); return v; }

// ---- Client -------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr size_t kRxMaxSamples = 8000 * 2;     // 2 s; older audio is dropped
constexpr size_t kTxMaxSamples = 8000 * 4;

sock_t udpBind(const std::string& addr, int port, std::string& err) {
    netInit();
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) { err = "cannot open a UDP socket"; return kBadSock; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (const sockaddr*)&a, sizeof a) != 0) {
        sockClose(s);
        err = "UDP port " + std::to_string(port) + " is in use or not allowed";
        return kBadSock;
    }
    return s;
}
std::string hhmm() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[8];
    std::strftime(b, sizeof b, "%H:%M", &tm);
    return b;
}
} // namespace

Client::Client() : conf_(new Conference) {}
Client::~Client() { close(); }

void Client::configure(const ClientConfig& c) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = c;
    cfg_.call = upperCall(c.call);
    sess_.setLocal(cfg_.call, cfg_.name, cfg_.info);
    sess_.setAcceptIncoming(cfg_.acceptIncoming);
    conf_->setLocal(cfg_.call, cfg_.name, cfg_.info);
    conf_->setMaxStations(cfg_.conferenceMax);
    conf_->setAcceptIncoming(cfg_.acceptIncoming);
}

// mu_ held (the network thread), or from open()
void Client::sendPacket(bool ctrl, const std::string& ip, int port, const std::vector<uint8_t>& pkt) {
    if (proxyMode_) {
        if (proxyUp_) proxyWrite(makeProxyBlock(ctrl ? ProxyType::UdpControl : ProxyType::UdpData, ip, pkt.data(), pkt.size()));
        return;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return;
    sockSendTo((sock_t)(ctrl ? sc_ : sa_), pkt.data(), pkt.size(), to);
}

void Client::proxyWrite(const std::vector<uint8_t>& b) {
    std::lock_guard<std::mutex> lk(pxWrite_);
    if (sp_ < 0 || !sockSendAll((sock_t)sp_, b.data(), b.size())) proxyUp_ = false;
}

// Connects to the proxy and answers its nonce. A wrong password is told afterwards (a SYSTEM block), read by the network thread.
bool Client::proxyOpen(const ClientConfig& c, std::string& err) {
    std::string e;
    sock_t s = tcpConnect(c.proxyHost, c.proxyPort, c.directoryTimeoutMs, e);
    if (s == kBadSock) { err = "cannot reach the proxy " + c.proxyHost + ": " + e; return false; }
    sockRecvTimeout(s, c.directoryTimeoutMs);
    px_.reset();
    char b[64];
    const int64_t t0 = monoMs();
    while (!px_.nonceReady()) {
        const long long n = sockRecv(s, b, sizeof b);
        if (n <= 0 || monoMs() - t0 > c.directoryTimeoutMs) { sockClose(s); err = "the proxy " + c.proxyHost + " sent no greeting"; return false; }
        px_.feed((const uint8_t*)b, (size_t)n);
    }
    const std::vector<uint8_t> auth = proxyAuthReply(c.call, c.proxyPassword, px_.nonce());
    if (!sockSendAll(s, auth.data(), auth.size())) { sockClose(s); err = "cannot send to the proxy " + c.proxyHost; return false; }
    sockRecvTimeout(s, 0);
    sp_ = (long long)s;
    proxyUp_ = true;
    pxTcp_ = 0;
    pxReply_.clear();
    proxyText_ = "connected to the proxy " + c.proxyHost;
    log(proxyText_);
    return true;
}

// mu_ held: what the proxy sent
void Client::proxyInput(int64_t now) {
    char b[8192];
    const long long n = recv((sock_t)sp_, b, (int)sizeof b, 0);
    if (n <= 0) {
        proxyUp_ = false;
        proxyText_ = "the proxy closed the connection";
    } else {
        px_.feed((const uint8_t*)b, (size_t)n);
        ProxyBlock blk;
        while (px_.next(blk)) {
            switch (blk.type) {
            case ProxyType::UdpData:
                if (confMode_) conf_->onAudio(blk.ip, kDefaultAudioPort, blk.data.data(), blk.data.size(), now);
                else sess_.onAudio(blk.ip, kDefaultAudioPort, blk.data.data(), blk.data.size(), now);
                break;
            case ProxyType::UdpControl:
                if (confMode_) conf_->onCtrl(blk.ip, kDefaultAudioPort + 1, blk.data.data(), blk.data.size(), now);
                else sess_.onCtrl(blk.ip, kDefaultAudioPort + 1, blk.data.data(), blk.data.size(), now);
                break;
            case ProxyType::TcpStatus: {
                uint32_t st = 1;
                if (blk.data.size() == 4) st = (uint32_t)blk.data[0] | (uint32_t)blk.data[1] << 8 | (uint32_t)blk.data[2] << 16 | (uint32_t)blk.data[3] << 24;
                if (pxTcp_ == 1) pxTcp_ = st == 0 ? 2 : 4;
                break;
            }
            case ProxyType::TcpData:
                if (pxTcp_ == 2 && pxReply_.size() < (1u << 24)) pxReply_.append((const char*)blk.data.data(), blk.data.size());
                break;
            case ProxyType::TcpClose:
                if (pxTcp_ == 1 || pxTcp_ == 2) pxTcp_ = 3;
                break;
            case ProxyType::System:
                proxyUp_ = false;
                proxyText_ = proxySystemText(blk.data.empty() ? 0 : blk.data[0]);
                break;
            default:   // TCP_OPEN is the client's message
                break;
            }
        }
        if (px_.failed()) { proxyUp_ = false; proxyText_ = "the proxy sent a broken stream"; }
    }
    if (!proxyUp_) {
        log(proxyText_);
        sess_.disconnect();
        conf_->disconnectAll();
        if (pxTcp_ == 1 || pxTcp_ == 2) pxTcp_ = 3;
    }
    pxCv_.notify_all();
}

// The directory command through the proxy: TCP_OPEN to the server's address, TCP_DATA, the answer until TCP_CLOSE (or a whole list)
bool Client::proxyDirExchange(const std::string& host, int timeoutMs, const std::string& cmd, bool list, std::string& reply, std::string& err) {
    std::string ip;
    {
        netInit();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { err = "cannot find the address " + host; return false; }
        char t[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &((const sockaddr_in*)res->ai_addr)->sin_addr, t, sizeof t);
        freeaddrinfo(res);
        ip = t;
    }
    std::unique_lock<std::mutex> lk(mu_);
    if (!proxyUp_) { err = "the proxy is not connected" + (proxyText_.empty() ? std::string() : ": " + proxyText_); return false; }
    pxTcp_ = 1;
    pxReply_.clear();
    lk.unlock();
    proxyWrite(makeProxyBlock(ProxyType::TcpOpen, ip));
    lk.lock();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    pxCv_.wait_until(lk, until, [&] { return pxTcp_ != 1; });
    if (pxTcp_ != 2) {
        err = pxTcp_ == 1 ? "the proxy did not answer" : "the proxy could not reach " + host;
        pxTcp_ = 0;
        return false;
    }
    lk.unlock();
    proxyWrite(makeProxyBlock(ProxyType::TcpData, "", (const uint8_t*)cmd.data(), cmd.size()));
    lk.lock();
    StationListParser lp;
    size_t fed = 0;
    pxCv_.wait_until(lk, until, [&] {
        if (pxTcp_ != 2) return true;
        if (list) { lp.feed(pxReply_.data() + fed, pxReply_.size() - fed); fed = pxReply_.size(); return lp.done() || lp.failed(); }
        return pxReply_.size() >= 2;
    });
    reply = pxReply_;
    const bool stillOpen = pxTcp_ == 2;
    pxTcp_ = 0;
    lk.unlock();
    if (stillOpen) proxyWrite(makeProxyBlock(ProxyType::TcpClose, ""));
    if (reply.empty()) { err = host + " closed the connection without an answer (through the proxy)"; return false; }
    return true;
}

void Client::log(const std::string& s) {
    if (log_.size() < 512) log_.push_back(s);
}

bool Client::open(std::string& err) {
    if (open_) return true;
    ClientConfig c;
    { std::lock_guard<std::mutex> lk(mu_); c = cfg_; }
    if (!validCallsign(c.call)) { err = "set your callsign first"; return false; }
    confMode_ = c.conference;
    proxyMode_ = !c.proxyHost.empty();
    if (proxyMode_) {
        if (!proxyOpen(c, err)) return false;
        boundAudio_ = 0;
    } else {
        sock_t a = udpBind(c.bindAddress, c.audioPort, err);
        if (a == kBadSock) return false;
        sock_t k = udpBind(c.bindAddress, c.audioPort + 1, err);
        if (k == kBadSock) { sockClose(a); return false; }
        sa_ = (long long)a; sc_ = (long long)k;
        boundAudio_ = c.audioPort;
    }
    auto send = [this](bool ctrl, const std::string& ip, int port, const std::vector<uint8_t>& pkt) { sendPacket(ctrl, ip, port, pkt); };
    sess_.setSend(send);
    conf_->setSend(send);
    stop_ = false;
    closing_ = false;
    open_ = true;
    net_ = std::thread([this] { run(); });
    return true;
}

void Client::close() {
    if (!open_) return;
    closing_ = true;                    // no more log-in refreshes
    disconnect();                       // the BYE still goes out: the sockets are open
    if (loggedIn_) logout();            // the network thread still runs: through a proxy it carries the answer
    if (dir_.joinable()) dir_.join();
    stop_ = true;
    if (net_.joinable()) net_.join();
    if (sa_ >= 0) sockClose((sock_t)sa_);
    if (sc_ >= 0) sockClose((sock_t)sc_);
    if (sp_ >= 0) { std::lock_guard<std::mutex> lk(pxWrite_); sockClose((sock_t)sp_); }
    sa_ = sc_ = sp_ = -1;
    proxyUp_ = false;
    open_ = false;
}

void Client::run() {
    std::vector<uint8_t> buf(2048);
    while (!stop_) {
        fd_set rd;
        FD_ZERO(&rd);
        const sock_t a = (sock_t)sa_, k = (sock_t)sc_, px = (sock_t)sp_;
        sock_t top = 0;
        if (proxyMode_) { if (proxyUp_) { FD_SET(px, &rd); top = px; } }
        else { FD_SET(a, &rd); FD_SET(k, &rd); top = a > k ? a : k; }
        timeval tv{0, 10000};
        const int r = proxyMode_ && !proxyUp_ ? (std::this_thread::sleep_for(std::chrono::milliseconds(10)), 0)
                                              : select((int)top + 1, &rd, nullptr, nullptr, &tv);
        const int64_t now = monoMs();
        bool refresh = false;
        {
        std::lock_guard<std::mutex> lk(mu_);
        if (proxyMode_ && r > 0 && FD_ISSET(px, &rd)) proxyInput(now);
        for (int which = 0; !proxyMode_ && r > 0 && which < 2; which++) {
            const sock_t s = which == 0 ? a : k;
            if (!FD_ISSET(s, &rd)) continue;
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            const long long n = recvfrom(s, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            const int port = ntohs(from.sin_port);
            if (confMode_) {
                if (which == 0) conf_->onAudio(ip, port, buf.data(), (size_t)n, now);
                else conf_->onCtrl(ip, port, buf.data(), (size_t)n, now);
            } else if (which == 0) sess_.onAudio(ip, port, buf.data(), (size_t)n, now);
            else sess_.onCtrl(ip, port, buf.data(), (size_t)n, now);
        }
        if (confMode_) {
            conf_->tick(now);
            int16_t mix[kMixBlockSamples];
            for (int got; (got = conf_->pullLocal(mix, kMixBlockSamples)) > 0;) rxq_.insert(rxq_.end(), mix, mix + got);
            while (rxq_.size() > kRxMaxSamples) rxq_.pop_front();
            for (std::string& s : conf_->takeChat()) chat_.push_back(std::move(s));
            for (std::string& s : conf_->takeEvents()) log(s);
            if (loggedIn_ && !dirBusy_ && !closing_ && now - lastLogin_ >= kRegistrationRefreshMs) { lastLogin_ = now; refresh = true; }
        } else {
        sess_.tick(now);
        for (const RxPacket& p : sess_.takeAudio()) {
            for (int f = 0; f < p.nframes; f++) {
                int16_t pcm[kGsmFrameSamples];
                if (!dec_.decode(p.frames[f], pcm)) continue;
                rxq_.insert(rxq_.end(), pcm, pcm + kGsmFrameSamples);
            }
        }
        while (rxq_.size() > kRxMaxSamples) rxq_.pop_front();
        for (std::string& s : sess_.takeChat()) chat_.push_back(std::move(s));
        for (std::string& s : sess_.takeEvents()) log(s);
        if (sess_.state() != QsoState::Connected) txq_.clear();
        flushTx(false);
        if (loggedIn_ && !dirBusy_ && !closing_ && now - lastLogin_ >= kRegistrationRefreshMs) { lastLogin_ = now; refresh = true; }
        }
        }
        if (refresh) login();   // outside the lock: the directory job takes it
    }
}

// mu_ held. Sends every complete packet; pad: also the rest, padded with silence
void Client::flushTx(bool pad) {
    const size_t per = (size_t)kFramesPerPacket * kGsmFrameSamples;
    if (pad && !txq_.empty() && txq_.size() % per) txq_.insert(txq_.end(), per - txq_.size() % per, 0);
    while (txq_.size() >= per && sess_.state() == QsoState::Connected) {
        uint8_t frames[kFramesPerPacket][kGsmFrameBytes];
        for (int f = 0; f < kFramesPerPacket; f++) {
            int16_t pcm[kGsmFrameSamples];
            for (int i = 0; i < kGsmFrameSamples; i++) { pcm[i] = txq_.front(); txq_.pop_front(); }
            enc_.encode(pcm, frames[f]);
        }
        sess_.sendAudio(frames);
    }
}

void Client::setTransmit(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (confMode_) { conf_->setLocalTalk(on); tx_ = on; return; }
    if (tx_ && !on) flushTx(true);
    if (!tx_ && on) { txq_.clear(); enc_.reset(); }
    tx_ = on;
}

void Client::pushTx(const int16_t* pcm, int n) {
    if (!tx_ || n <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (confMode_) { if (conf_->connectedCount() > 0) conf_->pushLocal(pcm, n); return; }
    if (sess_.state() != QsoState::Connected) return;
    txq_.insert(txq_.end(), pcm, pcm + n);
    while (txq_.size() > kTxMaxSamples) txq_.pop_front();
}

int Client::pullRx(int16_t* out, int n) {
    std::lock_guard<std::mutex> lk(mu_);
    int k = 0;
    while (k < n && !rxq_.empty()) { out[k++] = rxq_.front(); rxq_.pop_front(); }
    return k;
}

int Client::rxBuffered() const { std::lock_guard<std::mutex> lk(mu_); return (int)rxq_.size(); }

bool Client::connectTo(const std::string& ip, int remoteAudioPort) {
    if (!open_) return false;
    in_addr tmp{};
    if (inet_pton(AF_INET, ip.c_str(), &tmp) != 1) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (confMode_) {
        const bool ok = conf_->connect(ip, remoteAudioPort, monoMs());
        for (std::string& s : conf_->takeEvents()) log(s);
        return ok;
    }
    dec_.reset();
    rxq_.clear();
    return sess_.connect(ip, remoteAudioPort, monoMs());
}

bool Client::connectCall(const std::string& call) {
    std::string ip;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const std::string c = upperCall(call);
        for (const Station& s : stations_) if (s.call == c) { ip = s.ip; break; }
    }
    return !ip.empty() && connectTo(ip);
}

void Client::disconnect() {
    std::lock_guard<std::mutex> lk(mu_);
    sess_.disconnect();
    conf_->disconnectAll();
    txq_.clear();
    for (std::string& s : sess_.takeEvents()) log(s);
    for (std::string& s : conf_->takeEvents()) log(s);
}

bool Client::disconnectStation(const std::string& call) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool ok = confMode_ ? conf_->disconnect(call) : (upperCall(call) == sess_.peerCall() && (sess_.disconnect(), true));
    for (std::string& s : sess_.takeEvents()) log(s);
    for (std::string& s : conf_->takeEvents()) log(s);
    return ok;
}

bool Client::sendChat(const std::string& message) {
    std::lock_guard<std::mutex> lk(mu_);
    return confMode_ ? conf_->sendChat(message) : sess_.sendChat(message);
}

// ---- directory ----------------------------------------------------------------------------------------------------------------------------
void Client::login() {
    if (dirBusy_.exchange(true)) return;
    if (dir_.joinable()) dir_.join();
    dir_ = std::thread([this] { dirJob(0); });
}
void Client::logout() {
    if (dir_.joinable()) dir_.join();   // a running log-in or list finishes first
    dirBusy_ = true;
    dirJob(1);   // synchronous: close() relies on it
}
void Client::refreshList() {
    if (dirBusy_.exchange(true)) return;
    if (dir_.joinable()) dir_.join();
    dir_ = std::thread([this] { dirJob(2); });
}

// One command on one TCP connection, trying the servers in order
bool Client::dirExchange(const std::string& cmd, bool list, std::string& reply, std::string& err) {
    ClientConfig c;
    { std::lock_guard<std::mutex> lk(mu_); c = cfg_; }
    if (c.servers.empty()) { err = "no directory server set"; return false; }
    for (const std::string& host : c.servers) {
        std::string e;
        if (proxyMode_) {
            if (proxyDirExchange(host, c.directoryTimeoutMs, cmd, list, reply, e)) return true;
            err = e;
            continue;
        }
        sock_t s = tcpConnect(host, c.directoryPort, c.directoryTimeoutMs, e);
        if (s == kBadSock) { err = "cannot reach " + host + ": " + e; continue; }
        sockRecvTimeout(s, c.directoryTimeoutMs);
        if (!sockSendAll(s, cmd.data(), cmd.size())) { sockClose(s); err = "cannot send to " + host; continue; }
        reply.clear();
        StationListParser lp;
        char b[4096];
        const int64_t t0 = monoMs();
        for (;;) {
            const long long n = sockRecv(s, b, sizeof b);
            if (n <= 0) break;
            reply.append(b, (size_t)n);
            if (list) { lp.feed(b, (size_t)n); if (lp.done() || lp.failed()) break; }
            else if (reply.size() >= 2) break;
            if (monoMs() - t0 > c.directoryTimeoutMs || reply.size() > (1u << 24)) break;
        }
        sockClose(s);
        if (reply.empty()) { err = host + " closed the connection without an answer"; continue; }
        return true;
    }
    return false;
}

void Client::dirJob(int kind) {
    ClientConfig c;
    { std::lock_guard<std::mutex> lk(mu_); c = cfg_; }
    std::string reply, err, text;
    if (kind == 0 || kind == 1) {
        const DirStatus st = kind == 0 ? DirStatus::Online : DirStatus::Offline;
        const std::string t = hhmm();
        const std::string cmd = loginCommand(c.call, c.password, st, t, c.location);
        bool ok = false;
        if (!validCallsign(c.call)) text = "set your callsign first";
        else if (c.password.empty()) text = "set your EchoLink password first";
        else if (!dirExchange(cmd, false, reply, err)) text = err;
        else if (reply.compare(0, 2, "OK") == 0) { ok = true; text = kind == 0 ? "logged in" : "logged out"; }
        else text = "the directory server refused the log-in (check the callsign, the password and that the callsign is validated)";
        std::lock_guard<std::mutex> lk(mu_);
        log(loginCommandRedacted(c.call, st, t, c.location) + ": " + text);
        if (kind == 0) { loggedIn_ = ok; if (ok) lastLogin_ = monoMs(); }
        else loggedIn_ = false;
        dirText_ = text;
    } else {
        StationListParser lp;
        if (dirExchange(listCommand(), true, reply, err)) lp.feed(reply.data(), reply.size());
        std::lock_guard<std::mutex> lk(mu_);
        if (!err.empty() && reply.empty()) dirText_ = err;
        else if (lp.badPassword()) dirText_ = "incorrect password: log in again";
        else if (lp.done()) {
            stations_ = lp.stations();
            listAt_ = monoMs();
            dirText_ = std::to_string(stations_.size()) + " stations listed";
        } else dirText_ = "the station list could not be read";
        log("station list: " + dirText_);
    }
    dirBusy_ = false;
}

std::vector<Station> Client::stations() const { std::lock_guard<std::mutex> lk(mu_); return stations_; }

ClientStatus Client::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    ClientStatus s;
    const int64_t now = monoMs();
    s.portsOpen = open_;
    s.loggedIn = loggedIn_;
    s.busy = dirBusy_;
    s.directory = dirText_;
    s.qso = sess_.state();
    s.peerCall = sess_.peerCall(); s.peerName = sess_.peerName(); s.peerIp = sess_.peerIp(); s.peerInfo = sess_.peerInfo();
    s.rx = sess_.rxActive(now);
    s.tx = tx_ && sess_.state() == QsoState::Connected;
    s.packetsIn = sess_.packetsIn(); s.packetsOut = sess_.packetsOut(); s.lost = sess_.packetsLost();
    s.listAgeSec = listAt_ < 0 ? -1 : (int)((now - listAt_) / 1000);
    s.viaProxy = proxyMode_;
    s.proxy = proxyText_;
    if (confMode_) {
        s.conference = true;
        const int nc = conf_->connectedCount();
        s.qso = nc > 0 ? QsoState::Connected : conf_->size() > 0 ? QsoState::Connecting : QsoState::Idle;
        s.peerCall = std::to_string(nc) + (nc == 1 ? " station" : " stations");
        s.peerName.clear(); s.peerIp.clear();
        s.peerInfo = conf_->infoText();
        s.rx = conf_->anyTalking();
        s.tx = tx_ && nc > 0;
        s.packetsIn = s.lost = 0;
        for (const ConferenceMember& m : conf_->members()) {
            ConferenceMemberStatus ms;
            ms.call = m.call; ms.name = m.name; ms.ip = m.ip; ms.connecting = m.connecting; ms.talking = m.talking;
            ms.packetsIn = m.packetsIn; ms.lost = m.lost;
            s.members.push_back(ms);
            s.packetsIn += m.packetsIn; s.lost += m.lost;
        }
        s.packetsOut = conf_->packetsOut();
    }
    return s;
}

std::vector<std::string> Client::takeChat() { std::lock_guard<std::mutex> lk(mu_); std::vector<std::string> v; v.swap(chat_); return v; }
std::vector<std::string> Client::takeLog() { std::lock_guard<std::mutex> lk(mu_); std::vector<std::string> v; v.swap(log_); return v; }

} // namespace echolink
} // namespace dect2
