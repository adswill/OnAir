// AX.25 connected mode and the ARDOP-frame ARQ link for Winlink: see wl_link.h. AX.25 from the AX.25 2.0 / 2.2 specification
// (TAPR); the ARQ uses the ARDOP frame types of ardop_proto.h. New code.
#include "dect2/winlink/wl_link.h"
#include "dect2/ardop/ardop_proto.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace winlink {

namespace {

std::string upperTrim(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

ax25::Address addrOf(const std::string& s) {
    ax25::Address a;
    const std::string u = upperTrim(s);
    const size_t d = u.find('-');
    a.call = u.substr(0, d);
    if (a.call.size() > 6) a.call.resize(6);
    a.ssid = d == std::string::npos ? 0 : std::max(0, std::min(15, atoi(u.c_str() + d + 1)));
    return a;
}

bool sameAddr(const ax25::Address& a, const ax25::Address& b) { return a.call == b.call && a.ssid == b.ssid; }

// control field values (modulo 8)
constexpr uint8_t kSabm = 0x2F, kDisc = 0x43, kDm = 0x0F, kUa = 0x63, kFrmr = 0x87, kUi = 0x03;
constexpr uint8_t kRr = 0x01, kRnr = 0x05, kRej = 0x09;
constexpr uint8_t kPf = 0x10;

} // namespace

const char* linkStateName(LinkState s) {
    switch (s) {
    case LinkState::Idle: return "idle";
    case LinkState::Listening: return "listening";
    case LinkState::Connecting: return "connecting";
    case LinkState::Connected: return "connected";
    case LinkState::Disconnecting: return "disconnecting";
    case LinkState::Closed: return "closed";
    case LinkState::Failed: return "failed";
    }
    return "?";
}

void ByteLink::setState(LinkState s, const std::string& why) {
    if (s == st_) return;
    st_ = s;
    if (!why.empty()) err_ = why;
    if (log) log(std::string("link ") + linkStateName(s) + (why.empty() ? "" : ": " + why));
    if (onState) onState(s);
}

// ================================================================ AX.25

Ax25Link::Ax25Link(const Ax25LinkConfig& c) : c_(c) {
    c_.window = std::max(1, std::min(7, c_.window));
    c_.paclen = std::max(16, std::min(256, c_.paclen));
    me_ = addrOf(c_.mycall);
    if (!c_.peer.empty()) peer_ = addrOf(c_.peer);
    for (const auto& v : c_.via) via_.push_back(addrOf(v));
}

std::string Ax25Link::remoteCall() const { return st_ == LinkState::Idle || st_ == LinkState::Listening ? c_.peer : peer_.str(); }

size_t Ax25Link::pendingTx() const {
    size_t n = txq_.size();
    for (int i = 0; i < nbuf_; i++) n += win_[(va_ + i) & 7].size();
    return n;
}

void Ax25Link::sendU(uint8_t ctl, bool command) {
    ax25::Frame f;
    f.to = peer_; f.from = me_; f.path = via_;
    for (auto& p : f.path) p.h = false;
    f.to.h = command; f.from.h = !command;
    f.control = ctl;
    f.pid = (ctl & 0xEF) == kUi ? 0xF0 : -1;
    framesSent++;
    if (sendFrame) sendFrame(f);
}

void Ax25Link::sendS(uint8_t type, bool command, bool pf) {
    sendU((uint8_t)(((vr_ & 7) << 5) | (pf ? kPf : 0) | type), command);
}

void Ax25Link::sendI(int ns, bool poll) {
    ax25::Frame f;
    f.to = peer_; f.from = me_; f.path = via_;
    for (auto& p : f.path) p.h = false;
    f.to.h = true; f.from.h = false;
    f.control = (uint8_t)(((vr_ & 7) << 5) | (poll ? kPf : 0) | ((ns & 7) << 1));
    f.pid = 0xF0;
    const std::vector<uint8_t>& d = win_[ns & 7];
    f.info.assign(d.begin(), d.end());
    framesSent++;
    ackDue_ = false;
    if (sendFrame) sendFrame(f);
}

void Ax25Link::connect(double now) {
    now_ = now;
    if (peer_.call.empty()) { setState(LinkState::Failed, "no station to call"); return; }
    vs_ = vr_ = va_ = nbuf_ = 0;
    tries_ = 0;
    setState(LinkState::Connecting);
    sendU(kSabm | kPf, true);
    t1At_ = now + c_.t1Sec;
}

void Ax25Link::listen() { setState(LinkState::Listening); }

void Ax25Link::write(const uint8_t* d, size_t n) {
    txq_.insert(txq_.end(), d, d + n);   // sent from tick() or after the next frame, so that several writes share I frames
}

void Ax25Link::disconnect(double now) {
    now_ = now;
    if (st_ == LinkState::Connecting || st_ == LinkState::Listening || st_ == LinkState::Idle) { setState(LinkState::Closed); return; }
    if (st_ != LinkState::Connected) return;
    discWanted_ = true;
    pump(now);
}

void Ax25Link::abort() {
    txq_.clear();
    nbuf_ = 0;
    t1At_ = -1;
    if (st_ != LinkState::Failed) setState(LinkState::Closed);
}

bool Ax25Link::forMe(const ax25::Frame& f) const {
    if (!sameAddr(f.to, me_)) return false;
    for (const auto& p : f.path) if (!p.h) return false;   // not yet repeated by every digipeater: wait for the repeat
    if (!peer_.call.empty() && !sameAddr(f.from, peer_)) return false;
    return true;
}

void Ax25Link::ackTo(int nr, double now) {
    const int out = (vs_ - va_) & 7;
    const int held = nbuf_;
    const int adv = (nr - va_) & 7;
    if (adv > std::max(out, held)) return;   // N(R) outside the window: ignored
    for (int i = 0; i < adv; i++) { win_[va_].clear(); va_ = (va_ + 1) & 7; nbuf_--; }
    if (((vs_ - va_) & 7) > nbuf_) vs_ = va_;
    if (adv > 0) tries_ = 0;
    if (nbuf_ == 0) t1At_ = -1;
    else if (adv > 0) t1At_ = now + c_.t1Sec;
}

void Ax25Link::onFrame(const ax25::Frame& f, double now) {
    now_ = now;
    if (st_ == LinkState::Closed || st_ == LinkState::Failed || st_ == LinkState::Idle) {
        // answer a call to us with DM, so the caller does not wait
        if (sameAddr(f.to, me_) && (f.control & 0xEF) == kSabm) {
            const ax25::Address keep = peer_;
            peer_ = f.from; peer_.h = false;
            sendU(kDm | kPf, false);
            peer_ = keep;
        }
        return;
    }
    if (st_ == LinkState::Listening && peer_.call.empty()) {
        if (!sameAddr(f.to, me_) || (f.control & 0xEF) != kSabm) return;
        for (const auto& p : f.path) if (!p.h) return;
        peer_ = f.from; peer_.h = false;
        via_.clear();
        for (auto it = f.path.rbegin(); it != f.path.rend(); ++it) { ax25::Address a = *it; a.h = false; via_.push_back(a); }   // the way back
    }
    if (!forMe(f)) return;
    framesReceived++;
    heardAt_ = now;
    const uint8_t c = f.control;
    const bool pf = (c & kPf) != 0;
    const bool command = f.to.h && !f.from.h;
    if ((c & 1) == 0) {   // I frame
        if (st_ != LinkState::Connected) return;
        const int ns = (c >> 1) & 7, nr = (c >> 5) & 7;
        ackTo(nr, now);
        if (ns == vr_) {
            vr_ = (vr_ + 1) & 7;
            rejSent_ = false;
            ackDue_ = true;
            if (onData && !f.info.empty()) onData((const uint8_t*)f.info.data(), f.info.size());
        } else if (!rejSent_) {
            rejSent_ = true;
            sendS(kRej, false, pf);
            ackDue_ = false;
        }
        if (pf) { sendS(kRr, false, true); ackDue_ = false; }
        pump(now);
        return;
    }
    if ((c & 3) == 1) {   // S frame
        if (st_ != LinkState::Connected && st_ != LinkState::Disconnecting) return;
        const int nr = (c >> 5) & 7;
        ackTo(nr, now);
        const uint8_t type = c & 0x0F;
        if (type == kRej) { vs_ = va_; retries++; }
        if (command && pf) sendS(kRr, false, true);
        if (!command && pf) tries_ = 0;   // the other side answered our poll: it is there (AX.25 2.2: RC = 0 on F = 1)
        if (!command && pf && nbuf_ == 0) t1At_ = -1;   // an idle poll (T3) answered
        if (!command && pf && nbuf_ > 0) { vs_ = va_; retries++; }   // the answer to our poll: send again what is not acknowledged
        if (type == kRnr) t1At_ = now + c_.t1Sec;   // the other side is busy: wait
        pump(now);
        return;
    }
    const uint8_t u = c & 0xEF;
    if (u == kSabm) {
        if (!sameAddr(f.from, peer_)) return;
        // a new SABM while connected (our UA was lost): the link starts again, and what was not acknowledged is sent again
        std::vector<uint8_t> back;
        for (int i = 0; i < nbuf_; i++) { const auto& w = win_[(va_ + i) & 7]; back.insert(back.end(), w.begin(), w.end()); }
        txq_.insert(txq_.begin(), back.begin(), back.end());
        vs_ = vr_ = va_ = nbuf_ = 0;
        for (auto& w : win_) w.clear();
        sendU(kUa | (pf ? kPf : 0), false);
        t1At_ = -1;
        tries_ = 0;
        setState(LinkState::Connected);
        pump(now);
        return;
    }
    if (u == kUa) {
        if (st_ == LinkState::Connecting) {
            vs_ = vr_ = va_ = nbuf_ = 0;
            t1At_ = -1;
            tries_ = 0;
            setState(LinkState::Connected);
            pump(now);
        } else if (st_ == LinkState::Disconnecting) { t1At_ = -1; setState(LinkState::Closed); }
        return;
    }
    if (u == kDm) {
        if (st_ == LinkState::Connecting) setState(LinkState::Failed, "the station refused the connection (DM)");
        else if (st_ == LinkState::Disconnecting) setState(LinkState::Closed);
        else setState(LinkState::Closed, "the other station dropped the connection");
        t1At_ = -1;
        return;
    }
    if (u == kDisc) {
        sendU(kUa | (pf ? kPf : 0), false);
        t1At_ = -1;
        setState(LinkState::Closed);
        return;
    }
    if (u == kFrmr) { setState(LinkState::Failed, "frame reject (FRMR) from the other station"); t1At_ = -1; return; }
}

void Ax25Link::pump(double now) {
    if (st_ != LinkState::Connected) return;
    bool sent = false;
    for (;;) {
        const int out = (vs_ - va_) & 7;
        if (out < nbuf_) {   // again after REJ / timeout
            sendI(vs_, false);
            vs_ = (vs_ + 1) & 7;
            sent = true;
            continue;
        }
        if (nbuf_ >= c_.window || txq_.empty()) break;
        const size_t n = std::min(txq_.size(), (size_t)c_.paclen);
        std::vector<uint8_t>& w = win_[vs_];
        w.assign(txq_.begin(), txq_.begin() + (long)n);
        txq_.erase(txq_.begin(), txq_.begin() + (long)n);
        nbuf_++;
        sendI(vs_, false);
        vs_ = (vs_ + 1) & 7;
        sent = true;
    }
    if (sent && t1At_ < 0) t1At_ = now + c_.t1Sec;
    if (ackDue_) { sendS(kRr, false, false); ackDue_ = false; }
    if (discWanted_ && txq_.empty() && nbuf_ == 0) {
        discWanted_ = false;
        tries_ = 0;
        setState(LinkState::Disconnecting);
        sendU(kDisc | kPf, true);
        t1At_ = now + c_.t1Sec;
    }
}

void Ax25Link::tick(double now) {
    now_ = now;
    if (st_ == LinkState::Connected && (!txq_.empty() || discWanted_)) pump(now);
    if (st_ == LinkState::Connected && t1At_ < 0 && c_.t3Sec > 0 && now - heardAt_ >= c_.t3Sec) {   // T3: is the other station still there?
        heardAt_ = now;
        tries_ = 0;
        sendS(kRr, true, true);
        t1At_ = now + c_.t1Sec;
        return;
    }
    if (t1At_ < 0 || now < t1At_) return;
    if (++tries_ > c_.n2) {
        t1At_ = -1;
        if (st_ == LinkState::Disconnecting) setState(LinkState::Closed, "no answer to DISC");
        else setState(LinkState::Failed, st_ == LinkState::Connecting ? "no answer to the connect request" : "no acknowledgement from the other station");
        return;
    }
    retries++;
    t1At_ = now + c_.t1Sec;
    if (st_ == LinkState::Connecting) sendU(kSabm | kPf, true);
    else if (st_ == LinkState::Disconnecting) sendU(kDisc | kPf, true);
    else if (st_ == LinkState::Connected) {
        // poll (also the idle poll of T3): the other side answers with RR F=1 and its N(R); the frames it lacks are sent again then
        sendS(kRr, true, true);
    }
}

// ================================================================ ARDOP ARQ

namespace {
constexpr int kConReq500 = 0x32, kConAck500 = 0x3A, kData = 0x4A, kAck = 0xF0, kNak = 0x10;
constexpr int kBreak = 0x23, kIdle = 0x24, kDiscA = 0x29, kEnd = 0x2C;
constexpr size_t kDataBytes = 64;
}

uint8_t ardopSessionId(const std::string& caller, const std::string& target) {
    const std::string s = upperTrim(caller) + upperTrim(target);
    const uint16_t c = ardop::crc16((const uint8_t*)s.data(), s.size());
    uint8_t v = (uint8_t)(c & 0xFF);
    if (v == 0xFF) v = 0xFE;
    return v;
}

ArdopLink::ArdopLink(const ArdopLinkConfig& c) : c_(c) {}

void ArdopLink::send(int code, std::vector<uint8_t> body) {
    ArdopLinkFrame f;
    f.code = code;
    f.sid = (code >= 0x31 && code <= 0x38) ? 0xFF : sid_;
    f.body = std::move(body);
    framesSent++;
    if (sendFrame) sendFrame(f);
}

void ArdopLink::connect(double now) {
    if (c_.peer.empty()) { setState(LinkState::Failed, "no station to call"); return; }
    sid_ = ardopSessionId(c_.mycall, c_.peer);
    tries_ = 0;
    setState(LinkState::Connecting);
    send(kConReq500, ardop::makeCallFrame(kConReq500, upperTrim(c_.mycall), upperTrim(c_.peer)));
    timerAt_ = now + c_.ackTimeoutSec;
}

void ArdopLink::listen() { setState(LinkState::Listening); }

void ArdopLink::write(const uint8_t* d, size_t n) {
    txq_.insert(txq_.end(), d, d + n);
}

void ArdopLink::disconnect(double now) {
    if (st_ == LinkState::Connecting || st_ == LinkState::Listening || st_ == LinkState::Idle) { setState(LinkState::Closed); return; }
    if (st_ != LinkState::Connected) return;
    discWanted_ = true;
    tick(now);
}

void ArdopLink::abort() {
    txq_.clear(); cur_.clear();
    timerAt_ = -1;
    if (st_ != LinkState::Failed) setState(LinkState::Closed);
}

void ArdopLink::sendData(double now) {
    if (txq_.empty()) {
        if (discWanted_) {
            discWanted_ = false;
            tries_ = 0;
            setState(LinkState::Disconnecting);
            send(kDiscA);
            timerAt_ = now + c_.ackTimeoutSec;
            return;
        }
        curIsIdle_ = true;
        cur_.clear();
        send(kIdle);
        timerAt_ = now + c_.ackTimeoutSec;
        return;
    }
    curIsIdle_ = false;
    const size_t n = std::min(kDataBytes, txq_.size());
    cur_.assign(txq_.begin(), txq_.begin() + (long)n);
    txq_.erase(txq_.begin(), txq_.begin() + (long)n);
    std::vector<uint8_t> blk;
    ardop::makeDataBlock(kData | (curOdd_ ? 1 : 0), cur_, blk);
    send(kData | (curOdd_ ? 1 : 0), blk);
    timerAt_ = now + c_.ackTimeoutSec;
}

void ArdopLink::becomeIss(double now) {
    iss_ = true;
    breakPending_ = false;
    curOdd_ = false;
    cur_.clear();
    curIsIdle_ = false;
    tries_ = 0;
    sendData(now);
}

void ArdopLink::onFrame(const ArdopRxFrameIn& f, double now) {
    if (st_ == LinkState::Closed || st_ == LinkState::Failed || st_ == LinkState::Idle) return;
    const int code = f.code;
    if (code >= 0x31 && code <= 0x38) {   // a connect request
        if (upperTrim(f.to) != upperTrim(c_.mycall) || !f.bodyOk) return;
        if (st_ == LinkState::Listening || (st_ == LinkState::Connected && !iss_ && upperTrim(f.from) == upperTrim(c_.peer))) {
            c_.peer = upperTrim(f.from);
            sid_ = ardopSessionId(c_.peer, c_.mycall);
            framesReceived++;
            send(kConAck500, ardop::makeConAck(kConAck500, c_.leaderMs, sid_));
            iss_ = false;
            lastRxOdd_ = -1;
            timerAt_ = -1;
            setState(LinkState::Connected);
        }
        return;
    }
    if (f.sid != sid_) return;   // another session
    framesReceived++;
    if (code >= 0x39 && code <= 0x3C) {   // connect acknowledge
        if (st_ == LinkState::Connecting) { setState(LinkState::Connected); becomeIss(now); }
        return;
    }
    if (code == kDiscA) { send(kEnd); timerAt_ = -1; setState(LinkState::Closed); return; }
    if (code == kEnd) { if (st_ == LinkState::Disconnecting) { timerAt_ = -1; setState(LinkState::Closed); } return; }
    if (st_ != LinkState::Connected && st_ != LinkState::Disconnecting) return;
    if (st_ == LinkState::Disconnecting) {   // our DISC was not heard yet; anything else: send it again
        send(kDiscA);
        return;
    }
    if (code == 0x4A || code == 0x4B) {   // data
        if (iss_) { iss_ = false; cur_.clear(); timerAt_ = -1; }   // the other side holds the turn (our ACK of its BREAK was lost?)
        if (!f.bodyOk) { send(kNak); return; }
        const int odd = code & 1;
        if (odd != lastRxOdd_) {
            lastRxOdd_ = odd;
            if (onData && !f.payload.empty()) onData(f.payload.data(), f.payload.size());
        }
        send(kAck);
        return;
    }
    if (code == kIdle) {
        if (iss_) return;
        if (!txq_.empty() || discWanted_) {
            if (txq_.empty() && discWanted_) {   // nothing more to say: end the session from here
                discWanted_ = false; tries_ = 0;
                setState(LinkState::Disconnecting);
                send(kDiscA);
                timerAt_ = now + c_.ackTimeoutSec;
                return;
            }
            breakPending_ = true;
            send(kBreak);
            timerAt_ = now + c_.ackTimeoutSec;
        } else send(kAck);
        return;
    }
    if (code == kBreak) {
        if (!iss_) { send(kAck); return; }   // our earlier ACK was lost
        if (!curIsIdle_ && !cur_.empty()) {   // data still on its way: send it again, the BREAK waits
            std::vector<uint8_t> blk;
            ardop::makeDataBlock(kData | (curOdd_ ? 1 : 0), cur_, blk);
            send(kData | (curOdd_ ? 1 : 0), blk);
            timerAt_ = now + c_.ackTimeoutSec;
            return;
        }
        iss_ = false;
        cur_.clear();
        curIsIdle_ = false;
        lastRxOdd_ = -1;
        timerAt_ = -1;
        send(kAck);
        return;
    }
    if (code >= 0xE0) {   // ACK
        if (!iss_) {
            if (breakPending_) becomeIss(now);
            return;
        }
        tries_ = 0;
        if (curIsIdle_) {
            curIsIdle_ = false;
            if (!txq_.empty() || discWanted_) sendData(now);
            else timerAt_ = now + c_.idleRepeatSec;   // nothing to say: IDLE again a little later
            return;
        }
        if (!cur_.empty()) { cur_.clear(); curOdd_ = !curOdd_; }
        if (!txq_.empty() || discWanted_) sendData(now);
        else { curIsIdle_ = false; timerAt_ = now + 0.05; }   // let the session above answer first
        return;
    }
    if (code <= 0x1F) {   // NAK
        if (!iss_) return;
        if (++tries_ > c_.retries) { setState(LinkState::Failed, "too many NAKs"); return; }
        retries++;
        if (curIsIdle_ || cur_.empty()) { send(kIdle); curIsIdle_ = true; }
        else {
            std::vector<uint8_t> blk;
            ardop::makeDataBlock(kData | (curOdd_ ? 1 : 0), cur_, blk);
            send(kData | (curOdd_ ? 1 : 0), blk);
        }
        timerAt_ = now + c_.ackTimeoutSec;
        return;
    }
}

void ArdopLink::tick(double now) {
    if (st_ == LinkState::Connected && iss_ && cur_.empty() && !curIsIdle_ && (!txq_.empty() || discWanted_)) { sendData(now); return; }
    if (timerAt_ < 0 || now < timerAt_) return;
    const bool waitingAnswer = st_ == LinkState::Connecting || st_ == LinkState::Disconnecting || breakPending_ || (iss_ && (curIsIdle_ || !cur_.empty()));
    if (waitingAnswer && ++tries_ > c_.retries) {
        timerAt_ = -1;
        if (st_ == LinkState::Disconnecting) setState(LinkState::Closed, "no answer to DISC");
        else setState(LinkState::Failed, st_ == LinkState::Connecting ? "no answer to the connect request" : "no answer from the other station");
        return;
    }
    timerAt_ = now + c_.ackTimeoutSec;
    if (st_ == LinkState::Connecting) { retries++; send(kConReq500, ardop::makeCallFrame(kConReq500, upperTrim(c_.mycall), upperTrim(c_.peer))); return; }
    if (st_ == LinkState::Disconnecting) { retries++; send(kDiscA); return; }
    if (st_ != LinkState::Connected) return;
    if (breakPending_) { retries++; send(kBreak); return; }
    if (!iss_) { timerAt_ = -1; return; }
    if (curIsIdle_) { retries++; send(kIdle); return; }
    if (!cur_.empty()) {
        retries++;
        std::vector<uint8_t> blk;
        ardop::makeDataBlock(kData | (curOdd_ ? 1 : 0), cur_, blk);
        send(kData | (curOdd_ ? 1 : 0), blk);
        return;
    }
    tries_ = 0;
    sendData(now);   // the idle pause is over (or data arrived): IDLE or data
}

} // namespace winlink
} // namespace dect2
