#pragma once
// FLDIGI-faithful DSP primitives used by the DominoEX modem front-end.
// Ported from fldigi src/filters/filters.cxx / src/include/filters.h
// (C) Dave Freese W1HKJ, GPL.
//
//   * sfft        - sliding (recursive) DFT producing a contiguous bin range
//   * C_FIR_filter - FIR filter (Hilbert transform, lowpass, optional decimation)
//   * Cmovavg     - boxcar moving average (sync / video filters)
#include <complex>
#include <cmath>
#include <cstring>

namespace dominoex {

typedef std::complex<double> cmplx;

#ifndef DOMINOEX_TWOPI
#define DOMINOEX_TWOPI (2.0 * M_PI)
#endif

// ===================================================================
// Sliding FFT (recursive). Computes bins [first, last) every sample.
// Bins are not stable until `len` samples have been processed.
// ===================================================================
class sfft {
    static constexpr long double K1 = 0.99999999999L;
    struct vrot_bins_pair { cmplx vrot; cmplx bins; };
public:
    sfft(int len, int _first, int _last) {
        fftlen = len; first = _first; last = _last; ptr = 0;
        vrot_bins = new vrot_bins_pair[len];
        delay = new cmplx[len];
        double phi = 0.0, tau = 2.0 * M_PI / len;
        k2 = 1.0;
        for (int i = 0; i < fftlen; i++) {
            vrot_bins[i].vrot = cmplx((double)(K1 * cos(phi)), (double)(K1 * sin(phi)));
            phi += tau;
            delay[i] = vrot_bins[i].bins = cmplx(0.0, 0.0);
            k2 *= (double)K1;
        }
        count = 0;
    }
    ~sfft() { delete[] vrot_bins; delete[] delay; }
    sfft(const sfft&) = delete;
    sfft& operator=(const sfft&) = delete;

    void reset() {
        for (int i = 0; i < fftlen; i++) delay[i] = vrot_bins[i].bins = cmplx(0.0, 0.0);
        count = 0;
    }
    bool is_stable() const { return count >= fftlen; }

    // Compute bins first..last; write to result with the given stride.
    void run(const cmplx& input, cmplx* result, int stride) {
        cmplx& de = delay[ptr];
        const cmplx z(input.real() - k2 * de.real(), input.imag() - k2 * de.imag());
        de = input;
        if (++ptr >= fftlen) ptr = 0;
        for (vrot_bins_pair* itr = vrot_bins + first, *end = vrot_bins + last;
             itr != end; ++itr, result += stride) {
            *result = itr->bins = itr->bins * itr->vrot + z * itr->vrot;
        }
        if (count < fftlen) count++;
    }
private:
    int fftlen, first, last, ptr;
    vrot_bins_pair* vrot_bins;
    cmplx* delay;
    double k2;
    int count;
};

// ===================================================================
// FIR filter (Hilbert / lowpass), optional integer decimation.
// ===================================================================
class C_FIR_filter {
    static constexpr int FIRBufferLen = 4096;
    static inline double sinc(double x) { return (fabs(x) < 1e-10) ? 1.0 : sin(M_PI * x) / (M_PI * x); }
    static inline double cosc(double x) { return (fabs(x) < 1e-10) ? 0.0 : (1.0 - cos(M_PI * x)) / (M_PI * x); }
    static inline double hamming(double x) { return 0.54 - 0.46 * cos(2 * M_PI * x); }
    static inline double mac(const double* a, const double* b, unsigned int size) {
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (; size > 3; size -= 4, a += 4, b += 4) { s0 += a[0]*b[0]; s1 += a[1]*b[1]; s2 += a[2]*b[2]; s3 += a[3]*b[3]; }
        for (; size; --size) s0 += (*a++) * (*b++);
        return s0 + s1 + s2 + s3;
    }
public:
    C_FIR_filter() : length(0), decimateratio(1), ifilter(nullptr), qfilter(nullptr), pointer(0), counter(0) {
        memset(ibuffer, 0, sizeof(ibuffer));
        memset(qbuffer, 0, sizeof(qbuffer));
    }
    ~C_FIR_filter() { delete[] ifilter; delete[] qfilter; }
    C_FIR_filter(const C_FIR_filter&) = delete;
    C_FIR_filter& operator=(const C_FIR_filter&) = delete;

    void init(int len, int dec, double* itaps, double* qtaps) {
        length = len; decimateratio = dec;
        delete[] ifilter; ifilter = nullptr;
        delete[] qfilter; qfilter = nullptr;
        for (int i = 0; i < FIRBufferLen; i++) ibuffer[i] = qbuffer[i] = 0.0;
        if (itaps) { ifilter = new double[len]; for (int i = 0; i < len; i++) ifilter[i] = itaps[i]; }
        if (qtaps) { qfilter = new double[len]; for (int i = 0; i < len; i++) qfilter[i] = qtaps[i]; }
        pointer = len; counter = 0;
    }
    void init_lowpass(int len, int dec, double freq) {
        double* fi = bp_FIR(len, 0, 0.0, freq);
        init(len, dec, fi, fi);
        delete[] fi;
    }
    void init_hilbert(int len, int dec) {
        double* fi = bp_FIR(len, 0, 0.05, 0.45);
        double* fq = bp_FIR(len, 1, 0.05, 0.45);
        init(len, dec, fi, fq);
        delete[] fi; delete[] fq;
    }

    // complex in / complex out, returns 1 when a decimated output is ready
    int run(const cmplx& in, cmplx& out) {
        ibuffer[pointer] = in.real();
        qbuffer[pointer] = in.imag();
        counter++;
        if (counter == decimateratio)
            out = cmplx(mac(&ibuffer[pointer - length], ifilter, length),
                        mac(&qbuffer[pointer - length], qfilter, length));
        pointer++;
        if (pointer == FIRBufferLen) {
            memmove(ibuffer, ibuffer + FIRBufferLen - length, length * sizeof(double));
            memmove(qbuffer, qbuffer + FIRBufferLen - length, length * sizeof(double));
            pointer = length;
        }
        if (counter == decimateratio) { counter = 0; return 1; }
        return 0;
    }
    // real in / real out (I path), returns 1 when a decimated output is ready
    int Irun(const double& in, double& out) {
        double* iptr = ibuffer + pointer;
        pointer++; counter++;
        *iptr = in;
        if (counter == decimateratio) out = mac(iptr - length, ifilter, length);
        if (pointer == FIRBufferLen) {
            memcpy(ibuffer, ibuffer + FIRBufferLen - length, length * sizeof(double));
            pointer = length;
        }
        if (counter == decimateratio) { counter = 0; return 1; }
        return 0;
    }
private:
    double* bp_FIR(int len, int hilbert, double f1, double f2) {
        double* fir = new double[len];
        for (int i = 0; i < len; i++) {
            double t = i - (len - 1.0) / 2.0;
            double h = i * (1.0 / (len - 1.0));
            double x;
            if (!hilbert) x = (2 * f2 * sinc(2 * f2 * t) - 2 * f1 * sinc(2 * f1 * t)) * hamming(h);
            else          x = -((2 * f2 * cosc(2 * f2 * t) - 2 * f1 * cosc(2 * f1 * t)) * hamming(h));
            fir[i] = x;
        }
        return fir;
    }
    int length, decimateratio;
    double *ifilter, *qfilter;
    double ibuffer[FIRBufferLen], qbuffer[FIRBufferLen];
    int pointer, counter;
};

// ===================================================================
// Boxcar moving average
// ===================================================================
class Cmovavg {
public:
    explicit Cmovavg(int filtlen = 64) : len(filtlen), pint(0), out(0), empty(true) { in = new double[len]; }
    ~Cmovavg() { delete[] in; }
    Cmovavg(const Cmovavg&) = delete;
    Cmovavg& operator=(const Cmovavg&) = delete;

    double run(double a) {
        if (empty) {
            empty = false; out = 0;
            for (int i = 0; i < len; i++) { in[i] = a; out += a; }
            pint = 0; return a;
        }
        out = out - in[pint] + a;
        in[pint] = a;
        if (++pint >= len) pint = 0;
        return out / len;
    }
    void reset() { empty = true; }
private:
    double* in;
    int len, pint;
    double out;
    bool empty;
};

} // namespace dominoex
