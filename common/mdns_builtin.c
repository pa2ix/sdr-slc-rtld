/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* mdns_builtin.c - self-contained mDNS/DNS-SD responder (RFC 6762 / 6763).
 *
 * The 0.17 disc.c responder moved behind the shared mdns.h interface: raw
 * UDP multicast on 224.0.0.251:5353, no library.  It announces on start,
 * answers PTR queries for _sdr-slc._tcp.local. (and the _services meta
 * query), answers SRV/TXT/A queries for the instance and host it owns,
 * and sends goodbye (TTL 0) on stop.  It publishes exactly the TXT keys
 * the Avahi backend does, so a client cannot tell the two apart.
 *
 * Not implemented, deliberately: probing/conflict resolution (§8 of RFC
 * 6762) and unicast-response requests.  The instance name comes from a
 * MAC and is unique on a LAN in practice; a second daemon with the same
 * name is a configuration error either backend only logs.
 *
 * Coexists with a running avahi-daemon: both bind 5353 with SO_REUSEADDR,
 * both see every query, only this one answers for this service.        */
#include "mdns.h"
#include "log.h"
#include "../config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MDNS_PORT        5353
#define MDNS_GROUP       "224.0.0.251"
#define MDNS_TTL         120
#define MDNS_LINK_DELAY_MS 100
#define SLC_SERVICE_TYPE "_sdr-slc._tcp"

#define DNS_TYPE_A      1
#define DNS_TYPE_PTR   12
#define DNS_TYPE_TXT   16
#define DNS_TYPE_SRV   33
#define DNS_TYPE_ANY  255
#define DNS_CLASS_IN    1
#define DNS_CACHE_FLUSH 0x8000

static int              s_sock = -1;
static pthread_t        s_tid;
static volatile bool    s_running;
static bool             s_published;

static char     s_instance[64];
static char     s_iface[32];
static uint32_t s_ip;            /* network order */
static uint16_t s_port;

/* Owned copies of the TXT strings: the thread answers long after start. */
static char s_txt[8][160];
static int  s_ntxt;

/* ------------------------------------------------------------ helpers */

static bool read_ipv4(const char *iface, uint32_t *ip_net_order)
{
    struct ifaddrs *list, *ifa;
    if (getifaddrs(&list) < 0) return false;
    bool found = false;
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!ifa->ifa_name) continue;
        if (iface && iface[0] && strcmp(ifa->ifa_name, iface)) continue;
        if (!iface || !iface[0]) {
            /* no interface named: first non-loopback IPv4 */
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        }
        *ip_net_order = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        found = true;
        break;
    }
    freeifaddrs(list);
    return found;
}

/* ------------------------------------------------- DNS packet builder */

typedef struct {
    uint8_t buf[1024];
    int     pos;
    int     overflow;
} DNS;

static void dns_init(DNS *d) { memset(d, 0, sizeof *d); d->pos = 12; }

static void dns_put8(DNS *d, uint8_t v)
{
    if (d->pos < (int)sizeof d->buf) d->buf[d->pos++] = v;
    else d->overflow = 1;
}
static void dns_put16(DNS *d, uint16_t v) { dns_put8(d, v >> 8); dns_put8(d, v & 0xFF); }
static void dns_put32(DNS *d, uint32_t v) { dns_put16(d, v >> 16); dns_put16(d, v & 0xFFFF); }

/* "foo.bar.local." -> labels. Returns the start offset (for compression). */
static int dns_name(DNS *d, const char *name)
{
    int start = d->pos;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        if (!dot) break;
        int len = (int)(dot - p);
        dns_put8(d, (uint8_t)len);
        for (int i = 0; i < len; i++) dns_put8(d, (uint8_t)p[i]);
        p = dot + 1;
    }
    dns_put8(d, 0);
    return start;
}

static void dns_ptr(DNS *d, int off) { dns_put8(d, 0xC0 | (off >> 8)); dns_put8(d, off & 0xFF); }

static void dns_patch_rdlen(DNS *d, int rdlen_pos)
{
    int rdlen = d->pos - rdlen_pos - 2;
    d->buf[rdlen_pos]     = (uint8_t)(rdlen >> 8);
    d->buf[rdlen_pos + 1] = (uint8_t)(rdlen & 0xFF);
}

/* Record header; name by compression pointer (>=0) or in full. Returns
 * the RDLENGTH position for dns_patch_rdlen(). */
static int dns_rr(DNS *d, int name_off, const char *name,
                  uint16_t type, uint16_t cls, uint32_t ttl)
{
    if (name_off >= 0) dns_ptr(d, name_off);
    else               dns_name(d, name);
    dns_put16(d, type);
    dns_put16(d, cls);
    dns_put32(d, ttl);
    int rdlen_pos = d->pos;
    dns_put16(d, 0);
    return rdlen_pos;
}

/* Which records a response carries. */
enum { R_PTR = 1, R_SRV = 2, R_TXT = 4, R_A = 8, R_ALL = 15 };

static void send_records(unsigned which, uint32_t ttl)
{
    if (s_sock < 0) return;
    DNS d;
    dns_init(&d);

    char svc_type[32], inst_fqdn[96], host_fqdn[80];
    snprintf(svc_type,  sizeof svc_type,  SLC_SERVICE_TYPE ".local.");
    snprintf(inst_fqdn, sizeof inst_fqdn, "%s." SLC_SERVICE_TYPE ".local.", s_instance);
    snprintf(host_fqdn, sizeof host_fqdn, "%s.local.", s_instance);

    int ancount = 0;
    int inst_off = -1, host_off = -1;

    if (which & R_PTR) {
        int rdp = dns_rr(&d, -1, svc_type, DNS_TYPE_PTR, DNS_CLASS_IN, ttl);
        inst_off = dns_name(&d, inst_fqdn);
        dns_patch_rdlen(&d, rdp);
        ancount++;
    }
    if (which & R_SRV) {
        int name_at = d.pos;
        int rdp = dns_rr(&d, inst_off, inst_fqdn, DNS_TYPE_SRV,
                         DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl);
        if (inst_off < 0) inst_off = name_at;   /* written in full here */
        dns_put16(&d, 0);          /* priority */
        dns_put16(&d, 0);          /* weight   */
        dns_put16(&d, s_port);
        host_off = dns_name(&d, host_fqdn);
        dns_patch_rdlen(&d, rdp);
        ancount++;
    }
    if (which & R_TXT) {
        int name_at = d.pos;
        int rdp = dns_rr(&d, inst_off, inst_fqdn, DNS_TYPE_TXT,
                         DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl);
        if (inst_off < 0) inst_off = name_at;
        for (int i = 0; i < s_ntxt; i++) {
            int kl = (int)strlen(s_txt[i]);
            dns_put8(&d, (uint8_t)kl);
            for (int j = 0; j < kl; j++) dns_put8(&d, (uint8_t)s_txt[i][j]);
        }
        dns_patch_rdlen(&d, rdp);
        ancount++;
    }
    if (which & R_A) {
        int rdp = dns_rr(&d, host_off, host_fqdn, DNS_TYPE_A,
                         DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl);
        dns_put8(&d, (uint8_t)(s_ip));
        dns_put8(&d, (uint8_t)(s_ip >> 8));
        dns_put8(&d, (uint8_t)(s_ip >> 16));
        dns_put8(&d, (uint8_t)(s_ip >> 24));
        dns_patch_rdlen(&d, rdp);
        ancount++;
    }

    /* Header: ID 0, QR=1 AA=1, no questions, ancount answers. */
    d.buf[2] = 0x84; d.buf[3] = 0x00;
    d.buf[6] = (uint8_t)(ancount >> 8); d.buf[7] = (uint8_t)ancount;

    if (d.overflow) {
        LOGE(T_MDNS, "response would exceed the buffer; not sent");
        return;
    }
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = inet_addr(MDNS_GROUP),
        .sin_port = htons(MDNS_PORT),
    };
    if (sendto(s_sock, d.buf, (size_t)d.pos, 0,
               (struct sockaddr *)&dest, sizeof dest) < 0)
        LOGW(T_MDNS, "sendto: %s", strerror(errno));
    else
        LOGD(T_MDNS, "sent %d record%s, %d bytes, ttl %u",
             ancount, ancount == 1 ? "" : "s", d.pos, ttl);
}

/* --------------------------------------------------------- query side */

/* Decode one name at *pos into out (dots, no trailing dot). Follows one
 * compression pointer, which is all a query needs. */
static bool dns_read_name(const uint8_t *buf, int len, int *pos,
                          char *out, size_t cap)
{
    size_t n = 0;
    int p = *pos, jumped_to = -1;
    for (int guard = 0; guard < 64; guard++) {
        if (p >= len) return false;
        uint8_t l = buf[p++];
        if (l == 0) break;
        if ((l & 0xC0) == 0xC0) {
            if (p >= len) return false;
            int target = ((l & 0x3F) << 8) | buf[p++];
            if (jumped_to < 0) *pos = p;
            jumped_to = target;
            p = target;
            continue;
        }
        if (p + l > len) return false;
        if (n + l + 2 > cap) return false;
        if (n) out[n++] = '.';
        memcpy(out + n, buf + p, l);
        n += l;
        p += l;
    }
    out[n] = '\0';
    if (jumped_to < 0) *pos = p;
    return true;
}

/* Returns the set of records to answer with, 0 for "not for us". */
static unsigned records_for_query(const uint8_t *buf, int len)
{
    if (len < 12 || (buf[2] & 0x80)) return 0;   /* not a query */
    int qd = (buf[4] << 8) | buf[5];
    if (qd == 0) return 0;

    char svc[64], inst[128], host[96];
    snprintf(svc,  sizeof svc,  SLC_SERVICE_TYPE ".local");
    snprintf(inst, sizeof inst, "%s." SLC_SERVICE_TYPE ".local", s_instance);
    snprintf(host, sizeof host, "%s.local", s_instance);

    unsigned which = 0;
    int pos = 12;
    for (int q = 0; q < qd; q++) {
        char name[256];
        if (!dns_read_name(buf, len, &pos, name, sizeof name)) break;
        if (pos + 4 > len) break;
        uint16_t qtype = (buf[pos] << 8) | buf[pos + 1];
        pos += 4;
        bool any = (qtype == DNS_TYPE_ANY);
        if (!strcasecmp(name, svc) && (any || qtype == DNS_TYPE_PTR))
            which |= R_ALL;
        else if (!strcasecmp(name, "_services._dns-sd._udp.local") &&
                 (any || qtype == DNS_TYPE_PTR))
            which |= R_ALL;
        else if (!strcasecmp(name, inst)) {
            if (any || qtype == DNS_TYPE_SRV) which |= R_SRV | R_A;
            if (any || qtype == DNS_TYPE_TXT) which |= R_TXT;
        } else if (!strcasecmp(name, host) && (any || qtype == DNS_TYPE_A))
            which |= R_A;
    }
    return which;
}

static void *responder_thread(void *arg)
{
    (void)arg;
    uint8_t rx[1500];
    while (s_running) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s_sock, rx, sizeof rx, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!s_running) break;
            LOGW(T_MDNS, "recvfrom: %s", strerror(errno));
            continue;
        }
        unsigned which = records_for_query(rx, (int)n);
        if (which) {
            LOGD(T_MDNS, "query from %s: answering", inet_ntoa(from.sin_addr));
            send_records(which, MDNS_TTL);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- public */

int mdns_builtin_start(const char *iface, const char *instance,
                       uint16_t port, const mdns_txt_t *txt)
{
    snprintf(s_instance, sizeof s_instance, "%s", instance);
    snprintf(s_iface, sizeof s_iface, "%s", iface ? iface : "");
    s_port = port;

    s_ntxt = 0;
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "name=%s", txt->name);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "proto=%s", txt->proto);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "proto_ver=%s", txt->proto_ver);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "functions=%s", txt->functions);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "hw=%s", txt->hw);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "fw=%s", txt->fw);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "minhz=%llu",
             (unsigned long long)txt->minhz);
    snprintf(s_txt[s_ntxt++], sizeof s_txt[0], "maxhz=%llu",
             (unsigned long long)txt->maxhz);

    if (!read_ipv4(s_iface, &s_ip)) {
        LOGE(T_MDNS, "no IPv4 address on %s; cannot publish an A record",
             s_iface[0] ? s_iface : "any non-loopback interface");
        return -1;
    }
    char ip[INET_ADDRSTRLEN];
    struct in_addr ia = { .s_addr = s_ip };
    inet_ntop(AF_INET, &ia, ip, sizeof ip);

    s_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (s_sock < 0) { LOGE(T_MDNS, "socket: %s", strerror(errno)); return -1; }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#ifdef SO_REUSEPORT
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);
#endif
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(MDNS_PORT),
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) < 0) {
        LOGE(T_MDNS, "bind udp/%d: %s", MDNS_PORT, strerror(errno));
        close(s_sock); s_sock = -1;
        return -1;
    }
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr(MDNS_GROUP),
        .imr_interface.s_addr = s_ip,
    };
    if (setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0)
        LOGW(T_MDNS, "IP_ADD_MEMBERSHIP: %s", strerror(errno));
    int ttl = 255;                                  /* RFC 6762 §11 */
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct in_addr out = { .s_addr = s_ip };
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out);
    int loop = 1;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);

    LOGI(T_MDNS, "TXT: functions=%s hw=%s fw=%s minhz=%llu maxhz=%llu",
         txt->functions, txt->hw, txt->fw,
         (unsigned long long)txt->minhz, (unsigned long long)txt->maxhz);

    /* Let the switch learn the group before the first announcement. */
    struct timespec ts = { 0, MDNS_LINK_DELAY_MS * 1000000L };
    nanosleep(&ts, NULL);
    send_records(R_ALL, MDNS_TTL);
    s_published = true;
    LOGI(T_MDNS, "published \"%s\" as " SLC_SERVICE_TYPE ".local. on port %u "
         "(builtin responder, %s %s)", s_instance, s_port,
         s_iface[0] ? s_iface : "iface", ip);

    s_running = true;
    int r = pthread_create(&s_tid, NULL, responder_thread, NULL);
    if (r != 0) {
        LOGE(T_MDNS, "pthread_create: %s", strerror(r));
        s_running = false;
        return -1;
    }
    return 0;
}

void mdns_builtin_stop(void)
{
    if (s_sock < 0) return;
    if (s_published) send_records(R_ALL, 0);      /* goodbye */
    s_running = false;
    /* Wake the thread: shutdown() makes recvfrom return, then close. */
    shutdown(s_sock, SHUT_RDWR);
    pthread_join(s_tid, NULL);
    close(s_sock);
    s_sock = -1;
    if (s_published) LOGI(T_MDNS, "withdrew \"%s\"", s_instance);
    s_published = false;
}

bool mdns_builtin_published(void) { return s_published; }
