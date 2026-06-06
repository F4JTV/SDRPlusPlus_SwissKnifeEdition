/*
 *    morse.h  --  Morse code table for the SDR++ CW decoder
 *
 *    Derived from FLDigi (src/include/morse.h, src/cw_rtty/morse.cxx)
 *    Copyright (C) 2017 (FLDigi authors)
 *    Port for SDR++ (C) 2025 F4JTV
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version. See <http://www.gnu.org/licenses/>.
 */

#pragma once
#include <string>

#define CW_DOT_REPRESENTATION  '.'
#define CW_DASH_REPRESENTATION '-'

// One entry of the Morse table.
struct CWstruct {
    bool        enabled; // true if the character is active
    std::string chr;     // utf-8 representation of the character
    std::string prt;     // utf-8 printable representation (e.g. "<BT>")
    std::string rpr;     // dot-dash code representation (e.g. "-...-")
};

// Morse decoder lookup. The table itself is ported verbatim from FLDigi.
class cMorse {
public:
    cMorse() { init(); }
    ~cMorse() {}

    void init();

    // Enable/disable optional characters (accents, prosigns, punctuation).
    void enable(const std::string& s, bool val);

    // Whether prosigns are printed as "<BT>" (true) or as their substitute char.
    void setProsignDisplay(bool en) { prosignDisplay = en; }

    // Receive lookup: turn a dot/dash string into its printable character.
    // Returns an empty string when the code is not a valid character.
    std::string rx_lookup(const std::string& rx);

private:
    static CWstruct cw_table[];
    bool prosignDisplay = true;
};
