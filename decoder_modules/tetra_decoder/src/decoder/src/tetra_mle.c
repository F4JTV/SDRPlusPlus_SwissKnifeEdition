/* TETRA MLE layer dispatcher.
 *
 * The MLE (Mobile Link Entity) PDU is the top-level wrapping of upper
 * layer payloads on the air interface. It starts with a 3-bit protocol
 * discriminator that selects between MM / CMCE / SNDCP / MLE-itself.
 *
 * In Session 2 we additionally dispatch CMCE traffic to the CMCE parser
 * for call setup / connect / release / status / tx-ceased extraction.
 * MM/SNDCP/MGMT remain breadcrumb-only for now (sessions 3-5 will add
 * MM-side SDS routing).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tetra_mle_pdu.h"
#include "tetra_mle.h"
#include "tetra_mm_pdu.h"
#include "tetra_cmce_pdu.h"
#include "tetra_cmce_parser.h"
#include "tetra_sndcp_pdu.h"
#include "tetra_events.h"

/* Receive TL-SDU (LLC SDU == MLE PDU) */
int rx_tl_sdu(struct tetra_mac_state *tms, struct msgb *msg, unsigned int len)
{
	if (!msg || !msg->l3h || len < 3)
		return -1;

	uint8_t *bits = msg->l3h;
	uint8_t mle_pdisc = bits_to_uint(bits, 3);

	/* Inner PDU type depends on the protocol discriminator. The bit
	 * widths come from ETSI EN 300 392-2 clauses 14 (CMCE), 16 (MM),
	 * 18 (SNDCP), 18 (MLE itself). */
	uint8_t pdut = 0;
	switch (mle_pdisc) {
	case TMLE_PDISC_MM:
		if (len >= 7) pdut = bits_to_uint(bits + 3, 4);
		break;
	case TMLE_PDISC_CMCE:
		if (len >= 8) pdut = bits_to_uint(bits + 3, 5);
		break;
	case TMLE_PDISC_SNDCP:
		if (len >= 7) pdut = bits_to_uint(bits + 3, 4);
		break;
	case TMLE_PDISC_MLE:
		if (len >= 6) pdut = bits_to_uint(bits + 3, 3);
		break;
	default:
		break;
	}

	int slot = tms ? (int)tms->curr_active_timeslot : 0;
	int ssi  = tms ? (int)tms->ssi : 0;

	/* Emit a debug-level breadcrumb on every MLE PDU. This is useful
	 * to see traffic volume even when no higher-layer parser fires. */
	te_emit_mle_pdu(mle_pdisc, pdut, ssi, slot);

	/* Dispatch to higher-layer parsers. */
	switch (mle_pdisc) {
	case TMLE_PDISC_CMCE:
		/* Skip the 3 MLE pdisc bits and pass the rest to the CMCE
		 * parser, which will consume the 5-bit CMCE PDU type itself. */
		if (len > 3) {
			tetra_cmce_parse(bits + 3, len - 3, tms, slot);
		}
		break;
	default:
		break;
	}

	return len;
}
