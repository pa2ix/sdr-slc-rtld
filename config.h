/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

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
#define CP_PORT_DEFAULT       4620
#define VITA_SRC_PORT_DEFAULT 4621
#define CP_BACKLOG            4
#define MAX_SESSIONS          4

/* Runtime ports (--port / --rx-port), defined in main.c.  Fixed at 4620 and
 * 4621 in every previous drop; made settable so several daemons - or a test
 * suite - can share one host.                                            */
extern uint16_t g_cp_port;
extern uint16_t g_vita_src_port;

/* mDNS instance name and hello device_name ("SLC-XXYYZZ" from the MAC of
 * the announce interface, or --name).  §6.2: the client stores the device
 * under this name; it does not change across upgrades.  Defined in main.c. */
extern char g_device_name[64];

/* ---------- Identity and version -------------------------------------- */
/* Single source of truth for the daemon's name and version.  The pair is
 * what §6.2 puts in the discovery `fw` field and §10.1 in the hello
 * firmware_version: "<implementation>/<version>", so that two daemons at
 * the same version are distinguishable in a discovery list.  Both
 * reference implementations use the same scheme (sdr-slc-bladerfd/1.0). */
#define SLC_DAEMON_NAME     "sdr-slc-rtld"
#define SLC_DAEMON_VERSION  "1.0"
#define SLC_FW_STRING       SLC_DAEMON_NAME "/" SLC_DAEMON_VERSION
#define SLC_RTLD_VERSION    SLC_DAEMON_VERSION   /* older name, same value */

/* ---------- Configuration files ---------------------------------------- */
/* Same layout as sdr-slc-bladerfd: an EnvironmentFile holding SLC_ARGS for
 * the systemd unit, and a key file written by the keygen subcommand.     */
#define SLC_DEFAULT_CONF_DIR  "/etc/sdr-slc"
#define SLC_DEFAULT_CONF_FILE SLC_DEFAULT_CONF_DIR "/rtld.conf"
#define SLC_DEFAULT_KEYS_FILE SLC_DEFAULT_CONF_DIR "/rtld.keys"
/* The function block the §26 keys unlock on this device.                 */
#define SLC_AUTH_FUNCTION     "rx"

/* ---------- Protocol identity ------------------------------------------ */
#define PROTO_NAME          "SDR-SLC Control Plane"
#define PROTO_VERSION       "1.0"
#define MANUFACTURER        "IXChange BV"
#define DEVICE_NAME         "RTL-SDR via SLC-RTLd"
#define HW_REVISION         "RTL-SDR-v4"
/* §6.2 / §10.1: "<implementation>/<version>", never the dongle firmware. */
#define FW_VERSION          SLC_FW_STRING
#define MODEL_NAME          "RTL-SDR SDR-SLC Bridge"

/* The specification releases this build is written against.  The protocol
 * version negotiated in hello is PROTO_VERSION ("1.0"); the releases are
 * document revisions of that protocol and are informational only.       */
#define SLC_CP_RELEASE      "1.3"        /* SDR-SLC-CP-1.0 Release 1.3   */
#define SLC_VITA_RELEASE    "1.2"        /* SDR-SLC-VITA-1.0 Release 1.2 */

/* Capability schema version carried in get_capabilities.  §11.2: Release
 * 1.2 and 1.3 both define schema_version 2, and a device SHALL NOT
 * advertise a number the specification has not defined.                 */
#define CAP_SCHEMA_VERSION  2

/* Declared tier for the rx function block (SLC-CP-1.0 R1.3 §4.1).
 * Tier 2 = standard controls, Reference Level, Context packets,
 * ADC monitoring and the full event model.                              */
#define RX_TIER             2

/* VITA-49 Class Identifier */
#define VITA_OUI            0x402814u
#define VITA_ICC            0x0000u
#define VITA_PCC            0x0001u

/* ---------- Sampling mode ---------------------------------------------- */
/* Startup default from --normal/--direct-i/--direct-q; direct_sampling is
 * also a runtime control (§11.3), so this can change while running.     */
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

/* ---- librtlsdr async transfer sizing ---------------------------------
 * Each USB transfer completion delivers one buffer of RTL_ASYNC_BUF_LEN
 * bytes to the callback, and the daemon ships that whole buffer as a
 * single sendmmsg burst.  librtlsdr's default (buf_len=0 -> 262144) makes
 * one ~512-packet burst every ~64 ms at 2.048 MS/s.  That burst leaves the
 * NIC in ~2 ms and arrives at the client faster than a default-sized UDP
 * receive buffer (~385 packets) can drain, so the client drops the tail of
 * every burst: a fixed ~20% loss that only appears once its receive buffer
 * has ramped to steady state (the first ~100-200 ms are clean).
 *
 * A small buffer paces the output into frequent little bursts the client
 * absorbs without overflow, and keeps each callback short.  It does this
 * with no pacing loops in the daemon — the pacing is simply the USB
 * transfer cadence.
 *
 * 16384 bytes = 8192 IQ pairs = 32 packets per burst, one burst every
 * ~4 ms at 2.048 MS/s.  Constraints: must be a multiple of 512 (USB) and
 * of (VITA_SAMPLES_PER_PKT*2) so a callback leaves no remainder.
 * 16384 = 32 * 512 = 32 * (256*2), so both hold.                         */
#define RTL_ASYNC_BUF_LEN     16384u
#define RTL_ASYNC_BUF_NUM     0u      /* 0 = librtlsdr default (15 buffers) */

/* ---- Context packet, SDR-SLC-VITA-1.0 Release 1.2 §7 -----------------
 * Release 1.2 corrected the Context packet to the field positions and
 * encodings VITA-49.0 actually defines.  There is one encoding now; the
 * Release 1.1 five-word fallback (--no-ctx-reflevel) is gone, and so is
 * the R1.1 bit assignment (0x00000082) and the R1.1 Payload Format
 * constant (0x0F8700E0), which decoded to nothing under the standard.
 *
 * CIF0 (§7.3): bit 24 Reference Level, bit 15 Data Packet Payload
 * Format, bit 31 Context Field Change Indicator - set on the first packet
 * of a stream and whenever the Reference Level or the Payload Format
 * differs from the previous Context packet on that stream.             */
#define VITA_PKT_TYPE_IF_DATA     0x1u         /* IF Data + Stream ID       */
#define VITA_PKT_TYPE_IF_CONTEXT  0x4u         /* IF Context + Stream ID    */
#define VITA_CIF0_CHANGED         0x80000000u  /* bit 31                    */
#define VITA_CIF0_REF_LEVEL       0x01000000u  /* bit 24                    */
#define VITA_CIF0_PAYLOAD_FORMAT  0x00008000u  /* bit 15                    */
#define VITA_CIF0_BASE            (VITA_CIF0_REF_LEVEL | VITA_CIF0_PAYLOAD_FORMAT)

/* Payload Format word 0 (§7.5), computed for the u8 encoding this daemon
 * streams: [30:29] real/complex, [28:24]=10000b unsigned fixed point,
 * [11:6] packing size-1 = 7, [5:0] item size-1 = 7.  Word 1 is always 0. */
#define VITA_PAYLOAD_FMT_COMPLEX  0x300001C7u  /* complex Cartesian, u8     */
#define VITA_PAYLOAD_FMT_REAL     0x100001C7u  /* real (direct sampling), u8 */

/* ---- u8 zero point (SDR-SLC-CP-1.0 §11.3 addendum, VITA §5.1) --------
 * The code value in the unsigned 8-bit stream that represents zero signal.
 * The RTL2832U ADC is biased so its midpoint sits at ~127.4, not 128.  The
 * daemon cannot shift the samples by 0.6 LSB in 8 bits, so it declares the
 * value in iq_formats[] and the open_stream format echo, and the client
 * subtracts it before scaling.  Tunable per dongle with --u8-zero-point.
 * Defined in main.c.                                                     */
#define RTL_U8_ZERO_POINT_DEFAULT 127.4
extern double g_u8_zero_point;

/* Context packet size (§7.1): header, stream id, CIF0, Reference Level,
 * two Payload Format words.  Fixed at six words.                        */
#define VITA_CTX_PKT_SIZE_WORDS   6
#define VITA_CTX_PKT_BYTES        24

/* ---- Reference Level calibration [RX-T2] -----------------------------
 * SLC-CP-1.0 §19: the power at the antenna port that would produce
 * a full-scale I/Q sample, in dBm.  It tracks gain — raising rf_gain by
 * 10 dB lowers the Reference Level by 10 dB.
 *
 * An RTL-SDR dongle is not a calibrated instrument.  §19 puts an
 * uncharacterised dongle at ±5 to 8 dB absolute, and the figure below is
 * a nominal value for a stock R820T2, not a measurement of your unit.
 * Characterise your own hardware against a signal generator and edit
 * this one number if you want the S-meter to mean anything.            */
#define RTL_REF_LEVEL_DBM_AT_0DB   (-10.0)
#define RTL_REF_LEVEL_ACCURACY_DB  8.0

/* ---- ADC level monitoring [RX-T2] ------------------------------------
 * The RTL2832U is an 8-bit offset-binary ADC; full scale is 127.5 counts
 * either side of the midpoint.                                          */
#define RTL_BIAS_TEE_VOLTS        5.0    /* RTL-SDR V3/V4 bias tee supply  */
#define RTL_ADC_FULL_SCALE        127.5
#define RTL_ADC_STRIDE            8      /* measure every Nth byte         */
#define RTL_ADC_CLIP_LOW          1      /* <= this counts as clipped      */
#define RTL_ADC_CLIP_HIGH         254    /* >= this counts as clipped      */
#define RTL_ADC_CLIP_PPM_TRIGGER  100    /* clipped samples per million    */
#define ADC_OVERLOAD_MIN_GAP_MS   1000   /* re-arm interval for the event  */

/* ---------- Internal ring buffer --------------------------------------- */
#define RING_SLOTS          2048        /* Power of 2                       */
#define RING_MASK           (RING_SLOTS - 1)
#define RING_SLOT_BYTES     (VITA_SAMPLES_PER_PKT * 2)

/* ---------- Framing ----------------------------------------------------- */
#define MAX_MSG_BYTES       65536
#define MAX_JSON_TOKENS     256


/* ---------- Logging ----------------------------------------------------- */
/* The tagged logger shared with sdr-slc-bladerfd (common/log.c): ISO-8601
 * UTC stamps, a level set by -v, optional syslog.  The LOG_* names every
 * module already uses are kept as thin wrappers so nothing else moved.  */
#include "common/log.h"
#define LOG_ERR(tag,...)  log_msg(LL_ERR,   tag, __VA_ARGS__)
#define LOG_WARN(tag,...) log_msg(LL_WARN,  tag, __VA_ARGS__)
#define LOG_INFO(tag,...) log_msg(LL_INFO,  tag, __VA_ARGS__)
#define LOG_DBG(tag,...)  log_msg(LL_DEBUG, tag, __VA_ARGS__)

#endif /* CONFIG_H */
