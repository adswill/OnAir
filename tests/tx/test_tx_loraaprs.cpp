// The LoRa APRS transmit mode ("loraaprs"): the TNC2 packet built from the panel's fields, the radio settings of the presets and of the
// own settings, and the transmitted IQ decoded back by OnAir's LoRa demodulator: the payload is '<' 0xFF 0x01 + the packet, on the chosen
// spreading factor, bandwidth, coding rate and sync word (a demodulator on another sync word hears nothing). Generated into memory;
// NOTHING is transmitted. Before this mode, LoRa APRS in the transmitter sent a Meshtastic packet (sync 0x2B, 250 kHz).
#include "dect2/tx/tx.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include "../data/mesh/testutil.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using meshtest::Chain;

static std::vector<cf32> make(const TxRequest& r, double rate, double secs) {
    std::string err;
    const TxModeSpec* m = txModeById("loraaprs");
    CHECK(m != nullptr, "the loraaprs mode is on the list");
    if (!m) return {};
    auto g = makeTxGenerator(*m, r, rate, err);
    CHECK(g != nullptr, "generator: %s", err.c_str());
    if (!g) return {};
    std::vector<cf32> x((size_t)(secs * rate));
    for (size_t i = 0; i < x.size(); i += 65536) g->generate(x.data() + i, std::min<size_t>(65536, x.size() - i));
    return x;
}

static lora::Params paramsOf(const TxLoraAprsRadio& lr) {
    lora::Params p;
    p.sf = lr.sf; p.bwHz = lr.bwHz; p.cr = lr.cr; p.preamble = lr.preamble; p.syncWord = (uint8_t)lr.syncWord; p.crc = lr.crc;
    p.ldro = meshLoraLdro(lr.sf, lr.bwHz);
    return p;
}

static void packetText() {
    printf("TNC2 packet:\n");
    TxRequest r;
    CHECK(txLoraAprsTnc2(r).empty(), "no callsign: no packet (the demo stations are sent)");
    r.callsign = " on4abc-7 ";
    r.text = "OnAir test";
    CHECK(txLoraAprsTnc2(r) == "ON4ABC-7>APRS,WIDE1-1:>OnAir test", "defaults: '%s'", txLoraAprsTnc2(r).c_str());
    r.dest = "aplrt1"; r.path = "-";
    CHECK(txLoraAprsTnc2(r) == "ON4ABC-7>APLRT1:>OnAir test", "no path: '%s'", txLoraAprsTnc2(r).c_str());
    r.path = "WIDE1-1, WIDE2-1";
    r.lat = 50.85045; r.lon = -4.34878;
    const std::string pos = txLoraAprsTnc2(r);
    CHECK(pos == "ON4ABC-7>APLRT1,WIDE1-1,WIDE2-1:!5051.03N/00420.93W-OnAir test", "position: '%s'", pos.c_str());
    // the receiver's own LoRa APRS parser reads it back
    const auto b = loraAprsFrame(pos);
    const MeshDecodeResult d = meshDecodeLoraAprs(b.data(), b.size());
    CHECK(d.ok && d.aprsSource == "ON4ABC-7" && d.aprsDest == "APLRT1" && d.aprsPath == "WIDE1-1,WIDE2-1", "parsed header %s %s %s", d.aprsSource.c_str(), d.aprsDest.c_str(), d.aprsPath.c_str());
    CHECK(d.aprs.hasPos && std::fabs(d.aprs.lat - 50.85045) < 1e-3 && std::fabs(d.aprs.lon + 4.34878) < 1e-3 && d.aprs.comment == "OnAir test", "parsed position %.5f %.5f '%s'",
          d.aprs.lat, d.aprs.lon, d.aprs.comment.c_str());
}

static void radioSettings() {
    printf("radio settings:\n");
    TxRequest r;
    TxLoraAprsRadio p = txLoraAprsRadio(r);
    CHECK(p.mhz == 433.775 && p.sf == 12 && p.bwHz == 125000 && p.cr == 5 && p.preamble == 8 && p.syncWord == 0x12 && p.crc, "EU default");
    r.opt["lora.preset"] = 1;
    p = txLoraAprsRadio(r);
    CHECK(p.mhz == 434.855 && p.sf == 9 && p.cr == 7 && p.syncWord == 0x12, "PL");
    r.opt["lora.preset"] = 2;
    CHECK(txLoraAprsRadio(r).mhz == 439.9125, "UK");
    r.opt["lora.preset"] = 4;
    r.opt["lora.sf"] = 7; r.opt["lora.bwIdx"] = 2; r.opt["lora.cr"] = 8; r.opt["lora.preamble"] = 12; r.opt["lora.sync"] = 0x34; r.opt["lora.crc"] = 0;
    p = txLoraAprsRadio(r);
    CHECK(p.mhz == 0 && p.sf == 7 && p.bwHz == 250000 && p.cr == 8 && p.preamble == 12 && p.syncWord == 0x34 && !p.crc, "own settings");
    r.opt["lora.sf"] = 40; r.opt["lora.bwIdx"] = 9; r.opt["lora.sync"] = 999; r.opt["lora.cr"] = 1;
    p = txLoraAprsRadio(r);
    CHECK(p.sf == 12 && p.bwHz == 500000 && p.syncWord == 255 && p.cr == 5, "out of range values are clamped");
    CHECK(txModeDefaultMhz(*txModeById("loraaprs")) == 433.775, "the panel starts on 433.775 MHz");
    CHECK(std::fabs(txModeDefaultMhz(*txModeById("mesh")) - 869.525) < 1e-9, "Mesh (LoRa) keeps 869.525 MHz");
    CHECK(txPayloadKind("loraaprs") == TxPayloadKind::LoraAprs, "its own payload fields");
}

// the transmitted IQ through a LoRa demodulator on `lr`'s settings: the frames it finds
static std::vector<lora::RxFrame> decode(const std::vector<cf32>& x, double rate, const TxLoraAprsRadio& lr) {
    Chain ch(rate, -meshTuning().tuneOffsetHz, paramsOf(lr));   // the signal sits tuneOffsetHz below the radio's centre = the dial frequency
    ch.feedAll(x, 8192);
    return ch.frames;
}

static void roundTrip(const char* what, TxRequest r, double secs) {
    const double rate = 1e6;
    const TxLoraAprsRadio lr = txLoraAprsRadio(r);
    const std::string tnc2 = txLoraAprsTnc2(r);
    const auto x = make(r, rate, secs);
    if (x.empty()) return;
    const auto fr = decode(x, rate, lr);
    const auto want = loraAprsFrame(tnc2);
    {   // the frame fills its LoRa bandwidth: power spectra of 4096 samples averaged from 0.1 to 1 s (the preamble and data; one window
        // alone sees only the part of a chirp it catches)
        const size_t nfft = 4096;
        std::vector<double> acc(nfft, 0.0);
        std::vector<float> db;
        for (size_t at = (size_t)(0.1 * rate); at + nfft <= std::min(x.size(), (size_t)rate); at += nfft) {
            txSpectrumDb(x.data() + at, nfft, (int)nfft, db);
            for (size_t i = 0; i < nfft; i++) acc[i] += std::pow(10.0, db[i] / 10.0);
        }
        for (size_t i = 0; i < nfft; i++) db[i] = (float)(10.0 * std::log10(std::max(acc[i], 1e-30)));
        const double obw = txOccupiedBandwidthHz(db, rate);
        printf("  %s: occupied bandwidth %.1f kHz\n", what, obw / 1e3);
        CHECK(obw > 0.8 * lr.bwHz && obw < 1.3 * lr.bwHz, "%s: occupied bandwidth %.1f kHz for a %.0f kHz LoRa signal", what, obw / 1e3, lr.bwHz / 1e3);
    }
    int good = 0;
    for (const auto& f : fr) if (f.crcOk == lr.crc && f.payload == want) good++;
    printf("  %s: SF%d %.0f kHz 4/%d sync 0x%02X: %zu frames, %d with the packet\n", what, lr.sf, lr.bwHz / 1e3, lr.cr, lr.syncWord, fr.size(), good);
    CHECK(good >= 1, "%s: the packet decodes back (%zu frames)", what, fr.size());
    // the same signal through a demodulator on another sync word: nothing
    TxLoraAprsRadio other = lr;
    other.syncWord = lr.syncWord == 0x12 ? 0x2B : 0x12;
    const auto fo = decode(x, rate, other);
    int wrong = 0;
    for (const auto& f : fo) if (f.payload == want) wrong++;
    CHECK(wrong == 0, "%s: a receiver on sync word 0x%02X does not get the packet (%d)", what, other.syncWord, wrong);
}

static void roundTrips() {
    printf("round trips:\n");
    TxRequest r;
    r.callsign = "ON4ABC-7";
    r.text = "LoRa APRS from OnAir";
    r.lat = 50.85; r.lon = 4.35;
    roundTrip("EU preset", r, 3.5);
    r.opt["lora.preset"] = 4;
    r.opt["lora.sf"] = 9; r.opt["lora.bwIdx"] = 1; r.opt["lora.cr"] = 7; r.opt["lora.preamble"] = 10; r.opt["lora.sync"] = 0x34; r.opt["lora.crc"] = 1;
    roundTrip("own settings", r, 1.2);
    r.opt["lora.gapSec"] = 0.5;
    r.opt["lora.sf"] = 7;
    const auto x = make(r, 1e6, 2.0);
    const auto fr = decode(x, 1e6, txLoraAprsRadio(r));
    CHECK(fr.size() >= 3, "sent again after the pause: %zu frames in 2 s", fr.size());
}

static void demo() {
    printf("demo (no callsign):\n");
    TxRequest r;
    const auto x = make(r, 2e6, 0.5);
    double e = 0;
    for (const auto& v : x) e += std::norm(v);
    CHECK(!x.empty() && std::isfinite(e), "the demo scene is generated");
}

int main() {
    packetText();
    radioSettings();
    roundTrips();
    demo();
    if (meshtest::fails) { printf("%d FAILED\n", meshtest::fails); return 1; }
    printf("all LoRa APRS transmit checks passed\n");
    return 0;
}
