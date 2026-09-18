/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/*
 * rtlsdr_stub.c — a fake librtlsdr for protocol testing.
 *
 * Lets the daemon run end-to-end with no dongle attached so that the
 * SLC-CP wire format can be exercised on a build machine.  It models the
 * two behaviours that matter to the control plane: the tuner's discrete
 * gain table, and the RTL2832U clock divider, which cannot reach every
 * requested sample rate exactly.  That second one is why
 * actual_sample_rate_sps exists in §18, so a stub that returned the
 * requested rate verbatim would test nothing.
 *
 * Build with:  make test-stub
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>





static int      s_open        = 0;
static uint32_t s_freq        = 100000000;
static uint32_t s_rate        = 2048000;
static int      s_gain        = 200;
static int      s_gain_mode   = 1;
static int      s_agc         = 0;
static int      s_bias        = 0;
static int      s_ppm         = 0;
static int      s_ds          = 0;
static volatile int s_async   = 0;

/* R820T2 gain table, tenths of a dB. */
static const int s_gains[] = {
      0,  9,  14,  27,  37,  77, 87, 125, 144, 157, 166, 197, 207, 229,
    254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480,
    496
};
#define NGAINS ((int)(sizeof(s_gains) / sizeof(s_gains[0])))

#include <rtl-sdr.h>
int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index) {
    (void)index;
    s_open = 1;
    *dev = (rtlsdr_dev_t *)&s_open;
    return 0;
}
int rtlsdr_close(rtlsdr_dev_t *dev) { (void)dev; s_open = 0; return 0; }

int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq) {
    (void)dev;
    /* PLL step: round to the nearest 1 Hz — the R820T2 is fine enough
     * that the control plane sees the requested value.                 */
    s_freq = freq;
    return 0;
}
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev) { (void)dev; return s_freq; }

int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate) {
    (void)dev;
    /* RTL2832U: rate = 28.8 MHz / ratio, ratio in 1/4 Hz units.        */
    uint32_t ratio = (uint32_t)((28800000.0 * 4.0) / rate + 0.5) & ~3u;
    if (ratio == 0) ratio = 4;
    s_rate = (uint32_t)((28800000.0 * 4.0) / ratio + 0.5);
    return 0;
}
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev) { (void)dev; return s_rate; }

int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual) {
    (void)dev; s_gain_mode = manual; return 0;
}
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain) {
    (void)dev;
    int best = s_gains[0];
    for (int i = 0; i < NGAINS; i++)
        if (abs(s_gains[i] - gain) < abs(best - gain)) best = s_gains[i];
    s_gain = best;
    return 0;
}
int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev) { (void)dev; return s_gain; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains) {
    (void)dev;
    if (gains) memcpy(gains, s_gains, sizeof(s_gains));
    return NGAINS;
}
int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on) {
    (void)dev; s_agc = on; return 0;
}
int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on) {
    (void)dev; s_bias = on; return 0;
}
int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm) {
    (void)dev; s_ppm = ppm; return 0;
}
int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on) {
    (void)dev; s_ds = on; return 0;
}
int rtlsdr_reset_buffer(rtlsdr_dev_t *dev) { (void)dev; return 0; }

/* Synthesise a noise-floor buffer so the ADC level meter has something
 * real to measure.  Amplitude is scaled by the current gain so that a
 * high gain setting genuinely clips and fires adc_overload.            */
int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb,
                      void *ctx, uint32_t buf_num, uint32_t buf_len) {
    (void)dev; (void)buf_num;
    if (buf_len == 0) buf_len = 262144;
    unsigned char *buf = malloc(buf_len);
    if (!buf) return -1;

    s_async = 1;
    unsigned seed = 12345;
    double amp = 6.0 * pow(10.0, (s_gain / 10.0) / 20.0) / 10.0;

    while (s_async) {
        for (uint32_t i = 0; i < buf_len; i++) {
            seed = seed * 1103515245u + 12345u;
            double n = ((double)((seed >> 16) & 0x7FFF) / 16384.0) - 1.0;
            double v = 127.4 + n * amp;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            buf[i] = (unsigned char)v;
        }
        cb(buf, buf_len, ctx);
        /* Pace by the actual sample rate and buffer size, so the stub
         * delivers buffers at the same cadence real hardware would.  This
         * lets burst-pacing behaviour (RTL_ASYNC_BUF_LEN) be exercised
         * realistically: a smaller buf_len -> more frequent, smaller
         * callbacks at the same overall sample rate.                      */
        uint32_t rate = s_rate ? s_rate : 2048000u;
        uint64_t pairs = buf_len / 2;
        uint64_t us = pairs * 1000000ULL / rate;
        if (us == 0) us = 1;
        usleep((useconds_t)us);
    }
    free(buf);
    return 0;
}

int rtlsdr_cancel_async(rtlsdr_dev_t *dev) { (void)dev; s_async = 0; return 0; }
