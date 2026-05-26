#ifndef RTL_BRIDGE_H
#define RTL_BRIDGE_H

/*
 * rtl_bridge — librtlsdr integration
 *
 * Owns the RTL-SDR device handle.  The rtl_callback fires at the USB
 * isochronous rate and calls vita_send_callback() directly — no ring buffer
 * in the data path.
 *
 * All rtl_set_* functions are thread-safe (protected by g_rtl_mutex).
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "../config.h"

/* -------------------------------------------------------------------------
 * Ring buffer slot (kept for diagnostics / future use, not in data path)
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t  data[RING_SLOT_BYTES];
    uint32_t len;
} RingSlot;

/* -------------------------------------------------------------------------
 * Global hardware state
 * --------------------------------------------------------------------- */
extern pthread_mutex_t  g_rtl_mutex;

extern volatile uint64_t      g_freq_hz;
extern volatile uint32_t      g_rate_sps;
extern volatile int           g_gain_tenth_db;
extern volatile bool          g_agc_enabled;
extern volatile bool          g_bias_tee_enabled;
extern volatile int           g_ppm_correction;
extern volatile sampling_mode_t g_sampling_mode;
extern volatile uint32_t      g_samples_per_pkt;

/* -------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
int  rtl_bridge_init(sampling_mode_t mode, bool bias_tee,
                     bool agc, int ppm);
void rtl_bridge_destroy(void);
int  rtl_bridge_start(void);
void rtl_bridge_stop(void);
void rtl_bridge_force_stop(void);

/* -------------------------------------------------------------------------
 * Hardware control (thread-safe)
 * --------------------------------------------------------------------- */
uint32_t rtl_set_frequency(uint64_t freq_hz);
uint32_t rtl_set_sample_rate(uint32_t rate_sps);
double   rtl_set_gain(double gain_db);
double   rtl_get_gain(void);
int      rtl_set_agc(bool enable);
int      rtl_set_bias_tee(bool enable);
int      rtl_set_ppm(int ppm);
int      rtl_get_gain_list(int *gain_tenth_db_out, int max_count);

/* -------------------------------------------------------------------------
 * Ring buffer (diagnostic only — not in hot send path)
 * --------------------------------------------------------------------- */
bool rtl_ring_read(RingSlot *slot_out);
int  rtl_ring_used(void);

/* -------------------------------------------------------------------------
 * Signal type string for open_stream response
 * --------------------------------------------------------------------- */
static inline const char *sampling_mode_signal_type(sampling_mode_t m) {
    switch (m) {
    case SAMPLING_DIRECT_I: return "real_i_branch";
    case SAMPLING_DIRECT_Q: return "real_q_branch";
    default:                return "complex_iq";
    }
}

/* Payload Format word 0 for current mode */
static inline uint32_t sampling_mode_payload_fmt(sampling_mode_t m) {
    return (m == SAMPLING_NORMAL) ? VITA_PAYLOAD_FMT_COMPLEX
                                  : VITA_PAYLOAD_FMT_REAL;
}

#endif /* RTL_BRIDGE_H */
