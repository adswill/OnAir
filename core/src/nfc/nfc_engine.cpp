// NFC / RFID (13.56 MHz) in the engine: mixes the carrier to 0 Hz and runs the demodulator, then turns recovered frames into a
// frame log, a tag table and decoded NDEF. Receive only. See nfc_engine.h / nfc_rx.h / nfc_proto.h.
#include "dect2/nfc/nfc_engine.h"
#include <atomic>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxLog = 200;

std::string toHex(const std::vector<uint8_t>& b) {
    std::string s; char c[4];
    for (size_t i = 0; i < b.size(); i++) { snprintf(c, sizeof c, "%02X", b[i]); s += c; if (i + 1 < b.size()) s += ' '; }
    return s;
}
}  // namespace

struct NfcReceiver::Impl {
    mutable std::mutex mu;
    std::atomic<uint32_t> protoMask{0};   // setProtoMask(): 0 every protocol
    double rate = 0;
    double offsetHz = 0;
    double centerHz = 0;
    double phase = 0;
    uint64_t seq = 0;
    nfc::Demod demod{0};
    NfcTelemetry snap;
    std::function<void(const std::string&)> log;

    // session reassembly for the tag table
    NfcTag build;        // the tag being assembled from the current anticollision/select exchange
    std::vector<uint8_t> uidAcc, selAcc;
    bool haveBuild = false;
    bool crypto = false;   // a MIFARE Classic authentication was requested: what follows is encrypted (Crypto1) until the next wake-up / halt

    void emit(const std::string& s) { if (log) log(s); }

    void clearState() {
        phase = 0;
        snap = NfcTelemetry();
        snap.seq = seq;
        snap.inputRate = rate;
        snap.centerHz = centerHz;
        build = NfcTag();
        uidAcc.clear(); selAcc.clear();
        haveBuild = false;
        crypto = false;
    }

    void touch() { snap.seq = ++seq; }

    void upsertTag(const NfcTag& t) {
        for (auto& e : snap.tags) if (e.uid == t.uid && !t.uid.empty()) {
            e.lastSeen = t.lastSeen; e.seenCount++;
            if (t.haveAtqa) { e.atqa = t.atqa; e.haveAtqa = true; }
            if (t.haveSak) { e.sak = t.sak; e.haveSak = true; }
            if (!t.type.empty()) e.type = t.type;
            if (!t.ats.empty()) e.ats = t.ats;
            return;
        }
        snap.tags.push_back(t);
        snap.tags.back().seenCount = 1;
        if (snap.state < 3) snap.state = 3;
    }

    void onFrame(const nfc::RawFrame& f) {
        NfcLogEntry le;
        le.timeSec = f.timeSec;
        le.fromReader = f.fromReader;
        le.proto = f.proto == nfc::Proto::TypeA ? 0 : f.proto == nfc::Proto::TypeB ? 1 : 2;
        le.hex = toHex(f.bytes);
        le.parityOk = f.parityOk;
        le.levelDb = f.levelDb;

        if (f.proto == nfc::Proto::TypeA) {
            nfc::ATypeA a = nfc::parseTypeA(f.bytes, f.bits, f.fromReader);
            le.note = a.note;
            le.crcChecked = a.crcPresent; le.crcOk = a.crcOk;
            handleA(a, f.timeSec);
            // MIFARE Classic: AUTH A / AUTH B (0x60 / 0x61, block, CRC_A) opens an encrypted session. The traffic after it (the reader's
            // nonces, the reads and writes, and the parity bits that go with them) is Crypto1: OnAir names it and shows the bytes as
            // they came, and does not try to decode it (no keys are used or searched for).
            const bool wake = a.kind == nfc::AKind::Reqa || a.kind == nfc::AKind::Wupa || a.kind == nfc::AKind::Hlta || a.kind == nfc::AKind::AnticollReq || a.kind == nfc::AKind::SelectReq;
            if (wake) crypto = false;
            if (f.fromReader && f.bytes.size() == 4 && (f.bytes[0] == 0x60 || f.bytes[0] == 0x61) && nfc::crcAOk(f.bytes)) {
                char nb[96]; snprintf(nb, sizeof nb, "MIFARE Classic authentication request: key %c, block %u", f.bytes[0] == 0x60 ? 'A' : 'B', f.bytes[1]);
                le.note = nb; crypto = true;
            } else if (crypto && !wake && !(a.crcPresent && a.crcOk) && (a.kind == nfc::AKind::Unknown || a.kind == nfc::AKind::Apdu)) {
                le.note = "encrypted (MIFARE Classic Crypto1), not decoded";
                le.parityOk = true;   // the parity bits are encrypted too, so a failing check means nothing here
            }
        } else if (f.proto == nfc::Proto::TypeB) {
            nfc::ATypeB b = nfc::parseTypeB(f.bytes, f.fromReader);
            le.note = b.valid ? b.note : "Type B frame";
            le.crcChecked = true; le.crcOk = b.crcOk;
            if (b.isAtqb && !b.pupi.empty()) {
                std::string u; for (uint8_t v : b.pupi) { char c[3]; snprintf(c, sizeof c, "%02X", v); u += c; }
                NfcTag t; t.uid = u; t.type = "ISO 14443-B"; t.firstSeen = t.lastSeen = f.timeSec;
                upsertTag(t);
            }
        } else {
            nfc::AVicinity v = nfc::parseVicinity(f.bytes, f.fromReader);
            le.note = v.note.empty() ? "ISO 15693 frame" : v.note;
            le.crcChecked = true; le.crcOk = v.crcOk;
            if (!f.fromReader && !v.uid.empty()) {
                std::string u; for (uint8_t b2 : v.uid) { char c[3]; snprintf(c, sizeof c, "%02X", b2); u += c; }
                NfcTag t; t.uid = u; t.type = "ISO 15693"; t.firstSeen = t.lastSeen = f.timeSec;
                const char* mf = nfc::vicinityManufacturer(v.manufacturer);
                if (mf[0]) { t.type += " ("; t.type += mf; t.type += ")"; }
                upsertTag(t);
            }
        }

        snap.blocksOk++;
        if (le.crcChecked && !le.crcOk) snap.blocksBad++;
        if (!le.parityOk) snap.blocksBad++;
        snap.dataValid = true;
        if (snap.state < 2) snap.state = 2;
        snap.log.push_back(le);
        if (snap.log.size() > kMaxLog) snap.log.erase(snap.log.begin(), snap.log.begin() + (snap.log.size() - kMaxLog));
        snap.levelDb = f.levelDb;
        snap.subcarrierDb = demod.peakSubcarrierDb();
        touch();
    }

    void handleA(const nfc::ATypeA& a, double t) {
        using K = nfc::AKind;
        if (a.fromReader) {
            if (a.kind == K::Reqa || a.kind == K::Wupa) { build = NfcTag(); uidAcc.clear(); selAcc.clear(); haveBuild = true; build.firstSeen = t; }
            // The reader's SELECT names the tag too (its UID, with BCC and CRC_A checked). A weak capture of a phone or a reader next to
            // the antenna hears the field well and the tag's answer not at all, so the tag table is filled from it as well.
            if (a.kind == K::SelectReq && a.bccOk && a.crcPresent && a.crcOk && a.uidPart.size() == 4) {
                if (a.cascadeLevel == 1) selAcc.clear();
                if (a.cascadeTag) { for (int i = 1; i < 4; i++) selAcc.push_back(a.uidPart[(size_t)i]); }
                else {
                    for (uint8_t v : a.uidPart) selAcc.push_back(v);
                    NfcTag nt; nt.uid = toHex(selAcc); nt.uid.erase(std::remove(nt.uid.begin(), nt.uid.end(), ' '), nt.uid.end());
                    nt.type = "ISO 14443-A (UID from the reader's SELECT)"; nt.firstSeen = nt.lastSeen = t;
                    upsertTag(nt);
                    char b[96]; snprintf(b, sizeof b, "NFC: tag %s selected by the reader", nt.uid.c_str()); emit(b);
                    selAcc.clear();
                }
            }
            return;
        }
        // tag answers
        if (a.kind == K::Atqa) {
            if (!haveBuild) { build = NfcTag(); build.firstSeen = t; haveBuild = true; }
            build.atqa = a.atqa; build.haveAtqa = true;
        } else if (a.kind == K::AnticollResp) {
            if (a.cascadeTag && a.uidPart.size() == 4) for (int i = 1; i < 4; i++) uidAcc.push_back(a.uidPart[(size_t)i]);   // drop the 0x88 cascade tag
            else for (uint8_t v : a.uidPart) uidAcc.push_back(v);
        } else if (a.kind == K::Sak) {
            build.sak = a.sak; build.haveSak = true;
            build.type = a.iso144434 ? "ISO 14443-A (ISO 14443-4)" : "ISO 14443-A";
            std::string u; for (uint8_t v : uidAcc) { char c[3]; snprintf(c, sizeof c, "%02X", v); u += c; }
            build.uid = u;
            build.lastSeen = t;
            if (!build.uid.empty()) { upsertTag(build); char b[96]; snprintf(b, sizeof b, "NFC: tag %s, SAK 0x%02X", build.uid.c_str(), build.sak); emit(b); }
        } else if (a.kind == K::Ats) {
            build.ats = toHex(a.historical);
            if (!build.uid.empty()) upsertTag(build);
        } else if (a.kind == K::Apdu) {
            // a Type 2 READ answer carries 16 bytes of tag memory; try to read an NDEF message from it
            if (a.raw.size() >= 16) {
                std::vector<uint8_t> mem(a.raw.begin(), a.raw.end() - (a.crcPresent ? 2 : 0));
                nfc::NdefMessage msg = nfc::parseType2Ndef(mem);
                if (msg.valid) {
                    snap.ndef.clear();
                    for (const auto& r : msg.records) {
                        NfcNdef nd; nd.uid = build.uid; nd.tnf = r.tnf; nd.type = r.type;
                        nd.text = r.text; nd.uri = r.uri; nd.lang = r.lang;
                        snap.ndef.push_back(nd);
                    }
                    emit("NFC: NDEF message decoded");
                }
            }
        }
    }
};

NfcReceiver::NfcReceiver() : p_(new Impl) {
    p_->demod.setFrameCallback([this](const nfc::RawFrame& f) { p_->onFrame(f); });
}
NfcReceiver::~NfcReceiver() = default;

void NfcReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rate = inputRateHz;
    p_->demod.setFs(inputRateHz);
    p_->clearState();
}

void NfcReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->offsetHz = hz; }

void NfcReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->centerHz = hz; p_->snap.centerHz = hz;
}

bool NfcReceiver::ready() const {
    std::lock_guard<std::mutex> l(p_->mu);
    return p_->rate >= nfcTuning().minSampleRate;
}

void NfcReceiver::reset() {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->demod.reset();
    if (p_->rate > 0) p_->clearState();
}

void NfcReceiver::feed(const std::complex<float>* x, size_t n) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& s = *p_;
    if (s.rate < nfcTuning().minSampleRate) return;
    const double step = -2.0 * kPi * s.offsetHz / s.rate;
    if (s.offsetHz != 0) {
        std::vector<std::complex<float>> mixed(n);
        for (size_t i = 0; i < n; i++) {
            mixed[i] = x[i] * std::complex<float>((float)std::cos(s.phase), (float)std::sin(s.phase));
            s.phase += step; if (s.phase > kPi) s.phase -= 2 * kPi; if (s.phase < -kPi) s.phase += 2 * kPi;
        }
        s.demod.feed(mixed.data(), n);
    } else {
        s.demod.feed(x, n);
    }
    if (s.snap.state == 0 && s.demod.carrierDb() > -120) { s.snap.state = 1; s.snap.levelDb = s.demod.carrierDb(); s.touch(); }
    s.snap.subcarrierDb = s.demod.peakSubcarrierDb();
}

bool NfcReceiver::telemetry(NfcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.inputRate = p_->rate;
    out.centerHz = p_->centerHz;
    if (const uint32_t m = p_->protoMask.load(std::memory_order_relaxed)) {   // a view onto one protocol: its frames, tags and NDEF records only
        auto protoOfType = [](const std::string& t) { return t.find("14443-B") != std::string::npos ? 1 : t.find("15693") != std::string::npos ? 2 : 0; };
        std::vector<NfcLogEntry> lk; for (const auto& e : out.log) if ((m >> e.proto) & 1u) lk.push_back(e);
        std::vector<NfcTag> tk; std::set<std::string> uids; for (const auto& t : out.tags) if ((m >> protoOfType(t.type)) & 1u) { tk.push_back(t); uids.insert(t.uid); }
        std::vector<NfcNdef> nk; for (const auto& n : out.ndef) if (uids.count(n.uid)) nk.push_back(n);
        out.log.swap(lk); out.tags.swap(tk); out.ndef.swap(nk);
        out.blocksOk = out.log.size();
        out.dataValid = !out.log.empty();
        if (out.state >= 2 && out.log.empty()) out.state = 1;
        else if (out.state >= 3 && out.tags.empty()) out.state = 2;
    }
    return true;
}
void NfcReceiver::setProtoMask(uint32_t mask) { p_->protoMask.store(mask, std::memory_order_relaxed); }

void NfcReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->log = std::move(cb);
}

ModeTuning nfcTuning() {
    ModeTuning t;
    t.stdMode = 150;                 // family 148 + 2
    t.id = "nfc";
    t.name = "NFC / RFID 13.56 MHz";
    t.minMhz = 13.553;               // the 13.56 MHz ISM band
    t.maxMhz = 13.567;
    t.defMhz = 13.56;
    t.sampleRate = 4000000;          // covers the +-847.5 kHz subcarrier sidebands with room to spare
    t.basebandHz = 0;
    t.bandwidthMhz = 2.4;
    t.minSampleRate = 2000000;       // +-1 MHz holds the 847.5 kHz subcarrier and its data sidebands; the 2 Msps recordings of an RTL-SDR / HackRF are taken (#271)
    t.tuneOffsetHz = 0;              // the carrier is the wanted signal and sits at the centre
    t.carrierAtCentre = true;        // do not let the DC-spike removal eat the carrier
    t.notCircular = true;            // not circular around the centre
    return t;
}

std::string nfcSummary(const NfcTelemetry& t) {
    char b[160];
    if (t.state == 0) return "NFC: waiting for samples";
    if (t.state == 1) return "NFC: 13.56 MHz field present, no frames decoded";
    if (!t.tags.empty()) {
        snprintf(b, sizeof b, "NFC: %d tag(s), %llu frame(s)", (int)t.tags.size(), (unsigned long long)t.blocksOk);
        return b;
    }
    snprintf(b, sizeof b, "NFC: %llu frame(s) decoded", (unsigned long long)t.blocksOk);
    return b;
}

}  // namespace dect2
