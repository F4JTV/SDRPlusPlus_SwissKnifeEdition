#pragma once
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// PSK31 Varicode
//
// Table reproduced from the FLDIGI project (src/psk/pskvaricode.cxx,
// (C) 2006 Dave Freese W1HKJ, GPLv3+). Every codeword begins and ends with a
// '1' and never contains "00", so two consecutive zero bits act as the
// inter-character separator.
//
// This header provides a streaming decoder: feed it the differentially
// detected bits one by one and it emits ASCII codepoints as they complete.
// ---------------------------------------------------------------------------

namespace fldigi {

    // ASCII 0..127 -> Varicode bit string (MSB first, as transmitted)
    static const char* PSK_VARICODE[128] = {
    "1010101011",
    "1011011011",
    "1011101101",
    "1101110111",
    "1011101011",
    "1101011111",
    "1011101111",
    "1011111101",
    "1011111111",
    "11101111",
    "11101",
    "1101101111",
    "1011011101",
    "11111",
    "1101110101",
    "1110101011",
    "1011110111",
    "1011110101",
    "1110101101",
    "1110101111",
    "1101011011",
    "1101101011",
    "1101101101",
    "1101010111",
    "1101111011",
    "1101111101",
    "1110110111",
    "1101010101",
    "1101011101",
    "1110111011",
    "1011111011",
    "1101111111",
    "1",
    "111111111",
    "101011111",
    "111110101",
    "111011011",
    "1011010101",
    "1010111011",
    "101111111",
    "11111011",
    "11110111",
    "101101111",
    "111011111",
    "1110101",
    "110101",
    "1010111",
    "110101111",
    "10110111",
    "10111101",
    "11101101",
    "11111111",
    "101110111",
    "101011011",
    "101101011",
    "110101101",
    "110101011",
    "110110111",
    "11110101",
    "110111101",
    "111101101",
    "1010101",
    "111010111",
    "1010101111",
    "1010111101",
    "1111101",
    "11101011",
    "10101101",
    "10110101",
    "1110111",
    "11011011",
    "11111101",
    "101010101",
    "1111111",
    "111111101",
    "101111101",
    "11010111",
    "10111011",
    "11011101",
    "10101011",
    "11010101",
    "111011101",
    "10101111",
    "1101111",
    "1101101",
    "101010111",
    "110110101",
    "101011101",
    "101110101",
    "101111011",
    "1010101101",
    "111110111",
    "111101111",
    "111111011",
    "1010111111",
    "101101101",
    "1011011111",
    "1011",
    "1011111",
    "101111",
    "101101",
    "11",
    "111101",
    "1011011",
    "101011",
    "1101",
    "111101011",
    "10111111",
    "11011",
    "111011",
    "1111",
    "111",
    "111111",
    "110111111",
    "10101",
    "10111",
    "101",
    "110111",
    "1111011",
    "1101011",
    "11011111",
    "1011101",
    "111010101",
    "1010110111",
    "110111011",
    "1010110101",
    "1011010111",
    "1110110101",
    };

    class VaricodeDecoder {
    public:
        VaricodeDecoder() {
            // Build reverse lookup: bit string -> ASCII code
            for (int i = 0; i < 128; i++) {
                rev[std::string(PSK_VARICODE[i])] = i;
            }
            reset();
        }

        void reset() {
            code.clear();
            prevZero = true; // line idles at 0
        }

        // Feed one differential bit. Returns the decoded ASCII code (0..127)
        // when a character completes, or -1 otherwise.
        int process(int bit) {
            if (bit == 0 && prevZero) {
                // Second consecutive zero -> character separator "00".
                int ch = -1;
                if (!code.empty()) {
                    // The first separator zero was optimistically appended; drop it.
                    if (code.back() == '0') { code.pop_back(); }
                    if (!code.empty()) {
                        auto it = rev.find(code);
                        if (it != rev.end()) { ch = it->second; }
                    }
                    code.clear();
                }
                prevZero = true;
                return ch;
            }
            code.push_back(bit ? '1' : '0');
            // Guard against runaway accumulation on noise (longest code is 10 bits).
            if (code.size() > 12) { code.clear(); }
            prevZero = (bit == 0);
            return -1;
        }

    private:
        std::unordered_map<std::string, int> rev;
        std::string code;
        bool prevZero;
    };

}
