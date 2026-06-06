#ifndef TETRA_EVENTS_H
#define TETRA_EVENTS_H

/* TETRA event emitter
 *
 * The decoder calls te_emit_*() functions when it observes interesting
 * upper-layer events (call setups, SDS messages, LIP reports, status
 * codes...). Those are formatted as a single line of JSON and routed to
 * an emitter callback registered by the C++ host layer.
 *
 * Decoupling lets us avoid pulling SDR++/Qt/networking headers into the
 * pure-C decoder source files: the host registers ONE function pointer
 * and gets a stream of JSON-Lines ready to send over a socket.
 *
 * Threading: te_emit_*() may be called from the demod thread. The
 * registered callback must be thread-safe (the C++ side uses a mutex
 * around its outbound queue).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JSON-Lines callback. `line` is null-terminated, does NOT include the
 * trailing newline (the network layer appends it). `len` is strlen(line). */
typedef void (*te_emit_cb)(const char *line, int len, void *ctx);

/* Register / clear the global emitter. Passing NULL disables emission. */
void te_set_emitter(te_emit_cb cb, void *ctx);

/* Structured emitter — alternative to the JSON-Lines callback. When set,
 * the events are delivered as native C data to the host, which can then
 * decide independently for each event type whether to:
 *   - push it onto an in-memory ring buffer (UI display),
 *   - send it as JSON over TCP (network export),
 *   - or both.
 *
 * Both emitters can be active simultaneously — the JSON-Lines callback
 * is invoked unconditionally for ALL events, the structured callbacks
 * only for events that match. This lets the host opt in to whichever
 * dispatch model fits each use case.                                  */
struct te_structured_cb {
    /* Each callback may be NULL; the dispatcher silently skips it. */
    void (*on_call_setup)(int caller_ssi, int callee_ssi,
                          const char *call_type, int slot, int call_id,
                          void *ctx);
    void (*on_cmce_event)(const char *event_name,
                          int call_id, int cause, int slot, void *ctx);
    void (*on_sds_text)(int src_ssi, int dst_ssi,
                        int protocol_id, const char *text, void *ctx);
    void (*on_status)(int src_ssi, int dst_ssi, int code, void *ctx);
    void (*on_lip)(int src_ssi, double lat, double lon,
                   int accuracy_m, int velocity_kph, int direction_deg,
                   void *ctx);
    void *ctx;
};

void te_set_structured_emitter(const struct te_structured_cb *cb);

/* Debug-level breadcrumbs - useful in session 1 to verify the pipe works
 * before higher-level parsers are implemented. */
void te_emit_mle_pdu(uint8_t pdisc, uint8_t pdut, int ssi, int slot);

/* Future session 2-4 emitters (declared now so call sites don't need to
 * change later). All `ssi` fields are 24-bit SSI; 0 means unknown.    */
void te_emit_call_setup(int caller_ssi, int callee_ssi,
                        const char *call_type, int slot, int call_id);

/* Generic CMCE event emitter — used for D-CONNECT, D-RELEASE,
 * D-CALL-PROCEEDING, D-TX-CEASED... `event_name` becomes the JSON
 * "type" field directly (snake_case, e.g. "call_connect",
 * "call_release"). `cause` is interpreted by the consumer based on the
 * event (release reason for D-RELEASE, 0 otherwise). */
void te_emit_cmce_event(const char *event_name,
                        int call_id, int cause, int slot);

void te_emit_sds_text(int src_ssi, int dst_ssi,
                      int protocol_id, const char *text);
void te_emit_status(int src_ssi, int dst_ssi, int status_code);
void te_emit_lip(int src_ssi, double lat, double lon,
                 int accuracy_m, int velocity_kph, int direction_deg);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_EVENTS_H */
