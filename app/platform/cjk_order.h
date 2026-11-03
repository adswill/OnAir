// The order the CJK system fonts are merged into the UI fonts. ImGui draws each character from the first merged font that has it, and the
// Korean and Chinese fonts carry kana and most kanji too, but not the Japanese simplified forms (声 録 断 気): with the Korean font first,
// a Japanese line mixed two fonts of different sizes and shapes. So the fonts of the interface language go first; the other languages
// keep the order from before (Korean, Chinese, Japanese), since Hangul is only in the Korean fonts.
#pragma once
#include <string>
#include <vector>

namespace plat {

// lang: the interface language as a code such as "ja", "zh-Hans" or "ja_JP.UTF-8" (the first two letters decide)
inline std::vector<std::string> cjkFontOrder(const std::string& lang, const std::vector<std::string>& ja, const std::vector<std::string>& zh,
                                             const std::vector<std::string>& ko) {
    const std::string l = lang.substr(0, 2);
    const std::vector<const std::vector<std::string>*> groups =
        l == "ja" ? std::vector<const std::vector<std::string>*>{&ja, &zh, &ko}
        : l == "zh" ? std::vector<const std::vector<std::string>*>{&zh, &ja, &ko}
                    : std::vector<const std::vector<std::string>*>{&ko, &zh, &ja};
    std::vector<std::string> out;
    for (const auto* g : groups) out.insert(out.end(), g->begin(), g->end());
    return out;
}

} // namespace plat
