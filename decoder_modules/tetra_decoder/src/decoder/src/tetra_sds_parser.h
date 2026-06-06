#ifndef TETRA_SDS_PARSER_H
#define TETRA_SDS_PARSER_H

/* TETRA SDS (Short Data Service) parser.
 *
 * Handles the user-data portion of a D-SDS-DATA CMCE PDU, extracts the
 * protocol-identifier byte, and routes to the appropriate sub-parser:
 *
 *   0x02         Simple Text Messaging                   (handled here)
 *   0x09         Simple Immediate Text Messaging         (handled here)
 *   0x82         Text Messaging with SDS-TL header       (handled here)
 *   0x83         Complex SDS-TL                          (skeleton only)
 *   0x0A / 0x89  Location Information Protocol (LIP)     (session 4)
 *   others       breadcrumb event only
 *
 * Text decoding supports:
 *   - GSM 7-bit packed alphabet (3GPP TS 23.038)
 *   - 8-bit ISO-8859-1
 *   - UCS-2/UTF-16BE (transcoded to UTF-8)
 *
 * References: ETSI TS 100 392-18-1 (SDS), TS 100 392-2 clause 14.8.40
 * (D-SDS-DATA layout).
 */

#include "tetra_common.h"
#include <stdint.h>

/* Entry point invoked by the CMCE parser when a D-SDS-DATA PDU arrives.
 * `bits` points at the start of the D-SDS-DATA PDU body (the 5-bit
 * CMCE PDU type byte has already been consumed by the CMCE parser).
 * `len_bits` is the remaining bit count, `slot` the TDMA timeslot. */
int tetra_sds_parse(const uint8_t *bits, unsigned int len_bits,
                    struct tetra_mac_state *tms, int slot);

#endif /* TETRA_SDS_PARSER_H */
