/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "cp_session.h"
#include "cp_commands.h"
#include "../json/jsmn.h"
#include "../json/json_builder.h"
#include "../rtl/rtl_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <inttypes.h>

/* =========================================================================
 * Global session table
 * ========================================================================= */
static Session        s_sessions[MAX_SESSIONS];
static pthread_mutex_t s_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/* §26 configuration, populated by main.c.                               */
bool           g_rx_auth = false;
auth_keyring_t g_keys;

/* Monotonic stream ID counter (global, never resets) */
static uint32_t       s_stream_counter = 0;
static pthread_mutex_t s_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

/* §17: holder of the exclusive rx channel, or NULL.  Protected by
 * s_table_mutex.                                                        */
static Session       *s_rx_owner = NULL;

/* =========================================================================
 * Stream ID allocation
 * ========================================================================= */
void alloc_stream_id(char *out, size_t cap, uint32_t *vita_id_out) {
    pthread_mutex_lock(&s_counter_mutex);
    uint32_t n = ++s_stream_counter;
    pthread_mutex_unlock(&s_counter_mutex);
    snprintf(out, cap, "rx-%u", n);
    if (vita_id_out) *vita_id_out = n;
}

/* =========================================================================
 * Framing
 * ========================================================================= */
int recv_exact(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

int send_framed(Session *s, const char *json) {
    if (!s || s->fd <= 0) return -1;
    uint32_t len     = (uint32_t)strlen(json);
    uint32_t len_net = htonl(len);
    int      fd      = s->fd;
    int      rc      = 0;

    pthread_mutex_lock(&s->send_mutex);
    if (send(fd, &len_net, 4, MSG_MORE) != 4) {
        rc = -1;
    } else {
        ssize_t sent = 0;
        while ((size_t)sent < len) {
            ssize_t r = send(fd, json + sent, len - (size_t)sent, 0);
            if (r <= 0) { rc = -1; break; }
            sent += r;
        }
    }
    pthread_mutex_unlock(&s->send_mutex);
    return rc;
}

/* =========================================================================
 * Access control [§17]
 * ========================================================================= */
bool cp_rx_claim(Session *s) {
    bool ok;
    pthread_mutex_lock(&s_table_mutex);
    if (s_rx_owner == NULL || s_rx_owner == s) {
        s_rx_owner = s;
        s->owns_rx = true;
        ok = true;
    } else {
        ok = false;
    }
    pthread_mutex_unlock(&s_table_mutex);
    return ok;
}

void cp_rx_release(Session *s) {
    bool released = false;
    pthread_mutex_lock(&s_table_mutex);
    if (s_rx_owner == s) { s_rx_owner = NULL; released = true; }
    s->owns_rx = false;
    pthread_mutex_unlock(&s_table_mutex);

    /* §21.2 control_revoked: an exclusive resource was taken or released.
     * Everyone waiting for the receiver wants to know it is free.       */
    if (released)
        cp_broadcast_event_except(s, "control_revoked",
            "{\"function\":\"rx\",\"control_state\":\"free\"}");
}

bool cp_rx_owned_by_other(const Session *s) {
    pthread_mutex_lock(&s_table_mutex);
    bool other = (s_rx_owner != NULL && s_rx_owner != s);
    pthread_mutex_unlock(&s_table_mutex);
    return other;
}

const char *cp_rx_control_state(const Session *s) {
    pthread_mutex_lock(&s_table_mutex);
    const char *st = (s_rx_owner == NULL)  ? "free"
                   : (s_rx_owner == s)     ? "owned_by_me"
                                           : "owned_by_other";
    pthread_mutex_unlock(&s_table_mutex);
    return st;
}

/* =========================================================================
 * Event broadcast [§21]
 *
 * The table mutex is held across the sends.  With MAX_SESSIONS at 4 and
 * event payloads of a couple of hundred bytes this is a handful of
 * microseconds into a socket buffer; it is not a path worth making
 * lock-free, and holding the mutex is what guarantees a session cannot be
 * torn down underneath a send.
 * ========================================================================= */
static void broadcast(const Session *skip,
                      const char *event, const char *params_json) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"event\",\"event\":\"%s\",\"params\":%s}",
             event, params_json ? params_json : "{}");

    pthread_mutex_lock(&s_table_mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *t = &s_sessions[i];
        if (t->fd <= 0) continue;
        if (t == skip) continue;
        if (t->state == SESSION_AWAIT_HELLO) continue;
        send_framed(t, buf);
    }
    pthread_mutex_unlock(&s_table_mutex);
}

void cp_broadcast_event(const char *event, const char *params_json) {
    broadcast(NULL, event, params_json);
}

void cp_broadcast_event_except(const Session *skip,
                               const char *event, const char *params_json) {
    broadcast(skip, event, params_json);
}

/* §21.3: emitted at the onset of clipping, not continuously.  Called from
 * the USB callback thread, already rate-limited by rtl_bridge.          */
void cp_on_adc_overload(double adc_level_dbfs) {
    /* §21.3 keys the event by stream_id; §21.1 wants function too.  The
     * stream is whichever session is streaming - there is one tuner, so
     * at most one.                                                      */
    char sid[32] = "";
    pthread_mutex_lock(&s_table_mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].fd > 0 && s_sessions[i].state == SESSION_STREAMING) {
            snprintf(sid, sizeof(sid), "%s", s_sessions[i].stream_id);
            break;
        }
    }
    pthread_mutex_unlock(&s_table_mutex);

    char params[224];
    snprintf(params, sizeof(params),
             "{\"stream_id\":\"%s\",\"function\":\"rx\","
             "\"adc_level_dbfs\":%.1f,"
             "\"suggested_action\":\"reduce_rf_gain\"}",
             sid, adc_level_dbfs);
    cp_broadcast_event("adc_overload", params);
    LOG_WARN("cp", "ADC overload — level %.1f dBFS", adc_level_dbfs);
}

/* =========================================================================
 * Command dispatch table
 * ========================================================================= */
typedef void (*cmd_fn)(Session *, const char *, jsmntok_t *, int, const char *);

typedef struct {
    const char *name;
    cmd_fn      fn;
} CmdEntry;

/* Envelope + block rx Tier 2.  Nothing else is listed, and §24.1 is
 * explicit that an unimplemented command answers unknown_command — so
 * the tx and codec commands are absent rather than stubbed.  A command
 * that names a function this device does not have (function:"tx" on
 * set_frequency, say) is a different case and answers not_supported;
 * that check lives in require_function() in cp_commands.c.             */
static const CmdEntry s_dispatch[] = {
    /* Envelope */
    { "hello",               cmd_hello            },
    { "authenticate",        cmd_authenticate     },
    { "get_capabilities",    cmd_get_capabilities },
    /* Block rx, Tier 1 */
    { "set_frequency",       cmd_set_frequency    },
    { "set_sample_rate",     cmd_set_sample_rate  },
    { "set_control",         cmd_set_control      },
    { "open_stream",         cmd_open_stream      },
    { "start_rx",            cmd_start_rx         },
    { "stop_rx",             cmd_stop_rx          },
    { "close_stream",        cmd_close_stream     },
    /* Block rx, Tier 2 */
    { "ping",                cmd_ping             },
    { "get_status",          cmd_get_status       },
    { "get_control",         cmd_get_control      },
    { "get_clock_status",    cmd_get_clock_status },
    { NULL, NULL }
};

/* =========================================================================
 * Session lifecycle
 * ========================================================================= */
Session *session_create(int fd, struct in_addr client_ip) {
    pthread_mutex_lock(&s_table_mutex);
    Session *s = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].fd == 0) {
            s = &s_sessions[i];
            memset(s, 0, sizeof(*s));
            s->fd            = fd;
            s->client_ip     = client_ip;
            s->state         = SESSION_AWAIT_HELLO;
            s->owns_rx       = false;
            s->vita_token    = -1;
            pthread_mutex_init(&s->send_mutex, NULL);
            s->frequency_hz  = RTL_DEFAULT_FREQ;
            s->sample_rate_sps = RTL_DEFAULT_RATE_NORMAL;
            s->rf_gain_db    = g_gain_tenth_db / 10.0;
            break;
        }
    }
    pthread_mutex_unlock(&s_table_mutex);
    return s;
}

void session_destroy(Session *s) {
    if (!s) return;

    /* §14: on TCP disconnect the device stops all active streams and
     * releases all open stream sessions.                                */
    if (s->vita_token >= 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
    }
    if (s->state == SESSION_STREAMING) {
        rtl_bridge_stop();
        rtl_reset_adc_level();
        /* §21.2: the stream stopped and it was not a command that did it.
         * The owner is gone, so this is for the observers (§17.4).     */
        char ev[192];
        snprintf(ev, sizeof(ev),
                 "{\"stream_id\":\"%s\",\"streaming\":false,"
                 "\"function\":\"rx\",\"reason\":\"control_lost\"}",
                 s->stream_id);
        cp_broadcast_event_except(s, "stream_state", ev);
    }

    /* §17.2: control is released implicitly on loss of the control
     * connection.  Do this before the slot is freed so the broadcast can
     * still identify this session as the one to skip.                   */
    cp_rx_release(s);

    /* §13.3: bias_tee falls back to 0.0 V after any TCP disconnect.
     * --bias-tee-persist restores the operator's startup value instead,
     * for installations where a masthead LNA must stay powered.  That is
     * a deliberate deviation and is logged as one at startup.           */
    if (g_bias_tee_enabled && !g_bias_tee_persist) {
        rtl_set_bias_tee(0.0);
        LOG_INFO("cp", "bias tee off — client disconnected (§13.3)");
    }

    pthread_mutex_lock(&s_table_mutex);
    int fd = s->fd;
    pthread_mutex_destroy(&s->send_mutex);
    memset(s, 0, sizeof(*s)); /* fd=0 marks slot as free */
    pthread_mutex_unlock(&s_table_mutex);

    /* Closed after the slot is marked free so that a broadcast in flight
     * (which holds s_table_mutex) can never write to a closed fd.       */
    if (fd > 0) close(fd);

    LOG_INFO("cp", "session destroyed");
}

/* =========================================================================
 * Command loop
 * ========================================================================= */
void *cp_session_thread(void *arg) {
    Session *s = (Session *)arg;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s->client_ip, ip_str, sizeof(ip_str));
    LOG_INFO("cp", "session started for %s", ip_str);

    static char  msg_buf[MAX_MSG_BYTES + 1];  /* NOTE: one thread per session — static is safe */
    jsmntok_t    tokens[MAX_JSON_TOKENS];
    char         id_buf[128];
    char         cmd_buf[64];

    for (;;) {
        /* ---- Read length prefix ---- */
        uint32_t len_net;
        if (recv_exact(s->fd, &len_net, 4) < 0) {
            LOG_INFO("cp", "client %s disconnected", ip_str);
            break;
        }
        uint32_t len = ntohl(len_net);
        if (len == 0 || len > MAX_MSG_BYTES) {
            LOG_WARN("cp", "invalid frame length %u from %s", len, ip_str);
            break;
        }

        /* ---- Read payload ---- */
        if (recv_exact(s->fd, msg_buf, len) < 0) break;
        msg_buf[len] = '\0';

        /* ---- Parse JSON ---- */
        jsmn_parser parser;
        jsmn_init(&parser);
        int ntok = jsmn_parse(&parser, msg_buf, len, tokens, MAX_JSON_TOKENS);
        if (ntok < 1 || tokens[0].type != JSMN_OBJECT) {
            send_error(s, "?", "invalid_request", "Malformed JSON envelope");
            continue;
        }

        /* ---- Extract type, id, command ---- */
        int idx_type    = tok_find_key(msg_buf, tokens, 0, "type");
        int idx_id      = tok_find_key(msg_buf, tokens, 0, "id");
        int idx_command = tok_find_key(msg_buf, tokens, 0, "command");

        if (idx_type < 0 || idx_id < 0 || idx_command < 0) {
            send_error(s, "?", "invalid_request", "Missing type/id/command");
            continue;
        }

        if (!tok_eq(msg_buf, &tokens[idx_type], "request")) {
            /* Clients must not send events; ignore silently */
            continue;
        }

        tok_str(msg_buf, &tokens[idx_id],      id_buf,  sizeof(id_buf));
        tok_str(msg_buf, &tokens[idx_command],  cmd_buf, sizeof(cmd_buf));

        LOG_DBG("cp", "[%s] command=%s", ip_str, cmd_buf);

        /* ---- Enforce hello-first rule ---- */
        if (s->state == SESSION_AWAIT_HELLO &&
            strcmp(cmd_buf, "hello") != 0) {
            send_error(s, id_buf, "invalid_request",
                       "hello must be the first command");
            continue;
        }

        /* ---- §26.1: rx is gated until this connection authenticated ---- */
        /* Checked before dispatch and therefore before ownership, so a
         * locked-out client is told the real reason rather than in_use. */
        if (cp_rx_requires_auth() && !s->rx_unlocked &&
            cmd_addresses_rx(cmd_buf, tokens, ntok, msg_buf)) {
            if (g_keys.n == 0)
                send_error(s, id_buf, "unauthorized",
                           "the rx function requires authentication and "
                           "this device has no pre-shared key configured, "
                           "so rx is locked (SDR-SLC-CP-1.0 26.3). On the "
                           "device, run: sudo " SLC_DAEMON_NAME " keygen");
            else
                send_error(s, id_buf, "unauthorized",
                           "the rx function requires authentication "
                           "(psk-hmac-sha256); send authenticate with the "
                           "nonce from hello before addressing rx");
            continue;
        }

        /* ---- Dispatch ---- */
        bool found = false;
        for (int i = 0; s_dispatch[i].name; i++) {
            if (strcmp(cmd_buf, s_dispatch[i].name) == 0) {
                s_dispatch[i].fn(s, id_buf, tokens, ntok, msg_buf);
                found = true;
                break;
            }
        }
        if (!found) {
            send_error(s, id_buf, "unknown_command", "Unrecognised command");
        }

        /* §26.3: the auth_failed reply has gone out; now the socket. */
        if (s->close_after_reply) {
            LOG_WARN("cp", "closing %s: too many failed authentication "
                     "attempts", ip_str);
            break;
        }
    }

    session_destroy(s);
    return NULL;
}
