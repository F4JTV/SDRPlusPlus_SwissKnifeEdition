/*
 *    morse.cpp  --  Morse code table for the SDR++ CW decoder
 *
 *    Derived from FLDigi (src/cw_rtty/morse.cxx)
 *    Copyright (C) 2017 (FLDigi authors)
 *    Port for SDR++ (C) 2025 F4JTV
 *
 *    GNU General Public License v3 or later. See <http://www.gnu.org/licenses/>.
 */

#include "morse.h"

/*
 * Morse code character table. Shapes are held as a string, with '-' for
 * dash and '.' for dot. This is ported directly from FLDigi's main table.
 */
CWstruct cMorse::cw_table[] = {
    // Prosigns
    { true,  "=", "<BT>",  "-...-"  },
    { false, "~", "<AA>",  ".-.-"   },
    { true,  "<", "<AS>",  ".-..."  },
    { true,  ">", "<AR>",  ".-.-."  },
    { true,  "%", "<SK>",  "...-.-" },
    { true,  "+", "<KN>",  "-.--."  },
    { true,  "&", "<INT>", "..-.-"  },
    { true,  "{", "<HM>",  "....--" },
    { true,  "}", "<VE>",  "...-."  },
    // ASCII letters
    { true, "A", "A", ".-"   }, { true, "B", "B", "-..."  },
    { true, "C", "C", "-.-." }, { true, "D", "D", "-.."   },
    { true, "E", "E", "."    }, { true, "F", "F", "..-."  },
    { true, "G", "G", "--."  }, { true, "H", "H", "...."  },
    { true, "I", "I", ".."   }, { true, "J", "J", ".---"  },
    { true, "K", "K", "-.-"  }, { true, "L", "L", ".-.."  },
    { true, "M", "M", "--"   }, { true, "N", "N", "-."    },
    { true, "O", "O", "---"  }, { true, "P", "P", ".--."  },
    { true, "Q", "Q", "--.-" }, { true, "R", "R", ".-."   },
    { true, "S", "S", "..."  }, { true, "T", "T", "-"     },
    { true, "U", "U", "..-"  }, { true, "V", "V", "...-"  },
    { true, "W", "W", ".--"  }, { true, "X", "X", "-..-"  },
    { true, "Y", "Y", "-.--" }, { true, "Z", "Z", "--.."  },
    // Numerals
    { true, "0", "0", "-----" }, { true, "1", "1", ".----" },
    { true, "2", "2", "..---" }, { true, "3", "3", "...--" },
    { true, "4", "4", "....-" }, { true, "5", "5", "....." },
    { true, "6", "6", "-...." }, { true, "7", "7", "--..." },
    { true, "8", "8", "---.." }, { true, "9", "9", "----." },
    // Punctuation
    { true, "\\", "\\", ".-..-." }, { true, "'",  "'", ".----." },
    { true, "$",  "$",  "...-..-" },{ true, "(",  "(", "-.--."  },
    { true, ")",  ")",  "-.--.-" }, { true, ",",  ",", "--..--" },
    { true, "-",  "-",  "-....-" }, { true, ".",  ".", ".-.-.-" },
    { true, "/",  "/",  "-..-."  }, { true, ":",  ":", "---..." },
    { true, ";",  ";",  "-.-.-." }, { true, "?",  "?", "..--.." },
    { true, "_",  "_",  "..--.-" }, { true, "@",  "@", ".--.-." },
    { true, "!",  "!",  "-.-.--" },
    // Accented characters (disabled by default, like FLDigi)
    { true,  "Ä", "Ä", ".-.-"  }, { false, "Æ", "Æ", ".-.-"  },
    { false, "Å", "Å", ".--.-" }, { true,  "Ç", "Ç", "-.-.." },
    { false, "È", "È", ".-..-" }, { true,  "É", "É", "..-.." },
    { false, "Ó", "Ó", "---."  }, { true,  "Ö", "Ö", "---."  },
    { false, "Ø", "Ø", "---."  }, { true,  "Ñ", "Ñ", "--.--" },
    { true,  "Ü", "Ü", "..--"  }, { false, "Û", "Û", "..--"  },
    // Table terminator
    { false, "", "", "" }
};

void cMorse::enable(const std::string& s, bool val) {
    for (int i = 0; cw_table[i].rpr.length(); i++) {
        if (cw_table[i].chr == s || cw_table[i].prt == s) {
            cw_table[i].enabled = val;
            return;
        }
    }
}

void cMorse::init() {
    // Resolve the ".-.-" ambiguity: enable <AA> by default, disable Ä.
    enable("<AA>", true);
    enable("Ä", false);
}

std::string cMorse::rx_lookup(const std::string& rx) {
    for (int i = 0; cw_table[i].rpr.length(); i++) {
        if (rx == cw_table[i].rpr && cw_table[i].enabled) {
            return prosignDisplay ? cw_table[i].prt : cw_table[i].chr;
        }
    }
    return "";
}
