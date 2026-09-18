/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "rtl_bridge.h"
#include "../vita/vita_tx.h"
#include <rtl-sdr.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Globals
 * --------------------------------------------------------------------- */
pthread_mutex_t      g_rtl_mutex        = PTHREAD_MUTEX_INITIALIZER;
volatile uint64_t    g_freq_hz          = RTL_DEFAULT_FREQ;
volatile uint32_t    g_rate_sps         = RTL_DEFAULT_RATE_NORMAL;
volatile uint32_t    g_rate_requested_sps = RTL_DEFAULT_RATE_NORMAL;
volatile int         g_gain_tenth_db    = RTL_DEFAULT_GAIN;
volatile bool        g_agc_enabled      = false;
volatile bool        g_bias_tee_enabled = false;
volatile int         g_ppm_correction   = 0;
volatile sampling_mode_t g_sampling_mode = SAMPLING_NORMAL;
volatile uint32_t    g_samples_per_pkt  = VITA_SAMPLES_PER_PKT;
volatile double      g_bias_tee_volts   = 0.0;
volatile bool        g_bias_tee_persist = false;
volatile bool        g_adc_overload     = false;

static rtlsdr_dev_t   *s_dev          = NULL;
static pthread_t       s_async_tid;
static volatile bool   s_running      = false;
static _Atomic int     s_stream_count = 0;
static pthread_mutex_t s_start_mutex  = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        s_cb_count     = 0;

/* ---- ADC level measurement [RX-T2] ----------------------------------
 * The hot path (measure_adc_level, on the USB callback thread) now stores
 * only the raw integer sum-of-squares and the sample count.  The sqrt and
 * log10 that turn those into dBFS are deferred to rtl_get_adc_level_dbfs(),
 * which runs on the control-plane thread when the client polls get_status.
 *
 * This is the fix for the v15 regression: v15 ran a double-precision loop
 * plus sqrt and log10 on every USB buffer, inside the callback, ahead of
 * the sender.  On a Raspberry Pi that stretched the callback past the USB
 * deadline and libusb dropped whole buffers — the packet loss and stutter.
 * Keeping the hot path integer-only (and the transcendentals off it) is
 * what keeps the callback short enough to always return in time.        */
static _Atomic uint64_t s_adc_sumsq       = 0;      /* sum of (v-128)^2   */
static _Atomic uint32_t s_adc_n           = 0;      /* samples in sumsq   */
static _Atomic bool     s_adc_level_valid = false;
static _Atomic uint64_t s_last_overload_ms = 0;
static adc_overload_cb_t s_overload_cb    = NULL;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

void rtl_set_overload_handler(adc_overload_cb_t cb) { s_overload_cb = cb; }

bool rtl_get_adc_level_dbfs(double *out) {
    if (!atomic_load_explicit(&s_adc_level_valid, memory_order_relaxed))
        return false;
    uint64_t sumsq = atomic_load_explicit(&s_adc_sumsq, memory_order_relaxed);
    uint32_t n     = atomic_load_explicit(&s_adc_n,     memory_order_relaxed);
    if (n == 0) return false;
    if (out) {
        /* Deferred transcendental — runs here (control-plane thread on a
         * get_status poll), never on the USB callback hot path.          */
        double rms  = sqrt((double)sumsq / (double)n);
        double dbfs = (rms > 0.0) ? 20.0 * log10(rms / RTL_ADC_FULL_SCALE)
                                  : -120.0;
        if (dbfs < -120.0) dbfs = -120.0;
        *out = dbfs;
    }
    return true;
}

void rtl_reset_adc_level(void) {
    atomic_store_explicit(&s_adc_level_valid, false, memory_order_relaxed);
    atomic_store_explicit(&s_adc_sumsq, 0, memory_order_relaxed);
    atomic_store_explicit(&s_adc_n,     0, memory_order_relaxed);
    g_adc_overload = false;
}

/* -------------------------------------------------------------------------
 * Measure RMS level and clipping over one USB buffer.
 *
 * The buffer is offset binary around 127.4 (see vita_tx.c for why that
 * number and not 127.5).  Every RTL_ADC_STRIDE'th byte is sampled: at
 * 2.048 MSPS that is still ~32 k samples per buffer, far more than
 * enough for a level meter, and it keeps the cost off the hot path.
 *
 * §21.3: adc_overload is emitted at the onset of clipping, not
 * continuously, so the event is edge-triggered and rate-limited.
 * --------------------------------------------------------------------- */
static void measure_adc_level(const uint8_t *buf, uint32_t len) {
    if (len == 0) return;

    /* Integer-only accumulation.  The 0.4-LSB difference between the true
     * ADC midpoint (~127.4) and the integer 128 used here is immaterial
     * for a level meter and buys us an all-integer inner loop with no
     * per-sample float work.                                             */
    uint64_t sumsq   = 0;
    uint32_t n       = 0;
    uint32_t clipped = 0;

    for (uint32_t i = 0; i < len; i += RTL_ADC_STRIDE) {
        int v = (int)buf[i];
        int d = v - 128;                 /* -128 .. +127                  */
        sumsq += (uint64_t)(d * d);      /* max 16384 * n, fits uint64    */
        n++;
        if (v <= RTL_ADC_CLIP_LOW || v >= RTL_ADC_CLIP_HIGH) clipped++;
    }
    if (n == 0) return;

    /* Publish the raw integers; dBFS is computed later in
     * rtl_get_adc_level_dbfs() on the control-plane thread.             */
    atomic_store_explicit(&s_adc_sumsq, sumsq, memory_order_relaxed);
    atomic_store_explicit(&s_adc_n,     n,     memory_order_relaxed);
    atomic_store_explicit(&s_adc_level_valid, true, memory_order_relaxed);

    /* Overload detection is integer (one divide, some compares).        */
    uint32_t clip_ppm = (uint32_t)((uint64_t)clipped * 1000000ULL / n);
    bool overload = (clip_ppm >= RTL_ADC_CLIP_PPM_TRIGGER);
    bool was      = g_adc_overload;
    g_adc_overload = overload;

    if (overload && !was && s_overload_cb) {
        uint64_t t = now_ms();
        uint64_t last = atomic_load_explicit(&s_last_overload_ms,
                                             memory_order_relaxed);
        if (t - last >= ADC_OVERLOAD_MIN_GAP_MS) {
            atomic_store_explicit(&s_last_overload_ms, t,
                                  memory_order_relaxed);
            /* sqrt/log10 only here — on the rare, rate-limited overload
             * edge (at most once per ADC_OVERLOAD_MIN_GAP_MS), never per
             * buffer.  This is the one place the transcendental is still
             * acceptable because it cannot fire on the steady hot path. */
            double rms  = sqrt((double)sumsq / (double)n);
            double dbfs = (rms > 0.0) ? 20.0 * log10(rms / RTL_ADC_FULL_SCALE)
                                      : -120.0;
            s_overload_cb(dbfs);
        }
    }
}

/* -------------------------------------------------------------------------
 * Ring buffer (diagnostic only)
 * --------------------------------------------------------------------- */
static RingSlot         s_ring[RING_SLOTS];
static _Atomic uint32_t s_ring_write = 0;
static _Atomic uint32_t s_ring_read  = 0;

bool rtl_ring_read(RingSlot *slot_out) {
    uint32_t r = atomic_load_explicit(&s_ring_read,  memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&s_ring_write, memory_order_acquire);
    if (r == w) return false;
    *slot_out = s_ring[r & RING_MASK];
    atomic_fetch_add_explicit(&s_ring_read, 1, memory_order_release);
    return true;
}

int rtl_ring_used(void) {
    uint32_t w = atomic_load_explicit(&s_ring_write, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&s_ring_read,  memory_order_relaxed);
    return (int)((w - r) & RING_MASK);
}

/* -------------------------------------------------------------------------
 * Callback — this IS the clock.  Called at USB isochronous rate.
 * --------------------------------------------------------------------- */
static void rtl_callback(unsigned char *buf, uint32_t len, void *ctx) {
    (void)ctx;
    if (!s_running) return;
    s_cb_count++;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    LOG_DBG("rtl", "cb#%llu t=%ld.%06ld len=%u pairs=%u",
            (unsigned long long)s_cb_count,
            (long)ts.tv_sec, ts.tv_nsec / 1000,
            len, len / 2);

    g_samples_per_pkt = VITA_SAMPLES_PER_PKT;
    measure_adc_level(buf, len);
    vita_send_callback(buf, len);
}

static void *rtl_async_thread(void *arg) {
    (void)arg;
    LOG_INFO("rtl", "async read thread started (buf_num=%u buf_len=%u "
             "-> %u pkts/burst)",
             RTL_ASYNC_BUF_NUM, RTL_ASYNC_BUF_LEN,
             RTL_ASYNC_BUF_LEN / (VITA_SAMPLES_PER_PKT * 2));
    /* Small buffers pace the output into frequent little bursts the client
     * can absorb; see RTL_ASYNC_BUF_LEN in config.h for the reasoning.    */
    int r = rtlsdr_read_async(s_dev, rtl_callback, NULL,
                              RTL_ASYNC_BUF_NUM, RTL_ASYNC_BUF_LEN);
    if (r < 0 && s_running)
        LOG_ERR("rtl", "rtlsdr_read_async returned %d", r);
    LOG_INFO("rtl", "async read thread exiting");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
int rtl_bridge_init(sampling_mode_t mode, double bias_tee_volts,
                    bool agc, int ppm) {
    int r = rtlsdr_open(&s_dev, RTL_DEVICE_INDEX);
    if (r < 0) {
        LOG_ERR("rtl", "rtlsdr_open(%d) failed: %d", RTL_DEVICE_INDEX, r);
        return -1;
    }

    g_sampling_mode = mode;

    /* Apply sampling mode */
    int ds_mode = 0;
    if      (mode == SAMPLING_DIRECT_I) ds_mode = 1;
    else if (mode == SAMPLING_DIRECT_Q) ds_mode = 2;
    rtlsdr_set_direct_sampling(s_dev, ds_mode);

    /* Default sample rate by mode */
    uint32_t default_rate = (mode == SAMPLING_NORMAL)
                            ? RTL_DEFAULT_RATE_NORMAL
                            : RTL_DEFAULT_RATE_DIRECT;
    rtlsdr_set_sample_rate(s_dev, default_rate);
    g_rate_requested_sps = default_rate;
    g_rate_sps = rtlsdr_get_sample_rate(s_dev);

    /* Frequency */
    rtlsdr_set_center_freq(s_dev, (uint32_t)RTL_DEFAULT_FREQ);
    g_freq_hz = rtlsdr_get_center_freq(s_dev);

    /* Gain */
    rtlsdr_set_tuner_gain_mode(s_dev, agc ? 0 : 1);
    if (!agc) rtlsdr_set_tuner_gain(s_dev, RTL_DEFAULT_GAIN);
    rtlsdr_set_agc_mode(s_dev, agc ? 1 : 0);
    g_agc_enabled = agc;
    g_gain_tenth_db = rtlsdr_get_tuner_gain(s_dev);

    /* Bias tee.  §13.3 makes 0.0 V the startup default; a non-zero value
     * here is an explicit operator override via --bias-tee.             */
    if (rtlsdr_set_bias_tee(s_dev, (bias_tee_volts > 0.0) ? 1 : 0) == 0) {
        g_bias_tee_enabled = (bias_tee_volts > 0.0);
        g_bias_tee_volts   = g_bias_tee_enabled ? RTL_BIAS_TEE_VOLTS : 0.0;
    } else {
        LOG_WARN("rtl", "bias_tee not supported by this librtlsdr build");
        g_bias_tee_enabled = false;
        g_bias_tee_volts   = 0.0;
    }

    /* PPM correction */
    rtlsdr_set_freq_correction(s_dev, ppm);
    g_ppm_correction = ppm;

    rtlsdr_reset_buffer(s_dev);

    static const char *mode_names[] = {"normal","direct-i","direct-q"};
    LOG_INFO("rtl", "opened device %d: mode=%s freq=%.3f MHz rate=%u sps "
             "gain=%.1f dB agc=%s bias_tee=%s ppm=%d",
             RTL_DEVICE_INDEX, mode_names[mode],
             (double)g_freq_hz / 1e6, g_rate_sps,
             g_gain_tenth_db / 10.0,
             g_agc_enabled ? "on" : "off",
             g_bias_tee_enabled ? "on" : "off",
             g_ppm_correction);
    return 0;
}

void rtl_bridge_destroy(void) {
    if (s_dev) { rtlsdr_close(s_dev); s_dev = NULL; }
}

int rtl_bridge_start(void) {
    pthread_mutex_lock(&s_start_mutex);
    int count = atomic_fetch_add_explicit(&s_stream_count, 1,
                                          memory_order_relaxed) + 1;
    if (count == 1) {
        atomic_store_explicit(&s_ring_write, 0, memory_order_relaxed);
        atomic_store_explicit(&s_ring_read,  0, memory_order_relaxed);
        s_cb_count = 0;
        rtl_reset_adc_level();
        rtlsdr_reset_buffer(s_dev);
        s_running = true;
        int r = pthread_create(&s_async_tid, NULL, rtl_async_thread, NULL);
        if (r != 0) {
            LOG_ERR("rtl", "pthread_create failed: %s", strerror(r));
            s_running = false;
            atomic_fetch_sub_explicit(&s_stream_count, 1,
                                      memory_order_relaxed);
            pthread_mutex_unlock(&s_start_mutex);
            return -1;
        }
        LOG_INFO("rtl", "RTL async read started (stream_count=1)");
    }
    pthread_mutex_unlock(&s_start_mutex);
    return 0;
}

void rtl_bridge_stop(void) {
    pthread_mutex_lock(&s_start_mutex);
    int count = atomic_fetch_sub_explicit(&s_stream_count, 1,
                                          memory_order_relaxed) - 1;
    if (count < 0) {
        atomic_store_explicit(&s_stream_count, 0, memory_order_relaxed);
        count = 0;
    }
    if (count == 0 && s_running) {
        s_running = false;
        if (s_dev) rtlsdr_cancel_async(s_dev);
        pthread_mutex_unlock(&s_start_mutex);
        pthread_join(s_async_tid, NULL);
        LOG_INFO("rtl", "RTL async read stopped (no active streams)");
        return;
    }
    pthread_mutex_unlock(&s_start_mutex);
}

void rtl_bridge_force_stop(void) {
    atomic_store_explicit(&s_stream_count, 0, memory_order_relaxed);
    if (s_running) {
        s_running = false;
        if (s_dev) rtlsdr_cancel_async(s_dev);
        pthread_join(s_async_tid, NULL);
    }
}

/* -------------------------------------------------------------------------
 * Hardware control
 * --------------------------------------------------------------------- */
uint32_t rtl_set_frequency(uint64_t freq_hz) {
    if (!s_dev) return 0;
    pthread_mutex_lock(&g_rtl_mutex);
    rtlsdr_set_center_freq(s_dev, (uint32_t)freq_hz);
    uint32_t applied = rtlsdr_get_center_freq(s_dev);
    if (applied) g_freq_hz = applied;
    pthread_mutex_unlock(&g_rtl_mutex);
    LOG_INFO("rtl", "frequency set to %u Hz (requested %" PRIu64 ")",
             applied, freq_hz);
    return applied;
}

uint32_t rtl_set_sample_rate(uint32_t rate_sps) {
    if (!s_dev) return 0;
    pthread_mutex_lock(&g_rtl_mutex);
    rtlsdr_set_sample_rate(s_dev, rate_sps);
    uint32_t applied = rtlsdr_get_sample_rate(s_dev);
    if (applied) g_rate_sps = applied;
    g_rate_requested_sps = rate_sps;
    pthread_mutex_unlock(&g_rtl_mutex);
    /* §18.2: the RTL2832U clock divider cannot reach every rate exactly.
     * The difference is small enough to miss and large enough to put every
     * signal at the wrong place on a client's frequency axis, which is why
     * actual_sample_rate_sps is reported separately in get_status.       */
    if (applied != rate_sps)
        LOG_INFO("rtl", "sample rate %u sps requested, %u sps achieved",
                 rate_sps, applied);
    else
        LOG_INFO("rtl", "sample rate set to %u sps", applied);
    return applied;
}

double rtl_set_gain(double gain_db) {
    if (!s_dev) return -1.0;
    int tenth = (int)round(gain_db * 10.0);
    pthread_mutex_lock(&g_rtl_mutex);
    rtlsdr_set_tuner_gain_mode(s_dev, 1);
    int r = rtlsdr_set_tuner_gain(s_dev, tenth);
    int applied = rtlsdr_get_tuner_gain(s_dev);
    g_gain_tenth_db = applied;
    pthread_mutex_unlock(&g_rtl_mutex);
    LOG_DBG("rtl", "rtlsdr_set_tuner_gain(%d) ret=%d applied=%d (%.1f dB)",
            tenth, r, applied, applied / 10.0);
    return applied / 10.0;
}

double rtl_get_gain(void) { return g_gain_tenth_db / 10.0; }

int rtl_set_agc(bool enable) {
    if (!s_dev) return -1;
    pthread_mutex_lock(&g_rtl_mutex);
    rtlsdr_set_agc_mode(s_dev, enable ? 1 : 0);
    if (!enable) {
        rtlsdr_set_tuner_gain_mode(s_dev, 1);
        rtlsdr_set_tuner_gain(s_dev, g_gain_tenth_db);
    } else {
        rtlsdr_set_tuner_gain_mode(s_dev, 0);
    }
    g_agc_enabled = enable;
    pthread_mutex_unlock(&g_rtl_mutex);
    LOG_INFO("rtl", "AGC %s", enable ? "enabled" : "disabled");
    return 0;
}

/* §11.3: bias_tee is a float control in volts.  The RTL-SDR V3/V4 bias
 * tee is a fixed 4.5-5 V supply with no level control, so the only two
 * legal values are 0.0 and RTL_BIAS_TEE_VOLTS — which is exactly what
 * allowed_values in the control descriptor advertises.                 */
int rtl_set_bias_tee(double volts) {
    if (!s_dev) return -1;
    bool enable = (volts > 0.0);
    pthread_mutex_lock(&g_rtl_mutex);
    int r = rtlsdr_set_bias_tee(s_dev, enable ? 1 : 0);
    if (r == 0) {
        g_bias_tee_enabled = enable;
        g_bias_tee_volts   = enable ? RTL_BIAS_TEE_VOLTS : 0.0;
    }
    pthread_mutex_unlock(&g_rtl_mutex);
    if (r == 0)
        LOG_INFO("rtl", "bias_tee %.1f V", g_bias_tee_volts);
    else
        LOG_WARN("rtl", "bias_tee not supported (ret=%d)", r);
    return r;
}

/* -------------------------------------------------------------------------
 * direct_sampling [RX-T2]
 *
 * §11.3 lists direct_sampling as a standard Tier 2 enum control, so it
 * has to be settable at runtime and not only from the command line.
 * Switching branches changes the frequency range and the usable sample
 * rate list, so the caller (cp_commands) refuses the change while a
 * stream session is open and re-clamps frequency and rate afterwards.
 * --------------------------------------------------------------------- */
int rtl_set_direct_sampling(sampling_mode_t mode) {
    if (!s_dev) return -1;
    int ds = (mode == SAMPLING_DIRECT_I) ? 1
           : (mode == SAMPLING_DIRECT_Q) ? 2 : 0;
    pthread_mutex_lock(&g_rtl_mutex);
    int r = rtlsdr_set_direct_sampling(s_dev, ds);
    if (r == 0) g_sampling_mode = mode;
    pthread_mutex_unlock(&g_rtl_mutex);
    if (r == 0) {
        static const char *names[] = {"off", "i_branch", "q_branch"};
        LOG_INFO("rtl", "direct_sampling = %s", names[mode]);
    } else {
        LOG_WARN("rtl", "rtlsdr_set_direct_sampling(%d) failed: %d", ds, r);
    }
    return r;
}

/* -------------------------------------------------------------------------
 * Mode-dependent limits.  Kept here so that cp_commands and disc read the
 * same numbers from one place after a runtime direct_sampling change.
 * --------------------------------------------------------------------- */
uint64_t rtl_freq_min(void) {
    return (g_sampling_mode == SAMPLING_NORMAL)
           ? RTL_FREQ_MIN_NORMAL : RTL_FREQ_MIN_DIRECT;
}

uint64_t rtl_freq_max(void) {
    return (g_sampling_mode == SAMPLING_NORMAL)
           ? RTL_FREQ_MAX_NORMAL : RTL_FREQ_MAX_DIRECT;
}

const uint32_t *rtl_rate_list(int *count_out) {
    if (g_sampling_mode == SAMPLING_NORMAL) {
        if (count_out) *count_out = RTL_RATE_COUNT_NORMAL;
        return RTL_SAMPLE_RATES_NORMAL;
    }
    if (count_out) *count_out = RTL_RATE_COUNT_DIRECT;
    return RTL_SAMPLE_RATES_DIRECT;
}

uint32_t rtl_preferred_rate(void) {
    return (g_sampling_mode == SAMPLING_NORMAL)
           ? RTL_DEFAULT_RATE_NORMAL : RTL_DEFAULT_RATE_DIRECT;
}

int rtl_set_ppm(int ppm) {
    if (!s_dev) return -1;
    pthread_mutex_lock(&g_rtl_mutex);
    int r = rtlsdr_set_freq_correction(s_dev, ppm);
    if (r == 0) g_ppm_correction = ppm;
    pthread_mutex_unlock(&g_rtl_mutex);
    LOG_INFO("rtl", "PPM correction set to %d (ret=%d)", ppm, r);
    return r;
}

int rtl_get_gain_list(int *gain_tenth_db_out, int max_count) {
    if (!s_dev) return 0;
    (void)max_count;
    return rtlsdr_get_tuner_gains(s_dev, gain_tenth_db_out);
}
