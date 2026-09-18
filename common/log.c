/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "log.h"
#include "../config.h"   /* SLC_DAEMON_NAME */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static int  g_level = LL_INFO;
static bool g_syslog = false;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(int level, bool to_syslog)
{
    g_level = level;
    g_syslog = to_syslog;
    if (to_syslog)
        openlog(SLC_DAEMON_NAME, LOG_PID, LOG_DAEMON);
}

void log_set_level(int level) { g_level = level; }
int  log_level(void) { return g_level; }

static int to_syslog_prio(int level)
{
    switch (level) {
    case LL_ERR:  return LOG_ERR;
    case LL_WARN: return LOG_WARNING;
    case LL_INFO: return LOG_INFO;
    default:      return LOG_DEBUG;
    }
}

void log_msg(int level, const char *tag, const char *fmt, ...)
{
    if (level > g_level)
        return;

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    if (g_syslog) {
        syslog(to_syslog_prio(level), "[%s] %s", tag, body);
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char stamp[40];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);

    static const char *lvl[] = { "ERR ", "WARN", "INFO", "DBG ", "TRC " };

    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "%s.%03ldZ %s [%s] %s\n", stamp, ts.tv_nsec / 1000000L,
            lvl[level > LL_TRACE ? LL_TRACE : level], tag, body);
    fflush(stderr);
    pthread_mutex_unlock(&g_lock);
}
