// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW language model (see cw_lm.h). Clean-room: the vocabulary and the sentences below are written from ordinary amateur-radio practice.
#include "dect2/cw/cw_lm.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace dect2 {
namespace cw {

namespace {

const char* const kWords[] = {
    // procedure and Q-codes
    "CQ", "DE", "K", "KN", "AR", "SK", "BT", "BK", "R", "RR", "AS", "TEST", "QRZ", "QRL", "QRM", "QRN", "QRO", "QRP", "QRQ", "QRS", "QRT",
    "QRU", "QRV", "QRX", "QSB", "QSL", "QSO", "QSY", "QTH", "QTR", "QRA", "QSK", "QRG", "QSX",
    // abbreviations
    "ABT", "AGN", "ANT", "BCNU", "BD", "C", "CFM", "CL", "CPY", "CUAGN", "CUL", "CW", "DX", "ES", "FB", "FER", "FM", "GA", "GB", "GD", "GE",
    "GL", "GM", "GN", "GND", "GUD", "HI", "HR", "HV", "HW", "INFO", "LID", "MNI", "MSG", "N", "NIL", "NR", "NW", "OB", "OM", "OP", "OT", "PSE",
    "PWR", "RCVR", "RIG", "RPT", "RST", "RX", "SED", "SIG", "SRI", "TEMP", "TKS", "TNX", "TU", "TX", "UR", "VY", "WX", "WID", "WKD", "XCVR",
    "XYL", "YL", "73", "88", "55", "99", "ANTENNA", "BEAM", "DIPOLE", "VERTICAL", "LOOP", "YAGI", "WIRE", "WATTS", "W", "FINE", "GOOD", "BEST",
    "REGARDS", "THANKS", "CALL", "AGAIN", "NAME", "AGE", "HERE", "SUNNY", "CLOUDY", "COLD", "WARM", "RAIN", "SNOW", "WIND", "HOT", "NICE",
    "BAND", "BANDS", "CONDX", "COND", "CONTEST", "LOG", "CARD", "EQSL", "LOTW", "POTA", "SOTA", "FIELD", "DAY", "PORTABLE", "MOBILE", "HOME",
    "STATION", "QSO", "NAME", "OP", "PLS", "PLEASE", "SORRY", "COPY", "COPIED", "SOLID", "WEAK", "STRONG", "LOUD", "FADING", "NOISE", "NOISY",
    "KEY", "BUG", "PADDLE", "PADDLES", "KEYER", "SPEED", "WPM", "SLOW", "FAST", "DOWN", "UP", "FREQ", "FQ", "KHZ", "MHZ", "LISTEN", "LISTENING",
    "CALLING", "ANSWER", "WORK", "WORKED", "HEARD", "AGN", "REPEAT", "AGAIN", "NUMBER", "SERIAL", "ZONE", "STATE", "COUNTY", "GRID", "LOC",
    "TO", "THE", "AND", "FOR", "IS", "IN", "ON", "AT", "OF", "MY", "YOUR", "YOU", "I", "A", "IT", "BE", "WE", "ALL", "NO", "YES", "OK", "WITH",
    "HAVE", "HAD", "THIS", "THAT", "WILL", "WAS", "ARE", "NOT", "BUT", "FROM", "WELL", "VERY", "MUCH", "MANY", "NOW", "NEW", "OLD", "SEE", "NEXT",
    "TIME", "WORLD", "RADIO", "AMATEUR", "HAM", "HAPPY", "BIRTHDAY", "CHRISTMAS", "EASTER",
    "JOHN", "TOM", "PETE", "MIKE", "HANS", "BOB", "ED", "STEVE", "PAUL", "ALEX", "TONY", "FRED", "KARL", "OLE", "JAN", "DAVE", "MARK", "LUCA",
    "ANDREW", "JIM", "BILL", "JOE", "DAN", "GEORGE", "HARRY", "TED", "JACK", "SAM", "KEN", "RON", "RAY", "PHIL", "NICK", "PAT", "MARTIN", "PETER",
    "IVAN", "OLEG", "BORIS", "SERGEI", "JURGEN", "KLAUS", "FRANK", "UWE", "UDO", "RALF", "JEAN", "PIERRE", "LUIGI", "MARCO", "ANTON", "ANDRIUS",
    "VILNIUS", "KAUNAS", "BERLIN", "LONDON", "PARIS", "ROMA", "WARSAW", "OSLO", "RIGA", "WIEN", "PRAHA", "MADRID", "DUBLIN", "HELSINKI",
    "TALLINN", "MOSCOW", "KIEV", "SOFIA", "ATHENS", "LISBON", "BONN", "LEEDS", "TEXAS", "OHIO", "MAINE", "UK", "USA", "EU", "DL", "OK", "LY",
};

const char* const kSentences[] = {
    "CQ CQ CQ DE LY2ABC LY2ABC LY2ABC K", "CQ CQ DE DL1XYZ DL1XYZ PSE K", "CQ TEST DL1XYZ TEST", "CQ DX CQ DX DE W1AW W1AW K",
    "CQ POTA CQ POTA DE K1ABC K1ABC K", "CQ SOTA DE OH2XYZ OH2XYZ K", "QRZ DE G4ABC K", "QRZ? DE LY2ABC", "LY2ABC DE G4ABC K",
    "G4ABC DE LY2ABC UR RST 599 599 <BT> NAME JOHN JOHN <BT> QTH LONDON LONDON <BT> HW? <KN>",
    "LY2ABC DE G4ABC R R TNX FER CALL <BT> UR RST 579 579 <BT> OP ANDRIUS QTH VILNIUS <BT> RIG IC7300 PWR 100 W ANT DIPOLE <BT> WX SUNNY TEMP 20 C <AR>",
    "G4ABC DE LY2ABC GM JOHN ES TNX FER FB QSO <BT> UR RST IS 559 <BT> QTH IS VILNIUS <BT> NAME IS ANDRIUS <BT> HW CPY? <KN>",
    "R R FB JOHN TNX FER INFO <BT> RIG HR IS FT991 PWR 50 W ANT IS VERTICAL <BT> WX HR IS RAIN ES COLD <BT> BTU JOHN <KN>",
    "DL1XYZ DE LY2ABC TU 73 ES GL <SK>", "TU 73 <SK>", "TNX FER QSO 73 ES CUL <SK>", "OK 73 GL DX <SK> E E", "5NN TU", "TU 5NN 14", "599 023 TU",
    "UR 5NN 5NN IN 15 15", "QSL TNX", "QSL VIA LOTW", "PSE QSL VIA BURO", "AGN PSE", "PSE RPT UR CALL", "QRS PSE", "QRS PSE OM", "QRM QRM PSE AGN", "QSB QSB",
    "SRI QRM", "SRI NIL", "NIL", "R R R", "R TU", "AS AS", "<AS> PSE", "VVV VVV VVV DE LY2ABC", "DE LY2ABC K", "K", "KN", "BK",
    "GM OM ES TNX FER CALL", "GA OM", "GE OM ES TNX FER CALL", "HI HI", "FB OM", "VY FB", "VY 73", "BEST 73", "73 ES GB", "73 DE LY2ABC <SK>",
    "WX HR SUNNY ES WARM TEMP 25 C", "WX HR CLOUDY ES COLD", "TNX FER NICE QSO ES HPE CUAGN", "ANT HR IS 3 EL YAGI AT 12 M", "PWR HR 100 W", "RIG HR IS K3 ES ANT IS DIPOLE",
    "MY NAME IS ANDRIUS", "MY QTH IS VILNIUS", "NAME HR IS TOM", "QTH HR KAUNAS", "AGE 45", "AGE 62 YRS", "UR RST 599 599 599", "UR RST IS 449", "RST 559 559 559",
    "PSE QSY 14.025", "QRG 7.030", "QSY UP 5", "QRZ DX", "LY2ABC UP 2", "LISTENING UP", "TEST DL1XYZ", "TEST DL1XYZ 5NN 001", "DL1XYZ 5NN 015", "NR 123",
    "CFM 73", "CFM ALL", "MNI TNX", "TKS FER CALL", "TKS ES 73", "THANKS FOR THE QSO", "THANK YOU AND GOOD LUCK", "GOOD LUCK ES 73", "HAPPY NEW YEAR ES 73",
    "<BT> HW? <KN>", "<BT> QTH <BT>", "<AR>", "<SK>", "<KN>", "<BT>",
};

const char* const kPrefixes[] = {"LY", "DL", "G", "M", "OK", "SP", "OH", "SM", "ON", "PA", "F", "I", "EA", "HB9", "OE", "YL", "ES", "UR", "RA", "UA", "JA", "VK",
                                 "ZL", "W", "K", "N", "AA", "WA", "VE", "LU", "PY", "9A", "S5", "YU", "HA", "OM", "TF", "SV", "LZ", "YO", "CT", "EI", "GM", "GW",
                                 "DK", "DJ", "DF", "DG", "DH", "SQ", "SP", "OZ", "LA", "SA", "OH", "RW", "RK", "UA3", "RA3", "R3", "9K", "A4", "EA8", "CN8", "3B8"};

int symOfChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ' ') return kLmSpace;
    return 37;
}

struct Model {
    std::vector<float> logp;                  // [c2][c1][c]
    std::vector<std::string> words;           // sorted
    Model() {
        std::vector<std::string> corpus;
        // the sentences, each a few times; the word list as short phrases
        for (const char* s : kSentences) for (int r = 0; r < 3; r++) corpus.push_back(s);
        for (const char* w : kWords) corpus.push_back(w);
        {   // callsigns in the usual places
            uint32_t st = 12345;
            auto rnd = [&](uint32_t n) { st = st * 1664525u + 1013904223u; return (st >> 8) % n; };
            for (int i = 0; i < 700; i++) {
                std::string c = kPrefixes[rnd(sizeof kPrefixes / sizeof *kPrefixes)];
                if (c.size() == 1 && rnd(2)) c += (char)('A' + rnd(26));
                if (!std::isdigit((unsigned char)c.back())) c += (char)('0' + rnd(10));
                const int n = 1 + (int)rnd(3) + (rnd(5) == 0);
                for (int k = 0; k < n; k++) c += (char)('A' + rnd(26));
                if (rnd(15) == 0) c += "/P";
                std::string line;
                switch (rnd(5)) {
                case 0: line = "CQ CQ DE " + c + " " + c + " K"; break;
                case 1: line = c + " DE " + c; break;
                case 2: line = "TEST " + c; break;
                case 3: line = "QRZ DE " + c + " <AR>"; break;
                default: line = "R " + c + " TU 5NN"; break;
                }
                corpus.push_back(line);
            }
            for (int i = 0; i < 200; i++) {   // reports and numbers
                std::string line = "UR RST " + std::to_string(4 + rnd(2)) + std::to_string(3 + rnd(7)) + std::to_string(5 + rnd(5)) + " " + std::to_string(rnd(300));
                corpus.push_back(line);
            }
        }
        const int N = kLmSyms;
        std::vector<float> c3((size_t)N * N * N, 0.f), c2((size_t)N * N, 0.f), c1((size_t)N, 0.f);
        std::vector<float> n2((size_t)N * N, 0.f), n1((size_t)N, 0.f);   // context totals
        for (const std::string& line : corpus) {
            std::vector<int> s;
            s.push_back(kLmSpace); s.push_back(kLmSpace);
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == '<') {
                    const size_t e = line.find('>', i);
                    if (e != std::string::npos) { s.push_back(lmSymbol(line.substr(i, e - i + 1).c_str())); i = e; continue; }
                }
                s.push_back(symOfChar(line[i]));
            }
            s.push_back(kLmSpace);
            for (size_t i = 2; i < s.size(); i++) {
                c3[((size_t)s[i - 2] * N + s[i - 1]) * N + s[i]] += 1; n2[(size_t)s[i - 2] * N + s[i - 1]] += 1;
                c2[(size_t)s[i - 1] * N + s[i]] += 1; n1[(size_t)s[i - 1]] += 1;
                c1[(size_t)s[i]] += 1;
            }
        }
        float tot = 0;
        for (float v : c1) tot += v;
        logp.assign((size_t)N * N * N, 0.f);
        for (int a = 0; a < N; a++)
            for (int b = 0; b < N; b++)
                for (int c = 0; c < N; c++) {
                    const float p1 = (c1[(size_t)c] + 0.5f) / (tot + 0.5f * N);
                    const float p2 = n1[(size_t)b] > 0 ? (c2[(size_t)b * N + c] + 1.5f * p1) / (n1[(size_t)b] + 1.5f) : p1;
                    const float p3 = n2[(size_t)a * N + b] > 0 ? (c3[((size_t)a * N + b) * N + c] + 2.0f * p2) / (n2[(size_t)a * N + b] + 2.0f) : p2;
                    logp[((size_t)a * N + b) * N + c] = std::log(p3);
                }
        for (const char* w : kWords) words.push_back(w);
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
    }
};

const Model& model() {
    static const Model m;
    return m;
}

// the shape of a callsign: a prefix of one to three characters with a letter, one figure, a suffix of one to four letters, "/P" style tail
bool callShape(const std::string& w0) {
    std::string w = w0;
    const size_t sl = w.find('/');
    if (sl != std::string::npos) { if (sl < 3 || w.size() - sl > 4) return false; w.resize(sl); }
    if (w.size() < 3 || w.size() > 9) return false;
    // the last figure that is followed only by letters (1 - 4)
    int d = -1;
    for (int i = (int)w.size() - 1; i >= 0; i--) {
        if (std::isdigit((unsigned char)w[(size_t)i])) { d = i; break; }
        if (!std::isalpha((unsigned char)w[(size_t)i])) return false;
    }
    if (d < 1) {
        if (d == 0 && w.size() >= 3 && w.size() <= 5 && std::isalpha((unsigned char)w[1])) return false;
        return false;
    }
    const int suf = (int)w.size() - 1 - d;
    if (suf < 1 || suf > 4) return false;
    bool letter = false;
    for (int i = 0; i < d; i++) { if (!std::isalnum((unsigned char)w[(size_t)i])) return false; if (std::isalpha((unsigned char)w[(size_t)i])) letter = true; }
    return letter && d <= 3;
}

} // namespace

int lmSymbol(const char* t) {
    if (!t || !*t) return kLmSpace;
    if (t[1] == 0) return symOfChar(t[0]);
    if (!std::strcmp(t, "<AR>")) return 38;
    if (!std::strcmp(t, "<SK>")) return 39;
    if (!std::strcmp(t, "<BT>")) return 40;
    if (!std::strcmp(t, "<KN>")) return 41;
    return 42;
}

float lmLogP(int c2, int c1, int c) {
    return model().logp[((size_t)c2 * kLmSyms + (size_t)c1) * kLmSyms + (size_t)c];
}

float lmWordBonus(const uint8_t* syms, int n) {
    if (n <= 0) return 0.f;
    std::string w;
    bool alnum = true, digits = true;
    for (int i = 0; i < n; i++) {
        const int s = syms[i];
        if (s < 26) { w += (char)('A' + s); digits = false; }
        else if (s < 36) w += (char)('0' + (s - 26));
        else { alnum = false; digits = false; w += '?'; }
    }
    if (!alnum) return n == 1 ? 0.f : -0.5f;
    const auto& v = model().words;
    if (std::binary_search(v.begin(), v.end(), w)) return n == 1 ? 0.8f : (n == 2 ? 1.8f : 3.0f);
    if (digits) return n == 3 ? 1.5f : (n <= 3 ? 0.8f : 0.f);
    if (callShape(w)) return n >= 5 ? 3.0f : 1.8f;
    return -0.6f;     // a word of nothing known: slightly worse than the same letters made into a known one
}

} // namespace cw
} // namespace dect2
