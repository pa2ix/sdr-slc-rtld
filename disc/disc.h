#ifndef DISC_H
#define DISC_H

/*
 * disc — SLC-DISC service discovery (RFC 6762 mDNS / RFC 6763 DNS-SD)
 *
 * Implements:
 *   - IGMP join for 224.0.0.251 on startup
 *   - Unsolicited mDNS announcement on link-up (PTR + SRV + TXT + A)
 *   - Response to PTR queries for _sdr-slc._tcp.local.
 *   - Goodbye packets (TTL=0) on clean shutdown
 *
 * Does NOT require libavahi — uses raw UDP multicast sockets.
 */

#include <stdint.h>
#include <netinet/in.h>

/*
 * Resolved device name — set by disc_init(), readable by cp_commands.c
 * for use in the hello response device_name field.
 */
extern char g_device_name[64];

/*
 * Initialise the mDNS responder.
 *   iface_name  — e.g. "eth0"
 *   ctrl_port   — SLC-CP TCP port (usually 4620)
 *   custom_name — human-readable device name, or NULL to use MAC-derived
 *                 default (SLC-XXYYZZ).  Spaces replaced with hyphens.
 *                 Max 63 characters (DNS label limit).
 *
 * Returns 0 on success, -1 on error.
 */
int  disc_init(const char *iface_name, uint16_t ctrl_port,
               const char *custom_name);

/*
 * Send the initial mDNS announcement (call once after disc_init).
 * Blocks for MDNS_LINK_DELAY_MS to let the switch forwarding table update.
 */
void disc_announce(void);

/*
 * Start the mDNS query-response background thread.
 * Returns 0 on success.
 */
int  disc_start(void);

/*
 * Send goodbye packets (TTL=0) and stop the background thread.
 */
void disc_stop(void);

#endif /* DISC_H */
