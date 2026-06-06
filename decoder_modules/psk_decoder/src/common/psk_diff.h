#pragma once
#include <dsp/processor.h>
#include <dsp/taps/root_raised_cosine.h>
#include <dsp/filter/fir.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/clock_recovery/mm.h>

// ---------------------------------------------------------------------------
//  PSKDiff
//
//  A Costas-free PSK symbol-recovery block: RRC matched filter + AGC +
//  Mueller-and-Muller clock recovery. Outputs one complex symbol per
//  baud rate; the caller does differential phase decoding.
//
//  This matches FLDIGI's PSK receiver chain exactly. FLDIGI uses NO Costas
//  loop on its PSK modes (BPSK/QPSK/8PSK) - it reads the matched filter
//  output at symbol times and computes the differential phase directly.
//
//  Why no Costas loop: for differential PSK, a constant phase offset is
//  cancelled by the differential operation. A Costas loop with order N
//  has N stable equilibria; for QPSK signals the natural symbol-to-symbol
//  90 degree phase jumps can push the loop across equilibria, causing
//  cycle slips that corrupt every transition. The same problem affects
//  8PSK with 45 degree jumps.
//
//  Slow carrier drift is not tracked here; the user must keep the AF
//  reasonably close to the signal centre (the band view + Auto AF make
//  this easy). The differential operation tolerates carrier drift up to
//  roughly +/- baud/4 between consecutive symbols.
// ---------------------------------------------------------------------------

namespace fldigi {

    class PSKDiff : public dsp::Processor<dsp::complex_t, dsp::complex_t> {
        using base_type = dsp::Processor<dsp::complex_t, dsp::complex_t>;
    public:
        PSKDiff() = default;

        ~PSKDiff() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            dsp::taps::free(rrcTaps);
        }

        void init(dsp::stream<dsp::complex_t>* in, double symbolRate, double sampleRate,
                  int rrcTapCount, double rrcBeta,
                  double agcRate, double omegaGain, double muGain,
                  double omegaRelLimit = 0.01)
        {
            _symbolRate  = symbolRate;
            _sampleRate  = sampleRate;
            _rrcTapCount = rrcTapCount;
            _rrcBeta     = rrcBeta;

            rrcTaps = dsp::taps::rootRaisedCosine<float>(
                _rrcTapCount, _rrcBeta, _symbolRate, _sampleRate);
            rrc.init(NULL, rrcTaps);
            agc.init(NULL, 1.0, 10e6, agcRate);
            recov.init(NULL, _sampleRate / _symbolRate,
                       omegaGain, muGain, omegaRelLimit);

            rrc.out.free();
            agc.out.free();
            recov.out.free();

            base_type::init(in);
        }

        inline int process(int count, const dsp::complex_t* in, dsp::complex_t* out) {
            rrc.process(count, in, out);
            agc.process(count, out, out);
            return recov.process(count, out, out);
        }

        int run() override {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

    private:
        double _symbolRate  = 0.0;
        double _sampleRate  = 0.0;
        int    _rrcTapCount = 0;
        double _rrcBeta     = 0.0;

        dsp::tap<float>                       rrcTaps;
        dsp::filter::FIR<dsp::complex_t, float> rrc;
        dsp::loop::FastAGC<dsp::complex_t>      agc;
        dsp::clock_recovery::MM<dsp::complex_t> recov;
    };

}
