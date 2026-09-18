/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#ifndef CP_SESSION_H
#define CP_SESSION_H

/*
 * cp_session — per-client SLC-CP session state
 *
 * One Session object lives for the duration of a single TCP connection.
 * The session thread runs the read → dispatch → respond loop.
 */

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <pthread.h>
#include "../config.h"
#include "../vita/vita_tx.h"
#include "../common/auth.h"

/* Session lifecycle states */
typedef enum {
    SESSION_AWAIT_HELLO = 0, /* Only hello is accepted                  */
    SESSION_IDLE,            /* Normal command processing                */
    SESSION_STREAM_OPEN,     /* open_stream called, not yet started      */
    SESSION_STREAMING,       /* start_rx called, VITA packets flowing    */
} SessionState;

/* -------------------------------------------------------------------------
 * Session object
 * --------------------------------------------------------------------- */
typedef struct {
    int           fd;            /* Client TCP socket                     */
    SessionState  state;
    struct in_addr client_ip;   /* Connected client's IP                  */

    /* Serialises writes to fd.  A session thread writing a response and
     * the event broadcaster writing an event are different threads on
     * the same socket, and a torn length-prefixed frame desynchronises
     * the client permanently.                                           */
    pthread_mutex_t send_mutex;

    /* §17: the rx channel is an exclusive resource.  Ownership is taken
     * by open_stream and released by close_stream or by loss of the
     * control connection.                                               */
    bool          owns_rx;

    /* Client identity from hello, for the log and for in_use messages.  */
    char          client_name[48];

    /* §26 authentication state for this connection.  The nonce is fresh
     * per connection (issued in hello) and single-use; rx_unlocked is
     * set by a successful authenticate and never cleared for the life of
     * the connection (§26.3).  Only meaningful when g_rx_auth is set.   */
    uint8_t       nonce[AUTH_NONCE_BYTES];
    char          nonce_hex[AUTH_NONCE_HEX];
    bool          nonce_valid;
    bool          rx_unlocked;
    unsigned      auth_failures;
    uint64_t      auth_window_ns;
    bool          close_after_reply;   /* §26.3: too many failures       */

    /* Stream parameters (set by open_stream) */
    bool          stream_open;
    char          stream_id[32]; /* e.g. "rx-1"                          */
    uint32_t      vita_stream_id;/* Numeric VITA-49 stream ID             */
    struct in_addr dest_ip;      /* VITA UDP destination                  */
    uint16_t      dest_port;     /* VITA UDP destination port             */
    iq_format_t   iq_fmt;        /* Negotiated IQ format                  */
    int           vita_token;    /* vita_session_add() return value       */

    /* Radio parameters as last set by this session */
    uint64_t      frequency_hz;
    uint32_t      sample_rate_sps;
    double        rf_gain_db;

    /* Stream ID counter (global, protected by g_stream_counter_mutex)   */
    /* Stored in the session for display / logging purposes only         */
} Session;

/* -------------------------------------------------------------------------
 * §26 authentication configuration (set once by main.c before the server
 * starts).  A receiver has no particular reason to require it (§26), so
 * the default is off; --rx-auth psk turns it on, and then every command
 * addressing rx answers unauthorized until the connection has presented
 * a key from g_keys.  With auth on and no key loaded the block is locked
 * (§26.3): the daemon refuses every key rather than accepting any.
 * --------------------------------------------------------------------- */
extern bool           g_rx_auth;
extern auth_keyring_t g_keys;

/* True when the rx function requires authentication on this device.     */
static inline bool cp_rx_requires_auth(void) { return g_rx_auth; }

/* -------------------------------------------------------------------------
 * Framing helpers (used by both session and command modules)
 * --------------------------------------------------------------------- */

/* Read exactly n bytes from fd.  Returns 0 on success, -1 on error/EOF.  */
int recv_exact(int fd, void *buf, size_t n);

/* Send a NUL-terminated JSON string wrapped in a 4-byte length prefix.
 * Takes the session so the write can be serialised against events.      */
int send_framed(Session *s, const char *json);

/* -------------------------------------------------------------------------
 * Session lifecycle
 * --------------------------------------------------------------------- */

/* Allocate and return a new Session for the given fd.
 * Returns NULL if the session table is full.                             */
Session *session_create(int fd, struct in_addr client_ip);

/* Release session resources (close fd, remove VITA session, free mem).  */
void session_destroy(Session *s);

/* Thread entry point — runs the command loop for one client connection.  */
void *cp_session_thread(void *arg);

/* -------------------------------------------------------------------------
 * Helpers shared with cp_commands.c
 * --------------------------------------------------------------------- */

/* Allocate and return the next stream ID string ("rx-N").
 * Thread-safe.  §24.1: stream IDs are never reused within a TCP session;
 * the counter here is global and monotonic, which is stronger.          */
void alloc_stream_id(char *out, size_t cap, uint32_t *vita_id_out);

/* -------------------------------------------------------------------------
 * Access control [§17]
 *
 * The rx block has one channel, one tuned frequency and one gain, so it
 * is an exclusive resource: two clients tuning it would fight over one
 * physical state with neither being told.  Ownership is claimed by
 * open_stream and released on close_stream or disconnect.
 * --------------------------------------------------------------------- */

/* Claim the rx channel for this session.  Returns false if another
 * session holds it — the caller answers in_use.                         */
bool cp_rx_claim(Session *s);

/* Release the rx channel if this session holds it.  Safe to call when it
 * does not.                                                             */
void cp_rx_release(Session *s);

/* True when some other session holds the rx channel.                    */
bool cp_rx_owned_by_other(const Session *s);

/* "owned_by_me" | "owned_by_other" | "free"  (§17.3)                    */
const char *cp_rx_control_state(const Session *s);

/* -------------------------------------------------------------------------
 * Event broadcast [§21]
 * --------------------------------------------------------------------- */

/* Emit an event to every connected session that has completed hello.    */
void cp_broadcast_event(const char *event, const char *params_json);

/* As above, but skipping one session — used when the originating client
 * has already been told in its own response.                            */
void cp_broadcast_event_except(const Session *skip,
                               const char *event, const char *params_json);

/* Registered with rtl_bridge; fires the adc_overload event (§21.3).     */
void cp_on_adc_overload(double adc_level_dbfs);

#endif /* CP_SESSION_H */
