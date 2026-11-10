// Winlink B2F session (client and gateway roles). After the public B2F notes and wl2k-go's fbb package (Copyright 2015-2016 Martin
// Hebnes Pedersen LA5NTA, MIT licence; see THIRD_PARTY.md). New C++ code.
//
// Parts ported from wl2k-go (https://github.com/la5nta/wl2k-go), under this notice:
//
// The MIT License (MIT)
// Copyright (c) 2014-2017 Martin Hebnes Pedersen (LA5NTA)
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "dect2/winlink/b2f.h"
#include "dect2/winlink/lzhuf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace dect2 {
namespace winlink {

namespace {
constexpr uint8_t SOH = 1, STX = 2, EOT = 4;
constexpr size_t kMaxBlock = 5;          // proposals per turn
constexpr size_t kMaxChunk = 125;        // data bytes per STX record
constexpr size_t kMaxIn = 4u << 20;      // a runaway stream is cut here

bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
} // namespace

B2Proposal makeProposal(const B2Message& m) {
    B2Proposal p;
    p.mid = m.mid();
    const std::vector<uint8_t> raw = encodeMessage(m);
    p.size = (int)raw.size();
    p.cdata = lzhufCompress(raw, true);
    p.csize = (int)p.cdata.size();
    p.title = m.subject();
    if (p.title.empty()) p.title = "No title";
    return p;
}

std::string proposalLine(const B2Proposal& p) {
    char b[160];
    snprintf(b, sizeof b, "F%c %s %s %d %d 0", p.code, p.type.c_str(), p.mid.c_str(), p.size, p.csize);
    return b;
}

int proposalChecksum(const std::vector<std::string>& lines) {
    long sum = 0;
    for (const auto& l : lines) { for (unsigned char c : l) sum += c; sum += '\r'; }
    return (int)((-sum) & 0xFF);
}

bool parseProposalLine(const std::string& line, B2Proposal& p) {
    if (line.size() < 4 || line[0] != 'F' || (line[1] != 'C' && line[1] != 'D')) return false;
    p = B2Proposal();
    p.code = line[1];
    std::vector<std::string> parts;
    size_t i = 3;
    while (i <= line.size()) {
        size_t e = line.find(' ', i);
        if (e == std::string::npos) e = line.size();
        parts.push_back(line.substr(i, e - i));
        i = e + 1;
    }
    if (parts.size() < 5 || parts.size() > 6) return false;
    if (parts[0] != "EM" && parts[0] != "CM") return false;
    p.type = parts[0];
    p.mid = parts[1];
    if (p.mid.empty() || p.mid.size() > 12) return false;
    p.size = atoi(parts[2].c_str());
    p.csize = atoi(parts[3].c_str());
    return p.size >= 0 && p.csize >= 0;
}

B2fSession::B2fSession(const Config& c, const Hooks& h) : cfg_(c), h_(h) {}

void B2fSession::trace(char dir, const std::string& s) {
    if (h_.log) h_.log(std::string(1, dir) + s);
}

void B2fSession::sendStr(const std::string& s) {
    bytesOut_ += s.size();
    if (h_.send) h_.send((const uint8_t*)s.data(), s.size());
}

void B2fSession::sendLine(const std::string& s) {
    trace('>', s);
    sendStr(s + "\r");
}

void B2fSession::fail(const std::string& why) {
    if (st_ == State::Done || st_ == State::Failed) return;
    err_ = why;
    st_ = State::Failed;
    trace('!', why);
}

void B2fSession::start() {
    if (st_ != State::Idle) return;
    st_ = State::Handshake;
    if (cfg_.role == Role::Gateway) {
        for (const auto& l : cfg_.motd) sendLine(l);
        sendLine("[WL2K-5.0-B2FWIHJM$]");
        if (!cfg_.challenge.empty()) sendLine(";PQ: " + cfg_.challenge);
        sendLine((cfg_.mycall.empty() ? std::string("CMS") : cfg_.mycall) + ">");
        loginOk_ = cfg_.expectPassword.empty();
    }
    process();
}

void B2fSession::linkClosed() {
    if (st_ == State::Done || st_ == State::Failed) return;
    fail("the connection ended before the session did");
}

void B2fSession::onBytes(const uint8_t* d, size_t n) {
    if (st_ == State::Done || st_ == State::Failed) return;
    bytesIn_ += n;
    in_.insert(in_.end(), d, d + n);
    if (in_.size() - inPos_ > kMaxIn) { fail("too much data without a protocol line"); return; }
    process();
}

bool B2fSession::takeLine(std::string& line) {
    for (size_t i = inPos_; i < in_.size(); i++) {
        if (in_[i] == '\r' || in_[i] == '\n') {
            line.assign((const char*)in_.data() + inPos_, i - inPos_);
            inPos_ = i + 1;
            return true;
        }
    }
    return false;
}

void B2fSession::process() {
    for (;;) {
        if (st_ == State::Receiving) { if (!receiveBinary()) break; continue; }
        if (st_ != State::Handshake && st_ != State::WaitAnswer && st_ != State::TheirTurn) break;
        std::string line;
        if (!takeLine(line)) break;
        if (line.empty()) continue;
        trace('<', line);
        if (startsWith(line, "***")) { fail("the other side says: " + line.substr(3)); break; }
        if (st_ == State::Handshake) handshakeLine(line);
        else if (st_ == State::WaitAnswer) answerLine(line);
        else theirLine(line);
    }
    if (inPos_ > 0 && (inPos_ > 4096 || inPos_ == in_.size())) { in_.erase(in_.begin(), in_.begin() + (long)inPos_); inPos_ = 0; }
}

void B2fSession::handshakeLine(const std::string& line) {
    const bool isSid = line.front() == '[' && line.back() == ']';
    if (isSid) {
        const size_t dash = line.rfind('-');
        remoteSid_ = upper(dash == std::string::npos ? std::string() : line.substr(dash + 1, line.size() - dash - 2));
        if (remoteSid_.find("B2") == std::string::npos) { fail("the other side does not offer B2F (SID " + line + ")"); return; }
        return;
    }
    if (cfg_.role == Role::Client) {
        if (startsWith(line, ";PQ:")) { challengeSeen_ = line.size() > 5 ? line.substr(5) : std::string(); while (!challengeSeen_.empty() && challengeSeen_.front() == ' ') challengeSeen_.erase(0, 1); return; }
        if (line.back() == '>') {
            if (remoteSid_.empty()) { fail("the gateway sent no SID"); return; }
            if (!challengeSeen_.empty() && cfg_.password.empty()) { fail("the gateway asks for the Winlink password (secure login): enter it in the Winlink settings"); return; }
            sendClientHandshake();
            ourTurn();
        }
        return;
    }
    // gateway: the caller's lines until the first protocol command
    if (startsWith(line, ";PR:")) {
        std::string r = line.size() > 5 ? line.substr(5) : std::string();
        while (!r.empty() && r.front() == ' ') r.erase(0, 1);
        if (!cfg_.expectPassword.empty() && !cfg_.challenge.empty()) loginOk_ = r == secureLoginResponse(cfg_.challenge, cfg_.expectPassword);
        return;
    }
    if (line[0] == ';') return;
    if (line[0] == 'F') {
        if (remoteSid_.empty()) { fail("the caller sent no SID"); return; }
        if (!loginOk_) {
            sendLine("*** Secure login failed - account password does not match. - Disconnecting");
            fail("secure login failed");
            return;
        }
        st_ = State::TheirTurn;
        theirLine(line);
    }
}

void B2fSession::sendClientHandshake() {
    sendLine(";FW: " + upper(cfg_.mycall));
    sendLine("[" + cfg_.appName + "-" + cfg_.appVersion + "-B2FHM$]");
    if (!challengeSeen_.empty()) sendLine(";PR: " + secureLoginResponse(challengeSeen_, cfg_.password));
    sendLine("; " + upper(cfg_.targetCall) + " DE " + upper(cfg_.mycall) + " (" + cfg_.locator + ")");
    loginOk_ = true;
}

void B2fSession::ourTurn() {
    st_ = State::OurTurn;
    txProps_.clear();
    if (h_.outbound) {
        std::set<std::string> seen;
        for (auto& p : h_.outbound()) {
            if (txProps_.size() >= kMaxBlock) break;
            if (p.csize < 6 || (int)p.cdata.size() != p.csize || p.mid.empty() || seen.count(p.mid)) continue;
            seen.insert(p.mid);
            txProps_.push_back(std::move(p));
        }
    }
    if (txProps_.empty()) {
        if (remoteNoMsgs_) { sendLine("FQ"); quitSent_ = true; st_ = State::Done; return; }
        sendLine("FF");
        st_ = State::TheirTurn;
        return;
    }
    std::vector<std::string> lines;
    for (const auto& p : txProps_) { lines.push_back(proposalLine(p)); sendLine(lines.back()); }
    char b[16];
    snprintf(b, sizeof b, "F> %02X", proposalChecksum(lines));
    sendLine(b);
    st_ = State::WaitAnswer;
}

void B2fSession::answerLine(const std::string& line) {
    if (line[0] == ';') return;
    if (!startsWith(line, "FS ")) { fail("expected the answer to our proposals, got: " + line); return; }
    std::string s = line.substr(3);
    size_t i = 0;
    for (size_t k = 0; k < s.size(); k++) {
        if (i >= txProps_.size()) { fail("answer for more proposals than were sent"); return; }
        B2Proposal& p = txProps_[i++];
        const char c = s[k];
        if (c == 'Y' || c == 'y' || c == '+') p.answer = '+';
        else if (c == 'N' || c == 'n' || c == 'R' || c == 'r' || c == '-') p.answer = '-';
        else if (c == 'L' || c == 'l' || c == '=' || c == 'H' || c == 'h') p.answer = '=';
        else if (c == 'A' || c == 'a' || c == '!') {
            size_t e = k + 1;
            while (e < s.size() && isdigit((unsigned char)s[e])) e++;
            if (e == k + 1) { fail("offset answer without an offset"); return; }
            p.answer = '+';
            p.offset = atoi(s.substr(k + 1, e - k - 1).c_str());
            if (p.offset > 999999 || p.offset >= p.csize) p.offset = 0;
            k = e - 1;
        } else { fail(std::string("bad character in the answer: ") + c); return; }
    }
    if (i != txProps_.size()) { fail("answer for fewer proposals than were sent"); return; }
    for (const auto& p : txProps_) {
        if (p.answer == '+') { sendBlock(p); pendingSent_.emplace_back(p.mid, false); }
        else if (p.answer == '-') pendingSent_.emplace_back(p.mid, true);
    }
    st_ = State::TheirTurn;
}

void B2fSession::sendBlock(const B2Proposal& p) {
    std::string title = qEncodeTitle(p.title);
    if (title.size() > 80) title.resize(80);
    if (title.empty()) title = "No title";
    const std::string off = std::to_string(p.offset);
    std::string h;
    h += (char)SOH;
    h += (char)(title.size() + off.size() + 2);
    h += title; h += '\0'; h += off; h += '\0';
    trace('>', "[message " + p.mid + ", " + std::to_string(p.csize - p.offset) + " bytes]");
    sendStr(h);
    long sum = 0;
    size_t pos = (size_t)p.offset;
    std::string chunk;
    while (pos < p.cdata.size()) {
        const size_t n = std::min(kMaxChunk, p.cdata.size() - pos);
        chunk.assign(1, (char)STX);
        chunk += (char)n;
        chunk.append((const char*)p.cdata.data() + pos, n);
        for (size_t i = 0; i < n; i++) sum += p.cdata[pos + i];
        sendStr(chunk);
        pos += n;
    }
    std::string e;
    e += (char)EOT;
    e += (char)((-sum) & 0xFF);
    sendStr(e);
}

void B2fSession::commitSent() {
    for (const auto& s : pendingSent_) {
        if (!s.second) nTx_++;
        if (h_.sent) h_.sent(s.first, s.second);
    }
    pendingSent_.clear();
}

void B2fSession::theirLine(const std::string& line) {
    if (line[0] == ';') { commitSent(); return; }
    if (line.size() < 2 || line[0] != 'F') { fail("unexpected protocol line: " + line); return; }
    commitSent();
    const std::string cmd = line.substr(0, 2);
    if (cmd == "FA" || cmd == "FB" || cmd == "FC" || cmd == "FD") {
        for (unsigned char c : line) rxSum_ += c;
        rxSum_ += '\r';
        B2Proposal p;
        if (!parseProposalLine(line, p)) {
            if (cmd == "FC" || cmd == "FD") { fail("bad proposal: " + line); return; }
            p = B2Proposal(); p.code = line[1]; p.mid = "?";
        }
        rxProps_.push_back(p);
        return;
    }
    if (cmd == "FF") { remoteNoMsgs_ = true; rxProps_.clear(); rxSum_ = 0; ourTurn(); return; }
    if (cmd == "FQ") { st_ = State::Done; return; }
    if (cmd == "F>") {
        const int ours = (int)((-rxSum_) & 0xFF);
        rxSum_ = 0;
        const int theirs = (int)strtol(line.size() > 3 ? line.c_str() + 3 : "", nullptr, 16);
        if (theirs != ours) { fail("proposal checksum error"); return; }
        if (rxProps_.empty()) { remoteNoMsgs_ = true; ourTurn(); return; }
        remoteNoMsgs_ = false;
        std::set<std::string> seen;
        std::string ans;
        for (auto& p : rxProps_) {
            if (seen.count(p.mid) || p.code != 'C') p.answer = '=';
            else p.answer = h_.inboundAnswer ? h_.inboundAnswer(p) : '+';
            if (p.answer != '+' && p.answer != '-' && p.answer != '=') p.answer = '=';
            seen.insert(p.mid);
            ans += p.answer;
        }
        sendLine("FS " + ans);
        curIdx_ = 0;
        while (curIdx_ < rxProps_.size() && rxProps_[curIdx_].answer != '+') curIdx_++;
        if (curIdx_ >= rxProps_.size()) { rxProps_.clear(); ourTurn(); return; }
        st_ = State::Receiving;
        haveHeader_ = false; dataBuf_.clear(); dataSum_ = 0;
        return;
    }
    fail("unknown protocol command: " + line);
}

bool B2fSession::receiveBinary() {
    const size_t avail = in_.size() - inPos_;
    const uint8_t* d = in_.data() + inPos_;
    B2Proposal& p = rxProps_[curIdx_];
    if (!haveHeader_) {
        if (avail < 1) return false;
        if (d[0] == '*') {   // an error line from the gateway instead of the message
            std::string l;
            if (!takeLine(l)) return false;
            fail("the other side says: " + l);
            return false;
        }
        if (d[0] != SOH) { fail("expected the start of a message block"); return false; }
        if (avail < 2) return false;
        const size_t hl = d[1];
        const uint8_t* nul1 = (const uint8_t*)memchr(d + 2, 0, avail - 2);
        if (!nul1) { if (avail > 300) fail("message header too long"); return false; }
        const size_t restAvail = avail - (size_t)(nul1 + 1 - d);
        const uint8_t* nul2 = (const uint8_t*)memchr(nul1 + 1, 0, restAvail);
        if (!nul2) { if (avail > 300) fail("message header too long"); return false; }
        const std::string title((const char*)d + 2, (size_t)(nul1 - d - 2));
        const std::string off((const char*)nul1 + 1, (size_t)(nul2 - nul1 - 1));
        if (title.size() + off.size() + 2 != hl) { fail("message header length mismatch"); return false; }
        if (atoi(off.c_str()) != p.offset) { fail("unexpected offset in the message header"); return false; }
        p.title = qDecodeTitle(title);
        inPos_ += (size_t)(nul2 + 1 - d);
        haveHeader_ = true;
        trace('<', "[message " + p.mid + " \"" + p.title + "\"]");
        return true;
    }
    if (avail < 1) return false;
    if (d[0] == STX) {
        if (avail < 2) return false;
        const size_t len = d[1] ? d[1] : 256;
        if (avail < 2 + len) return false;
        for (size_t i = 0; i < len; i++) { dataBuf_.push_back(d[2 + i]); dataSum_ = (dataSum_ + d[2 + i]) & 0xFF; }
        inPos_ += 2 + len;
        if (dataBuf_.size() > (size_t)p.csize + 256) { fail("message longer than proposed"); return false; }
        return true;
    }
    if (d[0] == EOT) {
        if (avail < 2) return false;
        dataSum_ = (dataSum_ + d[1]) & 0xFF;
        inPos_ += 2;
        if (dataSum_ != 0) { fail("message checksum error"); return false; }
        if ((int)dataBuf_.size() != p.csize) { fail("message length differs from the proposal"); return false; }
        std::vector<uint8_t> raw;
        std::string err;
        if (!lzhufDecompress(dataBuf_.data(), dataBuf_.size(), raw, true, &err)) { fail("message does not decompress: " + err); return false; }
        B2Message m;
        if (!parseMessage(raw.data(), raw.size(), m, &err)) { fail("message does not parse: " + err); return false; }
        p.cdata = dataBuf_;
        nRx_++;
        if (h_.received) h_.received(m, p);
        haveHeader_ = false; dataBuf_.clear(); dataSum_ = 0;
        curIdx_++;
        while (curIdx_ < rxProps_.size() && rxProps_[curIdx_].answer != '+') curIdx_++;
        if (curIdx_ >= rxProps_.size()) { rxProps_.clear(); ourTurn(); }
        return true;
    }
    fail("unexpected byte in the message stream");
    return false;
}

} // namespace winlink
} // namespace dect2
