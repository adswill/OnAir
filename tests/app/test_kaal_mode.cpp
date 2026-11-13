// The KAAL mode (family 122, engine standard 124, telemetry 123) and its neighbours in the group "Kris custom modes": KAVB (123, 125, built: tests/app/test_kavb_mode.cpp), KAPP (124, 126) and KAFT (125, 127, both built: tests/app/test_kapp_kaft_mode.cpp): the mode-table rows, the group, the modeui cases, the panel strings, the engine wiring, the transmit
// registration and the IQ-file destination are in the sources. Text checks only, so it builds without the app.
#include <cstdio>
#include <cstdlib>
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
        if (src.find(what) == std::string::npos) {
            std::printf("FAIL: %s: missing %s\n", file, what);
            fails++;
        }
    };
    auto absent = [&fails](const char* file, const std::string& src, const char* what) {
        if (src.find(what) != std::string::npos) {
            std::printf("FAIL: %s: must not contain %s\n", file, what);
            fails++;
        }
    };

    const std::string table = readSource("app/ui/mode_table.cpp");
    need("app/ui/mode_table.cpp", table, "{122, \"KAAL\", 15,");
    need("app/ui/mode_table.cpp", table, "{123, \"KAVB\", 15,");
    need("app/ui/mode_table.cpp", table, "{124, \"KAPP\", 15,");
    need("app/ui/mode_table.cpp", table, "{125, \"KAFT\", 15,");
    need("app/ui/mode_table.cpp", table, "N_(\"PLUGINS\"), \"Kris custom modes\", N_(\"CUSTOM MODES\")}");   // the Mode Builder's group follows
    need("app/ui/mode_table.cpp", table, "IM_COL32(236, 160, 84, 255),\n    IM_COL32(120, 200, 170, 255)};");

    const std::string app = readSource("app/app.h");
    need("app/app.h", app, "constexpr int kNumFamilies = ");
    need("app/app.h", app, "constexpr int kNumGroups = 17;");
    absent("app/app.h", app, "kaalOpen");

    const std::string modeui = readSource("app/panels/modeui.cpp");
    need("app/panels/modeui.cpp", modeui, "case 122: return &kKaalUi;");
    need("app/panels/modeui.cpp", modeui, "case 123: return &kKavbUi;");
    need("app/panels/modeui.cpp", modeui, "case 124: return &kKappUi;");
    need("app/panels/modeui.cpp", modeui, "case 125: return &kKaftUi;");

    const std::string shell = readSource("app/ui/ui2_shell.cpp");
    absent("app/ui/ui2_shell.cpp", shell, "KAAL (WIP)");

    const std::string panel = readSource("app/kaal/kaal_ui.cpp");
    need("app/kaal/kaal_ui.cpp", panel, "const ModeUi kKaalUi");
    need("app/kaal/kaal_ui.cpp", panel, "a.rx.standard == 123");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"frames\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"SNR\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"CFO\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"sweep level\"");
    need("app/kaal/kaal_ui.cpp", panel, "\"audio level\"");
    need("app/kaal/kaal_ui.cpp", panel, "\"Last beacon\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"callsign\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"grid\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"location\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"power\"");
    need("app/kaal/kaal_ui.cpp", panel, "row(\"quality\"");
    need("app/kaal/kaal_ui.cpp", panel, "\"bio text\"");
    need("app/kaal/kaal_ui.cpp", panel, "void kaalPanel(TxUi& s, bool running)");
    need("app/kaal/kaal_ui.cpp", panel, "Callsign (9 characters at most)");
    need("app/kaal/kaal_ui.cpp", panel, "Bio text (100 characters at most)");
    need("app/kaal/kaal_ui.cpp", panel, "Power (dBm, -40 to 87.5)");
    need("app/kaal/kaal_ui.cpp", panel, "Send the beacon (station identity)");
    need("app/kaal/kaal_ui.cpp", panel, "tx.kaal.callsign");

    const std::string kavbUi = readSource("app/kavb/kavb_ui.cpp");   // KAVB has its own screen now (tests/app/test_kavb_mode.cpp)
    need("app/kavb/kavb_ui.cpp", kavbUi, "const ModeUi kKavbUi");

    const std::string live = readSource("core/src/kaal/kaal_live.cpp");
    need("core/src/kaal/kaal_live.cpp", live, "t.stdMode = 124;");
    need("core/src/kaal/kaal_live.cpp", live, "t.id = \"kaal\";");
    need("core/src/kaal/kaal_live.cpp", live, "t.minSampleRate = 48000;");
    need("core/src/kaal/kaal_live.cpp", live, "t.tuneOffsetHz = 0;");
    need("core/src/kaal/kaal_live.cpp", live, "BeaconSliceRx");
    need("core/src/kaal/kaal_live.cpp", live, "AudioOut");

    const std::string modes = readSource("core/src/engine/modes.cpp");
    need("core/src/engine/modes.cpp", modes, "kaalTuning(), kavbTuning(), kappTuning(), kaftTuning(),");
    need("core/src/engine/modes.cpp", modes, "case 123: return kaalSummary(t.kaal);");

    const std::string telemetry = readSource("core/src/engine/engine_telemetry.cpp");
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 123: return pollStd(rxKaal_, 123");

    const std::string engine = readSource("core/src/engine/engine.cpp");
    need("core/src/engine/engine.cpp", engine, "else if (a == 123) rxKaal_.feed(x, n);");
    need("core/src/engine/engine.cpp", engine, "case 123: rxKaal_.reset(); break;");

    const std::string place = readSource("core/src/engine/engine_place.cpp");
    need("core/src/engine/engine_place.cpp", place, "case 123: rxKaal_.configure(r); break;");
    need("core/src/engine/engine_place.cpp", place, "case 123: rxKaal_.setMuted(m); break;");

    const std::string start = readSource("core/src/engine/engine_start.cpp");
    need("core/src/engine/engine_start.cpp", start, "rxKaal_.configure(rate_);");

    // KAVB has its engine case now: 124 (tests/app/test_kavb_mode.cpp)
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 124: return pollStd(rxKavb_, 124");

    // the transmit side
    const std::string tx = readSource("core/src/tx/tx.cpp");
    need("core/src/tx/tx.cpp", tx, "mk(\"kaal\", \"KAAL\", 124,");
    need("core/src/tx/tx.cpp", tx, "if (id == \"kaal\") return TxPayloadKind::Kaal;");
    need("core/src/tx/tx.cpp", tx, "makeKaalTxGenerator(r,");
    const std::string txui = readSource("app/panels/tx_ui.cpp");
    need("app/panels/tx_ui.cpp", txui, "makeIqFileTxSink(fc)");
    need("app/panels/tx_ui.cpp", txui, "kaal.powerDbm");
    const std::string txset = readSource("app/panels/tx_step_settings.cpp");
    need("app/panels/tx_step_settings.cpp", txset, "tr(\"Send to\")");   // 4.0: the destination is the "Send to" choice (radio, dummy load, IQ file)
    const std::string txpar = readSource("core/src/tx/tx_params.cpp");
    need("core/src/tx/tx_params.cpp", txpar, "case TxPayloadKind::Kaal:");
    need("core/src/tx/tx_params.cpp", txpar, "custom(\"kaal.panel\"");
    need("app/panels/tx_payload_ui.cpp", readSource("app/panels/tx_payload_ui.cpp"), "k == \"kaal.panel\") kaalPanel(s, running);");
    need("app/panels/tx_step_settings.cpp", txset, "\"IQ file\"");
    need("app/panels/tx_step_settings.cpp", txset, "destinationPanel(s, *mode, running);");

    // no clash: each case label of the new numbers appears once per switch
    const std::string cm = readSource("CMakeLists.txt");
    need("CMakeLists.txt", cm, " kaal ");

    if (fails == 0) std::printf("kaal mode: registered, panel strings, wiring and transmit present\n");
    return fails == 0 ? 0 : 1;
}
