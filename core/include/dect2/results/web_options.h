// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The option controls of every mode for OnAir Web (docs/WEB-SCHEMA-3.9.md, "Mode options"): the receivers' own public setters (a bool, a
// number, an enum, or an int whose comment lists its values), found in the headers by tools/gen_web_options.py the same way the panel
// schema is (web_schema.h), so the page draws one set of controls for every mode. core/src/results/web_options_gen.cpp is generated.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
class Engine;
namespace webschema {

// A JSON array per standard (RxTelemetry::standard, as the panel uses), declaration order:
//   {"n":"setThresholdDb","k":"num"|"int"|"bool"|"choice"|"mask","l":label,"h":help?,"u":unit?,"c":[{"v":0,"l":"automatic"},...]?,"t":"s"|"a"}
// "t": Simple (the first ones) or Advanced. nullptr: the mode has no options of its own.
const char* optionsJson(int standard);
std::vector<int> optionStandards();
std::string allOptionsJson();                // {"<standard>":[...], ...}
// Checks the value against the option's kind (finite, whole for int / choice / mask, 0 / 1 for bool, one of the listed values for a choice,
// only the listed bits for a mask) and calls the receiver's setter. false with why: no such option, a bad value.
bool applyOption(Engine& e, int standard, const std::string& name, double value, std::string& why);
// The generated dispatch alone (no checks)
bool applyOptionUnchecked(Engine& e, int standard, const std::string& name, double value);
// The checks alone (tests)
bool checkOption(int standard, const std::string& name, double value, std::string& why);

} // namespace webschema
} // namespace dect2
