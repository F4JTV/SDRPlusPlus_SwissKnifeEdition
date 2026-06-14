/*
 * js8_varicode.cpp - JS8 token interpretation. Ported from JS8Call's
 * Varicode.cpp (GPL-3.0). See js8_varicode.h for scope.
 */
#include "js8_varicode.h"
#include "js8_core.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace js8 {
namespace {

// 66-symbol alphabet used by unpack72bits. The first 64 entries match the
// js8_core message alphabet, so a 12-char token maps consistently.
constexpr const char* ALPHABET72 =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.";

// Callsign / grid alphabet (index 0..38).
constexpr const char* ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";

const uint32_t NBASECALL = 37u * 36u * 10u * 27u * 27u * 27u;
const uint16_t NBASEGRID = 180u * 180u;
const uint16_t NUSERGRID = NBASEGRID + 10u;
const uint16_t NMAXGRID  = (1u << 15) - 1u;

int idx72(char c) {
    for (int i = 0; ALPHABET72[i]; ++i)
        if (ALPHABET72[i] == c) return i;
    return 0;
}

// --- bit helpers (mirror Varicode::intToBits / bitsToInt / unpack72bits) ---
using Bits = std::vector<uint8_t>;

Bits intToBits(uint64_t value, int expected) {
    Bits bits;
    while (value) {
        bits.insert(bits.begin(), value & 1);
        value >>= 1;
    }
    while (static_cast<int>(bits.size()) < expected)
        bits.insert(bits.begin(), 0);
    return bits;
}

uint64_t bitsToInt(const Bits& b, int start, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v = (v << 1) | (b[start + i] & 1);
    return v;
}

// Unpack 12 token chars to a 64-bit value + 8-bit remainder.
uint64_t unpack72bits(const std::string& text, uint8_t& rem) {
    uint64_t value = 0;
    for (int i = 0; i < 10; ++i)
        value |= static_cast<uint64_t>(idx72(text[i])) << (58 - 6 * i);
    uint8_t remHigh = static_cast<uint8_t>(idx72(text[10]));
    value |= (remHigh >> 2);
    uint8_t remLow = static_cast<uint8_t>(idx72(text[11]));
    rem = static_cast<uint8_t>(((remHigh & 0x3) << 6) | remLow);
    return value;
}

// --- directed command table (value -> text) -------------------------------
const std::map<int, std::string>& directedCmds() {
    static const std::map<int, std::string> m = {
        {0, " SNR?"},      {1, " DIT DIT"},   {2, " NACK"},
        {3, " HEARING?"},  {4, " GRID?"},     {5, ">"},
        {6, " STATUS?"},   {7, " STATUS"},    {8, " HEARING"},
        {9, " MSG"},       {10, " MSG TO:"},  {11, " QUERY"},
        {12, " QUERY MSGS"},{13, " QUERY CALL"},{14, " ACK"},
        {15, " GRID"},     {16, " INFO?"},    {17, " INFO"},
        {18, " FB"},       {19, " HW CPY?"},  {20, " SK"},
        {21, " RR"},       {22, " QSL?"},     {23, " QSL"},
        {24, " CMD"},      {25, " SNR"},      {26, " NO"},
        {27, " YES"},      {28, " 73"},       {29, " HEARTBEAT SNR"},
        {30, " AGN?"},     {31, " "}};
    return m;
}

bool isSnrCmdValue(int v) { return v == 25 || v == 29; }

// --- special base-call values ----------------------------------------------
const std::map<uint32_t, std::string>& baseCalls() {
    static const std::map<uint32_t, std::string> m = [] {
        std::map<uint32_t, std::string> x;
        x[NBASECALL + 1]  = "<....>";
        x[NBASECALL + 2]  = "@ALLCALL";
        x[NBASECALL + 3]  = "@JS8NET";
        x[NBASECALL + 4]  = "@DX/NA";  x[NBASECALL + 5]  = "@DX/SA";
        x[NBASECALL + 6]  = "@DX/EU";  x[NBASECALL + 7]  = "@DX/AS";
        x[NBASECALL + 8]  = "@DX/AF";  x[NBASECALL + 9]  = "@DX/OC";
        x[NBASECALL + 10] = "@DX/AN";
        x[NBASECALL + 11] = "@REGION/1"; x[NBASECALL + 12] = "@REGION/2";
        x[NBASECALL + 13] = "@REGION/3";
        for (int i = 0; i <= 9; ++i)
            x[NBASECALL + 14 + i] = "@GROUP/" + std::to_string(i);
        x[NBASECALL + 24] = "@COMMAND"; x[NBASECALL + 25] = "@CONTROL";
        x[NBASECALL + 26] = "@NET";     x[NBASECALL + 27] = "@NTS";
        for (int i = 0; i <= 4; ++i)
            x[NBASECALL + 28 + i] = "@RESERVE/" + std::to_string(i);
        x[NBASECALL + 33] = "@APRSIS";  x[NBASECALL + 34] = "@RAGCHEW";
        x[NBASECALL + 35] = "@JS8";     x[NBASECALL + 36] = "@EMCOMM";
        x[NBASECALL + 37] = "@ARES";    x[NBASECALL + 38] = "@MARS";
        x[NBASECALL + 39] = "@AMRRON";  x[NBASECALL + 40] = "@RACES";
        x[NBASECALL + 41] = "@RAYNET";  x[NBASECALL + 42] = "@RADAR";
        x[NBASECALL + 43] = "@SKYWARN"; x[NBASECALL + 44] = "@CQ";
        x[NBASECALL + 45] = "@HB";      x[NBASECALL + 46] = "@QSO";
        x[NBASECALL + 47] = "@QSOPARTY";x[NBASECALL + 48] = "@CONTEST";
        x[NBASECALL + 49] = "@FIELDDAY";x[NBASECALL + 50] = "@SOTA";
        x[NBASECALL + 51] = "@IOTA";    x[NBASECALL + 52] = "@POTA";
        x[NBASECALL + 53] = "@QRP";     x[NBASECALL + 54] = "@QRO";
        return x;
    }();
    return m;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

// --- callsign / grid unpacking ---------------------------------------------
std::string unpackCallsign(uint32_t value, bool portable) {
    auto it = baseCalls().find(value);
    if (it != baseCalls().end()) return it->second;

    char word[7] = {0};
    uint32_t tmp;
    tmp = value % 27 + 10; word[5] = ALNUM[tmp]; value /= 27;
    tmp = value % 27 + 10; word[4] = ALNUM[tmp]; value /= 27;
    tmp = value % 27 + 10; word[3] = ALNUM[tmp]; value /= 27;
    tmp = value % 10;      word[2] = ALNUM[tmp]; value /= 10;
    tmp = value % 36;      word[1] = ALNUM[tmp]; value /= 36;
    tmp = value % 37;      word[0] = ALNUM[tmp];

    std::string callsign(word, 6);
    if (callsign.rfind("3D0", 0) == 0)
        callsign = "3DA0" + callsign.substr(3);
    if (callsign[0] == 'Q' && callsign[1] >= 'A' && callsign[1] <= 'Z')
        callsign = "3X" + callsign.substr(1);
    callsign = trim(callsign);
    if (portable) callsign += "/P";
    return callsign;
}

std::string deg2grid(float dlong, float dlat) {
    char grid[7] = {0};
    if (dlong < -180) dlong += 360;
    if (dlong >  180) dlong -= 360;
    int nlong = static_cast<int>(60.0 * (180.0 - dlong) / 5);
    int n1 = nlong / 240;
    int n2 = (nlong - 240 * n1) / 24;
    int n3 = (nlong - 240 * n1 - 24 * n2);
    grid[0] = static_cast<char>('A' + n1);
    grid[2] = static_cast<char>('0' + n2);
    grid[4] = static_cast<char>('a' + n3);
    int nlat = static_cast<int>(60.0 * (dlat + 90) / 2.5);
    n1 = nlat / 240;
    n2 = (nlat - 240 * n1) / 24;
    n3 = (nlat - 240 * n1 - 24 * n2);
    grid[1] = static_cast<char>('A' + n1);
    grid[3] = static_cast<char>('0' + n2);
    grid[5] = static_cast<char>('a' + n3);
    return std::string(grid, 6);
}

std::string unpackGrid(uint16_t value) {
    if (value > NBASEGRID) return "";
    float dlat  = value % 180 - 90;
    float dlong = value / 180 * 2 - 180 + 2;
    return deg2grid(dlong, dlat).substr(0, 4);
}

std::string unpackAlphaNumeric50(uint64_t packed) {
    char word[12] = {0};
    uint64_t tmp;
    tmp = packed % 38; word[10] = ALNUM[tmp]; packed /= 38;
    tmp = packed % 38; word[9]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 38; word[8]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 2;  word[7]  = tmp ? '/' : ' '; packed /= 2;
    tmp = packed % 38; word[6]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 38; word[5]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 38; word[4]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 2;  word[3]  = tmp ? '/' : ' '; packed /= 2;
    tmp = packed % 38; word[2]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 38; word[1]  = ALNUM[tmp]; packed /= 38;
    tmp = packed % 39; word[0]  = ALNUM[tmp];
    std::string s(word, 11);
    std::string out;
    for (char c : s) if (c != ' ') out += c;
    return out;
}

int unpackCmd(uint8_t value, uint8_t& num) {
    if (value & (1 << 7)) {
        num = value & ((1 << 6) - 1);
        int cmd = 25;  // SNR
        if (value & (1 << 6)) cmd = 29;  // HEARTBEAT SNR
        return cmd;
    }
    num = 0;
    return value & ((1 << 7) - 1);
}

std::string formatSNR(int snr) {
    std::string s = (snr >= 0 ? "+" : "-");
    int a = snr < 0 ? -snr : snr;
    if (a < 10) s += "0";
    s += std::to_string(a);
    return s;
}

// --- default Huffman table for free-text data frames -----------------------
struct HuffEntry { const char* ch; const char* code; };
const std::vector<HuffEntry>& huffTable() {
    static const std::vector<HuffEntry> t = {
        {" ", "01"}, {"E", "100"}, {"T", "1101"}, {"A", "0011"},
        {"O", "11111"}, {"I", "11100"}, {"N", "10111"}, {"S", "10100"},
        {"H", "00011"}, {"R", "00000"}, {"D", "111011"}, {"L", "110011"},
        {"C", "110001"}, {"U", "101101"}, {"M", "101011"}, {"W", "001011"},
        {"F", "001001"}, {"G", "000101"}, {"Y", "000011"}, {"P", "1111011"},
        {"B", "1111001"}, {".", "1110100"}, {"V", "1100101"}, {"K", "1100100"},
        {"-", "1100001"}, {"+", "1100000"}, {"?", "1011001"}, {"!", "1011000"},
        {"\"", "1010101"}, {"X", "1010100"}, {"0", "0010101"}, {"J", "0010100"},
        {"1", "0010001"}, {"Q", "0010000"}, {"2", "0001001"}, {"Z", "0001000"},
        {"3", "0000101"}, {"5", "0000100"}, {"4", "11110101"}, {"9", "11110100"},
        {"8", "11110001"}, {"6", "11110000"}, {"7", "11101011"}, {"/", "11101010"}};
    return t;
}

std::string huffDecode(const std::string& bits) {
    std::string text;
    std::size_t pos = 0;
    while (pos < bits.size()) {
        bool found = false;
        for (const auto& e : huffTable()) {
            std::size_t len = std::string(e.code).size();
            if (pos + len <= bits.size() &&
                bits.compare(pos, len, e.code) == 0) {
                text += e.ch;
                pos += len;
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    return text;
}

std::string bitsToStr(const Bits& b) {
    std::string s;
    s.reserve(b.size());
    for (auto bit : b) s += (bit ? '1' : '0');
    return s;
}

int lastIndexOfZero(const Bits& b) {
    for (int i = static_cast<int>(b.size()) - 1; i >= 0; --i)
        if (b[i] == 0) return i;
    return -1;
}

// --- frame interpreters -----------------------------------------------------
std::string interpretCompound(const std::string& token, int i3) {
    uint8_t packed8 = 0;
    uint64_t value = unpack72bits(token, packed8);
    Bits bits = intToBits(value, 64);

    uint8_t packed5 = packed8 >> 3;
    uint32_t packed_callsign = static_cast<uint32_t>(bitsToInt(bits, 3, 50));
    uint16_t packed11 = static_cast<uint16_t>(bitsToInt(bits, 53, 11));
    uint16_t num = (packed11 << 5) | packed5;

    std::string callsign = unpackAlphaNumeric50(packed_callsign);
    std::string out = callsign;

    if (i3 == FrameHeartbeat) {
        std::string grid = unpackGrid(num & ((1 << 15) - 1));
        if (!grid.empty()) out += " " + grid;
        out = callsign + ": HB" + (grid.empty() ? "" : " " + grid);
    } else if (num <= NBASEGRID) {
        std::string grid = unpackGrid(num);
        if (!grid.empty()) out += " " + grid;
    } else if (num >= NUSERGRID && num < NMAXGRID) {
        uint8_t n = 0;
        int cmd = unpackCmd(static_cast<uint8_t>(num - NUSERGRID), n);
        auto it = directedCmds().find(cmd);
        if (it != directedCmds().end()) {
            out += it->second;
            if (isSnrCmdValue(cmd)) out += " " + formatSNR(static_cast<int>(n) - 31);
        }
    }
    return out;
}

std::string interpretDirected(const std::string& token) {
    uint8_t extra = 0;
    uint64_t value = unpack72bits(token, extra);
    Bits bits = intToBits(value, 64);

    uint32_t packed_from = static_cast<uint32_t>(bitsToInt(bits, 3, 28));
    uint32_t packed_to   = static_cast<uint32_t>(bitsToInt(bits, 31, 28));
    uint8_t  packed_cmd  = static_cast<uint8_t>(bitsToInt(bits, 59, 5));

    bool portable_from = ((extra >> 7) & 1) == 1;
    bool portable_to   = ((extra >> 6) & 1) == 1;
    int extraNum = extra % 64;

    std::string from = unpackCallsign(packed_from, portable_from);
    std::string to   = unpackCallsign(packed_to, portable_to);
    auto it = directedCmds().find(packed_cmd % 32);
    std::string cmd = (it != directedCmds().end()) ? it->second : "";

    std::string out = from + ": " + to + cmd;
    if (extraNum != 0) {
        if (isSnrCmdValue(packed_cmd % 32))
            out += " " + formatSNR(extraNum - 31);
        else
            out += " " + std::to_string(extraNum - 31);
    }
    return out;
}

std::string interpretData(const std::string& token) {
    uint8_t rem = 0;
    uint64_t value = unpack72bits(token, rem);
    Bits bits = intToBits(value, 64);
    Bits more = intToBits(rem, 8);
    bits.insert(bits.end(), more.begin(), more.end());  // 72 bits

    if (bits.empty() || bits[0] == 0) return "";
    bits.erase(bits.begin());  // drop data flag

    bool compressed = (!bits.empty() && bits[0] == 1);
    int n = lastIndexOfZero(bits);
    if (n < 1) return "";
    // bits.mid(1, n-1)
    Bits payload(bits.begin() + 1, bits.begin() + 1 + (n - 1));

    if (compressed)
        return "";  // JSC dictionary not vendored; caller falls back to token
    return huffDecode(bitsToStr(payload));
}

} // namespace

std::string interpretMessage(const std::string& token, int i3) {
    if (token.size() != 12) return token;
    std::string out;
    try {
        switch (i3) {
            case FrameHeartbeat:
            case FrameCompound:
            case FrameCompoundDirected:
                out = interpretCompound(token, i3);
                break;
            case FrameDirected:
                out = interpretDirected(token);
                break;
            default:  // 4..7 -> data
                out = interpretData(token);
                break;
        }
    } catch (...) {
        out.clear();
    }
    if (trim(out).empty()) return token;  // graceful fallback
    return out;
}

} // namespace js8
