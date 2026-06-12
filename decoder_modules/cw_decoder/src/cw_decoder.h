/*
 *    cw_decoder.h  --  CW (Morse) receive decoder for SDR++
 *
 *    The receive algorithm (adaptive threshold envelope detection, the
 *    KEYDOWN/KEYUP/QUERY state machine and the dot/dash speed tracking)
 *    is ported from FLDigi (src/cw_rtty/cw.cxx, by Lawrence Glaister VE7IT
 *    and the FLDigi authors).
 *
 *    Port for SDR++ (C) 2025 F4JTV
 *
 *    GNU General Public License v3 or later. See <http://www.gnu.org/licenses/>.
 */

#pragma once
#include <string>
#include <cstring>
#include <cmath>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include "morse.h"

namespace cw {

    // ---- Timing magic numbers (kept identical to FLDigi at 8 kHz) ----
    static const int    CW_SAMPLERATE = 8000;                 // internal decode rate
    static const int    DEC_RATIO     = 16;                   // envelope decimation
    static const int    KWPM          = (12 * CW_SAMPLERATE) / 10; // samples-in-dot = KWPM / WPM
    static const int    TRACKING_FILTER_SIZE = 16;
    static const int    MAX_MORSE_ELEMENTS   = 6;

    // Hard bounds for the user-selectable speed range (WPM).
    static const int    CW_LOWER_LIMIT = 5;
    static const int    CW_UPPER_LIMIT = 60;

    // Return codes / events (from FLDigi).
    enum { CW_SUCCESS = 0, CW_ERROR = -1 };
    enum CW_RX_STATE { RS_IDLE = 0, RS_IN_TONE, RS_AFTER_TONE };
    enum CW_EVENT { CW_RESET_EVENT, CW_KEYDOWN_EVENT, CW_KEYUP_EVENT, CW_QUERY_EVENT };

    // Exponential decay average (FLDigi misc.h).
    static inline double decayavg(double average, double input, int weight) {
        if (weight <= 1) return input;
        return ((input - average) / (double)weight) + average;
    }

    static inline double clampd(double x, double lo, double hi) {
        return (x < lo) ? lo : ((x > hi) ? hi : x);
    }

    // Sliding moving-average filter (FLDigi Cmovavg).
    class Cmovavg {
    public:
        Cmovavg(int filtlen = 16) { setLength(filtlen); }
        void setLength(int filtlen) {
            if (filtlen < 1) filtlen = 1;
            buf.assign(filtlen, 0.0);
            len = filtlen;
            acc = 0.0;
            ptr = 0;
            empty = true;
        }
        void reset() {
            std::fill(buf.begin(), buf.end(), 0.0);
            acc = 0.0;
            ptr = 0;
            empty = true;
        }
        double run(double a) {
            if (empty) {
                empty = false;
                acc = 0.0;
                for (int i = 0; i < len; i++) { buf[i] = a; acc += a; }
                ptr = 0;
                return a;
            }
            acc = acc - buf[ptr] + a;
            buf[ptr] = a;
            if (++ptr >= len) ptr = 0;
            return acc / (double)len;
        }
    private:
        std::vector<double> buf;
        double acc = 0.0;
        int len = 16, ptr = 0;
        bool empty = true;
    };

    // ------------------------------------------------------------------
    // CW decoder. Feed it the magnitude envelope of the tuned channel at
    // CW_SAMPLERATE; it emits decoded characters through the onChar callback.
    // ------------------------------------------------------------------
    class Decoder {
    public:
        Decoder() {
            trackingfilter.setLength(TRACKING_FILTER_SIZE);
            morse.init();
            setSpeed(cwSpeed);     // also sizes the bit filter and syncs timing
            reset();
        }

        // Callback fired for every decoded character (UTF-8 string, usually 1 char).
        std::function<void(const std::string&)> onChar;

        // -------- user controls --------
        void setSpeed(int wpm) {
            std::lock_guard<std::mutex> lck(mtx);
            cwSpeed = (int)clampd(wpm, CW_LOWER_LIMIT, CW_UPPER_LIMIT);
            updateBitFilter();
            syncParameters();
        }
        void setRange(int wpm) {
            std::lock_guard<std::mutex> lck(mtx);
            cwRange = wpm < 0 ? 0 : wpm;
            syncParameters();
        }
        void setTracking(bool en) {
            std::lock_guard<std::mutex> lck(mtx);
            trackEnabled = en;
            syncParameters();
        }
        void setSquelch(bool en, double level) {
            std::lock_guard<std::mutex> lck(mtx);
            squelchOn = en;
            squelchLevel = level;
            if (!en) { sqOpen.store(true, std::memory_order_relaxed); }
        }
        void setProsignDisplay(bool en) {
            std::lock_guard<std::mutex> lck(mtx);
            morse.setProsignDisplay(en);
        }

        int  getReceiveSpeed() { return cw_receive_speed; }
        double getMetric()     { return metric; }
        double getSigLevel()   { return siglevel; }

        // Squelch state for the audio gate (true = open / audio passes).
        std::atomic<bool>* squelchFlag() { return &sqOpen; }

        void reset() {
            std::lock_guard<std::mutex> lck(mtx);
            std::string sc;
            handleEvent(CW_RESET_EVENT, sc);
            sig_avg = 0.0;
            noise_floor = 1.0;
            agc_peak = 0.0;
            metric = 0.0;
            siglevel = 0.0;
            bitfilter.reset();
            trackingfilter.reset();
            two_dots = 2 * KWPM / cwSpeed;
            syncParameters();
        }

        // Process a block of envelope samples (magnitude) at CW_SAMPLERATE.
        void process(const float* env, int count) {
            std::lock_guard<std::mutex> lck(mtx);
            for (int i = 0; i < count; i++) {
                ++smpl_ctr;                      // timing counter at full rate
                if (smpl_ctr % DEC_RATIO) continue; // decimate the decode rate
                double v = bitfilter.run((double)env[i]);
                decodeStream(v);
            }
        }

    private:
        // ---- timing / tracking ----------------------------------------
        void updateBitFilter() {
            int symbollen = (int)std::lround(CW_SAMPLERATE * 1.2 / (double)cwSpeed);
            int bfv = symbollen / (2 * DEC_RATIO);
            if (bfv < 1) bfv = 1;
            bitfilter.setLength(bfv);
        }

        void syncParameters() {
            // Detect a change in tracking mode or nominal speed.
            if ((trackEnabled != lastTrack) || (cw_send_speed != cwSpeed)) {
                trackingfilter.reset();
                two_dots = 2 * (KWPM / cwSpeed);
            }
            lastTrack = trackEnabled;
            cw_send_speed = cwSpeed;

            // Receive speed window from the user "range" setting.
            lowerwpm = cwSpeed - cwRange;
            upperwpm = cwSpeed + cwRange;
            if (lowerwpm < CW_LOWER_LIMIT) lowerwpm = CW_LOWER_LIMIT;
            if (upperwpm > CW_UPPER_LIMIT) upperwpm = CW_UPPER_LIMIT;
            cw_lower_limit = 2 * KWPM / upperwpm;   // smallest two_dots (fastest)
            cw_upper_limit = 2 * KWPM / lowerwpm;   // largest two_dots (slowest)

            if (trackEnabled) {
                // Keep the tracked speed inside the requested range.
                if (two_dots < cw_lower_limit) two_dots = cw_lower_limit;
                if (two_dots > cw_upper_limit) two_dots = cw_upper_limit;
                cw_receive_speed = KWPM / (two_dots / 2);
            }
            else {
                cw_receive_speed = cw_send_speed;
                two_dots = 2 * KWPM / cwSpeed;
            }

            if (cw_receive_speed > 0) cw_receive_dot_length = KWPM / cw_receive_speed;
            else                      cw_receive_dot_length = KWPM / 5;
            cw_receive_dash_length    = 3 * cw_receive_dot_length;
            cw_noise_spike_threshold  = cw_receive_dot_length / 2;
        }

        void updateTracking(int dur_1, int dur_2) {
            static const int min_dot  = KWPM / 200;
            static const int max_dash = 3 * KWPM / 5;
            if ((dur_1 > dur_2) && (dur_1 > 4 * dur_2)) return;
            if ((dur_2 > dur_1) && (dur_2 > 4 * dur_1)) return;
            if (dur_1 < min_dot || dur_2 < min_dot)     return;
            if (dur_2 > max_dash)                       return;
            two_dots = (int)trackingfilter.run((dur_1 + dur_2) / 2);
            syncParameters();
        }

        static inline int usec_diff(unsigned int earlier, unsigned int later) {
            return (earlier >= later) ? 0 : (int)(later - earlier);
        }

        // ---- envelope detection (FLDigi decode_stream) ----------------
        void decodeStream(double value) {
            std::string sc;
            // Medium attack/decay (FLDigi default index 1).
            const int attack = 200;
            const int decay  = 1000;

            sig_avg = decayavg(sig_avg, value, decay);

            if (value < sig_avg) {
                if (value < noise_floor) noise_floor = decayavg(noise_floor, value, attack);
                else                     noise_floor = decayavg(noise_floor, value, decay);
            }
            if (value > sig_avg) {
                if (value > agc_peak) agc_peak = decayavg(agc_peak, value, attack);
                else                  agc_peak = decayavg(agc_peak, value, decay);
            }

            double norm_noise = (agc_peak > 0.0) ? noise_floor / agc_peak : 0.0;
            double norm_sig   = (agc_peak > 0.0) ? sig_avg     / agc_peak : 0.0;
            siglevel = norm_sig;

            value = (agc_peak > 0.0) ? value / agc_peak : 0.0;

            metric = 0.8 * metric;
            if ((noise_floor > 1e-4) && (noise_floor < sig_avg))
                metric += 0.2 * clampd(2.5 * (20.0 * std::log10(sig_avg / noise_floor)), 0, 100);

            double diff = norm_sig - norm_noise;
            CWupper = norm_sig  - 0.2 * diff;
            CWlower = norm_noise + 0.7 * diff;

            // Squelch decision, shared with the audio gate.
            bool open = (!squelchOn) || (metric > squelchLevel);
            sqOpen.store(open, std::memory_order_relaxed);

            if (open) {
                // Hysteresis detector: rising edge -> tone start.
                if ((value > CWupper) && (cw_receive_state != RS_IN_TONE))
                    handleEvent(CW_KEYDOWN_EVENT, sc);
                // Falling edge -> tone stop.
                if ((value < CWlower) && (cw_receive_state == RS_IN_TONE))
                    handleEvent(CW_KEYUP_EVENT, sc);
            }

            if (handleEvent(CW_QUERY_EVENT, sc) == CW_SUCCESS && onChar) {
                if (!sc.empty()) onChar(sc);
            }
        }

        // ---- the FLDigi receive state machine -------------------------
        int handleEvent(int cw_event, std::string& sc) {
            int element_usec;

            switch (cw_event) {
            case CW_RESET_EVENT:
                syncParameters();
                cw_receive_state = RS_IDLE;
                cw_ptr = 0;
                smpl_ctr = 0;
                rx_rep_buf.clear();
                hev_space_sent = true;
                hev_last_element = 0;
                break;

            case CW_KEYDOWN_EVENT:
                if (cw_receive_state == RS_IN_TONE) return CW_ERROR;
                if (cw_receive_state == RS_IDLE) {
                    smpl_ctr = 0;
                    rx_rep_buf.clear();
                    cw_ptr = 0;
                }
                cw_rr_start_timestamp = smpl_ctr;
                old_cw_receive_state  = cw_receive_state;
                cw_receive_state      = RS_IN_TONE;
                return CW_ERROR;

            case CW_KEYUP_EVENT:
                if (cw_receive_state != RS_IN_TONE) return CW_ERROR;
                cw_rr_end_timestamp = smpl_ctr;
                element_usec = usec_diff(cw_rr_start_timestamp, cw_rr_end_timestamp);

                syncParameters();
                // Reject spikes shorter than the noise threshold.
                if (cw_noise_spike_threshold > 0 && element_usec < cw_noise_spike_threshold) {
                    cw_receive_state = RS_IDLE;
                    return CW_ERROR;
                }

                // Speed tracking on dot-dash / dash-dot pairs (the 1:3 ratio).
                if (hev_last_element > 0) {
                    if ((element_usec > 2 * hev_last_element) && (element_usec < 4 * hev_last_element))
                        updateTracking(hev_last_element, element_usec);
                    if ((hev_last_element > 2 * element_usec) && (hev_last_element < 4 * element_usec))
                        updateTracking(element_usec, hev_last_element);
                }
                hev_last_element = element_usec;

                // Dot if shorter than 2 dot-times, else dash.
                if (element_usec <= two_dots) rx_rep_buf += CW_DOT_REPRESENTATION;
                else                          rx_rep_buf += CW_DASH_REPRESENTATION;

                // Too many elements -> noise, reset.
                if (rx_rep_buf.length() > (size_t)MAX_MORSE_ELEMENTS) {
                    cw_receive_state = RS_IDLE;
                    cw_ptr = 0;
                    smpl_ctr = 0;
                    return CW_ERROR;
                }
                cw_receive_state = RS_AFTER_TONE;
                return CW_ERROR;

            case CW_QUERY_EVENT:
                if (cw_receive_state == RS_IN_TONE) return CW_ERROR;
                syncParameters();
                element_usec = usec_diff(cw_rr_end_timestamp, smpl_ctr);

                // Short gap: still inside a character.
                if (element_usec < (2 * cw_receive_dot_length)) return CW_ERROR;

                // Medium gap: end of character.
                if (element_usec >= (2 * cw_receive_dot_length) &&
                    element_usec <= (4 * cw_receive_dot_length) &&
                    cw_receive_state == RS_AFTER_TONE) {
                    sc = morse.rx_lookup(rx_rep_buf);
                    if (sc.empty()) sc = errorChar; // configurable error marker
                    rx_rep_buf.clear();
                    cw_receive_state = RS_IDLE;
                    hev_space_sent   = false;
                    cw_ptr = 0;
                    return CW_SUCCESS;
                }

                // Long gap: word space.
                if ((element_usec > (4 * cw_receive_dot_length)) && !hev_space_sent) {
                    sc = " ";
                    hev_space_sent = true;
                    return CW_SUCCESS;
                }
                return CW_ERROR;
            }
            return CW_ERROR;
        }

        // ---- members --------------------------------------------------
        std::mutex mtx;
        cMorse morse;
        std::string errorChar = "";   // "" = drop bad decodes (set to "*" to show them)

        Cmovavg bitfilter{16};
        Cmovavg trackingfilter{TRACKING_FILTER_SIZE};

        // user controls
        int  cwSpeed = 18;            // nominal WPM
        int  cwRange = 10;            // +/- WPM tracking window
        bool trackEnabled = true;
        bool lastTrack = true;
        bool squelchOn = false;
        double squelchLevel = 10.0;
        std::atomic<bool> sqOpen{ true };

        // timing state
        int two_dots = 2 * KWPM / 18;
        int cw_send_speed = 18;
        int cw_receive_speed = 18;
        int cw_receive_dot_length = KWPM / 18;
        int cw_receive_dash_length = 3 * (KWPM / 18);
        int cw_noise_spike_threshold = (KWPM / 18) / 2;
        int cw_lower_limit = 0, cw_upper_limit = 0;
        int lowerwpm = 5, upperwpm = 60;

        unsigned int smpl_ctr = 0;
        unsigned int cw_rr_start_timestamp = 0;
        unsigned int cw_rr_end_timestamp = 0;
        int cw_receive_state = RS_IDLE;
        int old_cw_receive_state = RS_IDLE;
        std::string rx_rep_buf;
        int cw_ptr = 0;

        // handle_event statics (made into members so reset() works).
        bool hev_space_sent = true;
        int  hev_last_element = 0;

        // detection state
        double sig_avg = 0.0;
        double noise_floor = 1.0;
        double agc_peak = 0.0;
        double metric = 0.0;
        double siglevel = 0.0;
        double CWupper = 0.0, CWlower = 0.0;
    };

} // namespace cw
