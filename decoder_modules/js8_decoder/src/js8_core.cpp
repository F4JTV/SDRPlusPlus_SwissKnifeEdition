/*
 * js8_core.cpp - Self-contained JS8 encode/decode core (no Qt, no SDR++).
 *
 * Ported from JS8Call / JS8Call-improved (GPL-3.0), a derivative of WSJT-X.
 * The JS8 protocol and modulation were created by Jordan Sherer (KN4CRD).
 *
 * Implemented here, for the "Normal" submode (15 s slot, 6.25 Hz tone
 * spacing, ORIGINAL Costas arrays):
 *   - augmented CRC-12 (poly 0xc06, XOR 42), bit-serial MSB-first
 *   - LDPC(174,87) generator-matrix encode + belief-propagation decode
 *   - 8-FSK channel symbol layout (3 Costas blocks + 2 x 29 data symbols)
 *   - continuous-phase reference-signal / GFSK audio synthesis
 *   - a simple non-coherent DFT front-end with Costas sync search
 *
 * JS8 carries 3 bits per channel symbol with NO Gray coding: the tone value
 * is the raw 3-bit word, MSB first.
 */
#include "js8_core.h"
#include "js8_ldpc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace js8 {
namespace {

constexpr double PI  = 3.14159265358979323846;
constexpr double TAU = 2.0 * PI;

// 6-bit message alphabet (64 symbols).
constexpr const char* ALPHABET =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";

int alphabetWord(char c) {
    for (int i = 0; i < 64; ++i)
        if (ALPHABET[i] == c) return i;
    throw std::runtime_error("Invalid character in message");
}

// ORIGINAL Costas array (Normal submode): all three sync blocks identical.
constexpr std::array<int, 7> COSTAS_ORIGINAL = {4, 2, 5, 6, 1, 3, 0};

// --- Augmented CRC-12, poly 0xc06, final XOR 42 ----------------------------
// Equivalent to boost::augmented_crc<12,0xc06>(data,size) ^ 42. The trailing
// (zeroed) CRC bits of the buffer act as the augmentation; both the encoder
// and the verifier zero that region, guaranteeing a consistent check.
uint16_t crc12(const uint8_t* data, std::size_t n) {
    constexpr uint32_t poly   = 0xc06;
    constexpr uint32_t topbit = 1u << 11;
    constexpr uint32_t mask   = (1u << 12) - 1;
    uint32_t rem = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (int b = 7; b >= 0; --b) {
            int bit = (data[i] >> b) & 1;
            uint32_t hi = rem & topbit;
            rem = ((rem << 1) | bit) & mask;
            if (hi) rem ^= poly;
        }
    }
    return static_cast<uint16_t>((rem & mask) ^ 42u);
}

// --- LDPC generator (parity) matrix: 87 rows x 87 columns, packed as hex --
// Harvested from JS8Call (originally ldpc_174_87_params.f90). Each row is 22
// hex chars = 88 bits; only the first 87 columns are used. parity(i,j) is the
// j-th message bit's contribution to the i-th parity-check bit.
const char* const PARITY_HEX[87] = {
    "23bba830e23b6b6f50982e", "1f8e55da218c5df3309052",
    "ca7b3217cd92bd59a5ae20", "56f78313537d0f4382964e",
    "6be396b5e2e819e373340c", "293548a138858328af4210",
    "cb6c6afcdc28bb3f7c6e86", "3f2a86f5c5bd225c961150",
    "849dd2d63673481860f62c", "56cdaec6e7ae14b43feeee",
    "04ef5cfa3766ba778f45a4", "c525ae4bd4f627320a3974",
    "41fd9520b2e4abeb2f989c", "7fb36c24085a34d8c1dbc4",
    "40fc3e44bb7d2bb2756e44", "d38ab0a1d2e52a8ec3bc76",
    "3d0f929ef3949bd84d4734", "45d3814f504064f80549ae",
    "f14dbf263825d0bd04b05e", "db714f8f64e8ac7af1a76e",
    "8d0274de71e7c1a8055eb0", "51f81573dd4049b082de14",
    "d8f937f31822e57c562370", "b6537f417e61d1a7085336",
    "ecbd7c73b9cd34c3720c8a", "3d188ea477f6fa41317a4e",
    "1ac4672b549cd6dba79bcc", "a377253773ea678367c3f6",
    "0dbd816fba1543f721dc72", "ca4186dd44c3121565cf5c",
    "29c29dba9c545e267762fe", "1616d78018d0b4745ca0f2",
    "fe37802941d66dde02b99c", "a9fa8e50bcb032c85e3304",
    "83f640f1a48a8ebc0443ea", "3776af54ccfbae916afde6",
    "a8fc906976c35669e79ce0", "f08a91fb2e1f78290619a8",
    "cc9da55fe046d0cb3a770c", "d36d662a69ae24b74dcbd8",
    "40907b01280f03c0323946", "d037db825175d851f3af00",
    "1bf1490607c54032660ede", "0af7723161ec223080be86",
    "eca9afa0f6b01d92305edc", "7a8dec79a51e8ac5388022",
    "9059dfa2bb20ef7ef73ad4", "6abb212d9739dfc02580f2",
    "f6ad4824b87c80ebfce466", "d747bfc5fd65ef70fbd9bc",
    "612f63acc025b6ab476f7c", "05209a0abb530b9e7e34b0",
    "45b7ab6242b77474d9f11a", "6c280d2a0523d9c4bc5946",
    "f1627701a2d692fd9449e6", "8d9071b7e7a6a2eed6965e",
    "bf4f56e073271f6ab4bf80", "c0fc3ec4fb7d2bb2756644",
    "57da6d13cb96a7689b2790", "a9fa2eefa6f8796a355772",
    "164cc861bdd803c547f2ac", "cc6de59755420925f90ed2",
    "a0c0033a52ab6299802fd2", "b274db8abd3c6f396ea356",
    "97d4169cb33e7435718d90", "81cfc6f18c35b1e1f17114",
    "481a2a0df8a23583f82d6c", "081c29a10d468ccdbcecb6",
    "2c4142bf42b01e71076acc", "a6573f3dc8b16c9d19f746",
    "c87af9a5d5206abca532a8", "012dee2198eba82b19a1da",
    "b1ca4ea2e3d173bad4379c", "b33ec97be83ce413f9acc8",
    "5b0f7742bca86b8012609a", "37d8e0af9258b9e8c5f9b2",
    "35ad3fb0faeb5f1b0c30dc", "6114e08483043fd3f38a8a",
    "cd921fdf59e882683763f6", "95e45ecd0135aca9d6e6ae",
    "2e547dd7a05f6597aac516", "14cd0f642fc0c5fe3a65ca",
    "3a0a1dfd7eee29c2e827e0", "c8b5dffc335095dcdcaf2a",
    "3dd01a59d86310743ec752", "8abdb889efbe39a510a118",
    "3f231f212055371cf3e2a2"};

// Expanded parity bit table [87][87], built once on first use.
struct ParityTable {
    std::array<std::array<uint8_t, 87>, 87> bit{};
    ParityTable() {
        for (int row = 0; row < 87; ++row) {
            int col = 0;
            for (const char* p = PARITY_HEX[row]; *p && col < 87; ++p) {
                char c = *p;
                int v = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                               : 0;
                for (int m = 3; m >= 0 && col < 87; --m, ++col)
                    bit[row][col] = (v >> m) & 1;
            }
        }
    }
};
const ParityTable& parityTable() {
    static const ParityTable t;
    return t;
}

// --- CRC check + message extraction (operates on 87 decoded bits) ----------
bool checkCRC12(const std::array<int8_t, KK>& decoded) {
    std::array<uint8_t, 11> bits{};
    for (std::size_t i = 0; i < decoded.size(); ++i)
        if (decoded[i]) bits[i / 8] |= (1 << (7 - (i % 8)));

    uint16_t rxcrc = (static_cast<uint16_t>(bits[9] & 0x1F) << 7) |
                     (static_cast<uint16_t>(bits[10]) >> 1);
    bits[9] &= 0xE0;
    bits[10] = 0x00;
    return rxcrc == crc12(bits.data(), bits.size());
}

// Extract the 3-bit frame type (i3) carried in bits [72..74].
int extractI3(const std::array<int8_t, KK>& decoded) {
    return (decoded[72] << 2) | (decoded[73] << 1) | decoded[74];
}

// Map the 72 payload bits to the 12-character token.
std::string extractToken(const std::array<int8_t, KK>& decoded) {
    std::string msg;
    msg.reserve(12);
    for (int i = 0; i < 12; ++i) {
        int w = (decoded[i * 6 + 0] << 5) | (decoded[i * 6 + 1] << 4) |
                (decoded[i * 6 + 2] << 3) | (decoded[i * 6 + 3] << 2) |
                (decoded[i * 6 + 4] << 1) | (decoded[i * 6 + 5] << 0);
        msg += ALPHABET[w & 0x3F];
    }
    return msg;
}

// --- Belief-propagation LDPC decoder (port of bpdecode174) -----------------
int bpdecode174(const std::array<float, N>& llr,
                std::array<int8_t, KK>& decoded,
                std::array<int8_t, N>& cw) {
    std::array<std::array<float, BP_MAX_CHECKS>, N> tov{};
    std::array<std::array<float, BP_MAX_ROWS>, M>   toc{};
    std::array<std::array<float, BP_MAX_ROWS>, M>   tanhtoc{};
    std::array<float, N> zn{};
    std::array<int, M>   synd{};

    int ncnt = 0, nclast = 0;

    for (int i = 0; i < M; ++i)
        for (int j = 0; j < Nm[i].valid_neighbors; ++j)
            toc[i][j] = llr[Nm[i].neighbors[j]];

    for (int iter = 0; iter <= BP_MAX_ITERATIONS; ++iter) {
        for (int i = 0; i < N; ++i)
            zn[i] = llr[i] + std::accumulate(tov[i].begin(),
                                             tov[i].begin() + BP_MAX_CHECKS,
                                             0.0f);
        for (int i = 0; i < N; ++i)
            cw[i] = zn[i] > 0 ? 1 : 0;

        int ncheck = 0;
        for (int i = 0; i < M; ++i) {
            synd[i] = 0;
            for (int j = 0; j < Nm[i].valid_neighbors; ++j)
                synd[i] += cw[Nm[i].neighbors[j]];
            if (synd[i] % 2 != 0) ++ncheck;
        }

        if (ncheck == 0) {
            std::copy(cw.begin() + M, cw.end(), decoded.begin());
            int nerr = 0;
            for (int i = 0; i < N; ++i)
                if ((2 * cw[i] - 1) * llr[i] < 0.0f) ++nerr;
            return nerr;
        }

        if (iter > 0) {
            int nd = ncheck - nclast;
            ncnt = (nd < 0) ? 0 : ncnt + 1;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) return -1;
        }
        nclast = ncheck;

        for (int i = 0; i < M; ++i) {
            for (int j = 0; j < Nm[i].valid_neighbors; ++j) {
                int ibj  = Nm[i].neighbors[j];
                toc[i][j] = zn[ibj];
                for (int k = 0; k < BP_MAX_CHECKS; ++k)
                    if (Mn[ibj][k] == i) toc[i][j] -= tov[ibj][k];
            }
        }
        for (int i = 0; i < M; ++i)
            for (int j = 0; j < BP_MAX_ROWS; ++j)
                tanhtoc[i][j] = std::tanh(-toc[i][j] / 2.0f);

        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < BP_MAX_CHECKS; ++j) {
                int ichk = Mn[i][j];
                if (ichk >= 0) {
                    float Tmn = 1.0f;
                    for (int k = 0; k < Nm[ichk].valid_neighbors; ++k)
                        if (Nm[ichk].neighbors[k] != i)
                            Tmn *= tanhtoc[ichk][k];
                    tov[i][j] = 2.0f * std::atanh(-Tmn);
                }
            }
        }
    }
    return -1;
}

// --- Encoder helpers --------------------------------------------------------
// Build the 87-bit message (11-byte array) from token + frame type, append
// CRC-12, then derive the 79 tone indices using the ORIGINAL Costas arrays.
void encodeTones(const std::string& token12, int i3type,
                 int tones[NUM_SYMBOLS]) {
    if (token12.size() != 12)
        throw std::runtime_error("token must be 12 characters");

    std::array<uint8_t, 11> bytes{};
    for (int i = 0, j = 0; i < 12; i += 4, j += 3) {
        uint32_t w = (alphabetWord(token12[i])     << 18) |
                     (alphabetWord(token12[i + 1]) << 12) |
                     (alphabetWord(token12[i + 2]) << 6)  |
                      alphabetWord(token12[i + 3]);
        bytes[j]     = (w >> 16) & 0xFF;
        bytes[j + 1] = (w >> 8)  & 0xFF;
        bytes[j + 2] =  w        & 0xFF;
    }
    bytes[9] = static_cast<uint8_t>((i3type & 0x7) << 5);

    uint16_t crc = crc12(bytes.data(), bytes.size());
    bytes[9] |= (crc >> 7) & 0x1F;
    bytes[10] = static_cast<uint8_t>((crc & 0x7F) << 1);

    // 3 Costas blocks at symbol offsets 0, 36, 72.
    int* costasData = tones;
    for (int blk = 0; blk < 3; ++blk) {
        std::copy(COSTAS_ORIGINAL.begin(), COSTAS_ORIGINAL.end(), costasData);
        costasData += 36;
    }

    int* parityData = tones + 7;   // 29 parity symbols
    int* outputData = tones + 43;  // 29 message symbols

    const ParityTable& P = parityTable();

    std::size_t outputBits = 0, outputByte = 0;
    uint8_t outputMask = 0x80, outputWord = 0, parityWord = 0;

    for (std::size_t i = 0; i < 87; ++i) {
        std::size_t parityBits = 0, parityByte = 0;
        uint8_t parityMask = 0x80;
        for (std::size_t j = 0; j < 87; ++j) {
            parityBits += P.bit[i][j] && (bytes[parityByte] & parityMask);
            parityMask = (parityMask == 1) ? (++parityByte, 0x80)
                                           : (parityMask >> 1);
        }
        parityWord = (parityWord << 1) | (parityBits & 1);
        outputWord = (outputWord << 1) |
                     ((bytes[outputByte] & outputMask) != 0);
        outputMask = (outputMask == 1) ? (++outputByte, 0x80)
                                       : (outputMask >> 1);
        if (++outputBits == 3) {
            *parityData++ = parityWord;
            *outputData++ = outputWord;
            parityWord = 0;
            outputWord = 0;
            outputBits = 0;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void encodeNormal(const std::string& token12, int i3type,
                  int tones[NUM_SYMBOLS]) {
    encodeTones(token12, i3type, tones);
}

std::vector<float> genAudioNormal(const int tones[NUM_SYMBOLS], float f0,
                                  float startDelaySec, bool gfsk) {
    const int    nsps = NSPS_NORMAL;
    const double bfpi = TAU * f0 / SAMPLE_RATE;

    const int leadSamples = static_cast<int>(startDelaySec * SAMPLE_RATE);
    const int sigSamples  = NUM_SYMBOLS * nsps;
    const int slotSamples = 15 * SAMPLE_RATE;  // full 15 s Normal slot

    std::vector<float> out(slotSamples, 0.0f);

    // Smoothed instantaneous tone index across symbols (GFSK-like) so that
    // phase is continuous and transitions are not abrupt.
    std::vector<float> toneTrack(sigSamples);
    for (int i = 0; i < NUM_SYMBOLS; ++i)
        for (int s = 0; s < nsps; ++s)
            toneTrack[i * nsps + s] = static_cast<float>(tones[i]);

    if (gfsk) {
        // Simple Gaussian-ish smoothing over ~1/4 symbol to soften edges.
        const int win = nsps / 4;
        std::vector<float> sm(sigSamples);
        double acc = 0.0;
        for (int i = 0; i < sigSamples; ++i) {
            acc += toneTrack[i];
            if (i >= win) acc -= toneTrack[i - win];
            sm[i] = static_cast<float>(acc / std::min(i + 1, win));
        }
        toneTrack.swap(sm);
    }

    double phi = 0.0;
    for (int i = 0; i < sigSamples; ++i) {
        double dphi = bfpi + TAU * toneTrack[i] / nsps;
        int idx = leadSamples + i;
        if (idx >= 0 && idx < slotSamples)
            out[idx] = static_cast<float>(std::cos(phi));
        phi = std::fmod(phi + dphi, TAU);
    }
    return out;
}

std::vector<Decode> decodeNormal(const float* audio, std::size_t n,
                                 float fMin, float fMax) {
    std::vector<Decode> results;
    if (!audio || n < static_cast<std::size_t>(NUM_SYMBOLS * NSPS_NORMAL))
        return results;

    const int    nsps = NSPS_NORMAL;
    const double df   = static_cast<double>(SAMPLE_RATE) / nsps;  // 6.25 Hz
    const int    sigLen = NUM_SYMBOLS * nsps;

    // Candidate base-frequency grid (half-bin spacing) and time offsets.
    const double fStep = df / 2.0;
    const int    maxStart =
        static_cast<int>(n) - sigLen > 0
            ? std::min(static_cast<int>(n) - sigLen, 4 * SAMPLE_RATE)
            : 0;
    const int tStep = nsps / 4;  // coarse time search

    // Per (f0): precompute Goertzel-style tone powers per symbol is expensive
    // over the whole grid, so we compute on demand inside the sync search.
    auto tonePower = [&](int startSample, int symbol, double f0base,
                         std::array<float, NUM_TONES>& pw) {
        const int base = startSample + symbol * nsps;
        // Goertzel per tone: avoids per-sample trig in the inner loop.
        for (int t = 0; t < NUM_TONES; ++t) {
            double freq = f0base + t * df;
            double w    = TAU * freq / SAMPLE_RATE;
            double coeff = 2.0 * std::cos(w);
            double s0 = 0.0, s1 = 0.0, s2 = 0.0;
            for (int s = 0; s < nsps; ++s) {
                s0 = audio[base + s] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            double re = s1 - s2 * std::cos(w);
            double im = s2 * std::sin(w);
            pw[t] = static_cast<float>(re * re + im * im);
        }
    };

    struct Cand { double sync; int start; double f0; };
    std::vector<Cand> cands;

    std::array<float, NUM_TONES> pw;
    for (int start = 0; start <= maxStart; start += tStep) {
        for (double f0 = fMin; f0 <= fMax; f0 += fStep) {
            // Costas sync over the three blocks (symbols 0-6, 36-42, 72-78).
            double sync = 0.0;
            const int blocks[3] = {0, 36, 72};
            for (int b = 0; b < 3; ++b) {
                for (int k = 0; k < 7; ++k) {
                    int sym = blocks[b] + k;
                    tonePower(start, sym, f0, pw);
                    float tot = 0.0f;
                    for (int t = 0; t < NUM_TONES; ++t) tot += pw[t];
                    float exp = pw[COSTAS_ORIGINAL[k]];
                    if (tot > 0.0f) sync += exp / (tot / NUM_TONES);
                }
            }
            cands.push_back({sync, start, f0});
        }
    }

    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.sync > b.sync; });

    const std::size_t maxTry = std::min<std::size_t>(cands.size(), 40);
    std::vector<std::pair<std::string, double>> seen;

    for (std::size_t c = 0; c < maxTry; ++c) {
        const Cand& cd = cands[c];

        // Build LLRs for all 174 codeword bits.
        // Parity symbols are channel positions 7..35  -> cw[0..86].
        // Message symbols are channel positions 43..71 -> cw[87..173].
        std::array<float, N> llr{};
        auto fillSym = [&](int channelSym, int cwBase) {
            tonePower(cd.start, channelSym, cd.f0, pw);
            std::array<float, NUM_TONES> mag;
            for (int t = 0; t < NUM_TONES; ++t)
                mag[t] = std::sqrt(pw[t]);
            // Normalise.
            float mx = *std::max_element(mag.begin(), mag.end());
            if (mx <= 0.0f) mx = 1.0f;
            for (int t = 0; t < NUM_TONES; ++t) mag[t] /= mx;
            // 3 bits per symbol, MSB first, NO gray code.
            for (int bit = 0; bit < 3; ++bit) {
                int shift = 2 - bit;  // bit0 = MSB
                float m1 = -1e9f, m0 = -1e9f;
                for (int t = 0; t < NUM_TONES; ++t) {
                    if ((t >> shift) & 1) m1 = std::max(m1, mag[t]);
                    else                  m0 = std::max(m0, mag[t]);
                }
                llr[cwBase + bit] = (m1 - m0) * 4.0f;
            }
        };

        for (int s = 0; s < 29; ++s) fillSym(7 + s,  s * 3);          // parity
        for (int s = 0; s < 29; ++s) fillSym(43 + s, 87 + s * 3);     // message

        std::array<int8_t, KK> decoded{};
        std::array<int8_t, N>  cw{};
        int nerr = bpdecode174(llr, decoded, cw);
        if (nerr < 0) continue;
        if (!checkCRC12(decoded)) continue;

        Decode d;
        d.i3        = extractI3(decoded);
        d.token     = extractToken(decoded);
        d.harderrors = nerr;
        d.f0        = static_cast<float>(cd.f0);
        d.dt        = static_cast<float>(cd.start) / SAMPLE_RATE;
        d.text      = d.token;  // varicode layer refines this

        // SNR estimate (referenced to a 2500 Hz noise bandwidth, JS8-style):
        // re-encode to get the true tone sequence, then compare power in the
        // signal bin against the average power in the other 7 bins.
        {
            int rtones[NUM_SYMBOLS];
            try {
                encodeTones(d.token, d.i3, rtones);
                double sigAccum = 0.0, noiseAccum = 0.0;
                int ns = 0;
                std::array<float, NUM_TONES> p;
                for (int s = 0; s < NUM_SYMBOLS; ++s) {
                    tonePower(cd.start, s, cd.f0, p);
                    int tt = rtones[s];
                    double tot = 0.0;
                    for (int t = 0; t < NUM_TONES; ++t) tot += p[t];
                    double noisePerBin = (tot - p[tt]) / (NUM_TONES - 1);
                    sigAccum += std::max(0.0, p[tt] - noisePerBin);
                    noiseAccum += noisePerBin;
                    ++ns;
                }
                double sig = sigAccum / ns;
                double npb = noiseAccum / ns;
                // 2500 Hz / 6.25 Hz bin = 400 bins of noise bandwidth.
                double snr = (npb > 0.0)
                    ? 10.0 * std::log10(sig / (npb * 400.0) + 1e-12)
                    : 0.0;
                d.snr = static_cast<float>(std::max(-30.0, std::min(30.0, snr)));
            } catch (...) {
                d.snr = 0.0f;
            }
        }

        bool dup = false;
        for (auto& s : seen)
            if (s.first == d.token && std::abs(s.second - cd.f0) < 10.0)
                dup = true;
        if (dup) continue;
        seen.emplace_back(d.token, cd.f0);
        results.push_back(d);
    }

    return results;
}

} // namespace js8
