// KAVB (family 123, engine standard 125, telemetry slot 133) as a real mode in the group "Kris custom modes": the mode-table row (no longer work in
// progress), the modeui case, the receive panel, the transmit options, the engine wiring and the transmit registration are in the sources. Text checks only, so it
// builds without the app. The behaviour is tested in tests/kavb/test_kavb_engine.cpp.
#include <cstdlib>
#include <cstdio>
#include <fstream>
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
    need("app/ui/mode_table.cpp", table, "{123, \"KAVB\", 15,");
    absent("app/ui/mode_table.cpp", table, "KAVB (WIP)");
    const std::string modeui = readSource("app/panels/modeui.cpp");
    need("app/panels/modeui.cpp", modeui, "case 123: return &kKavbUi;");
    need("app/panels/modeui.cpp", modeui, "extern const ModeUi kKavbUi;");
    { const std::string wip = readSource("app/kaal/kris_wip_ui.cpp"); if (!wip.empty()) { std::printf("FAIL: the KAVB placeholder app/kaal/kris_wip_ui.cpp is still there\n"); fails++; } }

    const std::string ui = readSource("app/kavb/kavb_ui.cpp");
    need("app/kavb/kavb_ui.cpp", ui, "const ModeUi kKavbUi");
    need("app/kavb/kavb_ui.cpp", ui, "return a.rx.standard == 124;");
    need("app/kavb/kavb_ui.cpp", ui, "Video decoder not installed");
    need("app/kavb/kavb_ui.cpp", ui, "void kavbPanel(TxUi& s, bool running)");
    need("app/kavb/kavb_ui.cpp", ui, "tx.kavb.profile");
    need("app/kavb/kavb_ui.cpp", ui, "tx.kavb.file");
    need("app/kavb/kavb_ui.cpp", ui, "setOutputFile");

    const std::string txs = readSource("app/panels/tx_state.h");
    need("app/panels/tx_state.h", txs, "int kavbProfile = 0, kavbFec = 1, kavbSeconds = 5;");
    need("app/panels/tx_state.h", txs, "void kavbPanel(TxUi& s, bool running);");
    const std::string txui = readSource("app/panels/tx_ui.cpp");
    need("app/panels/tx_ui.cpp", txui, "r.opt[\"kavb.profile\"] = s.kavbProfile;");
    need("app/panels/tx_ui.cpp", txui, "r.file = s.kavbFile;");
    need("core/src/tx/tx_params.cpp", readSource("core/src/tx/tx_params.cpp"), "case TxPayloadKind::Kavb:");   // 4.0: the KA* panels are custom rows of the parameter table
    need("core/src/tx/tx_params.cpp", readSource("core/src/tx/tx_params.cpp"), "custom(\"kavb.panel\"");
    need("app/panels/tx_payload_ui.cpp", readSource("app/panels/tx_payload_ui.cpp"), "k == \"kavb.panel\") kavbPanel(s, running);");

    const std::string app = readSource("app/app.h");
    { const size_t q = app.find("constexpr int kNumFamilies = "); if (q == std::string::npos || std::atoi(app.c_str() + q + 29) < 124) { std::printf("FAIL: kNumFamilies below 124\n"); fails++; } }
    { const std::string e = readSource("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); if (q == std::string::npos || std::atoi(e.c_str() + q + 16) < 134) { std::printf("FAIL: kPollSeqSlots below 134\n"); fails++; }
      need("engine.h", e, "KavbLiveReceiver rxKavb_;"); need("engine.h", e, "KavbLiveReceiver& kavb()"); }

    need("core/src/kavb/kavb_live.cpp", readSource("core/src/kavb/kavb_live.cpp"), "t.stdMode = 125; t.id = \"kavb\"; t.name = \"KAVB\";");
    absent("core/src/kaal/kaal_live.cpp", readSource("core/src/kaal/kaal_live.cpp"), "kavbTuning");
    need("core/src/engine/modes.cpp", readSource("core/src/engine/modes.cpp"), "kaalTuning(), kavbTuning(), kappTuning(), kaftTuning(),");
    need("core/src/engine/modes.cpp", readSource("core/src/engine/modes.cpp"), "case 124: return kavbSummary(t.kavb);");
    need("core/src/engine/engine_telemetry.cpp", readSource("core/src/engine/engine_telemetry.cpp"), "case 124: return pollStd(rxKavb_, 124, t, seqs[133], &RxTelemetry::kavb);");
    need("core/src/engine/engine.cpp", readSource("core/src/engine/engine.cpp"), "case 124: rxKavb_.reset(); break;");
    need("core/src/engine/engine.cpp", readSource("core/src/engine/engine.cpp"), "else if (a == 124) rxKavb_.feed(x, n);");
    need("core/src/engine/engine_place.cpp", readSource("core/src/engine/engine_place.cpp"), "case 124: rxKavb_.configure(r); break;");
    need("core/src/engine/engine_start.cpp", readSource("core/src/engine/engine_start.cpp"), "rxKavb_.configure(rate_);");
    need("core/include/dect2/t2/t2rx.h", readSource("core/include/dect2/t2/t2rx.h"), "KavbTelemetry kavb;");

    const std::string tx = readSource("core/src/tx/tx.cpp");
    need("core/src/tx/tx.cpp", tx, "mk(\"kavb\", \"KAVB\", 125,");
    need("core/src/tx/tx.cpp", tx, "if (id == \"kavb\") return TxPayloadKind::Kavb;");
    need("core/src/tx/tx.cpp", tx, "if (m.stdMode == 125) return makeKavbTxGenerator(");
    need("core/src/tx/tx.cpp", tx, "std::string(m.id) == \"kavb\"");

    if (fails) { std::printf("test_kavb_mode: %d FAILED\n", fails); return 1; }
    std::printf("test_kavb_mode: all passed\n");
    return 0;
}
