/* TETRA event emitter - implementation
 *
 * Format-and-forward layer between the C decoder and the C++ host. All
 * events are emitted as one JSON object per line ("JSON Lines"), which
 * the host writes to a TCP socket connected to an external consumer
 * (typically a Django/Channels service).
 *
 * Timestamps are UTC ISO-8601 with millisecond precision. We rely on
 * gettimeofday() for portability; it is sufficient given that the rest
 * of the TETRA timing is anchored to the receiver clock anyway and the
 * consumer does its own clock skew handling.
 */

/* _POSIX_C_SOURCE >= 1 is needed for gmtime_r() prototype on glibc with
 * the default -std=c11 (which defines __STRICT_ANSI__). Define it BEFORE
 * including any system header. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "tetra_events.h"

/* Single global emitter slot. The host registers / unregisters via
 * te_set_emitter(). Reads/writes are atomic on x86_64 for word-sized
 * pointers but we leave proper synchronization to the host: in practice
 * the emitter is set once at module enable, cleared once at disable. */
static te_emit_cb g_emitter_cb  = NULL;
static void      *g_emitter_ctx = NULL;

void te_set_emitter(te_emit_cb cb, void *ctx) {
    g_emitter_cb  = cb;
    g_emitter_ctx = ctx;
}

/* Structured emitter slot. The structure is copied in, so the host can
 * declare it on the stack at registration time and let it go out of scope
 * afterwards. */
static struct te_structured_cb g_structured;
static int g_structured_set = 0;

void te_set_structured_emitter(const struct te_structured_cb *cb) {
    if (cb) {
        g_structured = *cb;
        g_structured_set = 1;
    } else {
        g_structured_set = 0;
    }
}

/* ---- helpers ----------------------------------------------------------- */

static void fmt_iso8601_utc(char *out, int outsz) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_;
    gmtime_r(&tv.tv_sec, &tm_);
    /* Clamp ms explicitly to [0..999] so the compiler can prove "%03d"
     * fits in 3 chars and -Wformat-truncation stops complaining. */
    int ms = (int)(tv.tv_usec / 1000);
    if (ms < 0) ms = 0;
    if (ms > 999) ms = 999;
    /* Clamp year/month/day fields similarly. We don't really care about
     * the year being defensive here, but it shuts up GCC's range
     * tracker which would otherwise assume any int.                  */
    int yr = tm_.tm_year + 1900; if (yr < 0) yr = 0; if (yr > 9999) yr = 9999;
    int mo = tm_.tm_mon + 1;     if (mo < 1) mo = 1; if (mo > 12)   mo = 12;
    int day = tm_.tm_mday;       if (day < 1) day = 1; if (day > 31) day = 31;
    int hr = tm_.tm_hour;        if (hr < 0) hr = 0; if (hr > 23)  hr = 23;
    int mn = tm_.tm_min;         if (mn < 0) mn = 0; if (mn > 59)  mn = 59;
    int sc = tm_.tm_sec;         if (sc < 0) sc = 0; if (sc > 60)  sc = 60;
    snprintf(out, outsz, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             yr, mo, day, hr, mn, sc, ms);
}

/* Escape a UTF-8 string for inclusion in a JSON string literal.
 * Output buffer must be large enough; on overflow the output is
 * truncated but always null-terminated. */
static void json_escape(char *out, int outsz, const char *in) {
    int o = 0;
    if (outsz <= 0) return;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default: break;
        }
        if (esc) {
            int n = (int)strlen(esc);
            if (o + n >= outsz - 1) break;
            memcpy(out + o, esc, n);
            o += n;
        } else if (c < 0x20) {
            /* control char -> \u00XX */
            if (o + 6 >= outsz - 1) break;
            o += snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            if (o + 1 >= outsz - 1) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void deliver(const char *line) {
    if (!g_emitter_cb) return;
    g_emitter_cb(line, (int)strlen(line), g_emitter_ctx);
}

/* ---- emitters ----------------------------------------------------------
 *
 * Each emitter does two independent things:
 *   1. If a JSON-Lines callback is registered, format the event as a JSON
 *      string and pass it along (used by network exporters).
 *   2. If a structured callback for that event type is registered, invoke
 *      it with native C values (used by in-process UI consumers).
 *
 * Either, both, or neither can be active. The helper `any_emitter()`
 * lets us bail out early if nobody is listening.
 */

static int any_emitter(void) {
    return g_emitter_cb != NULL || g_structured_set;
}

void te_emit_mle_pdu(uint8_t pdisc, uint8_t pdut, int ssi, int slot) {
    if (!any_emitter()) return;
    if (g_emitter_cb) {
        char ts[32];
        fmt_iso8601_utc(ts, sizeof(ts));
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"ts\":\"%s\",\"type\":\"mle_pdu\",\"pdisc\":%u,\"pdut\":%u,\"src\":%d,\"slot\":%d}",
            ts, (unsigned)pdisc, (unsigned)pdut, ssi, slot);
        deliver(buf);
    }
    /* MLE breadcrumb has no dedicated structured callback — it's
     * intentionally JSON-only since the UI doesn't show raw MLE PDUs. */
}

void te_emit_call_setup(int caller_ssi, int callee_ssi,
                        const char *call_type, int slot, int call_id) {
    if (!any_emitter()) return;
    if (!call_type) call_type = "unknown";
    if (g_emitter_cb) {
        char ts[32];
        fmt_iso8601_utc(ts, sizeof(ts));
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"ts\":\"%s\",\"type\":\"call_setup\","
            "\"caller\":%d,\"callee\":%d,\"call_type\":\"%s\","
            "\"slot\":%d,\"call_id\":%d}",
            ts, caller_ssi, callee_ssi, call_type, slot, call_id);
        deliver(buf);
    }
    if (g_structured_set && g_structured.on_call_setup) {
        g_structured.on_call_setup(caller_ssi, callee_ssi, call_type,
                                    slot, call_id, g_structured.ctx);
    }
}

void te_emit_cmce_event(const char *event_name,
                        int call_id, int cause, int slot) {
    if (!any_emitter()) return;
    if (!event_name) event_name = "cmce_event";
    if (g_emitter_cb) {
        char ts[32];
        fmt_iso8601_utc(ts, sizeof(ts));
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"ts\":\"%s\",\"type\":\"%s\","
            "\"call_id\":%d,\"cause\":%d,\"slot\":%d}",
            ts, event_name, call_id, cause, slot);
        deliver(buf);
    }
    if (g_structured_set && g_structured.on_cmce_event) {
        g_structured.on_cmce_event(event_name, call_id, cause, slot,
                                    g_structured.ctx);
    }
}

void te_emit_sds_text(int src_ssi, int dst_ssi,
                      int protocol_id, const char *text) {
    if (!any_emitter()) return;
    if (!text) text = "";
    if (g_emitter_cb) {
        char ts[32];
        fmt_iso8601_utc(ts, sizeof(ts));
        char esc[1024];
        json_escape(esc, sizeof(esc), text);
        char buf[1400];
        snprintf(buf, sizeof(buf),
            "{\"ts\":\"%s\",\"type\":\"sds_text\","
            "\"src\":%d,\"dst\":%d,\"proto\":%d,\"text\":\"%s\"}",
            ts, src_ssi, dst_ssi, protocol_id, esc);
        deliver(buf);
    }
    if (g_structured_set && g_structured.on_sds_text) {
        g_structured.on_sds_text(src_ssi, dst_ssi, protocol_id, text,
                                  g_structured.ctx);
    }
}

void te_emit_status(int src_ssi, int dst_ssi, int status_code) {
    if (!any_emitter()) return;
    if (g_emitter_cb) {
        char ts[32];
        fmt_iso8601_utc(ts, sizeof(ts));
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"ts\":\"%s\",\"type\":\"status\","
            "\"src\":%d,\"dst\":%d,\"code\":%d}",
            ts, src_ssi, dst_ssi, status_code);
        deliver(buf);
    }
    if (g_structured_set && g_structured.on_status) {
        g_structured.on_status(src_ssi, dst_ssi, status_code,
                                g_structured.ctx);
    }
}

void te_emit_lip(int src_ssi, double lat, double lon,
                 int accuracy_m, int velocity_kph, int direction_deg) {
    if (!any_emitter()) return;
    if (g_emitter_cb) {
        char ts[32];
        fmt_iso8601_utc(ts, sizeof(ts));
        char buf[384];
        snprintf(buf, sizeof(buf),
            "{\"ts\":\"%s\",\"type\":\"lip\","
            "\"src\":%d,\"lat\":%.7f,\"lon\":%.7f,"
            "\"accuracy_m\":%d,\"velocity_kph\":%d,\"direction_deg\":%d}",
            ts, src_ssi, lat, lon, accuracy_m, velocity_kph, direction_deg);
        deliver(buf);
    }
    if (g_structured_set && g_structured.on_lip) {
        g_structured.on_lip(src_ssi, lat, lon, accuracy_m,
                             velocity_kph, direction_deg,
                             g_structured.ctx);
    }
}
