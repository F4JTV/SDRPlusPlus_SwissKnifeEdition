#ifndef TETRA_BITCUR_H
#define TETRA_BITCUR_H

/* Tiny shared bit cursor for walking TETRA PDU bit-streams.
 *
 * TETRA PDUs are unpacked into byte-per-bit arrays by the lower MAC
 * (each byte = 1 bit, MSB first as it arrived on the air). This header
 * provides a non-destructive cursor that walks such a buffer, returning
 * 0 cleanly when it runs out of bits — so individual field parsers can
 * be defensive without scattering bounds checks everywhere.
 *
 * Header-only on purpose: every parser includes this directly and gets
 * an inline cursor with no link-time dependency. */

#include <stdint.h>

struct bcur {
	const uint8_t *bits;
	unsigned int   pos;   /* next bit to read */
	unsigned int   len;   /* total bits in the buffer */
};

static inline void bcur_init(struct bcur *c,
                             const uint8_t *bits, unsigned int len) {
	c->bits = bits;
	c->pos  = 0;
	c->len  = len;
}

/* Returns 1 if at least `n` more bits are available. */
static inline int bcur_have(const struct bcur *c, unsigned int n) {
	return (c->pos + n) <= c->len;
}

/* Reads up to 32 bits MSB-first and advances the cursor.
 * Caller is responsible for having checked bcur_have() first. */
static inline uint32_t bcur_get(struct bcur *c, unsigned int n) {
	uint32_t v = 0;
	for (unsigned int i = 0; i < n; i++) {
		v = (v << 1) | (c->bits[c->pos + i] & 1);
	}
	c->pos += n;
	return v;
}

/* Skip `n` bits unconditionally (clamp at end of buffer). */
static inline void bcur_skip(struct bcur *c, unsigned int n) {
	c->pos += n;
	if (c->pos > c->len) c->pos = c->len;
}

/* Remaining bits in the cursor. */
static inline unsigned int bcur_rem(const struct bcur *c) {
	return (c->pos >= c->len) ? 0 : (c->len - c->pos);
}

#endif /* TETRA_BITCUR_H */
