// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// View > Protocol lab: a workbench for an unknown digital signal. Load a recording (or paste packets), pick the signal inside it, and the
// lab tries demodulators, finds the baud rate, slices bits, tries the line codings, looks for a preamble / sync word and a CRC, and shows
// which bytes are constant, counters, checksums or data. The result exports as a protocol description. The stages are in
// core/src/protolab; this file is the window only. Nothing here decrypts anything.
#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "../app.h"
#include "dect2/common/u8file.h"
#include "dect2/protolab/protolab.h"
#include "dect2/modebuilder/modebuilder.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
namespace pl = dect2::protolab;

struct State {
    // input
    char path[512] = "";
    int fmt = 0;                    // 0 cf32, 1 cs16, 2 cu8, 3 cs8
    double rate = 250000;           // Hz
    double offset = 0, bw = 0;      // the signal inside the capture; bw 0 = use the whole capture
    int demod = 0;                  // 0 auto, then OOK, 2FSK, 4FSK, BPSK, QPSK, FM
    double baud = 0;
    float gapMs = 4;
    int demo = 0;
    std::string pasted;
    bool pastedBits = false;
    char name[64] = "unknown protocol";
    // run
    std::thread worker;
    std::atomic<int> run{0};        // 0 idle, 1 running, 2 finished
    std::mutex mu;
    std::string msg, err;
    // result
    pl::Report rep;
    double usedRate = 0;
    int sel = 0;                    // candidate
    pl::FieldReport fields;
    bool haveResult = false;
    int selPkt = 0, selByte = -1;
    int cmpA = 0, cmpB = 1;
    ~State() { if (worker.joinable()) worker.join(); }
};
State S;

pl::Demod demodOf(int i) {
    static const pl::Demod d[] = {pl::Demod::Auto, pl::Demod::OOK, pl::Demod::FSK2, pl::Demod::FSK4, pl::Demod::PSK2, pl::Demod::PSK4, pl::Demod::FM};
    return d[std::clamp(i, 0, 6)];
}

int crcBytesOf(const pl::Candidate& c) {
    for (const pl::CrcHit& h : c.crc)
        if (h.total >= 3 && h.matches >= 3 && h.matches * 10 >= h.total * 6) return !h.isChecksum && h.endSlack == 0 ? h.width / 8 : 0;
    return 0;
}

void setResult(pl::Report&& r, double rate) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.rep = std::move(r);
    S.usedRate = rate;
    S.sel = 0;
    S.selPkt = 0;
    S.selByte = -1;
    S.cmpA = 0;
    S.cmpB = 1;
    S.haveResult = true;
    S.fields = S.rep.candidates.empty() ? pl::FieldReport{} : pl::analyseFields(S.rep.candidates[0].packets, crcBytesOf(S.rep.candidates[0]));
}

pl::AnalyseOptions optionsNow() {
    pl::AnalyseOptions o;
    o.demod = demodOf(S.demod);
    o.baud = S.baud;
    o.bursts.minGapSec = S.gapMs / 1000.0;
    return o;
}

void startWork(std::function<void()> job) {
    if (S.worker.joinable()) S.worker.join();
    S.run = 1;
    S.err.clear();
    S.worker = dect2::namedThread("onair-protolab", [job] {
        try { job(); } catch (const std::exception& e) { DIAG(912131, "A Protocol lab job stopped with an error (%s), so its result is missing.", e.what()); std::lock_guard<std::mutex> lk(S.mu); S.err = e.what(); }
        S.run = 2;
    });
}

void analyseFile() {
    const std::string path = S.path;
    static const pl::IqFormat fmts[] = {pl::IqFormat::Cf32, pl::IqFormat::Cs16, pl::IqFormat::Cu8, pl::IqFormat::Cs8};
    const pl::IqFormat f = fmts[std::clamp(S.fmt, 0, 3)];
    const double rate = S.rate, off = S.offset, bw = S.bw;
    const pl::AnalyseOptions o = optionsNow();
    startWork([=] {
        std::vector<pl::cf> iq;
        std::string err;
        if (!pl::loadIq(path, f, iq, err, 30000000)) { std::lock_guard<std::mutex> lk(S.mu); S.err = err; return; }
        double r = rate;
        if (bw > 0 || off != 0) iq = pl::channelise(iq, rate, off, bw > 0 ? bw : rate / 4, r);
        setResult(pl::analyse(iq, r, o), r);
    });
}

void analyseDemo() {
    const pl::TestProto p = (pl::TestProto)std::clamp(S.demo, 0, 4);
    const pl::AnalyseOptions o = optionsNow();
    startWork([=] {
        pl::GenOptions g;
        setResult(pl::analyse(pl::generateTestSignal(p, g), g.fs, o), g.fs);
    });
}

void analysePasted() {
    std::vector<std::vector<uint8_t>> pk;
    std::istringstream in(S.pasted);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<uint8_t> bits;
        if (S.pastedBits) {
            for (char c : line) if (c == '0' || c == '1') bits.push_back((uint8_t)(c - '0'));
        } else {
            std::vector<uint8_t> bytes;
            int hi = -1;
            for (char c : line) {
                int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0) continue;
                if (hi < 0) hi = v;
                else { bytes.push_back((uint8_t)(hi * 16 + v)); hi = -1; }
            }
            bits = pl::bytesToBits(bytes, false);
        }
        if (bits.size() >= 16) pk.push_back(bits);
    }
    const pl::AnalyseOptions o = optionsNow();
    if (pk.size() < 2) {
        std::lock_guard<std::mutex> lk(S.mu);
        S.err = tr("Paste at least two packets, one per line.");
        return;
    }
    startWork([=] { setResult(pl::analysePackets(pk, o.baud, o), 0); });
}

bool saveText(const char* suggest, const std::string& text) {
    const std::string p = saveFileDialog(suggest);
    if (p.empty()) return false;
    std::ofstream f(dect2::u8file(p), std::ios::binary);
    f << text;
    return (bool)f;
}

ImU32 kindColor(pl::FieldKind k) {
    switch (k) {
        case pl::FieldKind::Constant: return IM_COL32(150, 150, 150, 255);
        case pl::FieldKind::Counter: return IM_COL32(110, 170, 255, 255);
        case pl::FieldKind::Checksum: return IM_COL32(110, 210, 120, 255);
        case pl::FieldKind::Enum: return IM_COL32(230, 200, 90, 255);
        case pl::FieldKind::Varying: return IM_COL32(235, 235, 235, 255);
    }
    return IM_COL32_WHITE;
}
const char* kindText(pl::FieldKind k) {
    switch (k) {
        case pl::FieldKind::Constant: return tr("constant");
        case pl::FieldKind::Counter: return tr("counter");
        case pl::FieldKind::Checksum: return tr("check value");
        case pl::FieldKind::Enum: return tr("few values");
        case pl::FieldKind::Varying: return tr("varying");
    }
    return "";
}

void inputPanel() {
    ImGui::TextWrapped("%s", tr("Load a recording of the unknown signal, or paste packets you already have, and the lab works out how the signal is built: modulation, baud rate, preamble, CRC and fields. Nothing is decrypted."));
    const bool busy = S.run == 1;
    if (ImGui::BeginTabBar("##plin")) {
        if (ImGui::BeginTabItem(trw("Recording"))) {
            ImGui::SetNextItemWidth(-90 * gUi);
            ImGui::InputText("##plpath", S.path, sizeof S.path);
            ImGui::SameLine();
            if (ImGui::Button(tr("Browse..."))) {
                const std::string p = openFileDialog();
                if (!p.empty()) {
                    snprintf(S.path, sizeof S.path, "%s", p.c_str());
                    switch (dect2::guessFormat(p)) {
                        case dect2::FileFormat::CF32: S.fmt = 0; break;
                        case dect2::FileFormat::CI16: S.fmt = 1; break;
                        case dect2::FileFormat::CU8: S.fmt = 2; break;
                        default: S.fmt = 3; break;
                    }
                    const double r = dect2::guessSampleRate(p);
                    if (r > 0) S.rate = r;
                }
            }
            ImGui::SetNextItemWidth(130 * gUi);
            ImGui::Combo(tr("Format"), &S.fmt, "complex float32\0complex int16\0complex uint8 (RTL-SDR)\0complex int8 (HackRF)\0");
            ImGui::SetNextItemWidth(130 * gUi);
            ImGui::InputDouble(tr("Sample rate (Hz)"), &S.rate, 0, 0, "%.0f");
            ImGui::SetNextItemWidth(130 * gUi);
            ImGui::InputDouble(tr("Signal offset from the centre (Hz)"), &S.offset, 0, 0, "%.0f");
            ImGui::SetNextItemWidth(130 * gUi);
            ImGui::InputDouble(tr("Signal bandwidth (Hz, 0 = all)"), &S.bw, 0, 0, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Read the offset and the width off the waterfall of the recording; the lab mixes the signal to the centre and cuts away the rest."));
            ImGui::BeginDisabled(busy || !S.path[0]);
            if (ImGui::Button(tr("Analyse the recording"))) analyseFile();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(trw("Packets"))) {
            ImGui::TextWrapped("%s", tr("One packet per line, as hex bytes or as bits. The packets are taken as sliced from the air: the preamble and the sync word may still be in them."));
            ImGui::Checkbox(tr("Lines are bits (0 and 1)"), &S.pastedBits);
            struct Cb { static int resize(ImGuiInputTextCallbackData* d) { if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) { auto* s = static_cast<std::string*>(d->UserData); s->resize((size_t)d->BufTextLen); d->Buf = s->data(); } return 0; } };
            ImGui::InputTextMultiline("##plpaste", S.pasted.data(), S.pasted.capacity() + 1, ImVec2(-1, 110 * gUi), ImGuiInputTextFlags_CallbackResize, Cb::resize, &S.pasted);
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Analyse the packets"))) analysePasted();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(trw("Demo signals"))) {
            ImGui::TextWrapped("%s", tr("Made-up signals with a known structure, to see what the lab finds. None of them is a real protocol."));
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##pldemo", &S.demo,
                         "2FSK packets, preamble + sync, CRC-16/CCITT-FALSE\0OOK sensor, Manchester, CRC-8 (0x31)\0"
                         "2FSK, NRZI + HDLC framing, CRC-16/X-25\0"
                         "2FSK, PN9 whitening, CRC-16 (0x8005, init 0xFFFF)\0"
                         "4FSK, preamble + sync, CRC-8 (0x31, init 0xFF)\0");
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Make it and analyse"))) analyseDemo();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(150 * gUi);
    comboTr(tr("Demodulator"), &S.demod, {N_("Auto"), N_("OOK / ASK"), N_("2FSK / GFSK"), N_("4FSK"), N_("BPSK"), N_("QPSK"), N_("FM")});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble(tr("Baud (0 = find it)"), &S.baud, 0, 0, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::DragFloat(tr("Gap (ms)"), &S.gapMs, 0.1f, 0.2f, 100.f, "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A pause shorter than this does not end a packet. Raise it for signals with long runs of silence inside a packet."));
    if (busy) ImGui::TextDisabled("%s", tr("Working..."));
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (!S.err.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "%s", S.err.c_str());
    }
}

void candidatesPanel() {
    pl::Report& r = S.rep;
    ImGui::Text(tr("%zu bursts found."), r.bursts.size());
    if (r.candidates.empty()) {
        ImGui::TextWrapped("%s", r.note.empty() ? tr("Nothing found.") : r.note.c_str());
        return;
    }
    if (ImGui::BeginTable("##plcand", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Demodulator"));
        ImGui::TableSetupColumn(tr("Baud"));
        ImGui::TableSetupColumn(tr("Line coding"));
        ImGui::TableSetupColumn(tr("Sync word"));
        ImGui::TableSetupColumn(tr("Check value"));
        ImGui::TableSetupColumn(tr("Whitening"));
        ImGui::TableSetupColumn(tr("Score"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.candidates.size(); i++) {
            const pl::Candidate& c = r.candidates[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char lab[96];
            snprintf(lab, sizeof lab, "%s%s##cand%zu", pl::demodName(c.demod), c.invert ? " (inv)" : "", i);
            if (ImGui::Selectable(lab, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) {
                S.sel = (int)i;
                S.selPkt = 0;
                S.selByte = -1;
                S.fields = pl::analyseFields(c.packets, crcBytesOf(c));
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.1f", c.baud);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(pl::codingName(c.coding));
            ImGui::TableSetColumnIndex(3);
            {
                std::string b;
                for (uint8_t v : c.sync.sync) b += (char)('0' + v);
                if (c.sync.preambleBits >= 12 && !b.empty()) ImGui::Text("%s  (%s)", b.c_str(), tr("after a preamble"));
                else ImGui::TextUnformatted(b.empty() ? "-" : b.c_str());
            }
            ImGui::TableSetColumnIndex(4);
            if (!c.crc.empty()) ImGui::Text("%s  %d/%d", c.crc[0].name.c_str(), c.crc[0].matches, c.crc[0].total);
            else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(c.whitening.empty() ? "-" : c.whitening.c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.0f", c.score);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", tr("The best reading is first. A check value that holds for most packets is strong evidence; without one, treat the rest as a guess."));
}

void packetsTab(const pl::Candidate& c) {
    if (c.packets.empty()) { ImGui::TextUnformatted(tr("No packets.")); return; }
    ImGui::TextDisabled("%s", tr("Bytes from the start of the sync word. Grey: constant, blue: counter, green: check value, yellow: few values."));
    ImGui::BeginChild("##plhex", ImVec2(0, 190 * gUi), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (size_t p = 0; p < c.packets.size(); p++) {
        ImGui::PushID((int)p);
        ImGui::TextDisabled("%2zu", p);
        for (size_t b = 0; b < c.packets[p].size(); b++) {
            ImGui::SameLine(0, b == 0 ? 8.f : 3.f);
            const pl::Column* col = b < S.fields.cols.size() ? &S.fields.cols[b] : nullptr;
            ImGui::PushStyleColor(ImGuiCol_Text, col ? kindColor(col->kind) : IM_COL32(235, 235, 235, 255));
            char t[8];
            snprintf(t, sizeof t, "%02X##%zu", c.packets[p][b], b);
            const bool sel = S.selPkt == (int)p && S.selByte == (int)b;
            if (ImGui::Selectable(t, sel, 0, ImVec2(ImGui::CalcTextSize("00").x, 0))) { S.selPkt = (int)p; S.selByte = (int)b; }
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (S.selPkt >= 0 && S.selPkt < (int)c.packets.size() && S.selByte >= 0 && S.selByte < (int)c.packets[(size_t)S.selPkt].size()) {
        const auto& v = c.packets[(size_t)S.selPkt];
        const size_t i = (size_t)S.selByte;
        auto at = [&](size_t k) -> uint32_t { return i + k < v.size() ? v[i + k] : 0; };
        ImGui::Text(tr("Packet %d, byte %d"), S.selPkt, S.selByte);
        ImGui::Text("u8 %u   i8 %d   bits ", at(0), (int)(int8_t)at(0));
        ImGui::SameLine(0, 0);
        char bits[10];
        for (int k = 0; k < 8; k++) bits[k] = (char)('0' + ((at(0) >> (7 - k)) & 1));
        bits[8] = 0;
        ImGui::TextUnformatted(bits);
        if (i + 2 <= v.size()) ImGui::Text("u16 LE %u   u16 BE %u   i16 LE %d", at(0) | at(1) << 8, at(0) << 8 | at(1), (int)(int16_t)(at(0) | at(1) << 8));
        if (i + 4 <= v.size()) {
            const uint32_t le = at(0) | at(1) << 8 | at(2) << 16 | at(3) << 24, be = at(0) << 24 | at(1) << 16 | at(2) << 8 | at(3);
            float fl, fb;
            std::memcpy(&fl, &le, 4);
            std::memcpy(&fb, &be, 4);
            ImGui::Text("u32 LE %u   u32 BE %u   float LE %.5g   float BE %.5g", le, be, (double)fl, (double)fb);
        }
        std::string a;
        for (size_t k = i; k < v.size() && k < i + 24; k++) a += v[k] >= 0x20 && v[k] < 0x7F ? (char)v[k] : '.';
        ImGui::Text("ASCII  %s", a.c_str());
        auto ints = pl::interpret(v);
        if (!ints.empty() && ImGui::TreeNode(trw("Readings that could fit this packet"))) {
            for (const auto& it : ints) ImGui::BulletText("@%d  %s  %s", it.offset, it.kind.c_str(), it.text.c_str());
            ImGui::TreePop();
        }
    }
}

void fieldsTab(const pl::Candidate& c) {
    if (S.fields.cols.empty()) { ImGui::TextUnformatted(tr("No fields.")); return; }
    ImGui::TextWrapped("%s", tr("C constant, N counter, K check value, E few values, V varying; one letter per byte from the start of the sync word."));
    ImGui::TextUnformatted(S.fields.layout.c_str());
    if (ImGui::BeginTable("##plfld", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 230 * gUi))) {
        ImGui::TableSetupColumn(tr("Byte"));
        ImGui::TableSetupColumn(tr("Kind"));
        ImGui::TableSetupColumn(tr("Entropy (bits per byte)"));
        ImGui::TableSetupColumn(tr("Distinct values"));
        ImGui::TableSetupColumn(tr("Note"));
        ImGui::TableHeadersRow();
        for (const pl::Column& col : S.fields.cols) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", col.index);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImColor(kindColor(col.kind)).Value, "%s", kindText(col.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f", col.entropy);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", col.distinct);
            ImGui::TableSetColumnIndex(4);
            if (col.kind == pl::FieldKind::Constant) ImGui::Text("0x%02X", col.constantValue);
            else if (col.kind == pl::FieldKind::Counter) ImGui::Text(tr("adds %d each packet"), col.counterStep);
            else if (col.correlatedWith >= 0) ImGui::Text(tr("follows byte %d (r = %.2f)"), col.correlatedWith, col.correlation);
            else if (col.entropy > 7.0 && c.packets.size() >= 8) ImGui::TextUnformatted(tr("looks random: a check value, a key or encrypted data"));
        }
        ImGui::EndTable();
    }
}

void compareTab(const pl::Candidate& c) {
    const int n = (int)c.packets.size();
    if (n < 2) { ImGui::TextUnformatted(tr("Two packets are needed.")); return; }
    S.cmpA = std::clamp(S.cmpA, 0, n - 1);
    S.cmpB = std::clamp(S.cmpB, 0, n - 1);
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt(tr("Packet A"), &S.cmpA);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt(tr("Packet B"), &S.cmpB);
    S.cmpA = std::clamp(S.cmpA, 0, n - 1);
    S.cmpB = std::clamp(S.cmpB, 0, n - 1);
    const std::string d = pl::bitDiff(c.packets[(size_t)S.cmpA], c.packets[(size_t)S.cmpB]);
    ImGui::BeginChild("##pldiff", ImVec2(0, 150 * gUi), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(d.c_str());
    ImGui::EndChild();
    ImGui::TextDisabled("%s", tr("^ marks the bits that differ."));
}

void crcTab(const pl::Candidate& c) {
    if (c.crc.empty()) {
        ImGui::TextWrapped("%s", tr("No check value found. Either the packets have none, it covers something this search does not try (for example header bytes in another order), the bits still hold errors, or the payload is whitened or encrypted. Try another demodulator, the other polarity or a cleaner recording."));
        return;
    }
    if (ImGui::BeginTable("##plcrc", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        const char* heads[] = {N_("Name"), N_("Packets"), N_("Covers bytes"), N_("Polynomial"), N_("Init"), N_("XOR out"), N_("Reflect"), N_("Byte order")};
        for (const char* h : heads) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        for (const pl::CrcHit& h : c.crc) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(h.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d / %d", h.matches, h.total);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d .. %d", h.dataStart, h.dataStart + h.coveredBytes - 1);
            if (h.isChecksum) { ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("-"); continue; }
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("0x%0*X", h.width / 4, h.poly);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("0x%0*X", h.width / 4, h.init);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("0x%0*X", h.width / 4, h.xorout);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(h.refin && h.refout ? tr("input and output") : h.refin ? tr("input only") : h.refout ? tr("output only") : "-");
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(h.width == 8 ? "-" : h.crcBigEndian ? tr("big endian") : tr("little endian"));
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("%s", tr("When all packets are the same length, the init value and the XOR-out value cannot be told apart: several pairs fit equally well and the first plain one is shown. A short check value (8 bit) with few packets can fit by chance; the more packets that agree, the better."));
}

// the candidate as a Mode Builder definition (docs/MODE-BUILDER.md): a starting point the user completes there
std::string modeDefinitionOf(const pl::Candidate& c, const char* name, double fs) {
    namespace mb = dect2::modebuilder;
    mb::Definition d;
    d.name = name[0] ? name : "unknown protocol";
    d.id = mb::makeId(d.name);
    d.description = "From the Protocol lab";
    d.sampleRate = fs > 0 ? fs : 250000;
    switch (c.demod) {
        case pl::Demod::OOK: d.mod = mb::Mod::OOK; break;
        case pl::Demod::FSK4: d.mod = mb::Mod::FSK; d.order = 4; break;
        case pl::Demod::PSK2: d.mod = mb::Mod::PSK; d.order = 2; break;
        case pl::Demod::PSK4: d.mod = mb::Mod::PSK; d.order = 4; break;
        default: d.mod = mb::Mod::FSK; d.order = 2; break;
    }
    d.symbolRate = c.baud > 0 ? std::round(c.baud) : 1000;
    d.deviationHz = d.mod == mb::Mod::FSK ? d.symbolRate : 0;
    d.bandwidthHz = std::round(std::min(d.bandwidth(), d.sampleRate * 0.8));
    d.polarity = c.invert ? mb::Polarity::Inverted : mb::Polarity::Normal;
    d.lsbFirst = c.lsbFirst;
    switch (c.coding) {
        case pl::Coding::NRZI: d.line = mb::Line::NRZI; break;
        case pl::Coding::ManchesterIEEE: d.line = mb::Line::Manchester; break;
        case pl::Coding::ManchesterThomas: d.line = mb::Line::ManchesterThomas; break;
        case pl::Coding::DiffManchester: d.line = mb::Line::DiffManchester; break;
        case pl::Coding::Hdlc: d.line = mb::Line::NRZI; break;
        default: d.line = mb::Line::NRZ; break;
    }
    const int syncBits = std::min<int>(64, (int)c.sync.sync.size());
    d.sync.bits = syncBits;
    for (int i = 0; i < syncBits; i++) d.sync.value = (d.sync.value << 1) | (c.sync.sync[(size_t)i] & 1);
    if (syncBits == 0) { d.sync.bits = 16; d.sync.value = 0x2DD4; d.description += "; no sync word was found: put it in"; }
    d.syncMaxErrors = std::min(2, d.sync.bits / 8);
    const int syncBytes = (syncBits + 7) / 8;
    size_t minLen = 0;
    for (const auto& p : c.packets) minLen = minLen ? std::min(minLen, p.size()) : p.size();
    if (c.coding == pl::Coding::Hdlc) {
        d.lenMode = mb::LenMode::EndFlag;
        d.flag = mb::BitPattern{0x7E, 8};
        d.bitStuffing = true;
        d.minBytes = 2;
        d.maxBytes = 400;
    } else d.fixedBytes = std::max(1, (int)minLen - syncBytes);
    for (const pl::CrcHit& h : c.crc) {
        if (h.matches < 3 || h.matches * 10 < h.total * 6) continue;
        if (h.isChecksum) break;
        d.crcKind = mb::CrcKind::Crc;
        d.crc = mb::CrcModel{h.name, h.width, h.poly, h.init, h.xorout, h.refin, h.refout};
        if (h.name.find("unnamed") != std::string::npos) d.crc.name.clear();
        d.crcFrom = std::max(0, h.dataStart - syncBytes);
        d.crcLittle = !h.crcBigEndian;
        if (h.endSlack > 0) d.crcPosition = std::max(0, h.dataStart - syncBytes + h.coveredBytes);
        break;
    }
    if (!c.whitening.empty()) d.description += "; whitened (" + c.whitening + "): set the scrambler";
    mb::Field f;
    f.name = "payload";
    f.type = mb::FieldType::Hex;
    f.widthBits = 8 * std::max(1, d.longestFrameBytes() - d.crcBytes());
    d.fields.push_back(f);
    d.display.push_back({"", 0, "{_len} bytes {payload}"});
    return mb::toJson(d);
}

void exportTab(const pl::Candidate& c) {
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText(tr("Protocol name"), S.name, sizeof S.name);
    const std::string json = pl::protocolJson(c, S.fields, S.name, S.usedRate);
    if (ImGui::Button(tr("Copy protocol (JSON)"))) ImGui::SetClipboardText(json.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Save protocol..."))) saveText("protocol.json", json);
    ImGui::SameLine();
    if (ImGui::Button(tr("Open in Mode Builder"))) modeBuilderOpenText(modeDefinitionOf(c, S.name, S.usedRate));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turn this result into a decoder: the Mode Builder opens with a definition made from it"));
    const std::string bits = pl::packetText(c);
    if (ImGui::Button(tr("Copy packets as bits"))) ImGui::SetClipboardText(bits.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Save packets as bits..."))) saveText("packets.txt", bits);
    ImGui::SameLine();
    {
        std::string hex;
        for (const auto& p : c.packets) hex += pl::hexDump(p) + "\n";
        if (ImGui::Button(tr("Copy packets as hex"))) ImGui::SetClipboardText(hex.c_str());
    }
    const std::string rtl = pl::rtl433Spec(c, S.name);
    ImGui::TextUnformatted(tr("rtl_433 decoder line (a starting point, not a tested decoder):"));
    ImGui::BeginChild("##plrtl", ImVec2(0, 72 * gUi), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(rtl.c_str());
    ImGui::EndChild();
    if (ImGui::Button(tr("Copy the rtl_433 line"))) ImGui::SetClipboardText(rtl.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("The bit file has one packet per line; tools such as Universal Radio Hacker can open protocol text like that."));
}

void resultPanel() {
    std::lock_guard<std::mutex> lk(S.mu);
    candidatesPanel();
    if (S.rep.candidates.empty()) return;
    S.sel = std::clamp(S.sel, 0, (int)S.rep.candidates.size() - 1);
    const pl::Candidate& c = S.rep.candidates[(size_t)S.sel];
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##plres")) {
        if (ImGui::BeginTabItem(trw("Bytes"))) { packetsTab(c); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Fields"))) { fieldsTab(c); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Compare"))) { compareTab(c); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Check value"))) { crcTab(c); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Export"))) { exportTab(c); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void windowPanel(App& a) {
    firstWindowSize(900 * gUi, 700 * gUi);
    if (!ImGui::Begin(trw("Protocol lab"), &a.protolabOpen)) { ImGui::End(); return; }
    if (S.run == 2) {
        if (S.worker.joinable()) S.worker.join();
        S.run = 0;
    }
    inputPanel();
    ImGui::Separator();
    if (S.haveResult) resultPanel();
    ImGui::End();
}
}   // namespace

void protolabWindow(App& a) {
    if (a.rxId != 1) {   // one lab for the program: RX 1's window
        if (App* f = receiverById(1)) { f->protolabOpen = a.protolabOpen; protolabWindow(*f); a.protolabOpen = f->protolabOpen; }
        return;
    }
    if (a.protolabOpen) windowPanel(a);
}
