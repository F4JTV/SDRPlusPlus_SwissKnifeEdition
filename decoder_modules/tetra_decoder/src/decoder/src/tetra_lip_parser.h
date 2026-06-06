#ifndef TETRA_LIP_PARSER_H
#define TETRA_LIP_PARSER_H

/* TETRA LIP (Location Information Protocol) parser.
 *
 * LIP is the standard way TETRA radios report their position over SDS.
 * Two transport variants exist:
 *
 *   Protocol 0x0A : LIP without SDS-TL header (carried directly in a
 *                   short SDS payload). Most common on portable radios
 *                   sending periodic position updates.
 *
 *   Protocol 0x89 : LIP with SDS-TL header (allows acknowledgement and
 *                   includes a service-centre timestamp). Less common
 *                   but mandatory for some emergency reporting modes.
 *
 * Within either transport, LIP itself carries one or more "Location
 * Report" PDUs. The most common are:
 *
 *   Type 0 : Short Location Report  (8 bytes, mandatory subset)
 *   Type 1 : Long Location Report   (>= 12 bytes, with altitude /
 *                                    timestamp / extended accuracy)
 *
 * Coordinate encoding (ETSI TS 100 392-18-1 §6.2):
 *   - Latitude  : 24 bits signed two's complement, LSB = 180/2^24 degrees
 *                 (≈ 1.07e-5°, ≈ 1.2 m resolution at the equator)
 *   - Longitude : 24 bits signed two's complement, LSB = 360/2^25 degrees
 *                 (yes — the longitude resolution is twice the latitude
 *                 resolution because the spec uses 2^25 as denominator)
 *
 * Velocity encoding is piecewise-linear in km/h: codes 0..15 step 1 km/h,
 * 16..47 step 2 km/h, 48..127 step 16 km/h. We translate back to km/h
 * exactly per the spec.
 *
 * Reference: ETSI TS 100 392-18-1 v1.7.1 §6 ("Location data transfer").
 */

#include "tetra_common.h"
#include <stdint.h>

/* Entry point invoked by the SDS parser when a LIP protocol id is seen.
 * `bits` points at the byte AFTER the LIP protocol identifier byte,
 * `len_bits` is the remaining bit count, `tms` provides src/dst SSI
 * threading from the surrounding D-SDS-DATA PDU. */
int tetra_lip_parse(const uint8_t *bits, unsigned int len_bits,
                    struct tetra_mac_state *tms);

#endif /* TETRA_LIP_PARSER_H */
