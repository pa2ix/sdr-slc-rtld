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
volatile int         g_gain_tenth_db    = RTL_DEFAULT_GAIN;
volatile bool        g_agc_enabled      = false;
volatile bool        g_bias_tee_enabled = false;
volatile int         g_ppm_correction   = 0;
volatile sampling_mode_t g_sampling_mode = SAMPLING_NORMAL;
volatile uint32_t    g_samples_per_pkt  = VITA_SAMPLES_PER_PKT;

static rtlsdr_dev_t   *s_dev          = NULL;
static pthread_t       s_async_tid;
static volatile bool   s_running      = false;
static _Atomic int     s_stream_count = 0;
static pthread_mutex_t s_start_mutex  = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        s_cb_count     = 0;

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
    vita_send_callback(buf, len);
}

static void *rtl_async_thread(void *arg) {
    (void)arg;
    LOG_INFO("rtl", "async read thread started");
    int r = rtlsdr_read_async(s_dev, rtl_callback, NULL, 0, 0);
    if (r < 0 && s_running)
        LOG_ERR("rtl", "rtlsdr_read_async returned %d", r);
    LOG_INFO("rtl", "async read thread exiting");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
int rtl_bridge_init(sampling_mode_t mode, bool bias_tee,
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

    /* Bias tee */
    if (rtlsdr_set_bias_tee(s_dev, bias_tee ? 1 : 0) == 0) {
        g_bias_tee_enabled = bias_tee;
    } else {
        LOG_WARN("rtl", "bias_tee not supported by this librtlsdr build");
        g_bias_tee_enabled = false;
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
    pthread_mutex_unlock(&g_rtl_mutex);
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

int rtl_set_bias_tee(bool enable) {
    if (!s_dev) return -1;
    pthread_mutex_lock(&g_rtl_mutex);
    int r = rtlsdr_set_bias_tee(s_dev, enable ? 1 : 0);
    if (r == 0) g_bias_tee_enabled = enable;
    pthread_mutex_unlock(&g_rtl_mutex);
    if (r == 0)
        LOG_INFO("rtl", "bias_tee %s", enable ? "enabled" : "disabled");
    else
        LOG_WARN("rtl", "bias_tee not supported (ret=%d)", r);
    return r;
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
