#include "vita_tx.h"
#include "../rtl/rtl_bridge.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>

/* -------------------------------------------------------------------------
 * VRT header constants — IF Data (VITA-A/1.0)
 * [31:28]=0001 Packet Type: IF Data + Stream ID
 * [27]=1 Class ID present  [23:22]=01 TSI:UTC  [21:20]=10 TSF:picoseconds
 * --------------------------------------------------------------------- */
#define VRT_IF_DATA_BASE  0x18600000u
#define CLASS_ID_W0       0x00402814u
#define CLASS_ID_W1       0x00000001u

/* VRT header constant — Context packet (VITA-A/1.0-CTX)
 * [31:28]=0100 IF Context + Stream ID  [27]=0 No Class ID
 * [23:22]=00 No TSI  [21:20]=00 No TSF  [15:0]=5 (fixed packet size)     */
#define VRT_CTX_HDR_BASE  0x40000005u  /* Packet Count inserted at runtime */

/* -------------------------------------------------------------------------
 * Session table
 * --------------------------------------------------------------------- */
#define MAX_VITA_SESSIONS 4

typedef struct {
    bool           active;
    char           stream_id[32];
    uint32_t       vita_stream_id;
    struct in_addr dest_ip;
    uint16_t       dest_port;
    struct sockaddr_in dest_addr;  /* pre-built, stable pointer for mmsghdr */
    iq_format_t    fmt;
    uint32_t       payload_fmt_word0;
    uint8_t        if_pkt_count;
    uint8_t        ctx_pkt_count;
    uint64_t       sample_count;
    uint64_t       cb_count;
    uint32_t       epoch_at_start;
    uint64_t       rate_hz_anchor;
} VitaSession;

static VitaSession     s_sessions[MAX_VITA_SESSIONS];
static pthread_mutex_t s_sess_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             s_sock       = -1;

/* -------------------------------------------------------------------------
 * u8 offset-binary → int16 big-endian
 *
 * The RTL2832U ADC has a hardware bias placing its midpoint at ~127.4,
 * not exactly 127 or 128.  Using 128 leaves a +0.4 LSB DC residual that
 * appears as a centre-frequency spike in the spectrum display.
 * Using 127.4 (matching RTL-TCP) eliminates the spike.
 *
 * Formula:  int16 = (sample - 127.4) / 128.0 * 32767
 *
 * Output is big-endian as required by VITA-A/1.0 §5.
 * --------------------------------------------------------------------- */
static inline uint16_t u8_to_int16_be(uint8_t u) {
    float   f  = ((float)u - 127.4f) * (32767.0f / 128.0f);
    /* Clamp to int16 range — only needed for sample values 0 and 255    */
    if (f >  32767.0f) f =  32767.0f;
    if (f < -32768.0f) f = -32768.0f;
    int16_t v  = (int16_t)f;
    /* Big-endian byte order */
    return (uint16_t)(((uint16_t)(v & 0xFF) << 8) | ((uint16_t)(v >> 8) & 0xFF));
}

/* -------------------------------------------------------------------------
 * IF Data packet builder
 * --------------------------------------------------------------------- */
size_t vita_build_if_data_packet(uint8_t       *pkt_out,
                                  uint32_t       stream_id,
                                  uint8_t        pkt_count,
                                  uint32_t       tsi,
                                  uint64_t       tsf_picos,
                                  const uint8_t *iq_u8,
                                  uint16_t       num_pairs,
                                  iq_format_t    fmt)
{
    uint16_t payload_words, payload_bytes;
    if (fmt == IQ_FMT_INT16) {
        payload_words = num_pairs;
        payload_bytes = num_pairs * 4;
    } else {
        payload_bytes = num_pairs * 2;
        payload_words = (uint16_t)((payload_bytes + 3) / 4);
    }

    uint16_t pkt_size = 7 + payload_words;
    uint32_t hdr = VRT_IF_DATA_BASE
                 | ((uint32_t)(pkt_count & 0xF) << 16)
                 | pkt_size;

    uint32_t *w = (uint32_t *)pkt_out;
    w[0] = htonl(hdr);
    w[1] = htonl(stream_id);
    w[2] = htonl(CLASS_ID_W0);
    w[3] = htonl(CLASS_ID_W1);
    w[4] = htonl(tsi);
    w[5] = htonl((uint32_t)(tsf_picos >> 32));
    w[6] = htonl((uint32_t)(tsf_picos & 0xFFFFFFFFu));

    uint8_t *payload = pkt_out + VITA_HDR_BYTES;
    if (fmt == IQ_FMT_INT16) {
        uint16_t *out16 = (uint16_t *)payload;
        for (uint16_t i = 0; i < num_pairs * 2; i++)
            out16[i] = u8_to_int16_be(iq_u8[i]);
    } else {
        memcpy(payload, iq_u8, num_pairs * 2);
        if ((num_pairs * 2) & 3)
            memset(payload + num_pairs * 2, 0, 4 - ((num_pairs * 2) & 3));
    }
    return VITA_HDR_BYTES + payload_words * 4;
}

/* -------------------------------------------------------------------------
 * Context packet builder (VITA-A/1.0-CTX, §7 of VITA-SLC spec)
 *
 * Fixed 20-byte packet:
 *   +0  VRT header (Packet Type=0x4, no CID, no timestamp, size=5)
 *   +4  Stream ID
 *   +8  CIF0 = 0x00000002 (Payload Format field present)
 *   +12 Payload Format word 0 (signal type)
 *   +16 Payload Format word 1 = 0x00000000
 * --------------------------------------------------------------------- */
size_t vita_build_context_packet(uint8_t  *pkt_out,
                                  uint32_t  stream_id,
                                  uint8_t   pkt_count,
                                  uint32_t  payload_fmt_word0)
{
    /* Insert packet count into header base */
    uint32_t hdr = VRT_CTX_HDR_BASE | ((uint32_t)(pkt_count & 0xF) << 16);
    uint32_t *w = (uint32_t *)pkt_out;
    w[0] = htonl(hdr);
    w[1] = htonl(stream_id);
    w[2] = htonl(VITA_CIF0_PAYLOAD_FORMAT);
    w[3] = htonl(payload_fmt_word0);
    w[4] = 0;   /* Payload Format word 1: no event/channel tags */
    return VITA_CTX_PKT_BYTES;
}

/* -------------------------------------------------------------------------
 * Send a Context packet for the given session token.
 * Called from cmd_start_rx before data flow begins.
 * --------------------------------------------------------------------- */
void vita_send_context_packet(int token) {
    if (token < 0 || token >= MAX_VITA_SESSIONS) return;
    if (s_sock < 0) return;

    pthread_mutex_lock(&s_sess_mutex);
    VitaSession *sess = &s_sessions[token];
    if (!sess->active) {
        pthread_mutex_unlock(&s_sess_mutex);
        return;
    }

    uint8_t pkt[VITA_CTX_PKT_BYTES];
    size_t pkt_len = vita_build_context_packet(
        pkt,
        sess->vita_stream_id,
        sess->ctx_pkt_count,
        sess->payload_fmt_word0
    );
    sess->ctx_pkt_count = (sess->ctx_pkt_count + 1) & 0xF;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_addr   = sess->dest_ip,
        .sin_port   = htons(sess->dest_port)
    };
    pthread_mutex_unlock(&s_sess_mutex);

    sendto(s_sock, pkt, pkt_len, 0,
           (struct sockaddr *)&dest, sizeof(dest));
    LOG_INFO("vita", "context packet sent: stream_id=%u payload_fmt=0x%08X",
             sess->vita_stream_id, sess->payload_fmt_word0);
}

/* -------------------------------------------------------------------------
 * Static sendmmsg buffers — allocated once, reused every callback.
 * Placing them here (file scope) keeps them off the stack and avoids
 * malloc.  They are only written from the RTL async callback thread so
 * no locking is needed for the buffers themselves.
 * --------------------------------------------------------------------- */
static uint8_t          s_pkt_bufs[VITA_MMSG_MAX_PKTS][VITA_PKT_MAX_BYTES];
static struct iovec     s_iovecs [VITA_MMSG_MAX_PKTS];
static struct mmsghdr   s_mmsg   [VITA_MMSG_MAX_PKTS];

/* -------------------------------------------------------------------------
 * vita_send_callback — called directly from rtl_callback()
 *
 * Builds all N VITA-49 packets into s_pkt_bufs[], then calls sendmmsg()
 * once.  One syscall → one softirq cycle → all N packets exit the kernel
 * together with no inter-packet scheduling gaps.
 * --------------------------------------------------------------------- */
void vita_send_callback(const uint8_t *buf, uint32_t len) {
    if (s_sock < 0) return;

    uint32_t bytes_per_pkt = VITA_SAMPLES_PER_PKT * 2;
    uint32_t num_pkts      = len / bytes_per_pkt;
    uint32_t remainder     = len % bytes_per_pkt;

    if (num_pkts == 0) return;
    if (num_pkts > VITA_MMSG_MAX_PKTS) num_pkts = VITA_MMSG_MAX_PKTS;

    pthread_mutex_lock(&s_sess_mutex);

    for (int si = 0; si < MAX_VITA_SESSIONS; si++) {
        VitaSession *sess = &s_sessions[si];
        if (!sess->active) continue;

        LOG_DBG("vita",
            "cb#%llu len=%u pkts=%u remainder=%u "
            "first=[%02x,%02x,%02x,%02x] last=[%02x,%02x,%02x,%02x] "
            "sample_count=%llu",
            (unsigned long long)sess->cb_count,
            len, num_pkts, remainder,
            buf[0], buf[1], buf[2], buf[3],
            buf[len-4], buf[len-3], buf[len-2], buf[len-1],
            (unsigned long long)sess->sample_count);

        if (remainder > 0) {
            LOG_WARN("vita",
                "cb#%llu: %u remainder bytes discarded — "
                "VITA_SAMPLES_PER_PKT=%u may not divide callback size %u",
                (unsigned long long)sess->cb_count,
                remainder, VITA_SAMPLES_PER_PKT, len);
        }

        /* Destination address — same for every packet in this burst      */
        struct sockaddr_in dest = {
            .sin_family = AF_INET,
            .sin_addr   = sess->dest_ip,
            .sin_port   = htons(sess->dest_port)
        };

        /* Destination address is pre-built in sess->dest_addr at
         * vita_session_add() — stable pointer, no stack lifetime issue.  */

        /* ---------------------------------------------------------------
         * Hoist timestamp base computation — two 64-bit divisions done
         * once here, then only additions inside the build loop.
         * ------------------------------------------------------------- */
        uint64_t rate_hz = sess->rate_hz_anchor;
        uint64_t sc      = sess->sample_count;

        uint64_t total_secs    = (rate_hz > 0) ? (sc / rate_hz) : 0;
        uint64_t sub_samp      = (rate_hz > 0) ? (sc % rate_hz) : 0;
        uint32_t tsi           = sess->epoch_at_start + (uint32_t)total_secs;
        uint64_t picos         = (rate_hz > 0)
                                 ? (sub_samp * 1000000000000ULL / rate_hz)
                                 : 0;
        uint64_t picos_per_pkt = (rate_hz > 0)
                                 ? (VITA_SAMPLES_PER_PKT * 1000000000000ULL
                                    / rate_hz)
                                 : 0;
        uint64_t picos_per_sec = 1000000000000ULL;

        /* ---------------------------------------------------------------
         * Build loop: fill s_pkt_bufs[] and set up iovec + mmsghdr.
         * No syscalls here — pure memory writes.
         * ------------------------------------------------------------- */
        const uint8_t *chunk = buf;
        for (uint32_t p = 0; p < num_pkts; p++, chunk += bytes_per_pkt) {

            size_t pkt_len = vita_build_if_data_packet(
                s_pkt_bufs[p],
                sess->vita_stream_id,
                sess->if_pkt_count,
                tsi, picos,
                chunk, VITA_SAMPLES_PER_PKT, sess->fmt);

            s_iovecs[p].iov_base = s_pkt_bufs[p];
            s_iovecs[p].iov_len  = pkt_len;

            /* Point directly at the pre-built address in the session —
             * stable for the lifetime of the session, no stack reference. */
            s_mmsg[p].msg_hdr.msg_name       = &sess->dest_addr;
            s_mmsg[p].msg_hdr.msg_namelen    = sizeof(sess->dest_addr);
            s_mmsg[p].msg_hdr.msg_iov        = &s_iovecs[p];
            s_mmsg[p].msg_hdr.msg_iovlen     = 1;
            s_mmsg[p].msg_hdr.msg_control    = NULL;
            s_mmsg[p].msg_hdr.msg_controllen = 0;
            s_mmsg[p].msg_hdr.msg_flags      = 0;
            s_mmsg[p].msg_len                = 0;

            sess->if_pkt_count  = (sess->if_pkt_count + 1) & 0xF;
            sess->sample_count += VITA_SAMPLES_PER_PKT;

            picos += picos_per_pkt;
            if (picos >= picos_per_sec) {
                picos -= picos_per_sec;
                tsi++;
            }
        }

        /* ---------------------------------------------------------------
         * Single sendmmsg() call — one kernel transition, one softirq,
         * all num_pkts packets dispatched atomically.
         *
         * Socket is non-blocking (SOCK_NONBLOCK).  If the UDP send buffer
         * is full, sendmmsg returns EAGAIN immediately rather than blocking
         * the RTL callback thread.  Dropped packets are preferable to
         * stalling the USB transfer pipeline.
         * ------------------------------------------------------------- */
        int sent = sendmmsg(s_sock, s_mmsg, num_pkts, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN("vita",
                    "cb#%llu: send buffer full (EAGAIN) — %u pkts dropped. "
                    "Increase SO_SNDBUF or reduce sample rate.",
                    (unsigned long long)sess->cb_count, num_pkts);
            } else {
                LOG_WARN("vita", "sendmmsg failed: %s", strerror(errno));
            }
        } else if ((uint32_t)sent < num_pkts) {
            LOG_WARN("vita", "sendmmsg partial: sent %d of %u packets",
                     sent, num_pkts);
        }

        sess->cb_count++;
    }

    pthread_mutex_unlock(&s_sess_mutex);
}

/* -------------------------------------------------------------------------
 * Session registration
 * --------------------------------------------------------------------- */
int vita_session_add(const char    *stream_id,
                     uint32_t       vita_stream_id,
                     struct in_addr dest_ip,
                     uint16_t       dest_port,
                     iq_format_t    fmt,
                     uint32_t       payload_fmt_word0)
{
    pthread_mutex_lock(&s_sess_mutex);
    int tok = -1;
    for (int i = 0; i < MAX_VITA_SESSIONS; i++) {
        if (!s_sessions[i].active) {
            VitaSession *s  = &s_sessions[i];
            s->active         = true;
            s->vita_stream_id = vita_stream_id;
            s->dest_ip        = dest_ip;
            s->dest_port      = dest_port;
            /* Pre-build the sockaddr so mmsghdr can point directly to it  */
            memset(&s->dest_addr, 0, sizeof(s->dest_addr));
            s->dest_addr.sin_family = AF_INET;
            s->dest_addr.sin_addr   = dest_ip;
            s->dest_addr.sin_port   = htons(dest_port);
            s->fmt            = fmt;
            s->payload_fmt_word0 = payload_fmt_word0;
            s->if_pkt_count   = 0;
            s->ctx_pkt_count  = 0;
            s->sample_count   = 0;
            s->cb_count       = 0;
            s->epoch_at_start = (uint32_t)time(NULL);
            s->rate_hz_anchor = (uint64_t)g_rate_sps;
            strncpy(s->stream_id, stream_id, sizeof(s->stream_id) - 1);
            LOG_INFO("vita", "session %s anchored: epoch=%u rate=%llu sps "
                     "signal=%s",
                     stream_id, s->epoch_at_start,
                     (unsigned long long)s->rate_hz_anchor,
                     (payload_fmt_word0 == VITA_PAYLOAD_FMT_COMPLEX)
                     ? "complex_iq" : "real");
            tok = i;
            break;
        }
    }
    pthread_mutex_unlock(&s_sess_mutex);
    return tok;
}

void vita_session_remove(int token) {
    if (token < 0 || token >= MAX_VITA_SESSIONS) return;
    pthread_mutex_lock(&s_sess_mutex);
    memset(&s_sessions[token], 0, sizeof(VitaSession));
    pthread_mutex_unlock(&s_sess_mutex);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
int vita_tx_init(void) {
    memset(s_sessions, 0, sizeof(s_sessions));

    s_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    if (s_sock < 0) {
        LOG_ERR("vita", "socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in src = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(VITA_SRC_PORT)
    };
    if (bind(s_sock, (struct sockaddr *)&src, sizeof(src)) < 0) {
        LOG_WARN("vita", "bind to port %d failed: %s",
                 VITA_SRC_PORT, strerror(errno));
    }

    /* Send buffer must hold at least one full callback burst.
     * 512 pkts × 1052 bytes = ~538 KB per burst at 256 samp/pkt int16.
     * 4 MB gives ~7× headroom — enough for transient network back-pressure
     * without blocking the RTL callback.  With SOCK_NONBLOCK the kernel
     * returns EAGAIN rather than blocking when this limit is hit.          */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(s_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    LOG_INFO("vita", "UDP socket ready on port %d", VITA_SRC_PORT);
    return 0;
}

void vita_tx_destroy(void) {
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}

int  vita_tx_start(void) {
    LOG_INFO("vita", "send path: callback-driven (no thread)");
    return 0;
}
void vita_tx_stop(void)  { /* nothing to join */ }
