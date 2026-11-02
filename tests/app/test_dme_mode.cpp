// The DME mode (family 121, engine standard 123, active standard 122) is registered: its mode-table row, its panel's strings and its
// engine wiring are in the sources. Text checks only, so it builds without the app.
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
        if (src.find(what) == std::string::npos) {
            std::printf("FAIL: %s: missing %s\n", file, what);
            fails++;
        }
    };

    const std::string table = readSource("app/ui/mode_table.cpp");
    need("app/ui/mode_table.cpp", table, "{121, \"DME\", 2,");

    const std::string modeui = readSource("app/panels/modeui.cpp");
    need("app/panels/modeui.cpp", modeui, "case 121: return &kDmeUi;");

    const std::string panel = readSource("app/dme/dme_ui.cpp");
    need("app/dme/dme_ui.cpp", panel, "const ModeUi kDmeUi");
    need("app/dme/dme_ui.cpp", panel, "row(\"channel\"");
    need("app/dme/dme_ui.cpp", panel, "row(\"pair rate\"");
    need("app/dme/dme_ui.cpp", panel, "row(\"level\"");
    need("app/dme/dme_ui.cpp", panel, "row(\"ident\"");
    need("app/dme/dme_ui.cpp", panel, "DME");

    const std::string live = readSource("core/src/dme/dme_live.cpp");
    need("core/src/dme/dme_live.cpp", live, "t.stdMode = 123;");
    need("core/src/dme/dme_live.cpp", live, "t.name = \"DME\";");

    const std::string modes = readSource("core/src/engine/modes.cpp");
    need("core/src/engine/modes.cpp", modes, "dmeTuning()");
    need("core/src/engine/modes.cpp", modes, "case 122: return dmeSummary(t.dme);");

    const std::string telemetry = readSource("core/src/engine/engine_telemetry.cpp");
    need("core/src/engine/engine_telemetry.cpp", telemetry, "case 122: return pollStd(rxDme_, 122");

    const std::string engine = readSource("core/src/engine/engine.cpp");
    need("core/src/engine/engine.cpp", engine, "else if (a == 122) rxDme_.feed(x, n);");

    if (fails == 0) std::printf("dme mode: registered, panel strings and wiring present\n");
    return fails == 0 ? 0 : 1;
}
