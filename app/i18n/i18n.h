// Interface translations. The key is the English text as written in the source: tr("Start") gives the text in the chosen
// language, or the English text itself when there is no translation. The catalogs are app/i18n/<code>.txt, compiled in.
// The returned pointers stay valid for the whole run (a language once loaded is never freed).
#pragma once
#include <string>

// The text in the current language. "Label##id": the part before "##" is translated and the suffix kept, so the widget ID changes
// only with the visible text (fine for buttons, check boxes, menu items: nothing keeps their state).
const char* tr(const char* english);
// For widgets whose state is kept by their label (windows, tabs, tree nodes, collapsing headers, modal popups): the translated text
// followed by a fixed "###<english>" ID, the same in every language (English included). A label that has its own "###id" keeps it.
const char* trw(const char* english);
// Marks a string for translation where it is defined (tables of names); tr() is applied where it is drawn.
#define N_(s) s

namespace i18n {
struct Language { const char* code; const char* name; };   // name: in the language itself
const Language* languages(int& count);                     // English first
// Choose the language by code ("de", "fr", "es", "zh", "lt", "hu", "pt", "pt-PT", "it", "ru" ... "en"); an unknown code or one with a region ("de-AT",
// "zh_CN.UTF-8", "zh-Hans") is matched by its first letters, unless the list has that region ("pt-PT", "pt_PT.UTF-8" -> pt-PT). Returns the code chosen.
// A regional catalog (pt-PT.txt) holds only the texts that differ from its base (pt.txt); the rest comes from the base.
const char* set(const std::string& code);
const char* current();                                     // "en" when nothing is chosen
// The catalog text of a language (for the tests): lines of "English<TAB>translation", '#' comments, \n \t \\ escapes
const char* catalogText(const char* code);
}
