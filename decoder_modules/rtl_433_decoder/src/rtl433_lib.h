/** @file
    Thin C++ wrapper around the rtl_433 static library (libr_433.a).

    Instead of re-implementing the decoders, this links the real rtl_433 and
    drives its internal pipeline directly:

        CU8 IQ  ->  magnitude_est_cu8 / envelope_detect   (AM envelope)
                ->  baseband_demod_FM                      (FM discriminator)
                ->  pulse_detect_package                   (OOK/FSK detection)
                ->  run_ook_demods / run_fsk_demods        (ALL ~320 decoders)
                ->  custom data_output_t                   (-> our callback)

    This mirrors the body of rtl_433.c's sdr_callback(), but feeds samples that
    come from an SDR++ VFO rather than from a physical dongle.

    Licensed under the GNU General Public License v3 or later.
    Links against rtl_433 (GPLv2+).
*/
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace rtl433br {

    /// One decoded field (key/value), already stringified.
    struct Field {
        std::string key;
        std::string value;
    };

    /// A decoded message lifted from an rtl_433 data_t.
    struct Message {
        std::string model;
        std::string mod;       ///< "OOK" / "FSK" if present
        std::vector<Field> fields;
    };

    /// Wraps an r_cfg with all protocols registered and a custom output sink.
    /// Not copyable. Thread-safety: feed() and setOutput() are guarded.
    class RTL433 {
    public:
        RTL433();
        ~RTL433();

        RTL433(const RTL433&) = delete;
        RTL433& operator=(const RTL433&) = delete;

        /// Register every built-in protocol. Pass includeDisabled=true to also
        /// enable the protocols rtl_433 ships disabled-by-default.
        void registerAllProtocols(bool includeDisabled);

        /// Set acquisition parameters before feeding samples.
        void configure(uint32_t sampleRate, uint32_t centerFreq);

        /// Tune detection levels (rtl_433 -Y minlevel / -Y minsnr).
        void setLevels(float minLevelDb, float minSnrDb);

        /// Where decoded messages go (called from the feeding thread).
        void setOutput(std::function<void(const Message&)> cb);

        /// Feed one chunk of interleaved CU8 IQ (I,Q,I,Q,...). len = #bytes.
        void feedCU8(const uint8_t* iq, uint32_t len);

        /// Number of protocols currently registered.
        int protocolCount() const;

        /// Name of protocol i (for the UI list).
        std::string protocolName(int i) const;

        /// Enable/disable a single protocol by index.
        void setProtocolEnabled(int i, bool enabled);

        /// rtl_433 version string.
        static std::string version();

    private:
        struct Impl;
        Impl* impl;
    };

}
