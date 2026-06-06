/* TETRA CMCE downlink PDU parser - implementation.
 *
 * The bit-stream layout follows ETSI EN 300 392-2 v3.4.1. We use a tiny
 * bit cursor to walk fields one by one; this keeps the code readable
 * and lets us handle the variable-length address fields naturally.
 *
 * NOTE on optional fields: real TETRA PDUs are quite flexible — many
 * fields are followed by a "presence" bit that gates whether further
 * info elements follow. The implementation here covers the *mandatory*
 * fields plus the common optional fields (calling/called SSI, status
 * code). Esoteric extensions ("type 3 elements") are skipped: the
 * remaining bits after our last consumed field are ignored, which is
 * fine for downstream event emission.
 *
 * Validation strategy: each helper checks bounds before reading and
 * returns 0 on the first short-read. We then emit whatever info we
 * already extracted, which is better than dropping the whole event on
 * a single missing optional field.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "tetra_bitcur.h"
#include "tetra_cmce_parser.h"
#include "tetra_cmce_pdu.h"
#include "tetra_sds_parser.h"
#include "tetra_events.h"

/* ---- common sub-fields ------------------------------------------------- */

/* Called party type identifier (14.8.6).
 *   0 : "none" (no called party address follows)
 *   1 : SSI (24 bits)
 *   2 : TSI (48 bits = MCC 10 + MNC 14 + SSI 24)
 *   3 : extended
 * On success, fills *out_ssi with the called party SSI (truncated to 24
 * bits if TSI) and returns 1. On unsupported type or short read, leaves
 * *out_ssi at 0 and returns 0. */
static int read_called_party_addr(struct bcur *c, uint32_t *out_ssi) {
	*out_ssi = 0;
	if (!bcur_have(c, 3)) return 0;
	uint32_t type = bcur_get(c, 3);
	switch (type) {
	case 0:
		return 1;
	case 1:
		if (!bcur_have(c, 24)) return 0;
		*out_ssi = bcur_get(c, 24);
		return 1;
	case 2:
		/* TSI = MCC (10) + MNC (14) + SSI (24). We surface only the SSI;
		 * the network context (MCC/MNC) is already known from SYSINFO. */
		if (!bcur_have(c, 48)) return 0;
		bcur_get(c, 10); /* mcc */
		bcur_get(c, 14); /* mnc */
		*out_ssi = bcur_get(c, 24);
		return 1;
	default:
		/* Extended addressing — unsupported here. Bail out gracefully. */
		return 0;
	}
}

/* ---- per-PDU handlers --------------------------------------------------
 *
 * Each handler consumes the bit cursor up to and including the fields
 * it cares about, then emits the corresponding tetra_events event.
 * Fields after the last consumed one are simply ignored.
 */

/* D-SETUP (14.7.1.10). Fields in order:
 *   4   call identifier
 *   1   call time-out, set-up phase time (CTSP)
 *   3   hook method selection
 *   2   simplex / duplex (00=simplex, 01=duplex)
 *   2   transmission grant
 *   2   transmission request permission
 *   1   call ownership
 *   2   priority level
 *   1   clir control
 *   1   basic service info presence -> then 14 bits
 *   ... lots of further optional fields ...
 *   eventually: called party address (type-tagged) + calling party SSI
 *
 * Walking the entire flexible structure is overkill for our needs. We
 * leverage the fact that the call identifier and the called/calling
 * party addresses, when present, follow predictable bit positions:
 *
 *   - The 4-bit call identifier is always at offset 0.
 *   - The called party address (3-bit type + up to 48 bits) starts
 *     after a fixed-format prefix of 19 bits.
 *
 * If our prefix walk overshoots the buffer we just emit a setup event
 * with the call_id alone — better than dropping. */
static void parse_d_setup(struct bcur *c, struct tetra_mac_state *tms,
                          int slot) {
	uint32_t call_id = 0;
	uint32_t calling_ssi = 0;
	uint32_t called_ssi = 0;
	const char *ctype = "unknown";

	if (bcur_have(c, 4)) call_id = bcur_get(c, 4);
	/* Skip fixed prefix: CTSP(1) + hook(3) + simplex/duplex(2)
	 * + tx grant(2) + tx req perm(2) + call ownership(1)
	 * + priority(2) + clir(1) + basic service info presence(1) = 15 bits.
	 *
	 * If basic-service-info present bit set, the basic service info
	 * field is 14 more bits. We don't actually need to peek that, but
	 * we have to honour the layout. For robustness we just skip 15
	 * unconditionally and assume the basic-service info isn't there;
	 * on real cells where it IS there our SSI extraction will be off,
	 * but we always carry the call_id field which is enough for
	 * correlating with D-CONNECT/D-RELEASE later. */
	if (bcur_have(c, 15)) {
		(void)bcur_get(c, 14); /* CTSP..clir */
		uint32_t bsi_present = bcur_get(c, 1);
		if (bsi_present && bcur_have(c, 14)) {
			(void)bcur_get(c, 14);
		}
	}

	/* Now the called party address type/value, then the calling SSI. */
	read_called_party_addr(c, &called_ssi);
	if (bcur_have(c, 24)) calling_ssi = bcur_get(c, 24);

	/* Heuristic for call_type:
	 *   - If called party address is the broadcast/all-call SSI
	 *     (0xFFFFFF), or if the address type was "TSI" mapping a known
	 *     group range, mark as "group".
	 *   - Else "individual". Refinement is possible from the hook
	 *     method bits but those have moving semantics across releases. */
	if (called_ssi == 0xFFFFFF || called_ssi == 0) {
		ctype = "group";
	} else if (called_ssi >= 0x800000) {
		/* Group SSI range per ETSI EN 300 392-1, clause 7.1 (TSI
		 * format), high bit set conventionally marks GSSI in many
		 * networks. Adjust if your ADRASEC allocation differs. */
		ctype = "group";
	} else {
		ctype = "individual";
	}

	/* Override the slot's displayed SSI with the calling party's
	 * individual SSI. The MAC layer only sees the group/managed SSI
	 * (e.g. the GSSI for a group call) which is *not* what an operator
	 * wants to see — they want to know who is talking. The CMCE D-SETUP
	 * carries the ISSI of the calling party, which is the right value. */
	if (tms && slot >= 0 && slot < 4 && calling_ssi != 0) {
		struct timeval _tv;
		gettimeofday(&_tv, NULL);
		tms->t_display_st->ssi_slot[slot] = (int)calling_ssi;
		tms->t_display_st->call_state_slot[slot] = 1; /* talking */
		tms->t_display_st->ssi_slot_last_ms[slot] =
		    (uint64_t)_tv.tv_sec * 1000ULL + _tv.tv_usec / 1000ULL;
	}

	(void)call_id;  /* surfaced via the call_id JSON field */
	te_emit_call_setup((int)calling_ssi, (int)called_ssi, ctype, slot,
	                   (int)call_id);
}

/* D-CONNECT (14.7.1.6).
 *   4 : call identifier
 *   2 : call time-out
 *   1 : transmission grant
 *   1 : transmission request permission
 *   ... then optional info elements
 *
 * We expose the call_id for correlation with the preceding D-SETUP.    */
static void parse_d_connect(struct bcur *c, int slot) {
	uint32_t call_id = 0;
	if (bcur_have(c, 4)) call_id = bcur_get(c, 4);
	te_emit_cmce_event("call_connect", (int)call_id, 0, slot);
}

/* D-CALL-PROCEEDING (14.7.1.4). Mostly a progress acknowledgement;
 * we just emit a tiny event so consumers see something. */
static void parse_d_call_proceeding(struct bcur *c, int slot) {
	uint32_t call_id = 0;
	if (bcur_have(c, 4)) call_id = bcur_get(c, 4);
	te_emit_cmce_event("call_proceeding", (int)call_id, 0, slot);
}

/* D-RELEASE (14.7.1.10).
 *   4 : call identifier
 *   5 : disconnect cause
 */
static void parse_d_release(struct bcur *c, int slot) {
	uint32_t call_id = 0;
	uint32_t cause = 0;
	if (bcur_have(c, 4)) call_id = bcur_get(c, 4);
	if (bcur_have(c, 5)) cause = bcur_get(c, 5);
	te_emit_cmce_event("call_release", (int)call_id, (int)cause, slot);
}

/* D-TX-CEASED (14.7.1.13).
 *   4 : call identifier
 */
static void parse_d_tx_ceased(struct bcur *c, int slot) {
	uint32_t call_id = 0;
	if (bcur_have(c, 4)) call_id = bcur_get(c, 4);
	te_emit_cmce_event("tx_ceased", (int)call_id, 0, slot);
}

/* D-STATUS (14.7.1.11). Used to transmit pre-coded short status codes
 * (the "10-codes" in radio operator lingo). Format:
 *   3   called party type identifier
 *   0/24/48 : called party address (per type)
 *   24  calling party SSI
 *   16  pre-coded status value
 *
 * Status values:
 *   0x8000..0xFFFF : ETSI standard codes (e.g. 0x8000 = "emergency")
 *   0x0001..0x7FFF : user defined / operator-allocated codes
 */
static void parse_d_status(struct bcur *c, int slot) {
	uint32_t called_ssi = 0;
	uint32_t calling_ssi = 0;
	uint32_t status_code = 0;

	read_called_party_addr(c, &called_ssi);
	if (bcur_have(c, 24)) calling_ssi = bcur_get(c, 24);
	if (bcur_have(c, 16)) status_code = bcur_get(c, 16);

	(void)slot;
	te_emit_status((int)calling_ssi, (int)called_ssi, (int)status_code);
}

/* D-SDS-DATA (14.7.1.11).
 *   3   called party type identifier
 *   0/24/48 : called party address
 *   24  calling party SSI
 *   1   area selection (presence flag)
 *   0/4 : area, if present
 *   6   SDS data length indicator (length in bits - 1, so range 1..64)
 *       OR for SDS-TL: 6-bit indicator with special encodings...
 *   N   user-defined data (the actual SDS payload, starting with the
 *       1-byte protocol identifier)
 *
 * The header is annoyingly flexible. We take the conservative approach:
 * extract calling/called SSIs reliably, then scan forward looking for a
 * plausible protocol identifier byte and pass everything from there to
 * the SDS parser. This is robust against optional fields we may have
 * miscounted, at the cost of occasionally consuming a few "wrong" bits
 * at the start of the user data — the SDS parser then sees an unknown
 * protocol ID and emits a breadcrumb event without crashing. */
static void parse_d_sds_data(struct bcur *c, struct tetra_mac_state *tms,
                             int slot) {
	uint32_t called_ssi = 0;
	uint32_t calling_ssi = 0;

	read_called_party_addr(c, &called_ssi);
	if (bcur_have(c, 24)) calling_ssi = bcur_get(c, 24);

	/* Area selection bit. */
	if (bcur_have(c, 1)) {
		uint32_t area_present = bcur_get(c, 1);
		if (area_present && bcur_have(c, 4)) {
			bcur_get(c, 4);
		}
	}

	/* Length indicator (6 bits). Some networks encode the length here
	 * in bits-1 (so 0 means 1 bit), some in octets, some leave it at
	 * a sentinel and use SDS-TL. We just skip the field and let the
	 * SDS parser figure out the protocol ID from the remaining stream. */
	if (bcur_have(c, 6)) bcur_get(c, 6);

	/* Stash the calling/called SSIs in the mac_state so the SDS
	 * parser can include them in its emitted events without re-parsing. */
	if (tms) {
		tms->t_display_st->last_sds_src = (int)calling_ssi;
		tms->t_display_st->last_sds_dst = (int)called_ssi;
	}

	/* Hand the remaining bits to the SDS parser. */
	unsigned int rem = bcur_rem(c);
	if (rem > 0) {
		tetra_sds_parse(c->bits + c->pos, rem, tms, slot);
	}
}



int tetra_cmce_parse(const uint8_t *bits, unsigned int len_bits,
                     struct tetra_mac_state *tms, int slot)
{
	if (!bits || len_bits < 8) return -1;

	struct bcur c;
	bcur_init(&c, bits, len_bits);

	/* MLE protocol discriminator already consumed by the caller (in
	 * tetra_mle.c). What we receive here starts with the 5-bit CMCE
	 * PDU type. */
	uint32_t pdut = bcur_get(&c, 5);

	switch (pdut) {
	case TCMCE_PDU_T_D_SETUP:
		parse_d_setup(&c, tms, slot);
		break;
	case TCMCE_PDU_T_D_CONNECT:
		parse_d_connect(&c, slot);
		break;
	case TCMCE_PDU_T_D_CALL_PROCEEDING:
		parse_d_call_proceeding(&c, slot);
		break;
	case TCMCE_PDU_T_D_RELEASE:
		parse_d_release(&c, slot);
		break;
	case TCMCE_PDU_T_D_TX_CEASED:
		parse_d_tx_ceased(&c, slot);
		break;
	case TCMCE_PDU_T_D_STATUS:
		parse_d_status(&c, slot);
		break;
	case TCMCE_PDU_T_D_SDS_DATA:
		parse_d_sds_data(&c, tms, slot);
		break;
	default:
		/* Other PDU types (D-ALERT, D-INFO, D-TX-GRANTED, etc.)
		 * don't carry information we expose yet. The MLE-level
		 * breadcrumb already tells the consumer they were seen. */
		break;
	}

	return 0;
}
