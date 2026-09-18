/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#ifndef VITA_TX_H
#define VITA_TX_H

/*
 * vita_tx — VITA-A/1.0 IF Data + VITA-A/1.0-CTX Context packet sender
 *
 * Sending is driven directly from rtlsdr_read_async callback — no thread.
 * Context packets (SDR-SLC-VITA-1.0 Release 1.2 §7) are emitted before
 * the first IF Data packet per start_rx, after any gain change, and
 * after a signal mode change (§7.6).
 */

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include "../config.h"

typedef enum {
    IQ_FMT_INT16 = 0,
    IQ_FMT_U8    = 1,
} iq_format_t;

/* -------------------------------------------------------------------------
 * Session registration
 * --------------------------------------------------------------------- */
int  vita_session_add(const char    *stream_id,
                      uint32_t       vita_stream_id,
                      struct in_addr dest_ip,
                      uint16_t       dest_port,
                      iq_format_t    fmt,
                      uint32_t       payload_fmt_word0); /* mode-specific  */

void vita_session_remove(int session_token);

/* -------------------------------------------------------------------------
 * Context packet emission
 *
 * §24.3 requires a Context packet before the first IF Data packet after
 * start_rx, after any gain change, and after a signal mode change.
 * vita_send_context_all() covers the last two: it emits for every active
 * session, so a gain change reaches every listener at once.
 * --------------------------------------------------------------------- */
void vita_send_context_packet(int session_token);
void vita_send_context_all(void);

/* --no-context-packets: suppress VITA-A/1.0-CTX emission entirely.  On by
 * default; there is a single encoding now (SDR-SLC-VITA-1.0 R1.2 §7).  */
void vita_set_context_packets(bool enable);

/* Update the Payload Format word of every active session after a runtime
 * direct_sampling change.                                              */
void vita_update_payload_format(uint32_t payload_fmt_word0);

/* -------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
int  vita_tx_init(void);
void vita_tx_destroy(void);
int  vita_tx_start(void);   /* no-op — sending is callback-driven          */
void vita_tx_stop(void);    /* no-op                                        */

/* -------------------------------------------------------------------------
 * Called directly from rtl_callback() on every USB buffer
 * --------------------------------------------------------------------- */
void vita_send_callback(const uint8_t *buf, uint32_t len);

/* -------------------------------------------------------------------------
 * Packet assembly (also used for unit testing)
 * --------------------------------------------------------------------- */
#define VITA_HDR_BYTES      28
#define VITA_PKT_MAX_BYTES  (VITA_HDR_BYTES + VITA_SAMPLES_PER_PKT * 8)

/* Maximum packets per sendmmsg() call.
 * librtlsdr max callback buffer = 512 KB = 262144 IQ pairs.
 * At VITA_SAMPLES_PER_PKT=256: 262144/256 = 1024 packets maximum.       */
#define VITA_MMSG_MAX_PKTS  1024

size_t vita_build_if_data_packet(uint8_t       *pkt_out,
                                  uint32_t       stream_id,
                                  uint8_t        pkt_count,
                                  uint32_t       tsi,
                                  uint64_t       tsf_picos,
                                  const uint8_t *iq_u8,
                                  uint16_t       num_pairs,
                                  iq_format_t    fmt);

size_t vita_build_context_packet(uint8_t  *pkt_out,
                                  uint32_t  stream_id,
                                  uint8_t   pkt_count,
                                  uint32_t  payload_fmt_word0,
                                  double    reference_level_dbm,
                                  bool      changed);   /* CIF0 bit 31 */

/* VITA-49.0 §7.1.5.9 Reference Level encoding: Q9.7 dBm in the low
 * half-word, upper half zero (SDR-SLC-VITA-1.0 R1.2 §7.4).             */
uint32_t vita_encode_reference_level(double dbm);

#endif /* VITA_TX_H */
