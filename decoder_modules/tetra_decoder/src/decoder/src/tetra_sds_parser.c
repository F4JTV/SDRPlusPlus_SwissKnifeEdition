/* TETRA SDS user-data parser - implementation.
 *
 * Receives the bit-stream starting at the protocol identifier byte of
 * an SDS user-data field (as carried inside a D-SDS-DATA CMCE PDU),
 * detects the protocol, decodes the text payload, and emits a
 * `sds_text` JSON event.
 *
 * Three text alphabets are supported:
 *
 *   GSM 7-bit packed (3GPP TS 23.038) — most common on legacy networks;
 *   characters are packed 7-to-the-octet with the residual bits of the
 *   next char carried over.
 *
 *   ISO-8859-1 (Latin-1) — 1 byte per char, trivial mapping into UTF-8.
 *
 *   UCS-2 / UTF-16BE — 2 bytes per char in network order, transcoded to
 *   UTF-8. The BMP code points are emitted directly; surrogate pairs
 *   are rare in TETRA SDS and we don't bother synthesising them.
 *
 * The output is always valid UTF-8, suitable for inclusion in the JSON
 * event without further escaping (tetra_events.c handles the JSON
 * string-escape independently).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tetra_bitcur.h"
#include "tetra_sds_parser.h"
#include "tetra_lip_parser.h"
#include "tetra_events.h"

/* ---- GSM 7-bit alphabet ------------------------------------------------ */
/* 3GPP TS 23.038 §6.2.1 default alphabet. Code points are Unicode. */
static const uint16_t gsm7_basic[128] = {
	0x0040,0x00A3,0x0024,0x00A5,0x00E8,0x00E9,0x00F9,0x00EC,
	0x00F2,0x00C7,0x000A,0x00D8,0x00F8,0x000D,0x00C5,0x00E5,
	0x0394,0x005F,0x03A6,0x0393,0x039B,0x03A9,0x03A0,0x03A8,
	0x03A3,0x0398,0x039E,0x001B,0x00C6,0x00E6,0x00DF,0x00C9,
	0x0020,0x0021,0x0022,0x0023,0x00A4,0x0025,0x0026,0x0027,
	0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
	0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
	0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
	0x00A1,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
	0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
	0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,
	0x0058,0x0059,0x005A,0x00C4,0x00D6,0x00D1,0x00DC,0x00A7,
	0x00BF,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
	0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
	0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
	0x0078,0x0079,0x007A,0x00E4,0x00F6,0x00F1,0x00FC,0x00E0,
};

/* ---- UTF-8 emission helper -------------------------------------------- */

/* Append code point `cp` as UTF-8 into `out` (out_pos updated). Bounds-
 * checked: silently drops the char if the buffer would overflow. */
static void utf8_append(char *out, int outsz, int *out_pos, uint32_t cp) {
	int p = *out_pos;
	if (cp < 0x80) {
		if (p + 1 >= outsz) return;
		out[p++] = (char)cp;
	} else if (cp < 0x800) {
		if (p + 2 >= outsz) return;
		out[p++] = (char)(0xC0 | (cp >> 6));
		out[p++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		if (p + 3 >= outsz) return;
		out[p++] = (char)(0xE0 | (cp >> 12));
		out[p++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[p++] = (char)(0x80 | (cp & 0x3F));
	} else {
		if (p + 4 >= outsz) return;
		out[p++] = (char)(0xF0 | (cp >> 18));
		out[p++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[p++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[p++] = (char)(0x80 | (cp & 0x3F));
	}
	*out_pos = p;
}

/* ---- text decoders ---------------------------------------------------- */

/* Decode GSM 7-bit packed payload. `bits_in` are the raw payload bits
 * (MSB-first from the air, so we unpack starting at bit 0 and group
 * septets little-endian within each octet per 3GPP convention). */
static void decode_gsm7(struct bcur *c, char *out, int outsz) {
	int out_pos = 0;
	while (bcur_have(c, 7) && out_pos < outsz - 4) {
		/* GSM 7-bit packing writes septets little-endian within the
		 * byte stream: bit-0 of the first septet is at offset 0,
		 * bit-1 at offset 1, ... bit-6 at offset 6; then bit-0 of
		 * the next septet is at offset 7. So reading 7 bits LSB-first
		 * from the unpacked array gives us the septet directly. */
		uint8_t s = 0;
		for (int i = 0; i < 7; i++) {
			s |= (c->bits[c->pos + i] & 1) << i;
		}
		c->pos += 7;
		if (s == 0) break;
		utf8_append(out, outsz, &out_pos, gsm7_basic[s & 0x7F]);
	}
	out[out_pos] = '\0';
}

/* Decode 8-bit ISO-8859-1. Each byte is a code point in the Latin-1
 * supplement; convert directly to UTF-8. */
static void decode_latin1(struct bcur *c, char *out, int outsz) {
	int out_pos = 0;
	while (bcur_have(c, 8) && out_pos < outsz - 3) {
		uint8_t b = (uint8_t)bcur_get(c, 8);
		if (b == 0) break;
		utf8_append(out, outsz, &out_pos, b);
	}
	out[out_pos] = '\0';
}

/* Decode UCS-2 / UTF-16BE. 2 bytes per char, big-endian. */
static void decode_ucs2(struct bcur *c, char *out, int outsz) {
	int out_pos = 0;
	while (bcur_have(c, 16) && out_pos < outsz - 4) {
		uint16_t cp = (uint16_t)bcur_get(c, 16);
		if (cp == 0) break;
		utf8_append(out, outsz, &out_pos, cp);
	}
	out[out_pos] = '\0';
}

/* ---- simple text formats (protocol 0x02, 0x09) ----------------------- */

/* The TETRA SDS "simple text" payload starts with a coding info byte:
 *   1 bit  : timestamp used (1 = the next 24 bits are a timestamp)
 *   7 bits : text coding scheme  (0 = GSM 7-bit, 1 = 8-bit, 2 = UCS-2)
 *
 * On real cells, simple-text PDUs rarely carry a timestamp — the
 * timestamp flag is typically 0, so we keep the path lightweight. */
static void parse_simple_text(struct bcur *c, int src, int dst,
                              int protocol_id) {
	if (!bcur_have(c, 8)) return;

	uint8_t info = (uint8_t)bcur_get(c, 8);
	int ts_used = (info >> 7) & 1;
	int scheme  = info & 0x7F;

	if (ts_used) {
		/* Skip the 24-bit timestamp (we surface our own wall-clock
		 * timestamp on the JSON event anyway). */
		if (bcur_have(c, 24)) bcur_get(c, 24);
	}

	char text[1024];
	text[0] = '\0';
	switch (scheme) {
	case 0:  decode_gsm7(c, text, sizeof(text)); break;
	case 1:  decode_latin1(c, text, sizeof(text)); break;
	case 2:  decode_ucs2(c, text, sizeof(text)); break;
	default:
		/* Unknown scheme — emit empty text but still surface the
		 * src/dst so the consumer at least sees a message landed. */
		break;
	}

	te_emit_sds_text(src, dst, protocol_id, text);
}

/* ---- text with SDS-TL header (protocol 0x82) ------------------------- */

/* SDS-TL header layout (ETSI TS 100 392-18-1 §7.5.1):
 *   8  : protocol identifier (= 0x82, already consumed before entry)
 *   4  : message type (1=Transfer, 2=Report, 3=Ack, 4=Short report)
 *   4  : delivery report request type
 *   8  : message reference (ID, for ack correlation)
 *   ... varies by message type ...
 *
 * For "Transfer" (type 1, the most common), after the message reference
 * we have the validity period (8 bits), optional forward address (1+0/24
 * bits), optional service-centre timestamp (24 bits if flag set), then
 * the SDU payload.
 *
 * The SDU payload semantics depend on the OUTER protocol identifier:
 *   - 0x82 / 0x83 : text  → coding scheme byte + chars
 *   - 0x89        : LIP   → raw LIP PDU stream
 *
 * On TMO ADRASEC networks "Short report" (type 4) is also common: it
 * carries no user data, just an ack/status, so we emit an empty text
 * with the protocol_id so the consumer can see the activity. */
static void parse_sds_tl(struct bcur *c, struct tetra_mac_state *tms,
                         int src, int dst, int protocol_id) {
	if (!bcur_have(c, 8)) return;
	uint32_t tl_hdr = bcur_get(c, 8);
	int msg_type = (tl_hdr >> 4) & 0x0F;
	/* drop the 4-bit delivery report request type */

	if (!bcur_have(c, 8)) {
		if (protocol_id != 0x89)
			te_emit_sds_text(src, dst, protocol_id, "");
		return;
	}
	bcur_get(c, 8);  /* message reference */

	if (msg_type != 1) {
		/* Not a "Transfer" — for text protocols we emit an empty
		 * event so the consumer at least sees activity. For LIP we
		 * just bail (a non-Transfer LIP carries no coordinate). */
		if (protocol_id != 0x89)
			te_emit_sds_text(src, dst, protocol_id, "");
		return;
	}

	/* Validity period */
	if (bcur_have(c, 8)) bcur_get(c, 8);

	/* Forward address presence (1 bit) + optional 24-bit address */
	if (bcur_have(c, 1)) {
		uint32_t fwd = bcur_get(c, 1);
		if (fwd && bcur_have(c, 24)) bcur_get(c, 24);
	}

	/* Service-centre timestamp presence (1 bit) + optional 24-bit ts */
	if (bcur_have(c, 1)) {
		uint32_t sc_ts = bcur_get(c, 1);
		if (sc_ts && bcur_have(c, 24)) bcur_get(c, 24);
	}

	/* Now the user-data SDU. Route by outer protocol:
	 *   0x89 → LIP payload (no coding scheme byte)
	 *   0x82/0x83 → text payload (coding scheme byte + chars) */
	if (protocol_id == 0x89) {
		unsigned int rem = bcur_rem(c);
		if (rem > 0) {
			tetra_lip_parse(c->bits + c->pos, rem, tms);
		}
	} else {
		parse_simple_text(c, src, dst, protocol_id);
	}
}

/* ---- public entry point ---------------------------------------------- */

int tetra_sds_parse(const uint8_t *bits, unsigned int len_bits,
                    struct tetra_mac_state *tms, int slot)
{
	(void)slot;
	if (!bits || len_bits < 8) return -1;

	struct bcur c;
	bcur_init(&c, bits, len_bits);

	uint8_t protocol = (uint8_t)bcur_get(&c, 8);

	int src = 0, dst = 0;
	if (tms) {
		src = tms->t_display_st->last_sds_src;
		dst = tms->t_display_st->last_sds_dst;
	}

	switch (protocol) {
	case 0x02:  /* Simple Text Messaging */
	case 0x09:  /* Simple Immediate Text Messaging */
		parse_simple_text(&c, src, dst, protocol);
		break;
	case 0x82:  /* Text Messaging with SDS-TL */
	case 0x83:  /* Complex SDS-TL — same header, more variations */
		parse_sds_tl(&c, tms, src, dst, protocol);
		break;
	case 0x0A:  /* LIP without SDS-TL — direct location report payload */
		tetra_lip_parse(c.bits + c.pos, bcur_rem(&c), tms);
		break;
	case 0x89:  /* LIP with SDS-TL header */
		parse_sds_tl(&c, tms, src, dst, protocol);
		break;
	default:
		/* Unknown / not surfaced. */
		break;
	}

	return 0;
}
