#ifndef CONFIG_H
#define CONFIG_H

/* =========================================================================
 * SDR-SLC RTL-SDR Daemon — Compile-time Configuration
 * =========================================================================
 * Edit these values before building.  All runtime state is derived from
 * the hardware at startup; these are only defaults and hard limits.
 * ========================================================================= */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>

/* ---------- Network ---------------------------------------------------- */
#define CP_PORT             4620
#define VITA_SRC_PORT       4621
#define MDNS_PORT           5353
#define MDNS_GROUP          "224.0.0.251"
#define CP_BACKLOG          4
#define MAX_SESSIONS        4

/* ---------- Protocol identity ------------------------------------------ */
#define PROTO_NAME          "SDR-SLC Control Plane"
#define PROTO_VERSION       "1.0"
#define MANUFACTURER        "IXChange BV"
#define DEVICE_NAME         "RTL-SDR via SLC-RTLd"
#define HW_REVISION         "RTL-SDR-v4"
#define FW_VERSION          "0.2.0"
#define MODEL_NAME          "RTL-SDR SDR-SLC Bridge"

/* VITA-49 Class Identifier */
#define VITA_OUI            0x402814u
#define VITA_ICC            0x0000u
#define VITA_PCC            0x0001u

/* ---------- Sampling mode ---------------------------------------------- */
/* Set at daemon startup; never changes at runtime.                        */
typedef enum {
    SAMPLING_NORMAL   = 0,   /* Normal tuner path. 500 kHz – 1766 MHz     */
    SAMPLING_DIRECT_I = 1,   /* Direct sampling, I-branch. 100 kHz–30 MHz */
    SAMPLING_DIRECT_Q = 2,   /* Direct sampling, Q-branch. 100 kHz–30 MHz */
} sampling_mode_t;

/* ---------- RTL-SDR hardware ------------------------------------------- */
#define RTL_DEVICE_INDEX    0
#define RTL_DEFAULT_FREQ    7000000
#define RTL_DEFAULT_GAIN    200         /* 20.0 dB in 0.1 dB units         */

/* Preferred and default sample rates by mode.
 * In normal mode: 2048000 — best ADC performance on R820T2.
 * In direct sampling: 1024000 — usable BW halved (mirrored spectrum),
 *   so 1 MSPS gives ~512 kHz usable which is ample for HF SSB/CW.       */
#define RTL_DEFAULT_RATE_NORMAL    2048000
#define RTL_DEFAULT_RATE_DIRECT    1024000

/* Discrete sample rates — normal tuner mode */
#define RTL_RATE_COUNT_NORMAL   6
static const uint32_t RTL_SAMPLE_RATES_NORMAL[RTL_RATE_COUNT_NORMAL] = {
    250000, 1024000, 1536000, 1800000, 2048000, 2400000
};

/* Discrete sample rates — direct sampling mode */
#define RTL_RATE_COUNT_DIRECT   4
static const uint32_t RTL_SAMPLE_RATES_DIRECT[RTL_RATE_COUNT_DIRECT] = {
    250000, 512000, 1024000, 2048000
};

/* Frequency ranges by mode (Hz) */
#define RTL_FREQ_MIN_NORMAL   500000ULL
#define RTL_FREQ_MAX_NORMAL   1766000000ULL
#define RTL_FREQ_MIN_DIRECT   100000ULL
#define RTL_FREQ_MAX_DIRECT   30000000ULL

/* ---------- VITA-SLC stream -------------------------------------------- */
/* MUST divide the librtlsdr callback buffer (262144 bytes = 131072 pairs).
 * 131072 / 256 = 512 packets exactly — zero remainder.                   */
#define VITA_SAMPLES_PER_PKT  256

/* Payload Format word 0 values (VITA-49.0 §9.13).
 * Word 1 is always 0x00000000 (no event/channel tags).
 * See VITA-SLC spec §7.7 for bit-field derivation.                       */
#define VITA_PAYLOAD_FMT_COMPLEX  0x0F8700E0u  /* Complex IQ, u8           */
#define VITA_PAYLOAD_FMT_REAL     0x0F0700E0u  /* Real (direct sampling)   */

/* Context packet constants */
#define VITA_PKT_TYPE_IF_DATA     0x1u         /* IF Data + Stream ID       */
#define VITA_PKT_TYPE_IF_CONTEXT  0x4u         /* IF Context + Stream ID    */
#define VITA_CIF0_PAYLOAD_FORMAT  0x00000002u  /* Only Payload Format set   */
#define VITA_CTX_PKT_SIZE_WORDS   5            /* 1 hdr+1 sid+1 cif0+2 pf  */
#define VITA_CTX_PKT_BYTES        20           /* 5 × 4                     */

/* ---------- Internal ring buffer --------------------------------------- */
#define RING_SLOTS          2048        /* Power of 2                       */
#define RING_MASK           (RING_SLOTS - 1)
#define RING_SLOT_BYTES     (VITA_SAMPLES_PER_PKT * 2)

/* ---------- Framing ----------------------------------------------------- */
#define MAX_MSG_BYTES       65536
#define MAX_JSON_TOKENS     256

/* ---------- mDNS -------------------------------------------------------- */
#define MDNS_TTL            120
#define MDNS_LINK_DELAY_MS  100
#define MDNS_ANNOUNCE_IFACE "eth0"

/* ---------- Logging ----------------------------------------------------- */
#define LOG_LEVEL_ERROR     0
#define LOG_LEVEL_WARN      1
#define LOG_LEVEL_INFO      2
#define LOG_LEVEL_DEBUG     3
#define LOG_LEVEL           LOG_LEVEL_INFO

static inline void _log(int level, const char *tag, const char *fmt, ...) {
    if (level > LOG_LEVEL) return;
    static const char *labels[] = {"ERR","WRN","INF","DBG"};
    char tbuf[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
    fprintf(stderr, "[%s %s] ", tbuf, labels[level]);
    if (tag) fprintf(stderr, "[%s] ", tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
#define LOG_ERR(tag,...)  _log(LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#define LOG_WARN(tag,...) _log(LOG_LEVEL_WARN,  tag, __VA_ARGS__)
#define LOG_INFO(tag,...) _log(LOG_LEVEL_INFO,  tag, __VA_ARGS__)
#define LOG_DBG(tag,...)  _log(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)

#endif /* CONFIG_H */
