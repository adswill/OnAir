// The screens of the modes added after FM: the table that maps a family to its ModeUi, and small helpers for the shell.
#include "app.h"

extern const ModeUi kDvbsUi;
extern const ModeUi kDtmbUi;
extern const ModeUi kAtvUi;
extern const ModeUi kDmrUi;
extern const ModeUi kDrmUi;
extern const ModeUi kAdsbUi;

const ModeUi* modeUi(int family) {
    switch (family) {
    case 6: return &kDvbsUi;
    case 7: return &kDtmbUi;
    case 8: return &kAtvUi;
    case 9: return &kDmrUi;
    case 10: return &kDrmUi;
    case 11: return &kAdsbUi;
    default: return nullptr;
    }
}

// the title of the list on the right: STATIONS for FM and DAB, the mode's own, or SERVICES
const char* listTitle(const App& a, bool upper) {
    if (const ModeUi* mu = modeUi(a.family)) {
        if (upper) return mu->sideTitle;
        static char b[8][32];
        static int k = 0;
        char* o = b[k++ & 7];
        snprintf(o, 32, "%s", mu->sideTitle);
        for (int i = 1; o[i]; i++) o[i] = (char)tolower((unsigned char)o[i]);
        return o;
    }
    const bool stations = a.fmMode || a.dabMode;
    return upper ? (stations ? "STATIONS" : "SERVICES") : (stations ? "Stations" : "Services");
}
