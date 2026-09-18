/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* log.h - tagged logging: "2026-08-18T09:12:03.114Z [SLC] ..." */
#ifndef SLC_LOG_H
#define SLC_LOG_H

#include <stdbool.h>

enum { LL_ERR = 0, LL_WARN = 1, LL_INFO = 2, LL_DEBUG = 3, LL_TRACE = 4 };

void log_init(int level, bool to_syslog);
void log_set_level(int level);
int  log_level(void);
void log_msg(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Tags are deliberately short and greppable; the operator pastes these back. */
#define T_SLC   "SLC"
#define T_RADIO "RADIO"
#define T_VITA  "VITA"
#define T_RX    "RX"
#define T_TX    "TX"
#define T_MDNS  "MDNS"
#define T_CTRL  "CTRL"

#define LOGE(tag, ...) log_msg(LL_ERR,   tag, __VA_ARGS__)
#define LOGW(tag, ...) log_msg(LL_WARN,  tag, __VA_ARGS__)
#define LOGI(tag, ...) log_msg(LL_INFO,  tag, __VA_ARGS__)
#define LOGD(tag, ...) log_msg(LL_DEBUG, tag, __VA_ARGS__)
#define LOGT(tag, ...) log_msg(LL_TRACE, tag, __VA_ARGS__)

#endif
