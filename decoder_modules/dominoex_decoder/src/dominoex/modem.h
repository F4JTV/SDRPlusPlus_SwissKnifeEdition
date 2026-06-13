#pragma once
// FLDIGI-faithful DominoEX modem (receive path).
//
// DominoEX is an IFK+ (Incremental Frequency Keying, with per-symbol offset)
// MFSK mode by Murray Greenman ZL1BPU and Con Wassilieff ZL2AFP. Data is carried
// in the *difference* between successive tone numbers (18 tones), which makes the
// mode highly tolerant of mistuning and drift. Each symbol carries a 4-bit nibble;
// a multi-nibble varicode (MSB of the leading nibble clear, continuation nibbles
// MSB set) assembles the nibbles into characters. Unlike its sister mode THOR,
// standard DominoEX uses *no* FEC, so the decoded nibble feeds the varicode
// directly.
//
// This is a direct port of fldigi src/dominoex/dominoex.cxx (non-FEC path) onto a
// self-contained, sample-rate-independent class so it can be unit-tested without
// the SDR++ runtime. The DSP front-end (Hilbert, first-IF mixer, sliding-FFT bank,
// hard decode, sync, S/N) is shared in structure with the working thor_decoder.
//
// The SDR++ front-end delivers real audio at the 48 kHz VFO rate. Each mode is run
// at an internal rate that is an exact integer division of 48 kHz (8000 = /6,
// 12000 = /4); tone spacing and baud are preserved in Hz, so the demodulator is
// equivalent to fldigi running at the mode's native 8000/11025 rate.
//
// (C) port: structure & algorithm from Tomi Manninen OH2BNS / Hamish Moffatt /
//           Dave Freese W1HKJ et al., GPL.
#include <vector>
#include <memory>
#include <complex>
#include <cmath>
#include <cstring>
#include <functional>
#include <algorithm>
#include "modem_dsp.h"
#include "varicode.h"

namespace dominoex {

static constexpr double AUDIO_SR     = 48000.0; // VFO audio rate fed to the modem
static constexpr int    NUMTONES     = 18;
static constexpr double BASEFREQ     = 1000.0;
static constexpr double FIRSTIF      = 1500.0;
static constexpr int    SCOPE_LEN    = 1024;

struct DominoMode {
    const char* name;
    int    nativeSR;    // fldigi native rate (defines the canonical tone spacing)
    int    symlen;      // fldigi symbol length at the native rate
    int    doublespaced;
};

// Order is part of the saved config / combo box -> do not reorder.
// (matches the dropdown: Micro, 4, 5, 8, 11, 16, 22, 44, 88)
static const DominoMode DOMINOEX_MODES[] = {
    {"DominoEX Micro",  8000, 4000, 1},
    {"DominoEX 4",      8000, 2048, 2},
    {"DominoEX 5",     11025, 2048, 2},
    {"DominoEX 8",      8000, 1024, 2},
    {"DominoEX 11",    11025, 1024, 1},
    {"DominoEX 16",     8000,  512, 1},
    {"DominoEX 22",    11025,  512, 1},
    {"DominoEX 44",    11025,  256, 2},
    {"DominoEX 88",    11025,  128, 1},
};
static constexpr int DOMINOEX_MODE_COUNT = 9;

class DominoEXModem {
public:
    DominoEXModem() { configure(4, BASEFREQ); }   // default DominoEX 11
    ~DominoEXModem() = default;
    DominoEXModem(const DominoEXModem&) = delete;
    DominoEXModem& operator=(const DominoEXModem&) = delete;

    // --- configuration ------------------------------------------------------
    void configure(int idx, double af) {
        modeIdx = std::clamp(idx, 0, DOMINOEX_MODE_COUNT - 1);
        const DominoMode& M = DOMINOEX_MODES[modeIdx];
        afFreq = af;

        // internal rate = exact integer division of 48 kHz
        if (M.nativeSR == 8000) { internalSR = 8000.0;  decim = 6; }
        else                    { internalSR = 12000.0; decim = 4; }   // 11025 -> 12000

        doublespaced = M.doublespaced;
        symlen = (int)std::lround((double)M.symlen * internalSR / M.nativeSR);
        if (symlen < 8) symlen = 8;
        tonespacing = internalSR * doublespaced / symlen;
        bandwidth   = NUMTONES * tonespacing;

        extones = NUMTONES / 2;   // fast-cpu path
        paths   = 5;

        basetone = (int)floor(BASEFREQ * symlen / internalSR + 0.5);
        lotone   = basetone - extones * doublespaced;
        hitone   = basetone + NUMTONES * doublespaced + extones * doublespaced;
        numbins  = hitone - lotone;
        stride   = paths * numbins;

        // anti-alias decimator 48k -> internalSR (cutoff ~3.4 kHz, above the
        // <=1.6 kHz DominoEX signal and the 3 kHz SSB passband edge)
        decimator = std::make_unique<C_FIR_filter>();
        decimator->init_lowpass(128, decim, 3400.0 / AUDIO_SR);

        // Hilbert + sliding-FFT bank
        hilbert = std::make_unique<C_FIR_filter>();
        hilbert->init_hilbert(37, 1);
        binsfft.clear();
        for (int i = 0; i < paths; i++)
            binsfft.push_back(std::make_unique<sfft>(symlen, lotone, hitone));

        syncfilter = std::make_unique<Cmovavg>(8);

        // pipe of sliding-FFT vectors
        twosym = 2 * symlen;
        pipe.assign((size_t)twosym * stride, cmplx(0.0, 0.0));
        pipeptr = 0;

        // running state
        for (int i = 0; i < 9; i++) phase[i] = 0.0;
        synccounter = 0;
        currsymbol = prev1symbol = prev2symbol = 0;
        currmag = prev1mag = prev2mag = 0.0;

        // varicode assembly
        symcounter = 0;
        memset(symbolbuf, 0, sizeof(symbolbuf));

        sig = noise = 6.0; s2n = 0.0; metric = 0.0;
        staticburst = false; outofrange = false;

        curTone = 0; curSNR = 0.0f;
        scopeRing.assign(SCOPE_LEN, cmplx(0, 0));
        scopePos = 0; scopeMax = 1e-6;
    }

    void setAFFreq(double f) { afFreq = f; }
    double getTrackedAF() const { return afFreq; }
    void setAFC(bool en) { afcEnabled = en; }
    bool getAFC() const { return afcEnabled; }
    void setReverse(bool r) { reverse = r; }
    bool getReverse() const { return reverse; }

    int   getTone() const { return curTone; }
    float getSNR()  const { return curSNR; }

    double getToneSpan() const { return NUMTONES * tonespacing; }
    double getEdgeFreq() const { return afFreq - 0.5 * bandwidth; }
    double getBandFlo()  const { return 300.0; }
    double getBandFhi()  const { return 2700.0; }

    // Band spectrum: magnitude of the current sliding-FFT bins folded over paths.
    int getBandSpectrum(float* out, int n) const {
        int bins = numbins;
        if (bins <= 0) { for (int i = 0; i < n; i++) out[i] = 0; return n; }
        std::vector<double> mag(bins, 0.0);
        const cmplx* v = &pipe[(size_t)pipeptr * stride];
        for (int b = 0; b < bins; b++) {
            double m = 0;
            for (int k = 0; k < paths; k++) m += std::abs(v[b * paths + k]);
            mag[b] = m / paths;
        }
        for (int i = 0; i < n; i++) {
            double fb = (double)i * (bins - 1) / std::max(1, n - 1);
            int b0 = (int)fb; int b1 = std::min(b0 + 1, bins - 1);
            double t = fb - b0;
            out[i] = (float)(mag[b0] * (1 - t) + mag[b1] * t);
        }
        return n;
    }

    void getScope(std::complex<float>* buf, int n) const {
        for (int i = 0; i < n; i++) {
            int idx = (scopePos - n + i + 2 * SCOPE_LEN) % SCOPE_LEN;
            cmplx c = scopeRing[idx];
            double s = (scopeMax > 1e-9) ? scopeMax : 1.0;
            buf[i] = std::complex<float>((float)(c.real() / s), (float)(c.imag() / s));
        }
    }

    // --- main entry: real audio at AUDIO_SR ---------------------------------
    template <typename CharSink>
    void process(const float* audio, int count, CharSink sink) {
        for (int i = 0; i < count; i++) {
            double d;
            if (decimator->Irun((double)audio[i], d)) rxSample(d, sink);
        }
    }

private:
    // first-IF + path mixers (fldigi dominoex::mixer)
    cmplx mixer(int n, const cmplx& in) {
        double f;
        if (n == 0) f = afFreq - FIRSTIF;
        else f = FIRSTIF - BASEFREQ - bandwidth * 0.5
                 + tonespacing * ((double)(n - 1) / paths);
        cmplx z(cos(phase[n]), sin(phase[n]));
        z *= in;
        phase[n] -= DOMINOEX_TWOPI * f / internalSR;
        if (phase[n] < 0) phase[n] += DOMINOEX_TWOPI;
        return z;
    }

    template <typename CharSink>
    void rxSample(double s, CharSink& sink) {
        cmplx zref(s, s);
        hilbert->run(zref, zref);
        zref = mixer(0, zref);

        cmplx* pv = &pipe[(size_t)pipeptr * stride];
        for (int k = 0; k < paths; k++) {
            cmplx z = mixer(k + 1, zref);
            binsfft[k]->run(z, pv + k, paths);
        }

        if (--synccounter <= 0) {
            synccounter = symlen;
            currsymbol = harddecode();
            currmag = std::abs(pv[currsymbol]);
            evalS2N();
            decodesymbol(sink);
            synchronize();
            prev2symbol = prev1symbol; prev1symbol = currsymbol;
            prev2mag = prev1mag; prev1mag = currmag;

            // scope: dominant-tone vector
            scopeRing[scopePos] = pv[currsymbol];
            scopeMax = std::max(scopeMax * 0.999, std::abs(pv[currsymbol]));
            scopePos = (scopePos + 1) % SCOPE_LEN;
            curTone = currsymbol / std::max(1, paths);
        }

        if (++pipeptr >= (unsigned)twosym) pipeptr = 0;
    }

    int harddecode() {
        double max = 0.0, avg = 0.0;
        int symbol = 0;
        const cmplx* v = &pipe[(size_t)pipeptr * stride];
        for (int i = 0; i < stride; i++) avg += std::abs(v[i]);
        avg /= std::max(1, stride);
        if (avg < 1e-10) avg = 1e-10;
        for (int i = 0; i < stride; i++) {
            double x = std::abs(v[i]);
            if (x > max) { max = x; symbol = i; }
        }
        staticburst = (max / avg < 1.2);
        return symbol;
    }

    // IFK+ -> nibble -> multi-nibble varicode (fldigi dominoex::decodesymbol /
    // decodeDomino, non-FEC path).
    template <typename CharSink>
    void decodesymbol(CharSink& sink) {
        double fdiff = currsymbol - prev1symbol;
        if (reverse) fdiff = -fdiff;
        fdiff /= doublespaced;
        fdiff /= paths;

        outofrange = (fabs(fdiff) > 17);

        int c = (int)floor(fdiff + 0.5) - 2;
        if (c < 0) c += NUMTONES;

        decodeDomino(c, sink);
    }

    template <typename CharSink>
    void decodeDomino(int c, CharSink& sink) {
        // If the new symbol is the start of a new character (MSB low), complete
        // the previous character from the accumulated nibble buffer.
        if (!(c & 0x8)) {
            if (symcounter > 0 && symcounter <= DOMINOEX_MAX_VARICODE_LEN) {
                unsigned int sym = 0;
                for (int i = 0; i < symcounter; i++)
                    sym |= (unsigned int)symbolbuf[i] << (4 * i);
                int ch = dominoex_varidec(sym);
                if (!staticburst && !outofrange) recvchar(ch, sink);
            }
            symcounter = 0;
        }

        // Add to the symbol buffer. Position 0 is the newest symbol.
        for (int i = DOMINOEX_MAX_VARICODE_LEN - 1; i > 0; i--)
            symbolbuf[i] = symbolbuf[i - 1];
        symbolbuf[0] = (unsigned char)c;

        symcounter++;
        if (symcounter > DOMINOEX_MAX_VARICODE_LEN + 1)
            symcounter = DOMINOEX_MAX_VARICODE_LEN + 1;
    }

    template <typename CharSink>
    void recvchar(int c, CharSink& sink) {
        if (c == -1) return;
        if (c & 0x100) return;        // secondary channel flag (unused here)
        sink((char)(c & 0xFF));
    }

    void synchronize() {
        if (staticburst) return;
        if (currsymbol == prev1symbol) return;
        if (prev1symbol == prev2symbol) return;
        double max = 0.0; double syn = -1;
        unsigned int j = pipeptr;
        for (unsigned int i = 0; i < (unsigned)twosym; i++) {
            double val = std::abs(pipe[(size_t)j * stride + prev1symbol]);
            if (val > max) { max = val; syn = i; }
            j = (j + 1) % twosym;
        }
        syn = syncfilter->run(syn);
        synccounter += (int)floor(1.0 * (syn - symlen) / NUMTONES + 0.5);
    }

    void evalS2N() {
        double s = std::abs(pipe[(size_t)pipeptr * stride + currsymbol]);
        double n = (NUMTONES - 1) *
                   std::abs(pipe[(size_t)((pipeptr + symlen) % twosym) * stride + currsymbol]);
        sig   = decayavg(sig, s, (fabs(s - sig) > 4) ? 4 : 32);
        noise = decayavg(noise, n, 64);
        s2n = (noise > 0) ? 20 * log10(sig / noise) - 6 : 0;
        double m = 6 * (s2n + 18.4);
        metric = std::clamp(m, 0.0, 100.0);
        curSNR = (float)s2n;
    }

    static double decayavg(double avg, double v, double weight) {
        if (weight <= 1.0) return v;
        return avg + (v - avg) / weight;
    }

    // --- state ---
    int modeIdx = 4;
    double afFreq = BASEFREQ;
    double internalSR = 8000.0;
    int decim = 6;
    int symlen = 0, doublespaced = 1, paths = 5, extones = 9;
    int basetone = 0, lotone = 0, hitone = 0, numbins = 0, stride = 0;
    int twosym = 0;
    double tonespacing = 0, bandwidth = 0;

    std::unique_ptr<C_FIR_filter> decimator, hilbert;
    std::vector<std::unique_ptr<sfft>> binsfft;
    std::unique_ptr<Cmovavg> syncfilter;

    std::vector<cmplx> pipe;
    unsigned int pipeptr = 0;

    double phase[9] = {0};
    int synccounter = 0;
    int currsymbol = 0, prev1symbol = 0, prev2symbol = 0;
    double currmag = 0, prev1mag = 0, prev2mag = 0;

    // varicode assembly
    unsigned char symbolbuf[DOMINOEX_MAX_VARICODE_LEN + 1] = {0};
    int symcounter = 0;

    double sig = 6, noise = 6, s2n = 0, metric = 0;
    bool staticburst = false;
    bool outofrange = false;
    bool reverse = false;
    bool afcEnabled = false;

    int curTone = 0;
    float curSNR = 0.0f;
    std::vector<cmplx> scopeRing;
    int scopePos = 0;
    double scopeMax = 1e-6;
};

} // namespace dominoex
