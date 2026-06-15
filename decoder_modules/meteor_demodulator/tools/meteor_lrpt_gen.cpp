// =====================================================================
//  meteor_lrpt_gen - Meteor-M LRPT test-signal generator
//
//  Builds a complete, decodable LRPT signal from a test image (or a
//  generated pattern) and writes it as an IQ file for HackRF replay
//  (CS8 / interleaved int8) or as raw soft symbols.
//
//  It is the exact inverse of the SDR++ meteor_demodulator LRPT decoder
//  and reuses its ported codings (Reed-Solomon, convolutional encoder,
//  CCSDS randomization, CCSDS headers).
//
//  Chain:
//    test image -> MSU-MR JPEG encode (FDCT + quantize + Huffman)
//               -> CCSDS packets (APID 64/65/66, 3 channels)
//               -> M-PDU / VCDU (VCID 5) -> RS(255,223) i=4 -> randomize
//               -> + ASM 0x1ACFFC1D -> convolutional r=1/2 k=7
//               -> [NRZ-M + interleave for M2-x] -> QPSK/OQPSK symbols
//               -> RRC pulse shaping / upsample -> CS8
//
//  Build (from the meteor_demodulator dir, with SDR++ libcorrect):
//    see tools/README in the archive.
// =====================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <random>
#include <algorithm>

#include "lrpt/codings/reedsolomon.h"
#include "lrpt/codings/randomization.h"
#include "lrpt/codings/nrzm.h"
#include "lrpt/codings/viterbi/cc_encoder.h"
#include "lrpt/codings/viterbi/viterbi27.h"
#include "lrpt/ccsds/ccsds.h"
#include "lrpt/msumr/tables.h"
#include "lrpt/msumr/huffman.h"
#include <array>

using namespace meteor::msumr::lrpt;

// ----------------------------------------------------------------------
// Bit writer (MSB-first), matching the decoder's bit ordering.
// ----------------------------------------------------------------------
struct BitWriter {
    std::vector<uint8_t> bytes;
    int nbits = 0;
    void put(bool b) {
        if ((nbits & 7) == 0) bytes.push_back(0);
        if (b) bytes.back() |= (uint8_t)(0x80 >> (nbits & 7));
        nbits++;
    }
    void putBits(uint32_t v, int len) { for (int i = len - 1; i >= 0; i--) put((v >> i) & 1); }
    void align() { while (nbits & 7) put(false); }
};

// ----------------------------------------------------------------------
// Forward 8x8 DCT-II (float, JPEG normalization). Pairs with the
// decoder's fixed-point IDCT closely enough for a clean test image.
// ----------------------------------------------------------------------
static void fdct8x8(const double in[64], double out[64]) {
    static double C[8][8];
    static bool init = false;
    if (!init) {
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                C[u][x] = cos((2.0 * x + 1.0) * u * M_PI / 16.0);
        init = true;
    }
    double tmp[64];
    // rows
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            double s = 0;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * C[u][x];
            tmp[y * 8 + u] = s * (u == 0 ? sqrt(1.0 / 8.0) : sqrt(2.0 / 8.0));
        }
    // cols
    for (int x = 0; x < 8; x++)
        for (int v = 0; v < 8; v++) {
            double s = 0;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + x] * C[v][y];
            out[v * 8 + x] = s * (v == 0 ? sqrt(1.0 / 8.0) : sqrt(2.0 / 8.0));
        }
}

static int bitSize(int v) { v = std::abs(v); int s = 0; while (v) { s++; v >>= 1; } return s; }

// AC Huffman lookup: (run, size) -> index in acCategories
static int acIndex(int run, int size) {
    for (int i = 0; i < 162; i++)
        if (acCategories[i].zlen == run && acCategories[i].clen == size) return i;
    return -1;
}

// Encode one 8-px-tall, 112-px-wide segment (14 MCUs) into Huffman bits.
// pix is row-major [8][112]. lastDC is carried across the 14 MCUs.
static void encodeSegmentPayload(const uint8_t* pix /*8*112*/, int qf,
                                 int mcun, int apid_unused,
                                 std::vector<uint8_t>& payload) {
    (void)apid_unused;
    // 14-byte header
    payload.clear();
    payload.push_back(0); payload.push_back(0);          // day_time
    payload.push_back(0); payload.push_back(0);
    payload.push_back(0); payload.push_back(0);          // ms_time
    payload.push_back(0); payload.push_back(0);          // us_time
    payload.push_back((uint8_t)mcun);                    // MCUN
    payload.push_back(0x00);                             // QT
    payload.push_back(0x00);                             // DC/AC
    payload.push_back(0xFF); payload.push_back(0xF0);    // QFM = 0xFFF0
    payload.push_back((uint8_t)qf);                      // QF

    std::array<int64_t, 64> qt = GetQuantizationTable((float)qf);

    BitWriter bw;
    int64_t lastDC = 0;
    for (int m = 0; m < 14; m++) {
        double spatial[64], dct[64];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                spatial[y * 8 + x] = (double)pix[y * 112 + m * 8 + x] - 128.0;
        fdct8x8(spatial, dct);

        // quantize (natural order), then store in zigzag-scan order:
        // decoder does idct[x] = blockzz[Zigzag[x]] * qt[x]  => blockzz[Zigzag[x]] = q[x]
        int64_t blockzz[64];
        for (int i = 0; i < 64; i++) blockzz[i] = 0;
        for (int x = 0; x < 64; x++) {
            int64_t q = (int64_t)llround(dct[x] / (double)qt[x]);
            blockzz[Zigzag[x]] = q;
        }

        // DC (differential)
        int dc = (int)(blockzz[0] - lastDC);
        lastDC = blockzz[0];
        int dcsize = bitSize(dc);
        const dc_t& dcm = dcCategories[dcsize];
        for (int i = 0; i < dcm.klen; i++) bw.put(dcm.code[i]);
        if (dcsize > 0) {
            uint32_t b = (dc >= 0) ? (uint32_t)dc : (uint32_t)(dc + (1 << dcsize) - 1);
            bw.putBits(b, dcsize);
        }

        // AC (run-length + Huffman)
        int run = 0;
        for (int k = 1; k < 64; k++) {
            int coef = (int)blockzz[k];
            if (coef == 0) { run++; continue; }
            while (run > 15) { // ZRL
                int zrl = acIndex(15, 0);
                const ac_t& z = acCategories[zrl];
                for (int i = 0; i < z.klen; i++) bw.put(z.code[i]);
                run -= 16;
            }
            int size = bitSize(coef);
            int idx = acIndex(run, size);
            const ac_t& acm = acCategories[idx];
            for (int i = 0; i < acm.klen; i++) bw.put(acm.code[i]);
            uint32_t b = (coef >= 0) ? (uint32_t)coef : (uint32_t)(coef + (1 << size) - 1);
            bw.putBits(b, size);
            run = 0;
        }
        // EOB
        const ac_t& eob = acCategories[0];
        for (int i = 0; i < eob.klen; i++) bw.put(eob.code[i]);
    }
    bw.align();
    for (uint8_t b : bw.bytes) payload.push_back(b);
}

// ----------------------------------------------------------------------
// CADU builder: stream CCSDS packets into 882-byte M-PDU zones, build
// VCDU (VCID 5) + insert zone, RS-encode, randomize, prepend ASM.
// ----------------------------------------------------------------------
struct CaduBuilder {
    static const int MPDU_DATA = 882;
    std::vector<uint8_t> stream;      // continuous CCSDS packet bytes
    std::vector<int> headerStarts;    // byte offsets where packet headers start
    uint32_t vcduCounter = 0;
    std::vector<std::vector<uint8_t>> cadus; // finished 1024-byte CADUs
    reedsolomon::ReedSolomon rs{reedsolomon::RS223};

    void addPacket(const ccsds::CCSDSPacket& pkt) {
        headerStarts.push_back((int)stream.size());
        for (int i = 0; i < 6; i++) stream.push_back(pkt.header.raw[i]);
        for (uint8_t b : pkt.payload) stream.push_back(b);
    }

    // Emit as many full CADUs as the buffered stream allows.
    void flush(bool finalFlush) {
        size_t consumed = 0;
        while (stream.size() - consumed >= (size_t)MPDU_DATA ||
               (finalFlush && stream.size() - consumed > 0)) {
            uint8_t cadu[1024];
            memset(cadu, 0, sizeof(cadu));
            // ASM
            cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D;
            // VCDU primary header (version 1, scid 0, vcid 5)
            uint16_t scid = 0;
            uint8_t vcid = 5;
            cadu[4] = (0b01 << 6) | ((scid >> 2) & 0x3F);
            cadu[5] = ((scid & 0x3) << 6) | (vcid & 0x3F);
            cadu[6] = (vcduCounter >> 16) & 0xFF;
            cadu[7] = (vcduCounter >> 8) & 0xFF;
            cadu[8] = vcduCounter & 0xFF;
            cadu[9] = 0; // signaling/replay
            vcduCounter++;
            // insert zone cadu[10..11] = 0
            // M-PDU header cadu[12..13]: first header pointer
            int fhp = 2047; // 0x7FF = no header starts in this zone
            for (int hs : headerStarts) {
                if (hs >= (int)consumed && hs < (int)consumed + MPDU_DATA) {
                    fhp = hs - (int)consumed;
                    break;
                }
            }
            cadu[12] = (uint8_t)((fhp >> 8) & 0x07);
            cadu[13] = (uint8_t)(fhp & 0xFF);
            // M-PDU data cadu[14..14+882-1]
            for (int i = 0; i < MPDU_DATA; i++) {
                size_t s = consumed + i;
                cadu[14 + i] = (s < stream.size()) ? stream[s] : 0;
            }
            consumed += MPDU_DATA;

            // RS encode over cadu[4..1023] (1020 bytes: 892 data + 128 parity),
            // conventional basis (ccsds=false), interleave 4.
            rs.encode_interlaved(&cadu[4], false, 4);
            // Randomize cadu[4..1023]
            derand_ccsds(&cadu[4], 1020);

            cadus.emplace_back(cadu, cadu + 1024);
            if (finalFlush && consumed >= stream.size()) break;
        }
        // keep leftover
        if (consumed > 0) {
            stream.erase(stream.begin(), stream.begin() + std::min(consumed, stream.size()));
            for (int& hs : headerStarts) hs -= (int)consumed;
            headerStarts.erase(std::remove_if(headerStarts.begin(), headerStarts.end(),
                                              [](int h) { return h < 0; }),
                               headerStarts.end());
        }
    }
};

// ----------------------------------------------------------------------
// Convolutional encode a CADU stream -> output bits (NRZ optional).
// ----------------------------------------------------------------------
static void convEncodeCADUs(const std::vector<std::vector<uint8_t>>& cadus,
                            bool diff, std::vector<uint8_t>& outBits /*unpacked 0/1*/) {
    viterbi::CCEncoder enc(8192, 7, 2, viterbi::CCSDS_R2_K7_POLYS, 0);

    // Concatenate all CADU bits into one continuous frame bitstream.
    std::vector<uint8_t> frameBits;
    frameBits.reserve(cadus.size() * 8192);
    for (const auto& cadu : cadus)
        for (int b = 0; b < 1024; b++)
            for (int k = 0; k < 8; k++)
                frameBits.push_back((cadu[b] >> (7 - k)) & 1);

    // M2-x: NRZ-M differential ENCODE on the frame bits (RX applies decode_bits).
    // encode: e[i] = d[i] ^ e[i-1], e[-1] = 0  (inverse of NRZMDiff::decode_bits)
    if (diff) {
        uint8_t last = 0;
        for (size_t i = 0; i < frameBits.size(); i++) {
            uint8_t e = frameBits[i] ^ last;
            frameBits[i] = e;
            last = e;
        }
    }

    // Convolutional encode continuously, 8192 info bits per call.
    for (size_t off = 0; off + 8192 <= frameBits.size(); off += 8192) {
        uint8_t enc_out[16384];
        enc.work(&frameBits[off], enc_out);
        for (int i = 0; i < 16384; i++) outBits.push_back(enc_out[i]);
    }
}

// ----------------------------------------------------------------------
// Build a test image: gradient + grid + a diagonal so structure is obvious.
// ----------------------------------------------------------------------
static std::vector<uint8_t> makeTestImage(int w, int h, int chOffset) {
    std::vector<uint8_t> img((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = ((x * 255 / w) + (y * 128 / h) + chOffset * 40) & 0xFF;
            if ((x % 128) < 3 || (y % 128) < 3) v = 230;   // grid
            if (std::abs((x % 256) - (y % 256)) < 3) v = 20; // diagonal
            img[(size_t)y * w + x] = (uint8_t)v;
        }
    return img;
}

int main(int argc, char** argv) {
    int lines8 = 40;          // number of 8-px line-groups (image height = lines8*8)
    int qf = 60;
    std::string outPath = "meteor_lrpt.cs8";
    bool selftest = false;
    double samplerate = 1024000.0;
    double symrate = 72000.0;
    std::string mode = "legacy"; // legacy | m2x

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--lines" && i + 1 < argc) lines8 = atoi(argv[++i]);
        else if (a == "--qf" && i + 1 < argc) qf = atoi(argv[++i]);
        else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--selftest") selftest = true;
        else if (a == "--samplerate" && i + 1 < argc) samplerate = atof(argv[++i]);
        else if (a == "--symrate" && i + 1 < argc) symrate = atof(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    }
    bool m2x = (mode == "m2x");

    const int WIDTH = 1568;
    int height = lines8 * 8;
    // 3 channels -> APID 64,65,66 (reader channels 0,1,2; matches default composite)
    int apids[3] = {64, 65, 66};
    std::vector<std::vector<uint8_t>> chImg(3);
    for (int c = 0; c < 3; c++) chImg[c] = makeTestImage(WIDTH, height, c);

    // Build CADUs.
    CaduBuilder cb;
    // Sequence layout: 43-packet transmission loop. Per line-group L:
    //   ch0 seg0..13 (seq base+0..13), ch1 seg0..13 (+14..27),
    //   ch2 seg0..13 (+28..41), telemetry (+42).
    // -> reader reconstructs id = L*14 + segment, channel offsets 0/14/28.
    int seqBase = 0;
    for (int L = 0; L < lines8; L++) {
        for (int c = 0; c < 3; c++) {
            for (int j = 0; j < 14; j++) {
                // Extract the 8x112 segment for channel c, line-group L, segment j.
                uint8_t seg[8 * 112];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 112; x++) {
                        int px = j * 112 + x;
                        int py = L * 8 + y;
                        seg[y * 112 + x] = chImg[c][(size_t)py * WIDTH + px];
                    }
                ccsds::CCSDSPacket pkt;
                pkt.header.version = 0;
                pkt.header.type = 0;
                pkt.header.secondary_header_flag = 0;
                pkt.header.apid = apids[c];
                pkt.header.sequence_flag = 3; // unsegmented
                pkt.header.packet_sequence_count = (seqBase + L * 43 + c * 14 + j) & 0x3FFF;
                encodeSegmentPayload(seg, qf, j * 14, apids[c], pkt.payload);
                pkt.encodeHDR();
                cb.addPacket(pkt);
            }
        }
        // telemetry packet (APID 70), small, keeps the 43-loop aligned
        ccsds::CCSDSPacket tlm;
        tlm.header.version = 0; tlm.header.type = 0; tlm.header.secondary_header_flag = 0;
        tlm.header.apid = 70; tlm.header.sequence_flag = 3;
        tlm.header.packet_sequence_count = (seqBase + L * 43 + 42) & 0x3FFF;
        tlm.payload.assign(64, 0);
        tlm.encodeHDR();
        cb.addPacket(tlm);
        cb.flush(false);
    }
    cb.flush(true);
    fprintf(stderr, "Built %zu CADUs\n", cb.cadus.size());

    // Convolutional encode -> bits -> soft symbols.
    std::vector<uint8_t> bits;
    convEncodeCADUs(cb.cadus, m2x, bits);
    fprintf(stderr, "Encoded %zu coded bits (%zu QPSK symbols)\n", bits.size(), bits.size() / 2);

    if (selftest) {
        // Feed soft symbols straight into the decoder and check an image comes out.
        // (compiled separately in the selftest harness)
        std::vector<int8_t> soft(bits.size());
        for (size_t i = 0; i < bits.size(); i++) soft[i] = bits[i] ? (int8_t)64 : (int8_t)-64;
        FILE* f = fopen(outPath.c_str(), "wb");
        fwrite(soft.data(), 1, soft.size(), f);
        fclose(f);
        fprintf(stderr, "Wrote %zu soft symbols to %s (selftest input)\n", soft.size(), outPath.c_str());
        return 0;
    }

    // ---- DSP: QPSK map + RRC pulse shape + upsample -> CS8 ----
    int sps = (int)llround(samplerate / symrate);
    if (sps < 4) sps = 4;
    // RRC taps
    int taps = 31 * sps;
    if (taps % 2 == 0) taps++;
    double beta = 0.6;
    std::vector<double> rrc(taps);
    {
        double Ts = sps;
        for (int i = 0; i < taps; i++) {
            double t = i - (taps - 1) / 2.0;
            double x = t / Ts;
            double v;
            if (fabs(t) < 1e-9) v = 1.0 - beta + 4.0 * beta / M_PI;
            else if (beta > 0 && fabs(fabs(4.0 * beta * x) - 1.0) < 1e-6)
                v = (beta / sqrt(2.0)) * ((1 + 2.0 / M_PI) * sin(M_PI / (4 * beta)) + (1 - 2.0 / M_PI) * cos(M_PI / (4 * beta)));
            else {
                double num = sin(M_PI * x * (1 - beta)) + 4 * beta * x * cos(M_PI * x * (1 + beta));
                double den = M_PI * x * (1 - (4 * beta * x) * (4 * beta * x));
                v = num / den;
            }
            rrc[i] = v;
        }
        double e = 0; for (double v : rrc) e += v * v;
        double n = sqrt(e); for (double& v : rrc) v /= n;
    }

    size_t nsym = bits.size() / 2;
    // Upsampled impulse train (QPSK)
    std::vector<double> iUp((nsym + 1) * sps, 0.0), qUp((nsym + 1) * sps, 0.0);
    int qoff = m2x ? (sps / 2) : 0; // OQPSK: delay Q by half a symbol
    for (size_t k = 0; k < nsym; k++) {
        double I = bits[2 * k] ? 1.0 : -1.0;
        double Q = bits[2 * k + 1] ? 1.0 : -1.0;
        iUp[k * sps] = I;
        if (k * sps + qoff < qUp.size()) qUp[k * sps + qoff] = Q;
    }
    // Filter
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", outPath.c_str()); return 1; }
    int half = (taps - 1) / 2;
    std::vector<int8_t> outbuf;
    outbuf.reserve(iUp.size() * 2);
    for (size_t n = 0; n < iUp.size(); n++) {
        double si = 0, sq = 0;
        for (int t = 0; t < taps; t++) {
            long idx = (long)n - half + t;
            if (idx < 0 || idx >= (long)iUp.size()) continue;
            si += iUp[idx] * rrc[t];
            sq += qUp[idx] * rrc[t];
        }
        int I = (int)llround(si * 90.0);
        int Q = (int)llround(sq * 90.0);
        if (I > 127) I = 127; if (I < -127) I = -127;
        if (Q > 127) Q = 127; if (Q < -127) Q = -127;
        outbuf.push_back((int8_t)I);
        outbuf.push_back((int8_t)Q);
    }
    fwrite(outbuf.data(), 1, outbuf.size(), f);
    fclose(f);
    fprintf(stderr, "Wrote %zu CS8 samples to %s (sps=%d, %.0f Msps, %.0f kSym/s, %s)\n",
            outbuf.size() / 2, outPath.c_str(), sps, samplerate / 1e6, symrate / 1e3, m2x ? "OQPSK/M2-x" : "QPSK/legacy");
    return 0;
}
