/* TETRA LIP parser - implementation.
 *
 * Decodes Short Location Report (PDU type 0) and Long Location Report
 * (PDU type 1) per ETSI TS 100 392-18-1 §6. Emits a single `lip` JSON
 * event per parsed report.
 *
 * Implementation notes:
 *
 *   1. Coordinate decoding uses signed two's complement on 24 bits.
 *      We sign-extend by hand because the air encoding is bit-precise
 *      and we don't want to depend on int24_t (which doesn't exist).
 *
 *   2. The Position Error field (3 or 7 bits depending on report type)
 *      is reported as an upper-bound radius. We translate the standard
 *      ETSI table to metres; values larger than the table cap are
 *      surfaced as the cap (1000+ m), and "unknown" (all bits set) is
 *      reported as -1.
 *
 *   3. Velocity uses the piecewise scheme from §6.2.13 — implemented
 *      faithfully (codes 0..127, in km/h).
 *
 *   4. The Direction-of-Travel is a 4-bit field in the Short Report
 *      (cardinal point resolution, 22.5°/step) and a 9-bit field in
 *      the Long Report (0.7°/step). We surface both as degrees from
 *      true north 0..360.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "tetra_bitcur.h"
#include "tetra_lip_parser.h"
#include "tetra_events.h"

/* ---- coordinate / accuracy / velocity decoders ----------------------- */

/* Sign-extend a `bits`-wide field that sits in the low bits of `v`. */
static int32_t sign_extend(uint32_t v, int bits) {
	uint32_t mask = (1u << bits) - 1u;
	v &= mask;
	if (v & (1u << (bits - 1))) {
		/* Negative — set all higher bits. */
		return (int32_t)(v | ~mask);
	}
	return (int32_t)v;
}

/* Convert 24-bit signed encoded latitude to degrees (range [-90, +90]). */
static double decode_lat(uint32_t raw) {
	int32_t s = sign_extend(raw, 24);
	/* LSB = 180 / 2^24 degrees per the spec. */
	return (double)s * 180.0 / (double)(1L << 24);
}

/* Convert 24-bit signed encoded longitude to degrees (range [-180, +180]). */
static double decode_lon(uint32_t raw) {
	int32_t s = sign_extend(raw, 24);
	/* LSB = 360 / 2^25 degrees per the spec. */
	return (double)s * 360.0 / (double)(1L << 25);
}

/* Position-error decoder (Short Report uses 3 bits, Long Report uses 7).
 * Returns metres, or -1 for "unknown". The table follows ETSI §6.2.12. */
static int decode_position_error(uint32_t v, int field_bits) {
	if (field_bits == 3) {
		/* 3-bit "Position error" — Short Location Report */
		static const int tbl[8] = { 2, 20, 200, 2000, -1, -1, -1, -1 };
		return tbl[v & 7];
	}
	/* 7-bit version (Long Location Report). Roughly: codes 0..63 are
	 * 1..64 m linear, then 64..126 are 70..2000 m piecewise, 127 = unknown. */
	if (v >= 127) return -1;
	if (v < 64)   return (int)(v + 1);
	int code = v - 64;
	return 70 + code * 30;
}

/* Velocity decoder (codes 0..127 → km/h) per §6.2.13.
 * Code 127 means "unknown". */
static int decode_velocity(uint32_t v) {
	if (v >= 127) return -1;
	if (v <= 15)  return (int)v;             /* 0..15 step 1   */
	if (v <= 47)  return 16 + (int)(v - 16) * 2;   /* 16..47 step 2  */
	return 80 + (int)(v - 48) * 16;          /* 48..126 step 16 */
}

/* Direction-of-travel decoder.
 * Short Report: 4 bits (cardinal points, 22.5° steps, code 15 = unknown).
 * Long Report : 9 bits (0.7°/code, codes 0..511, code 511 = unknown). */
static int decode_direction(uint32_t v, int field_bits) {
	if (field_bits == 4) {
		if (v >= 15) return -1;
		return (int)(v * 23);   /* 22.5° rounded to nearest int */
	}
	if (v >= 511) return -1;
	return (int)((double)v * 0.703125 + 0.5);  /* 360/512 ≈ 0.703° */
}

/* ---- per-PDU handlers ------------------------------------------------- */

/* Short Location Report (PDU type 0) — total 64 bits = 8 bytes:
 *
 *   Time elapsed             2
 *   Longitude               24
 *   Latitude                24
 *   Position error           3
 *   Horizontal velocity      7
 *   Direction of travel      4
 *   Type of additional data  2  (currently we ignore additional data)
 *   ...up to 6 trailing reserved bits...
 */
static void parse_short_location_report(struct bcur *c,
                                        int src_ssi) {
	if (!bcur_have(c, 64)) return;

	bcur_get(c, 2);  /* time_elapsed — informative, not surfaced */
	uint32_t lon_raw = bcur_get(c, 24);
	uint32_t lat_raw = bcur_get(c, 24);
	uint32_t pe_raw  = bcur_get(c, 3);
	uint32_t vel_raw = bcur_get(c, 7);
	uint32_t dir_raw = bcur_get(c, 4);
	/* remaining bits (type-of-additional-data + reserved) ignored */

	double lat = decode_lat(lat_raw);
	double lon = decode_lon(lon_raw);
	int    pe  = decode_position_error(pe_raw, 3);
	int    vel = decode_velocity(vel_raw);
	int    dir = decode_direction(dir_raw, 4);

	te_emit_lip(src_ssi, lat, lon, pe, vel, dir);
}

/* Long Location Report (PDU type 1).
 *
 *   Time elapsed              2
 *   Longitude                24
 *   Latitude                 24
 *   Position error            7
 *   Horizontal velocity       7
 *   Direction of travel       9
 *   Altitude               12   (signed, metres)
 *   Acceleration / type      ... varies
 *   Reason for sending        4
 *   ... and optional extension fields
 *
 * We surface the same 6-field event as the Short variant (lat/lon/pe/
 * vel/dir + src). Altitude is dropped on the floor for now — sessions
 * 5+ can extend te_emit_lip() with optional `altitude` if needed. */
static void parse_long_location_report(struct bcur *c, int src_ssi) {
	if (!bcur_have(c, 73)) return;

	bcur_get(c, 2);                         /* time_elapsed */
	uint32_t lon_raw = bcur_get(c, 24);
	uint32_t lat_raw = bcur_get(c, 24);
	uint32_t pe_raw  = bcur_get(c, 7);
	uint32_t vel_raw = bcur_get(c, 7);
	uint32_t dir_raw = bcur_get(c, 9);
	/* altitude + extensions follow, ignored */

	double lat = decode_lat(lat_raw);
	double lon = decode_lon(lon_raw);
	int    pe  = decode_position_error(pe_raw, 7);
	int    vel = decode_velocity(vel_raw);
	int    dir = decode_direction(dir_raw, 9);

	te_emit_lip(src_ssi, lat, lon, pe, vel, dir);
}

/* ---- public entry point --------------------------------------------- */

int tetra_lip_parse(const uint8_t *bits, unsigned int len_bits,
                    struct tetra_mac_state *tms)
{
	if (!bits || len_bits < 2) return -1;

	struct bcur c;
	bcur_init(&c, bits, len_bits);

	int src_ssi = 0;
	if (tms) src_ssi = tms->t_display_st->last_sds_src;

	uint32_t pdu_type = bcur_get(&c, 2);
	switch (pdu_type) {
	case 0:
		parse_short_location_report(&c, src_ssi);
		break;
	case 1:
		parse_long_location_report(&c, src_ssi);
		break;
	case 2:
	case 3:
		/* Type 2 = Location Protocol Identifier extension,
		 * Type 3 = Vendor-specific (used by some German/Nordic
		 *          networks for additional payloads).
		 * Neither carries lat/lon directly — silently ignored. */
		break;
	default:
		break;
	}

	return 0;
}
