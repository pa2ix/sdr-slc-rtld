/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/*
 * rtl-sdr.h — minimal shim for the no-dongle `make test-stub` build.
 *
 * This is NOT the real librtlsdr header.  It declares only the handful of
 * symbols the daemon and test/rtlsdr_stub.c use, so the control plane can
 * be exercised end-to-end on a build machine with no dongle and no
 * librtlsdr installed.  The production build (`make`) uses the real
 * <rtl-sdr.h> from librtlsdr-dev and links -lrtlsdr; it never sees this.
 */
#ifndef RTL_SDR_STUB_H
#define RTL_SDR_STUB_H

#include <stdint.h>

typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void (*rtlsdr_read_async_cb_t)(unsigned char *buf, uint32_t len,
                                       void *ctx);

int      rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index);
int      rtlsdr_close(rtlsdr_dev_t *dev);
int      rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq);
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev);
int      rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate);
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev);
int      rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual);
int      rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain);
int      rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev);
int      rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains);
int      rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on);
int      rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on);
int      rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on);
int      rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm);
int      rtlsdr_reset_buffer(rtlsdr_dev_t *dev);
int      rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb,
                           void *ctx, uint32_t buf_num, uint32_t buf_len);
int      rtlsdr_cancel_async(rtlsdr_dev_t *dev);

#endif /* RTL_SDR_STUB_H */
