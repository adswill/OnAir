// The KAWS mode (family 133, active standard 134, engine standard 135, telemetry slot 136) in the group "Kris custom modes": the mode-table row, the unique
// family numbers, the modeui case, the receive panel, the transmit options, the engine wiring and the transmit registration are in the sources. Text checks
// only, so it builds without the app.
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

static std::string readSource(const char* rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    int fails = 0;
    auto need = [&fails](const char* file, const std::string& src, const char* what) {
        if (src.find(what) == std::string::npos) { std::printf("FAIL: %s: missing %s\n", file, what); fails++; }
    };
    auto absent = [&fails](const char* file, const std::string& src, const char* what) {
        if (src.find(what) != std::string::npos) { std::printf("FAIL: %s: must not contain %s\n", file, what); fails++; }
    };

    const std::string table = readSource("app/ui/mode_table.cpp");
    need("app/ui/mode_table.cpp", table, "{133, \"KAWS\", 15,");
    absent("app/ui/mode_table.cpp", table, "KAWS (WIP)");
    need("app/ui/mode_table.cpp", table, "simulation-only");   // the honest claim is in the row
    {   // the family numbers of the rows are unique
        std::map<int, int> seen;
        std::istringstream in(table);
        std::string line;
        int rows = 0;
        while (std::getline(in, line)) {
            if (line.rfind("    {", 0) != 0) continue;
            const size_t comma = line.find(',');
            if (comma == std::string::npos || line.compare(comma, 3, ", \"") != 0) continue;
            const std::string num = line.substr(5, comma - 5);
            if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) continue;
            const int fam = std::stoi(num);
            {   // a protocol entry (a subId after the accent colour) shares its family on purpose: test_mode_numbers checks (family, subId)
                const size_t acc = line.rfind("ImVec4(");
                const size_t close = acc == std::string::npos ? acc : line.find(')', acc);
                if (close != std::string::npos && line.compare(close + 1, 3, ", \"") == 0) continue;
            }
            rows++;
            if (fam >= 122 && fam != 360 && ++seen[fam] > 1) { std::printf("FAIL: family %d appears twice in the mode table\n", fam); fails++; }
        }
        if (rows < 100) { std::printf("FAIL: only %d mode rows parsed\n", rows); fails++; }
    }
    const std::string app = readSource("app/app.h");
    { const size_t q = app.find("constexpr int kNumFamilies = "); if (q == std::string::npos || atoi(app.c_str() + q + 29) < 134) { std::printf("FAIL: kNumFamilies below 134\n"); fails++; } }
    const std::string engineH = readSource("core/include/dect2/engine/engine.h");
    { const size_t q = engineH.find("kPollSeqSlots = "); if (q == std::string::npos || atoi(engineH.c_str() + q + 16) < 134) { std::printf("FAIL: kPollSeqSlots below 134\n"); fails++; } }
    need("core/include/dect2/engine/engine.h", engineH, "KawsLiveReceiver& kaws()");
    need("core/include/dect2/engine/engine.h", engineH, "KawsLiveReceiver rxKaws_;");

    const std::string modeui = readSource("app/panels/modeui.cpp");
    need("app/panels/modeui.cpp", modeui, "case 133: return &kKawsUi;");

    const std::string ui = readSource("app/kaws/kaws_ui.cpp");
    need("app/kaws/kaws_ui.cpp", ui, "const ModeUi kKawsUi");
    need("app/kaws/kaws_ui.cpp", ui, "a.rx.standard == 134");
    need("app/kaws/kaws_ui.cpp", ui, "row(\"profile\"");
    need("app/kaws/kaws_ui.cpp", ui, "row(\"SNR estimate\"");
    need("app/kaws/kaws_ui.cpp", ui, "row(\"blocks\"");
    need("app/kaws/kaws_ui.cpp", ui, "\"Preamble (woven lattice)\"");
    need("app/kaws/kaws_ui.cpp", ui, "\"Signal (audio spectrum)\"");
    need("app/kaws/kaws_ui.cpp", ui, "\"Chat\"");
    need("app/kaws/kaws_ui.cpp", ui, "\"Beacons\"");
    need("app/kaws/kaws_ui.cpp", ui, "UTC");
    need("app/kaws/kaws_ui.cpp", ui, "Voice codec not installed");
    need("app/kaws/kaws_ui.cpp", ui, "simulation-only");
    need("app/kaws/kaws_ui.cpp", ui, "void kawsPanel(TxUi& s, bool running)");
    need("app/kaws/kaws_ui.cpp", ui, "\"Profile\"");
    need("app/kaws/kaws_ui.cpp", ui, "tx.kaws.profile");
    need("app/kaws/kaws_ui.cpp", ui, "txSelectMode(a, 133, nullptr)");

    const std::string live = readSource("core/src/kaws/kaws_live.cpp");
    need("core/src/kaws/kaws_live.cpp", live, "t.stdMode = 135;");
    need("core/src/kaws/kaws_live.cpp", live, "t.id = \"kaws\";");
    need("core/src/kaws/kaws_live.cpp", live, "t.minMhz = 1.8; t.maxMhz = 30;");

    const std::string modes = readSource("core/src/engine/modes.cpp");
    need("core/src/engine/modes.cpp", modes, "kawsTuning()");
    need("core/src/engine/modes.cpp", modes, "case 134: return kawsSummary(t.kaws);");
    const std::string telemetry = readSource("core/src/engine/engine_telemetry.cpp");
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 134: return pollStd(rxKaws_, 134, t, seqs[136]");
    const std::string engine = readSource("core/src/engine/engine.cpp");
    need("core/src/engine/engine.cpp", engine, "else if (a == 134) rxKaws_.feed(x, n);");
    need("core/src/engine/engine.cpp", engine, "case 134: rxKaws_.reset(); break;");
    const std::string place = readSource("core/src/engine/engine_place.cpp");
    need("core/src/engine/engine_place.cpp", place, "case 134: rxKaws_.configure(r); break;");
    need("core/src/engine/engine_place.cpp", place, "case 134: rxKaws_.setSignalOffset(o); break;");
    const std::string start = readSource("core/src/engine/engine_start.cpp");
    need("core/src/engine/engine_start.cpp", start, "rxKaws_.configure(rate_);");
    need("core/src/engine/engine_start.cpp", start, "rxKaws_.setSignalOffset(o);");
    const std::string t2rx = readSource("core/include/dect2/t2/t2rx.h");
    need("core/include/dect2/t2/t2rx.h", t2rx, "KawsTelemetry kaws;");

    const std::string tx = readSource("core/src/tx/tx.cpp");
    need("core/src/tx/tx.cpp", tx, "mk(\"kaws\", \"KAWS\", 135,");
    need("core/src/tx/tx.cpp", tx, "if (id == \"kaws\") return TxPayloadKind::Kaws;");
    need("core/src/tx/tx.cpp", tx, "makeKawsTxGenerator(r,");
    const std::string txui = readSource("app/panels/tx_ui.cpp");
    need("app/panels/tx_ui.cpp", txui, "kaws.profile");
    need("app/panels/tx_ui.cpp", txui, "makeIqFileTxSink(fc)");
    const std::string pay = readSource("app/panels/tx_payload_ui.cpp");
    const std::string txpar = readSource("core/src/tx/tx_params.cpp");
    need("core/src/tx/tx_params.cpp", txpar, "case TxPayloadKind::Kaws:");   // 4.0: the KA* panels are custom rows of the parameter table
    need("core/src/tx/tx_params.cpp", txpar, "custom(\"kaws.panel\"");
    need("app/panels/tx_payload_ui.cpp", pay, "k == \"kaws.panel\") kawsPanel(s, running);");

    if (fails == 0) std::printf("kaws mode: registered, panel strings, wiring and transmit present\n");
    return fails == 0 ? 0 : 1;
}
