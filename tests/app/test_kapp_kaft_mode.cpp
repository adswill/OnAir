// The KAPP (family 124, engine standard 126, telemetry 125) and KAFT (family 125, engine standard 127, telemetry 126) modes in the group "Kris custom
// modes": the mode-table rows (no longer work in progress), the unique family numbers, the modeui cases, the receive panels, the transmit options, the
// engine wiring and the transmit registration are in the sources. Text checks only, so it builds without the app.
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
    need("app/ui/mode_table.cpp", table, "{124, \"KAPP\", 15,");
    need("app/ui/mode_table.cpp", table, "{125, \"KAFT\", 15,");
    absent("app/ui/mode_table.cpp", table, "KAPP (WIP)");
    absent("app/ui/mode_table.cpp", table, "KAFT (WIP)");
    need("app/ui/mode_table.cpp", table, "{123, \"KAVB\", 15,");

    // the family numbers of the rows are unique
    {
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
            if (fam >= 122 && ++seen[fam] > 1) { std::printf("FAIL: family %d appears twice in the mode table\n", fam); fails++; }   // the Kris group and everything after it
        }
        if (rows < 100) { std::printf("FAIL: only %d mode rows parsed\n", rows); fails++; }
    }

    const std::string app = readSource("app/app.h");
    { const size_t q = app.find("constexpr int kNumFamilies = "); if (q == std::string::npos || atoi(app.c_str() + q + 29) < 129) { std::printf("FAIL: kNumFamilies below 129\n"); fails++; } }

    const std::string engineH = readSource("core/include/dect2/engine/engine.h");
    { const size_t q = engineH.find("kPollSeqSlots = "); if (q == std::string::npos || atoi(engineH.c_str() + q + 16) < 140) { std::printf("FAIL: kPollSeqSlots below 140\n"); fails++; } }
    need("core/include/dect2/engine/engine.h", engineH, "KappLiveReceiver& kapp()");
    need("core/include/dect2/engine/engine.h", engineH, "KaftLiveReceiver& kaft()");

    const std::string modeui = readSource("app/panels/modeui.cpp");
    need("app/panels/modeui.cpp", modeui, "case 124: return &kKappUi;");
    need("app/panels/modeui.cpp", modeui, "case 125: return &kKaftUi;");

    const std::string kapp = readSource("app/kapp/kapp_ui.cpp");
    need("app/kapp/kapp_ui.cpp", kapp, "const ModeUi kKappUi");
    need("app/kapp/kapp_ui.cpp", kapp, "a.rx.standard == 125");
    need("app/kapp/kapp_ui.cpp", kapp, "row(\"profile\"");
    need("app/kapp/kapp_ui.cpp", kapp, "row(\"Es/N0\"");
    need("app/kapp/kapp_ui.cpp", kapp, "row(\"blocks\"");
    need("app/kapp/kapp_ui.cpp", kapp, "row(\"messages\"");
    need("app/kapp/kapp_ui.cpp", kapp, "row(\"throughput\"");
    need("app/kapp/kapp_ui.cpp", kapp, "\"Bursts per profile\"");
    need("app/kapp/kapp_ui.cpp", kapp, "\"Constellation of the last burst\"");
    need("app/kapp/kapp_ui.cpp", kapp, "\"Messages\"");
    need("app/kapp/kapp_ui.cpp", kapp, "void kappPanel(TxUi& s, bool running)");
    need("app/kapp/kapp_ui.cpp", kapp, "\"Profile\"");
    need("app/kapp/kapp_ui.cpp", kapp, "\"Passes\"");
    need("app/kapp/kapp_ui.cpp", kapp, "tx.kapp.profile");
    need("app/kapp/kapp_ui.cpp", kapp, "only listens");   // honest about the missing acknowledgement

    const std::string kaft = readSource("app/kaft/kaft_ui.cpp");
    need("app/kaft/kaft_ui.cpp", kaft, "const ModeUi kKaftUi");
    need("app/kaft/kaft_ui.cpp", kaft, "a.rx.standard == 126");
    need("app/kaft/kaft_ui.cpp", kaft, "\"Save received files to\"");
    need("app/kaft/kaft_ui.cpp", kaft, "\"Transfer\"");
    need("app/kaft/kaft_ui.cpp", kaft, "\"progress\"");
    need("app/kaft/kaft_ui.cpp", kaft, "row(\"resume\"");
    need("app/kaft/kaft_ui.cpp", kaft, "\"profile ladder\"");
    need("app/kaft/kaft_ui.cpp", kaft, "\"Files received\"");
    need("app/kaft/kaft_ui.cpp", kaft, "setSaveFolder");
    need("app/kaft/kaft_ui.cpp", kaft, "void kaftPanel(TxUi& s, bool running)");
    need("app/kaft/kaft_ui.cpp", kaft, "\"File to send\"");
    need("app/kaft/kaft_ui.cpp", kaft, "\"Width\"");
    need("app/kaft/kaft_ui.cpp", kaft, "tx.kaft.profile");

    const std::string kavbUi = readSource("app/kavb/kavb_ui.cpp");
    need("app/kavb/kavb_ui.cpp", kavbUi, "const ModeUi kKavbUi");
    absent("app/kavb/kavb_ui.cpp", kavbUi, "kKappUi");
    absent("app/kavb/kavb_ui.cpp", kavbUi, "kKaftUi");

    const std::string live1 = readSource("core/src/kapp/kapp_live.cpp");
    need("core/src/kapp/kapp_live.cpp", live1, "t.stdMode = 126;");
    need("core/src/kapp/kapp_live.cpp", live1, "t.id = \"kapp\";");
    need("core/src/kapp/kapp_live.cpp", live1, "t.minSampleRate = 125000;");
    const std::string live2 = readSource("core/src/kaft/kaft_live.cpp");
    need("core/src/kaft/kaft_live.cpp", live2, "t.stdMode = 127;");
    need("core/src/kaft/kaft_live.cpp", live2, "t.id = \"kaft\";");
    need("core/src/kaft/kaft_live.cpp", live2, "t.minSampleRate = 1.25e6;");

    const std::string modes = readSource("core/src/engine/modes.cpp");
    need("core/src/engine/modes.cpp", modes, "kaalTuning(), kavbTuning(), kappTuning(), kaftTuning(),");
    need("core/src/engine/modes.cpp", modes, "case 125: return kappSummary(t.kapp);");
    need("core/src/engine/modes.cpp", modes, "case 126: return kaftSummary(t.kaft);");

    const std::string telemetry = readSource("core/src/engine/engine_telemetry.cpp");
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 125: return pollStd(rxKapp_, 125");
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 126: return pollStd(rxKaft_, 126");
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 124: return pollStd(rxKavb_, 124");   // KAVB is wired (tests/app/test_kavb_mode.cpp)

    const std::string engine = readSource("core/src/engine/engine.cpp");
    need("core/src/engine/engine.cpp", engine, "else if (a == 125) rxKapp_.feed(x, n); else if (a == 126) rxKaft_.feed(x, n);");
    need("core/src/engine/engine.cpp", engine, "case 125: rxKapp_.reset(); break;");
    need("core/src/engine/engine.cpp", engine, "case 126: rxKaft_.reset(); break;");

    const std::string place = readSource("core/src/engine/engine_place.cpp");
    need("core/src/engine/engine_place.cpp", place, "case 125: rxKapp_.configure(r); break;");
    need("core/src/engine/engine_place.cpp", place, "case 126: rxKaft_.configure(r); break;");

    const std::string start = readSource("core/src/engine/engine_start.cpp");
    need("core/src/engine/engine_start.cpp", start, "rxKapp_.configure(rate_);");
    need("core/src/engine/engine_start.cpp", start, "rxKaft_.configure(rate_);");

    // the transmit side
    const std::string tx = readSource("core/src/tx/tx.cpp");
    need("core/src/tx/tx.cpp", tx, "mk(\"kapp\", \"KAPP\", 126,");
    need("core/src/tx/tx.cpp", tx, "mk(\"kaft\", \"KAFT\", 127,");
    need("core/src/tx/tx.cpp", tx, "if (id == \"kapp\") return TxPayloadKind::Kapp;");
    need("core/src/tx/tx.cpp", tx, "if (id == \"kaft\") return TxPayloadKind::Kaft;");
    need("core/src/tx/tx.cpp", tx, "makeKappTxGenerator(r,");
    need("core/src/tx/tx.cpp", tx, "makeKaftTxGenerator(r,");
    const std::string txui = readSource("app/panels/tx_ui.cpp");
    need("app/panels/tx_ui.cpp", txui, "kapp.profile");
    need("app/panels/tx_ui.cpp", txui, "kaft.width");
    need("app/panels/tx_ui.cpp", txui, "makeIqFileTxSink(fc)");   // the IQ-file destination serves every transmit mode
    need("app/panels/tx_ui.cpp", txui, "!txModeFinite(*mode)");
    const std::string pay = readSource("app/panels/tx_payload_ui.cpp");
    need("app/panels/tx_payload_ui.cpp", pay, "case TxPayloadKind::Kapp:");
    need("app/panels/tx_payload_ui.cpp", pay, "case TxPayloadKind::Kaft:");
    const std::string txset = readSource("app/panels/tx_step_settings.cpp");
    need("app/panels/tx_step_settings.cpp", txset, "destinationPanel(s, *mode, running);");

    if (fails == 0) std::printf("kapp and kaft modes: registered, panel strings, wiring and transmit present\n");
    return fails == 0 ? 0 : 1;
}
