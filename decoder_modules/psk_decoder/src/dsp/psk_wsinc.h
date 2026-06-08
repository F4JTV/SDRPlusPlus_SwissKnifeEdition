#pragma once
// ---------------------------------------------------------------------------
//  PSKWSinc - PSK demodulator using a windowed-sinc receive filter.
//
//  This is a drop-in alternative to dsp::demod::PSK<ORDER> (which uses an
//  RRC matched filter). The RRC is the optimal matched filter only when the
//  TX pulse is also RRC-shaped; FLDIGI's PSK transmitter uses a different
//  shape (a half-cosine cross-fade between consecutive symbols, see
//  psk.cxx ~line 2256: `shapeA = 0.5*cos(i*pi/L)+0.5`, then mixing
//  `prevsymbol*shapeA + symbol*(1-shapeA)`). For 8PSK the constellation
//  points are only pi/4 apart, so the residual ISI from an unmatched RRC
//  is enough to push many symbols across decision boundaries.
//
//  FLDIGI itself uses a windowed-sinc low-pass for all the higher-baud PSK
//  modes (psk.cxx ~line 949: `wsincfilt(fir1c, 1.0/symbollen, FIRLEN)` with
//  a Blackman window). It is not a true matched filter for the half-cosine
//  TX pulse either, but it band-limits the signal without distorting the
//  symbol energy. Empirically (compared offline on the same real signals)
//  this filter recovers the clean transmitted text where the RRC produces
//  scattered character substitutions.
// ---------------------------------------------------------------------------
#include <dsp/processor.h>
#include <dsp/types.h>
#include <dsp/filter/fir.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/loop/costas.h>
#include <dsp/clock_recovery/mm.h>
#include <dsp/taps/windowed_sinc.h>
#include <dsp/window/blackman.h>

namespace fldigi {

    template<int ORDER>
    class PSKWSinc : public dsp::Processor<dsp::complex_t, dsp::complex_t> {
        using base_type = dsp::Processor<dsp::complex_t, dsp::complex_t>;
    public:
        PSKWSinc() {}

        void init(dsp::stream<dsp::complex_t>* in,
                  double symbolrate, double samplerate,
                  int firTapCount,
                  double agcRate,
                  double costasBandwidth,
                  double omegaGain, double muGain,
                  double omegaRelLimit = 0.01) {
            _symbolrate = symbolrate;
            _samplerate = samplerate;
            _firTapCount = firTapCount;

            // Build windowed-sinc taps. Cutoff is half the symbol rate; that is
            // tighter than FLDIGI's `1/symbollen` (the symbol rate itself) but
            // visibly produces lower phase-residuals on the test signals. The
            // Blackman window matches FLDIGI's `wsincfilt(blackman=true)`.
            double cutoff_hz = _symbolrate * 0.5;
            firTaps = dsp::taps::windowedSinc<float>(_firTapCount,
                                                     cutoff_hz, _samplerate,
                                                     dsp::window::blackman);

            fir.init(NULL, firTaps);
            agc.init(NULL, 1.0, 10e6, agcRate);
            costas.init(NULL, costasBandwidth);
            recov.init(NULL, _samplerate / _symbolrate,
                       omegaGain, muGain, omegaRelLimit);

            fir.out.free();
            agc.out.free();
            costas.out.free();
            recov.out.free();

            base_type::init(in);
        }

        ~PSKWSinc() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            dsp::taps::free(firTaps);
        }

        void setSymbolrate(double symbolrate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _symbolrate = symbolrate;
            dsp::taps::free(firTaps);
            firTaps = dsp::taps::windowedSinc<float>(_firTapCount,
                                                     _symbolrate * 0.5, _samplerate,
                                                     dsp::window::blackman);
            fir.setTaps(firTaps);
            recov.setOmega(_samplerate / _symbolrate);
            base_type::tempStart();
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _samplerate = samplerate;
            dsp::taps::free(firTaps);
            firTaps = dsp::taps::windowedSinc<float>(_firTapCount,
                                                     _symbolrate * 0.5, _samplerate,
                                                     dsp::window::blackman);
            fir.setTaps(firTaps);
            recov.setOmega(_samplerate / _symbolrate);
            base_type::tempStart();
        }

        void setCostasBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            costas.setBandwidth(bandwidth);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            fir.reset();
            agc.reset();
            costas.reset();
            recov.reset();
            base_type::tempStart();
        }

        inline int process(int count, const dsp::complex_t* in, dsp::complex_t* out) {
            fir.process(count, in, out);
            agc.process(count, out, out);
            costas.process(count, out, out);
            return recov.process(count, out, out);
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

    protected:
        double _symbolrate = 0;
        double _samplerate = 0;
        int    _firTapCount = 0;

        dsp::tap<float>                       firTaps;
        dsp::filter::FIR<dsp::complex_t, float> fir;
        dsp::loop::FastAGC<dsp::complex_t>      agc;
        dsp::loop::Costas<ORDER>                costas;
        dsp::clock_recovery::MM<dsp::complex_t> recov;
    };

}
