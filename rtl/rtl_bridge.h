/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

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
extern volatile uint32_t      g_rate_sps;        /* achieved rate       */
extern volatile uint32_t      g_rate_requested_sps; /* as asked for      */
extern volatile int           g_gain_tenth_db;
extern volatile bool          g_agc_enabled;
extern volatile bool          g_bias_tee_enabled;
extern volatile int           g_ppm_correction;
extern volatile sampling_mode_t g_sampling_mode;
extern volatile uint32_t      g_samples_per_pkt;

/* Startup bias tee value, in volts.  SLC-CP-1.0 R1.2 §13.3 requires
 * bias_tee to fall back to 0.0 V at startup and after any TCP
 * disconnect.  The operator can override the post-disconnect value with
 * --bias-tee-persist for permanently powered LNA installations; that is
 * a documented deviation and is logged as one at startup.              */
extern volatile double        g_bias_tee_volts;
extern volatile bool          g_bias_tee_persist;

/* ADC monitoring [RX-T2].  Updated from the USB callback thread.       */
extern volatile bool          g_adc_overload;

/* -------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
int  rtl_bridge_init(sampling_mode_t mode, double bias_tee_volts,
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
int      rtl_set_bias_tee(double volts);
int      rtl_set_ppm(int ppm);
int      rtl_set_direct_sampling(sampling_mode_t mode);
int      rtl_get_gain_list(int *gain_tenth_db_out, int max_count);

/* -------------------------------------------------------------------------
 * ADC level and Reference Level [RX-T2]
 * --------------------------------------------------------------------- */

/* Most recent measured ADC level in dBFS.  Returns false (and leaves
 * *out untouched) when nothing has been measured yet, which is what
 * get_status reports as JSON null.                                      */
bool   rtl_get_adc_level_dbfs(double *out);

/* Reset the ADC measurement to "no data" — called when streaming stops. */
void   rtl_reset_adc_level(void);

/* Register a callback fired at the onset of ADC clipping.  Fired from
 * the USB callback thread, rate-limited to ADC_OVERLOAD_MIN_GAP_MS.     */
typedef void (*adc_overload_cb_t)(double adc_level_dbfs);
void   rtl_set_overload_handler(adc_overload_cb_t cb);

/* Frequency range and sample rate list for the mode currently in force. */
uint64_t        rtl_freq_min(void);
uint64_t        rtl_freq_max(void);
const uint32_t *rtl_rate_list(int *count_out);
uint32_t        rtl_preferred_rate(void);

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

/* -------------------------------------------------------------------------
 * Reference Level [RX-T2]
 *
 * §19: the power at the antenna port corresponding to a full-scale I/Q
 * sample.  It tracks gain — 10 dB more gain means a 10 dB weaker signal
 * saturates the ADC, so the Reference Level drops by 10 dB.
 * --------------------------------------------------------------------- */
static inline double rtl_reference_level_dbm(double gain_db) {
    return RTL_REF_LEVEL_DBM_AT_0DB - gain_db;
}

/* Payload Format word 0 for current mode */
static inline uint32_t sampling_mode_payload_fmt(sampling_mode_t m) {
    return (m == SAMPLING_NORMAL) ? VITA_PAYLOAD_FMT_COMPLEX
                                  : VITA_PAYLOAD_FMT_REAL;
}

#endif /* RTL_BRIDGE_H */
