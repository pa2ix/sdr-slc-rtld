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
 * Framing helpers (used by both session and command modules)
 * --------------------------------------------------------------------- */

/* Read exactly n bytes from fd.  Returns 0 on success, -1 on error/EOF.  */
int recv_exact(int fd, void *buf, size_t n);

/* Send a NUL-terminated JSON string wrapped in a 4-byte length prefix.   */
int send_framed(int fd, const char *json);

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
 * Thread-safe.                                                           */
void alloc_stream_id(char *out, size_t cap, uint32_t *vita_id_out);

#endif /* CP_SESSION_H */
