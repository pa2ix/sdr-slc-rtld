#ifndef VITA_TX_H
#define VITA_TX_H

/*
 * vita_tx — VITA-A/1.0 IF Data + VITA-A/1.0-CTX Context packet sender
 *
 * Sending is driven directly from rtlsdr_read_async callback — no thread.
 * Context packets are emitted once before the first IF Data packet per
 * start_rx, and again on any mode change, per the VITA-SLC spec §7.3.
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
 * Context packet emission — call once from start_rx before data flows
 * --------------------------------------------------------------------- */
void vita_send_context_packet(int session_token);

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
                                  uint32_t  payload_fmt_word0);

#endif /* VITA_TX_H */
