#include "disc.h"
#include "../config.h"
#include "../rtl/rtl_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* =========================================================================
 * DNS packet constants
 * ========================================================================= */
#define DNS_TYPE_A      1
#define DNS_TYPE_PTR   12
#define DNS_TYPE_TXT   16
#define DNS_TYPE_SRV   33
#define DNS_CLASS_IN    1
#define DNS_CACHE_FLUSH 0x8000  /* mDNS cache-flush bit ORed into class   */
#define DNS_QR_RESPONSE 0x8400  /* QR=1 AA=1                              */

/* =========================================================================
 * Static state
 * ========================================================================= */
static int       s_sock   = -1;
static pthread_t s_tid;
static volatile bool s_running = false;

static char     s_instance[64];
static char     s_hostname[80];
static uint8_t  s_mac[6];
static uint32_t s_ip;
static uint16_t s_ctrl_port;

/* Exported so cp_commands.c can use it in the hello response */
char g_device_name[64] = DEVICE_NAME;

/* =========================================================================
 * Network interface helpers
 * ========================================================================= */

static int read_mac(const char *iface, uint8_t mac[6]) {
    /* Read MAC from sysfs */
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_WARN("disc", "cannot open %s: %s", path, strerror(errno));
        /* Use a fake MAC derived from PID */
        uint32_t pid = (uint32_t)getpid();
        mac[0] = 0x40; mac[1] = 0x28; mac[2] = 0x14;
        mac[3] = (pid >> 16) & 0xFF;
        mac[4] = (pid >>  8) & 0xFF;
        mac[5] = (pid      ) & 0xFF;
        return 0;
    }
    unsigned int m[6];
    fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]);
    fclose(f);
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
    return 0;
}

static int read_ipv4(const char *iface, uint32_t *ip_net_order) {
    struct ifaddrs *ifa_list, *ifa;
    if (getifaddrs(&ifa_list) < 0) return -1;
    int found = 0;
    for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || strcmp(ifa->ifa_name, iface) != 0) continue;
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        *ip_net_order = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        found = 1;
        break;
    }
    freeifaddrs(ifa_list);
    return found ? 0 : -1;
}

/* =========================================================================
 * DNS packet builder
 *
 * We build DNS packets into a stack buffer.  The layout uses message
 * compression (pointer 0xC0xx) to avoid repeating long names.
 * ========================================================================= */

typedef struct {
    uint8_t  buf[512];
    int      pos;
    int      overflow;
} DNS;

static void dns_init(DNS *d) {
    memset(d, 0, sizeof(*d));
    d->pos = 12; /* leave space for 12-byte DNS header */
}

static void dns_put8(DNS *d, uint8_t v) {
    if (d->pos < 512) d->buf[d->pos++] = v;
    else d->overflow = 1;
}

static void dns_put16(DNS *d, uint16_t v) {
    dns_put8(d, (uint8_t)(v >> 8));
    dns_put8(d, (uint8_t)(v & 0xFF));
}

static void dns_put32(DNS *d, uint32_t v) {
    dns_put8(d, (uint8_t)(v >> 24));
    dns_put8(d, (uint8_t)(v >> 16));
    dns_put8(d, (uint8_t)(v >>  8));
    dns_put8(d, (uint8_t)(v & 0xFF));
}

/* Write a DNS name as length-prefixed labels.
 * name:  "foo.bar.local." — trailing dot required.
 * Returns the offset where the name started (useful for compression ptr). */
static int dns_name(DNS *d, const char *name) {
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
    dns_put8(d, 0); /* root label */
    return start;
}

/* Write a compression pointer to a previously written name offset. */
static void dns_ptr(DNS *d, int offset) {
    dns_put8(d, 0xC0 | (uint8_t)(offset >> 8));
    dns_put8(d, (uint8_t)(offset & 0xFF));
}

/* Patch a 16-bit RDLENGTH field at pos 'rdlen_pos' with the number of
 * bytes written since 'rdlen_pos + 2'.                                   */
static void dns_patch_rdlen(DNS *d, int rdlen_pos) {
    int rdlen = d->pos - rdlen_pos - 2;
    d->buf[rdlen_pos]   = (uint8_t)(rdlen >> 8);
    d->buf[rdlen_pos+1] = (uint8_t)(rdlen & 0xFF);
}

/* Write a complete answer record header:
 *   name_ptr_offset  — compression pointer offset, or -1 to write full name
 *   name             — full name (used only when name_ptr_offset == -1)
 *   type, class, ttl
 * Returns position of the 2-byte RDLENGTH field (caller must patch it).  */
static int dns_rr_header(DNS *d, int name_ptr_off, const char *name,
                          uint16_t type, uint16_t cls, uint32_t ttl) {
    if (name_ptr_off >= 0) dns_ptr(d, name_ptr_off);
    else                   dns_name(d, name);
    dns_put16(d, type);
    dns_put16(d, cls);
    dns_put32(d, ttl);
    int rdlen_pos = d->pos;
    dns_put16(d, 0); /* placeholder RDLENGTH */
    return rdlen_pos;
}

/* Build a TXT RDATA string: one length-prefixed key=value entry */
static void dns_txt_kv(DNS *d, const char *k, const char *v) {
    char kv[256];
    int  kl = snprintf(kv, sizeof(kv), "%s=%s", k, v);
    if (kl < 0 || kl >= (int)sizeof(kv)) kl = (int)sizeof(kv) - 1;
    dns_put8(d, (uint8_t)kl);
    for (int i = 0; i < kl; i++) dns_put8(d, (uint8_t)kv[i]);
}

/* =========================================================================
 * Build and send an mDNS response/announcement
 *
 * ttl_override: if >= 0, use this TTL for all records (0 = goodbye)
 * ========================================================================= */
static void send_announcement(uint32_t ttl_override) {
    DNS d;
    dns_init(&d);

    uint32_t ttl = (ttl_override <= MDNS_TTL) ? ttl_override : MDNS_TTL;

    /* Precompute name strings.
     * s_instance is up to 63 chars (DNS label limit enforced at init).
     * host_fqdn:  63 + len(".local.") + NUL = 71  → use 80
     * inst_fqdn:  63 + len("._sdr-slc._tcp.local.") + NUL = 85 → use 96
     * svc_type:   fixed "_sdr-slc._tcp.local." = 21 + NUL          */
    char svc_type[32];   /* _sdr-slc._tcp.local.                          */
    char inst_fqdn[96];  /* <name>._sdr-slc._tcp.local.                   */
    char host_fqdn[80];  /* <name>.local.                                 */

    snprintf(svc_type,  sizeof(svc_type),  "_sdr-slc._tcp.local.");
    snprintf(inst_fqdn, sizeof(inst_fqdn), "%s._sdr-slc._tcp.local.", s_instance);
    snprintf(host_fqdn, sizeof(host_fqdn), "%s.local.", s_instance);

    /* ---- DNS header (12 bytes at offset 0) ---- */
    /* ID=0 (mDNS always 0), QR=1 AA=1, QDCOUNT=0, ANCOUNT=4, NSCOUNT=0 */
    d.buf[0] = 0x00; d.buf[1] = 0x00; /* ID */
    d.buf[2] = 0x84; d.buf[3] = 0x00; /* QR=1 AA=1 */
    d.buf[4] = 0x00; d.buf[5] = 0x00; /* QDCOUNT */
    d.buf[6] = 0x00; d.buf[7] = 0x04; /* ANCOUNT = 4 records */
    d.buf[8] = 0x00; d.buf[9] = 0x00; /* NSCOUNT */
    d.buf[10]= 0x00; d.buf[11]= 0x00; /* ARCOUNT */

    /* ---- Record 1: PTR ---- */
    /* _sdr-slc._tcp.local. PTR SLC-XXYYZZ._sdr-slc._tcp.local.          */
    int svc_name_off = d.pos;
    (void)svc_name_off;
    int rdp = dns_rr_header(&d, -1, svc_type, DNS_TYPE_PTR,
                             DNS_CLASS_IN, ttl);
    int inst_name_off = d.pos;         /* remember for compression below  */
    dns_name(&d, inst_fqdn);
    dns_patch_rdlen(&d, rdp);

    /* ---- Record 2: SRV ---- */
    /* SLC-XXYYZZ._sdr-slc._tcp.local. SRV 0 0 <port> SLC-XXYYZZ.local. */
    rdp = dns_rr_header(&d, inst_name_off, NULL,
                         DNS_TYPE_SRV,
                         DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl);
    dns_put16(&d, 0);              /* priority */
    dns_put16(&d, 0);              /* weight   */
    dns_put16(&d, s_ctrl_port);   /* port     */
    int host_name_off = d.pos;
    dns_name(&d, host_fqdn);
    dns_patch_rdlen(&d, rdp);

    /* ---- Record 3: TXT ---- */
    rdp = dns_rr_header(&d, inst_name_off, NULL,
                         DNS_TYPE_TXT,
                         DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl);
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", s_ctrl_port);
    char freq_min[24], freq_max[24];
    /* Match get_capabilities: report the range for the active sampling mode */
    uint64_t fmin = (g_sampling_mode == SAMPLING_NORMAL)
                    ? RTL_FREQ_MIN_NORMAL : RTL_FREQ_MIN_DIRECT;
    uint64_t fmax = (g_sampling_mode == SAMPLING_NORMAL)
                    ? RTL_FREQ_MAX_NORMAL : RTL_FREQ_MAX_DIRECT;
    snprintf(freq_min, sizeof(freq_min), "%llu", (unsigned long long)fmin);
    snprintf(freq_max, sizeof(freq_max), "%llu", (unsigned long long)fmax);
    dns_txt_kv(&d, "proto",        "sdr-slc");
    dns_txt_kv(&d, "proto_ver",    "1.0");
    dns_txt_kv(&d, "name",         s_instance);
    dns_txt_kv(&d, "manufacturer", MANUFACTURER);
    dns_txt_kv(&d, "hw",           HW_REVISION);
    dns_txt_kv(&d, "fw",           FW_VERSION);
    dns_txt_kv(&d, "role",         "rx");
    dns_txt_kv(&d, "rx",           "1");
    dns_txt_kv(&d, "tx",           "0");
    dns_txt_kv(&d, "model",        MODEL_NAME);
    dns_txt_kv(&d, "stream",       "vita-49-slc");
    dns_txt_kv(&d, "sampling",
               (g_sampling_mode == SAMPLING_DIRECT_I) ? "direct-i" :
               (g_sampling_mode == SAMPLING_DIRECT_Q) ? "direct-q" : "normal");
    dns_txt_kv(&d, "minhz",        freq_min);
    dns_txt_kv(&d, "maxhz",        freq_max);
    dns_patch_rdlen(&d, rdp);

    /* ---- Record 4: A ---- */
    rdp = dns_rr_header(&d, host_name_off, NULL,
                         DNS_TYPE_A,
                         DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl);
    /* s_ip is already in network byte order */
    d.buf[d.pos++] = (s_ip      ) & 0xFF;
    d.buf[d.pos++] = (s_ip >>  8) & 0xFF;
    d.buf[d.pos++] = (s_ip >> 16) & 0xFF;
    d.buf[d.pos++] = (s_ip >> 24) & 0xFF;
    dns_patch_rdlen(&d, rdp);

    if (d.overflow) {
        LOG_ERR("disc", "mDNS packet overflow — packet not sent");
        return;
    }

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = inet_addr(MDNS_GROUP),
        .sin_port        = htons(MDNS_PORT)
    };
    ssize_t n = sendto(s_sock, d.buf, (size_t)d.pos, 0,
                       (struct sockaddr *)&dest, sizeof(dest));
    if (n < 0)
        LOG_ERR("disc", "sendto mDNS: %s", strerror(errno));
    else
        LOG_DBG("disc", "mDNS announcement sent (%zd bytes, TTL=%u)", n, ttl);
}

/* =========================================================================
 * Query parser — minimal PTR query detection
 * ========================================================================= */

/* Returns true if the packet at buf[0..len-1] contains a PTR query for
 * _sdr-slc._tcp.local.                                                   */
static bool is_ptr_query_for_us(const uint8_t *buf, int len) {
    if (len < 12) return false;
    /* Check QR=0 (query), QDCOUNT >= 1 */
    if (buf[2] & 0x80) return false;       /* QR=1 means response, skip  */
    int qdcount = (buf[4] << 8) | buf[5];
    if (qdcount == 0) return false;

    /* Walk questions — find one matching _sdr-slc._tcp.local. QTYPE=PTR  */
    int pos = 12;
    for (int q = 0; q < qdcount && pos < len; q++) {
        /* Decode name labels */
        char name[128] = "";
        int  nlen = 0;
        while (pos < len) {
            uint8_t llen = buf[pos++];
            if (llen == 0) break;
            if ((llen & 0xC0) == 0xC0) { pos++; break; } /* compression ptr */
            if (nlen + llen + 1 < (int)sizeof(name)) {
                if (nlen > 0) name[nlen++] = '.';
                memcpy(name + nlen, buf + pos, llen);
                nlen += llen;
                name[nlen] = '\0';
            }
            pos += llen;
        }
        if (pos + 4 > len) break;
        uint16_t qtype  = (buf[pos] << 8) | buf[pos+1]; pos += 2;
        /* uint16_t qclass = */ pos += 2; /* skip class */

        if (qtype == DNS_TYPE_PTR &&
            strcasecmp(name, "_sdr-slc._tcp.local") == 0)
            return true;

        /* Skip meta-query too: _services._dns-sd._udp.local */
        if (qtype == DNS_TYPE_PTR &&
            strcasecmp(name, "_services._dns-sd._udp.local") == 0)
            return true;
    }
    return false;
}

/* =========================================================================
 * Background thread — listens for mDNS queries and responds
 * ========================================================================= */

static void *disc_thread(void *arg) {
    (void)arg;
    LOG_INFO("disc", "mDNS responder thread started");

    uint8_t rxbuf[512];
    struct sockaddr_in from;
    socklen_t fromlen;

    while (s_running) {
        fromlen = sizeof(from);
        ssize_t n = recvfrom(s_sock, rxbuf, sizeof(rxbuf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!s_running) break;
            LOG_WARN("disc", "recvfrom: %s", strerror(errno));
            continue;
        }

        if (is_ptr_query_for_us(rxbuf, (int)n)) {
            LOG_DBG("disc", "PTR query received — responding");
            send_announcement(MDNS_TTL);
        }
    }

    LOG_INFO("disc", "mDNS responder thread exiting");
    return NULL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int disc_init(const char *iface_name, uint16_t ctrl_port,
              const char *custom_name) {
    s_ctrl_port = ctrl_port;

    /* Read MAC — always needed for hostname uniqueness even with custom name */
    if (read_mac(iface_name, s_mac) < 0) {
        LOG_WARN("disc", "MAC read failed, using random suffix");
    }

    if (custom_name && custom_name[0] != '\0') {
        /* Use caller-supplied name, sanitised: replace spaces with hyphens,
         * truncate to 63 chars (DNS label limit).                           */
        strncpy(s_instance, custom_name, 63);
        s_instance[63] = '\0';
        for (char *p = s_instance; *p; p++) {
            if (*p == ' ' || *p == '\t') *p = '-';
        }
    } else {
        /* Default: SLC-XXYYZZ derived from last 3 MAC octets */
        snprintf(s_instance, sizeof(s_instance), "SLC-%02X%02X%02X",
                 s_mac[3], s_mac[4], s_mac[5]);
    }
    snprintf(s_hostname, sizeof(s_hostname), "%s.local.", s_instance);

    /* Export for hello response */
    strncpy(g_device_name, s_instance, sizeof(g_device_name) - 1);
    g_device_name[sizeof(g_device_name) - 1] = '\0';

    /* Read IPv4 */
    if (read_ipv4(iface_name, &s_ip) < 0) {
        LOG_WARN("disc", "IPv4 read failed for %s, using 127.0.0.1", iface_name);
        s_ip = htonl(INADDR_LOOPBACK);
    }

    char ip_str[INET_ADDRSTRLEN];
    struct in_addr ia = { .s_addr = s_ip };
    inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
    LOG_INFO("disc", "instance=%s ip=%s", s_instance, ip_str);

    /* Create mDNS socket */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) { LOG_ERR("disc", "socket: %s", strerror(errno)); return -1; }

    /* Allow multiple processes on same port */
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    /* Bind to mDNS port on INADDR_ANY */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(MDNS_PORT)
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERR("disc", "bind: %s", strerror(errno)); return -1;
    }

    /* Join mDNS multicast group */
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr(MDNS_GROUP),
        .imr_interface.s_addr = INADDR_ANY
    };
    if (setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        LOG_WARN("disc", "IP_ADD_MEMBERSHIP: %s (may need root)", strerror(errno));
    }

    /* Set multicast TTL (link-local = 1) */
    int ttl = 1;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* Set multicast outgoing interface */
    struct in_addr out_iface = { .s_addr = INADDR_ANY };
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &out_iface, sizeof(out_iface));

    /* Receive our own multicast (needed so we can hear our own queries) */
    int loop = 1;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    LOG_INFO("disc", "mDNS socket ready");
    return 0;
}

void disc_announce(void) {
    /* Wait for switch forwarding table to update (spec §8.1) */
    struct timespec ts = { .tv_sec  = 0,
                           .tv_nsec = MDNS_LINK_DELAY_MS * 1000000L };
    nanosleep(&ts, NULL);
    send_announcement(MDNS_TTL);
    LOG_INFO("disc", "initial mDNS announcement sent");
}

int disc_start(void) {
    s_running = true;
    int r = pthread_create(&s_tid, NULL, disc_thread, NULL);
    if (r != 0) {
        LOG_ERR("disc", "pthread_create: %s", strerror(r));
        s_running = false;
        return -1;
    }
    return 0;
}

void disc_stop(void) {
    /* Send goodbye (TTL=0) */
    send_announcement(0);
    s_running = false;
    /* Unblock recvfrom by closing the socket */
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    pthread_join(s_tid, NULL);
    LOG_INFO("disc", "mDNS responder stopped");
}
