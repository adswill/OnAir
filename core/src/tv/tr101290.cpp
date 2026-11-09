#include "dect2/tv/tr101290.h"
#include "dect2/tv/ts_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
enum { SyncLoss, SyncByte, PatErr, CcErr, PmtErr, PidErr, TeiErr, CrcErr, PcrRep, PcrAcc, PtsErr, CatErr, NitErr, SiRep, UnrefPid, SdtErr, EitErr, RstErr, TdtErr, kN };
const struct { const char* n; int p; } kNames[kN] = {
    {"1.1 TS_sync_loss", 1}, {"1.2 Sync_byte_error", 1}, {"1.3 PAT_error", 1}, {"1.4 Continuity_count_error", 1}, {"1.5 PMT_error", 1}, {"1.6 PID_error", 1},
    {"2.1 Transport_error", 2}, {"2.2 CRC_error", 2}, {"2.3 PCR_repetition_error", 2}, {"2.4 PCR_accuracy_error", 2}, {"2.5 PTS_error", 2}, {"2.6 CAT_error", 2},
    {"3.1 NIT_error", 3}, {"3.2 SI_repetition_error", 3}, {"3.4 Unreferenced_PID", 3}, {"3.5 SDT_error", 3}, {"3.6 EIT_error", 3}, {"3.7 RST_error", 3}, {"3.8 TDT_error", 3}};
constexpr double kPat = 0.5, kPmt = 0.5, kPid = 5.0, kPcr = 0.04, kPts = 0.7, kNit = 10, kSdt = 2, kEit = 2, kTdt = 30;
}

Tr290Monitor::Tr290Monitor() { reset(); }
void Tr290Monitor::reset() { c_.assign(kN, {}); for (int i = 0; i < kN; i++) { c_[(size_t)i].name = kNames[i].n; c_[(size_t)i].priority = kNames[i].p; } n_ = 0; syncRun_ = 0; synced_ = false; cc_.clear(); lastSeen_.clear(); lastSection_.clear(); ccInit_.clear(); pcrLast_.clear(); pcrWall_.clear(); ptsLast_.clear(); sec_.clear(); pmtPids_.clear(); pcrPids_.clear(); esPids_.clear(); known_.clear(); jitterMs_ = 0; lineA_ = lineB_ = 0; lineN_ = 0; lastCheck_ = -1; }
void Tr290Monitor::fire(int i, double t) { c_[(size_t)i].count++; c_[(size_t)i].lastSec = t; }

void Tr290Monitor::section(int pid, const uint8_t* s, int len, double t) {
    if (len < 8) return;
    const int tid = s[0];
    const bool crc = tid != 0x70 && (tid < 0x72 || tid > 0x73);
    if (crc && tsp::crc32(s, len) != 0) { fire(CrcErr, t); return; }
    if (pid == 0 && tid == 0) {
        if (lastSection_.count(0) && t - lastSection_[0] > kPat) fire(PatErr, t);
        lastSection_[0] = t;
        std::vector<tsp::PatEntry> e; int ts = 0;
        tsp::parsePat(s, len, ts, e);
        for (auto& x : e) if (x.prog != 0) pmtPids_[x.pid] = x.prog;
    } else if (tid == 2 && pmtPids_.count(pid)) {
        if (lastSection_.count(pid) && t - lastSection_[pid] > kPmt) fire(PmtErr, t);
        lastSection_[pid] = t;
        // PCR PID and elementary streams
        const int pil = (s[10] & 0x0F) << 8 | s[11];
        const int pcr = (s[8] & 0x1F) << 8 | s[9];
        pcrPids_[pcr] = true; known_[pcr] = true;
        int i = 12 + pil;
        while (i + 5 <= len - 4) { const int ep = (s[i + 1] & 0x1F) << 8 | s[i + 2]; const int es = (s[i + 3] & 0x0F) << 8 | s[i + 4]; esPids_[ep] = true; known_[ep] = true; i += 5 + es; }
    } else if (pid == 1 && tid == 1) {
        if (lastSection_.count(1) && t - lastSection_[1] > 0.5) fire(CatErr, t);
        lastSection_[1] = t;
    } else if (pid == 0x10) { if (lastSection_.count(0x10) && t - lastSection_[0x10] > kNit) fire(NitErr, t); lastSection_[0x10] = t; }
    else if (pid == 0x11) { if (lastSection_.count(0x11) && t - lastSection_[0x11] > kSdt) fire(SdtErr, t); lastSection_[0x11] = t; }
    else if (pid == 0x12) { if (lastSection_.count(0x12) && t - lastSection_[0x12] > kEit) fire(EitErr, t); lastSection_[0x12] = t; }
    else if (pid == 0x14) { if (lastSection_.count(0x14) && t - lastSection_[0x14] > kTdt) fire(TdtErr, t); lastSection_[0x14] = t; }
}

void Tr290Monitor::feed(const uint8_t* p, double t) {
    n_++;
    if (p[0] != 0x47) { fire(SyncByte, t); syncRun_ = 0; if (synced_) { fire(SyncLoss, t); synced_ = false; } }
    else { if (!synced_ && ++syncRun_ >= 5) synced_ = true; }
    if (p[0] != 0x47) return;
    const int pid = (p[1] & 0x1F) << 8 | p[2];
    if (p[1] & 0x80) { fire(TeiErr, t); return; }
    if (pid == 0x1FFF) return;
    lastSeen_[pid] = t;
    const int afc = (p[3] >> 4) & 3, cc = p[3] & 15;
    if (afc & 1) {
        auto it = cc_.find(pid);
        if (it != cc_.end() && !(cc == it->second && (afc & 1)) && cc != ((it->second + 1) & 15)) fire(CcErr, t);
        cc_[pid] = cc;
    }
    int off = 4;
    if (afc & 2) {
        const int al = p[4];
        if (al >= 7 && (p[5] & 0x10)) {
            const int64_t base = ((int64_t)p[6] << 25) | (p[7] << 17) | (p[8] << 9) | (p[9] << 1) | (p[10] >> 7);
            const int ext = ((p[10] & 1) << 8) | p[11];
            const double pcr = (double)(base * 300 + ext) / 27e6;
            if (pcrLast_.count(pid)) {
                const double dv = t - pcrWall_[pid];
                if (dv > kPcr) fire(PcrRep, t);
                // accuracy: PCR against the arrival time, from a slow-moving straight line
                const double dp = pcr - pcrLast_[pid];
                if (dv > 0 && dv < 1 && dp > 0 && dp < 1) {
                    lineA_ = lineN_ == 0 ? t - pcr : lineA_ + 0.05 * ((t - pcr) - lineA_);
                    lineN_++;
                    const double err = (t - pcr) - lineA_;
                    jitterMs_ = std::max(jitterMs_, std::fabs(err) * 1e3);
                    if (lineN_ > 20 && std::fabs(err) > 0.0005) fire(PcrAcc, t);
                }
            }
            pcrLast_[pid] = pcr; pcrWall_[pid] = t;
        }
        off += 1 + al;
    }
    if (off >= 188 || !(afc & 1)) return;
    const bool pusi = p[1] & 0x40;
    const bool psi = pid == 0 || pid == 1 || pid == 0x10 || pid == 0x11 || pid == 0x12 || pid == 0x14 || pmtPids_.count(pid);
    if (psi) {
        auto& b = sec_[pid];
        int d = off;
        if (pusi) { const int ptr = p[d]; d++; if (d + ptr <= 188) { b.insert(b.end(), p + d, p + d + ptr); } section(pid, b.data(), (int)b.size(), t); b.clear(); d += ptr; if (d > 188) d = 188; if (d < 188) b.assign(p + d, p + 188); }
        else if (!b.empty()) b.insert(b.end(), p + d, p + 188);
        if (b.size() >= 3) { const size_t l = 3u + ((size_t)(b[1] & 0x0F) << 8 | b[2]); if (b.size() >= l) { section(pid, b.data(), (int)l, t); b.clear(); } }
    }
    // PTS on PES starts of the elementary streams
    if (pusi && esPids_.count(pid) && off + 14 <= 188 && p[off] == 0 && p[off + 1] == 0 && p[off + 2] == 1) {
        if ((p[off + 7] >> 6) & 2) {
            if (ptsLast_.count(pid) && t - ptsLast_[pid] > kPts) fire(PtsErr, t);
            ptsLast_[pid] = t;
        }
    }
    // the once-a-second checks: PID repetition, unreferenced PIDs
    if (lastCheck_ < 0 || t - lastCheck_ >= 1.0) {
        lastCheck_ = t;
        for (auto& kv : esPids_) { auto s = lastSeen_.find(kv.first); if (s != lastSeen_.end() && t - s->second > kPid) { fire(PidErr, t); lastSeen_[kv.first] = t; } }
        for (auto& kv : lastSeen_) {
            const int q = kv.first;
            if (q < 0x20 || q == 0x1FFB || pmtPids_.count(q) || known_.count(q)) continue;
            if (!pmtPids_.empty()) fire(UnrefPid, t);
        }
    }
}

}  // namespace dect2
