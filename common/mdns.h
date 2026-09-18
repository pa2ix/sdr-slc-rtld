/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* mdns.h - publish _sdr-slc._tcp.local. per SDR-SLC-CP-1.0 §6
 *
 * Same interface as sdr-slc-bladerfd's mdns.h, with a backend choice
 * underneath.  Two backends:
 *
 *   builtin  a self-contained RFC 6762/6763 responder on a raw multicast
 *            socket (mdns_builtin.c, from sdr-slc-rtld 0.17's disc.c).
 *            No dependencies.  Default.
 *   avahi    bladerfd's module on top of avahi-daemon (mdns_avahi.c).
 *            Needs libavahi-client at build time and avahi-daemon at
 *            runtime.  Compiled in with make MDNS=avahi or MDNS=both.
 *
 * Whichever is used, the TXT record is the eight keys §6.1 shows.        */
#ifndef SLC_MDNS_H
#define SLC_MDNS_H

#include <stdbool.h>
#include <stdint.h>

/* TXT record contents. Every field here is read by the reference client's
 * discovery module; omitting one degrades what the operator sees in the
 * device list, so none of them is optional in practice. */
typedef struct {
    const char *name;        /* instance name, echoed as a TXT key too   */
    const char *proto;       /* "sdr-slc"                                */
    const char *proto_ver;   /* "1.0"                                    */
    const char *functions;   /* comma-separated: "rx" or "rx,tx"         */
    const char *hw;          /* hardware revision                        */
    const char *fw;          /* firmware/daemon version                  */
    uint64_t    minhz;
    uint64_t    maxhz;
} mdns_txt_t;

/* Choose the backend before mdns_start(): "builtin" or "avahi".  iface is
 * the interface the builtin responder announces on (its IPv4 goes in the
 * A record); the Avahi backend publishes on every interface and ignores
 * it.  Returns -1 if that backend was not compiled in.                   */
int  mdns_set_backend(const char *name, const char *iface);
const char *mdns_backend(void);        /* the one in force                */
const char *mdns_backends_available(void);   /* "builtin" / "builtin, avahi" */

/* instance is the durable identity ("SLC-6107C0"); never auto-renamed. */
int  mdns_start(const char *instance, uint16_t port, const mdns_txt_t *txt);
void mdns_stop(void);
bool mdns_published(void);

/* Backend entry points (internal). */
int  mdns_builtin_start(const char *iface, const char *instance,
                        uint16_t port, const mdns_txt_t *txt);
void mdns_builtin_stop(void);
bool mdns_builtin_published(void);
#ifdef SLC_MDNS_AVAHI
int  mdns_avahi_start(const char *instance, uint16_t port, const mdns_txt_t *txt);
void mdns_avahi_stop(void);
bool mdns_avahi_published(void);
#endif

#endif
