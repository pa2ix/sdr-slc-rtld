/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* mdns.c - backend dispatcher; see mdns.h. */
#include "mdns.h"
#include "../config.h"

#include <string.h>

static bool s_avahi = false;
static char s_iface[32] = "";

int mdns_set_backend(const char *name, const char *iface)
{
    if (iface) snprintf(s_iface, sizeof s_iface, "%s", iface);
    if (!strcmp(name, "builtin")) { s_avahi = false; return 0; }
    if (!strcmp(name, "avahi")) {
#ifdef SLC_MDNS_AVAHI
        s_avahi = true;
        return 0;
#else
        return -1;
#endif
    }
    return -1;
}

const char *mdns_backend(void) { return s_avahi ? "avahi" : "builtin"; }

const char *mdns_backends_available(void)
{
#ifdef SLC_MDNS_AVAHI
    return "builtin, avahi";
#else
    return "builtin";
#endif
}

int mdns_start(const char *instance, uint16_t port, const mdns_txt_t *txt)
{
#ifdef SLC_MDNS_AVAHI
    if (s_avahi) return mdns_avahi_start(instance, port, txt);
#endif
    return mdns_builtin_start(s_iface, instance, port, txt);
}

void mdns_stop(void)
{
#ifdef SLC_MDNS_AVAHI
    if (s_avahi) { mdns_avahi_stop(); return; }
#endif
    mdns_builtin_stop();
}

bool mdns_published(void)
{
#ifdef SLC_MDNS_AVAHI
    if (s_avahi) return mdns_avahi_published();
#endif
    return mdns_builtin_published();
}
