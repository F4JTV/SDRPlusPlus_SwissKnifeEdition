#ifndef TETRA_CMCE_PARSER_H
#define TETRA_CMCE_PARSER_H

/* TETRA CMCE (Circuit Mode Control Entity) downlink PDU parser.
 *
 * Walks the bit-packed PDUs received over the MLE layer and emits
 * events via tetra_events.h for the following PDU types:
 *
 *   D-SETUP             call setup announcement
 *   D-CONNECT           call connected
 *   D-CALL-PROCEEDING   call setup acknowledged by SwMI
 *   D-RELEASE           call released
 *   D-TX-CEASED         end of transmission on a voice channel
 *   D-STATUS            short status code (1..32767, including the
 *                       "ETSI emergency" range and operator codes
 *                       commonly used as "10-codes" / brevity codes)
 *
 * Only downlink PDUs are parsed (a passive receiver never sees uplink
 * frames from MS unless it's also located in the cell coverage AND
 * doing reverse demodulation, which is out of scope here).
 *
 * Format references: ETSI EN 300 392-2 v3.4.1 clauses 14.7.x.
 */

#include "tetra_common.h"
#include <stdint.h>

/* Entry point. `bits` is the unpacked CMCE PDU (each byte = 1 bit, MSB
 * first), `len_bits` its length in bits. `slot` is the timeslot the PDU
 * arrived on, used for the slot field of emitted events. Returns 0 on
 * success, negative on malformed input. */
int tetra_cmce_parse(const uint8_t *bits, unsigned int len_bits,
                     struct tetra_mac_state *tms, int slot);

#endif /* TETRA_CMCE_PARSER_H */
