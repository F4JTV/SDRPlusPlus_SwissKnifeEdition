#pragma once
#include <string>
#include <ctime>
#include <stdint.h>
#include <vector>
#include <utils/new_event.h>

// Public C++ API for the FLEX paging protocol decoder.
//
// The underlying state machine and symbol detector are adapted from
// multimon-ng's demod_flex.c (GPL-3.0, Copyright Free Software Foundation
// and contributors); BCH(31,21) error correction is from multimon-ng's
// bch.c (Unlicense / public domain).

namespace flex {

    // Message page types matching the FLEX standard categories.
    // Numbering matches multimon-ng's internal page-type enum, which is
    // useful when correlating with the broader literature.
    enum PageType {
        PAGE_TYPE_SECURE             = 0,
        PAGE_TYPE_SHORT_INSTRUCTION  = 1,
        PAGE_TYPE_TONE               = 2,
        PAGE_TYPE_STANDARD_NUMERIC   = 3,
        PAGE_TYPE_SPECIAL_NUMERIC    = 4,
        PAGE_TYPE_ALPHANUMERIC       = 5,
        PAGE_TYPE_BINARY             = 6,
        PAGE_TYPE_NUMBERED_NUMERIC   = 7,
        PAGE_TYPE_UNKNOWN            = 8
    };

    // Single decoded message reported to the application
    struct Message {
        std::time_t timestamp;      // Wall-clock time the message was decoded
        int64_t     capcode;        // FLEX CAPCODE (can be very large for long addresses)
        PageType    type;           // Decoded page type
        std::string content;        // Decoded text content (UTF-8 / ASCII)
        char        phase;          // 'A'/'B'/'C'/'D' - the phase the message arrived on
        int         baud;           // 1600 / 3200 / 6400
        int         levels;         // 2 or 4 (2-FSK or 4-FSK)
        int         cycle;          // FIW cycle number (0..14)
        int         frame;          // FIW frame number (0..127)
        bool        fragmented;     // True if this message is a fragment
        bool        groupMessage;   // True if delivered as a group call
    };

    class Decoder {
    public:
        // Sample rate that the decoder expects on the input. FLEX is
        // historically decoded at 22050 sps in multimon-ng; the symbol
        // detector constants are tuned around this rate. Pass any rate
        // here and it will be used internally as sample_freq.
        explicit Decoder(double sampleRate);
        ~Decoder();

        // Disable copy (the internal state is non-trivial)
        Decoder(const Decoder&)            = delete;
        Decoder& operator=(const Decoder&) = delete;

        // Feed FM-demodulated audio samples (floats around 0.0). The
        // decoder runs its own symbol synchronisation and frame parsing
        // and emits messages via onMessage as they are decoded.
        void process(const float* samples, int count);

        // Reset all decoder state (sync, lock, partial frame buffers).
        void reset();

        // Emitted whenever a complete message has been decoded
        NewEvent<const Message&> onMessage;

        // True when the demodulator considers itself symbol-locked
        bool isLocked() const;

        // Current sync mode (0 if not yet detected)
        unsigned int currentSyncBaud()   const;  // 1600 / 3200 / 6400
        unsigned int currentSyncLevels() const;  // 2 / 4

        struct Impl;

    private:
        Impl* impl;
    };
}
