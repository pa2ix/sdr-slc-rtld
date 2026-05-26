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

/* Monotonic stream ID counter (global, never resets) */
static uint32_t       s_stream_counter = 0;
static pthread_mutex_t s_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

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

int send_framed(int fd, const char *json) {
    uint32_t len     = (uint32_t)strlen(json);
    uint32_t len_net = htonl(len);
    /* Send length prefix */
    if (send(fd, &len_net, 4, MSG_MORE) != 4) return -1;
    /* Send payload */
    ssize_t sent = 0;
    while ((size_t)sent < len) {
        ssize_t r = send(fd, json + sent, len - (size_t)sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

/* =========================================================================
 * Command dispatch table
 * ========================================================================= */
typedef void (*cmd_fn)(Session *, const char *, jsmntok_t *, int, const char *);

typedef struct {
    const char *name;
    cmd_fn      fn;
} CmdEntry;

static const CmdEntry s_dispatch[] = {
    { "hello",               cmd_hello            },
    { "ping",                cmd_ping             },
    { "get_capabilities",    cmd_get_capabilities },
    { "get_status",          cmd_get_status       },
    { "set_frequency",       cmd_set_frequency    },
    { "set_sample_rate",     cmd_set_sample_rate  },
    { "set_control",         cmd_set_control      },
    { "get_control",         cmd_get_control      },
    { "open_stream",         cmd_open_stream      },
    { "close_stream",        cmd_close_stream     },
    { "start_rx",            cmd_start_rx         },
    { "stop_rx",             cmd_stop_rx          },
    { "get_clock_status",    cmd_get_clock_status },
    { "get_location",        cmd_get_location     },
    /* TX — all return not_supported */
    { "start_tx",            cmd_not_supported    },
    { "stop_tx",             cmd_not_supported    },
    { "set_tx_power",        cmd_not_supported    },
    { "get_tx_capabilities", cmd_not_supported    },
    { "get_vswr",            cmd_not_supported    },
    { "auto_tune",           cmd_not_supported    },
    { "set_tune_params",     cmd_not_supported    },
    { "get_tuner_status",    cmd_not_supported    },
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
            s->vita_token    = -1;
            s->frequency_hz  = RTL_DEFAULT_FREQ;
            s->sample_rate_sps = RTL_DEFAULT_RATE_NORMAL;
            s->rf_gain_db    = RTL_DEFAULT_GAIN / 10.0;
            break;
        }
    }
    pthread_mutex_unlock(&s_table_mutex);
    return s;
}

void session_destroy(Session *s) {
    if (!s) return;

    /* Stop VITA output for this session */
    if (s->vita_token >= 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
    }
    /* Decrement RTL streaming refcount if we were actively streaming */
    if (s->state == SESSION_STREAMING) {
        rtl_bridge_stop();
    }

    /* Close socket */
    if (s->fd > 0) {
        close(s->fd);
    }

    pthread_mutex_lock(&s_table_mutex);
    memset(s, 0, sizeof(*s)); /* fd=0 marks slot as free */
    pthread_mutex_unlock(&s_table_mutex);

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
        if (strcmp(cmd_buf, "set_control") == 0) {
            /* Write directly to stderr to bypass any LOG_LEVEL filtering issues */
            fprintf(stderr, "[set_control RAW] %s\n", msg_buf);
            fflush(stderr);
        }

        /* ---- Enforce hello-first rule ---- */
        if (s->state == SESSION_AWAIT_HELLO &&
            strcmp(cmd_buf, "hello") != 0) {
            send_error(s, id_buf, "invalid_request",
                       "hello must be the first command");
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
    }

    session_destroy(s);
    return NULL;
}
