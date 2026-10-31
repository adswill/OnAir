// Multi-block ACARS message reassembly, shared by the ACARS and VDL2 receivers. A long message (weather, flight plan, ADS-C) is sent
// as several blocks: every block but the last ends in ETB, the last in ETX. The blocks of one message share the address, the label and
// (downlinks) the message number M##, and a downlink's block sequence letter goes A, B, C ... This holds the ETB blocks back and gives
// one complete message when the ETX block arrives, or the partial message flagged incomplete when the next block does not come in time.
// Pure data and rules: no radio, no UI.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

constexpr double kAcarsReasmTimeoutSec = 45.0;     // signal seconds without the next block before the partial message is given out
constexpr size_t kAcarsReasmMaxPending = 64;
constexpr size_t kAcarsReasmMaxText = 8192;

// What identifies a block's message.
struct AcarsPiece {
    std::string reg, label, msgNum;
    bool downlink = true;
    char blockId = 0;            // the block's own id: a resend keeps it
    char msgSeq = 0;             // downlink block sequence letter ('A'..), 0 when the block has none
    bool finalBlock = true;      // ETX
};

template <class T, std::string T::*Text>
class AcarsReassembler {
public:
    struct Done { T msg; int blocks = 1; bool incomplete = false; };   // msg.*Text holds all the text received

    enum class Result { Single, Held, Complete };

    // A block arrived. Single: not part of a multi-block message, the caller handles it as is (and `out` is not set). Held: stored,
    // nothing to show yet. Complete: `out` is the whole message (also when an earlier, different message was cut off: that one is
    // in `cut`, partial, and the caller shows it first).
    Result feed(const AcarsPiece& p, T&& msg, double now, Done& out, std::vector<Done>& cut) {
        flush(now, cut);
        const std::string key = p.reg + "|" + (p.downlink ? "D" : "U") + "|" + p.label;
        Entry* e = find(key);
        if (e && p.downlink && !e->piece.msgNum.empty() && !p.msgNum.empty() && e->piece.msgNum != p.msgNum) {
            cut.push_back(finish(*e, true));                // another message of the same aircraft and label: the old one lost its end
            erase(e);
            e = nullptr;
        }
        if (!e) {
            if (p.finalBlock) return Result::Single;
            if (pending_.size() >= kAcarsReasmMaxPending) { cut.push_back(finish(pending_.front(), true)); pending_.erase(pending_.begin()); }
            Entry n; n.key = key; n.piece = p; n.first = std::move(msg); n.lastSeen = now; n.lastSeq = p.msgSeq; n.lastId = p.blockId; n.blocks = 1;
            pending_.push_back(std::move(n));
            return Result::Held;
        }
        if ((p.msgSeq && e->lastSeq && p.msgSeq == e->lastSeq) || (!p.msgSeq && p.blockId && p.blockId == e->lastId)) { e->lastSeen = now; return Result::Held; }   // a repeat of the last block
        if (p.msgSeq && e->lastSeq) {
            if (p.msgSeq < e->lastSeq) { e->gap = true; }                  // out of order: cannot be joined cleanly
            else if (p.msgSeq > e->lastSeq + 1) e->gap = true;             // a block was missed
        }
        const std::string& add = msg.*Text;
        if (e->text.size() + add.size() <= kAcarsReasmMaxText) e->text += add;
        else e->gap = true;
        e->blocks++; e->lastSeen = now; if (p.msgSeq) e->lastSeq = p.msgSeq;
        e->lastId = p.blockId;
        if (!p.finalBlock) return Result::Held;
        out = finish(*e, e->gap);
        erase(e);
        return Result::Complete;
    }

    // Give out the messages whose next block is overdue.
    void flush(double now, std::vector<Done>& out) {
        for (size_t i = 0; i < pending_.size();) {
            if (now - pending_[i].lastSeen > kAcarsReasmTimeoutSec || now < pending_[i].lastSeen - 1.0) {
                out.push_back(finish(pending_[i], true));
                pending_.erase(pending_.begin() + (long)i);
            } else i++;
        }
    }
    void flushAll(std::vector<Done>& out) {
        for (auto& e : pending_) out.push_back(finish(e, true));
        pending_.clear();
    }
    size_t pending() const { return pending_.size(); }
    void clear() { pending_.clear(); }

private:
    struct Entry {
        std::string key; AcarsPiece piece; T first{}; std::string text;
        double lastSeen = 0; char lastSeq = 0, lastId = 0; int blocks = 1; bool gap = false;
    };
    std::vector<Entry> pending_;

    Entry* find(const std::string& key) { for (auto& e : pending_) if (e.key == key) return &e; return nullptr; }
    void erase(Entry* e) { pending_.erase(pending_.begin() + (e - pending_.data())); }
    Done finish(Entry& e, bool incomplete) {
        Done d; d.msg = std::move(e.first);
        d.msg.*Text += e.text;          // the first block's text, then the others
        d.blocks = e.blocks; d.incomplete = incomplete;
        return d;
    }
};

} // namespace dect2
