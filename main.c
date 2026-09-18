/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* sdr-slc-rtld - SDR-SLC bridge for an RTL-SDR dongle.
 * SDR-SLC-CP-1.0 Release 1.3, SDR-SLC-VITA-1.0 Release 1.2.
 *
 * Block rx at Tier 2, no transmitter.  The daemon shape - argument
 * parsing, the keygen subcommand, the key file, the EnvironmentFile
 * config, Avahi-based mDNS and the tagged logger - is shared with
 * sdr-slc-bladerfd so the two reference implementations administer the
 * same way.  Everything in rtl/ and vita/ is this daemon's own.
 */

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>

#include "config.h"
#include "common/auth.h"
#include "common/log.h"
#include "common/mdns.h"
#include "cp/cp_server.h"
#include "cp/cp_session.h"
#include "rtl/rtl_bridge.h"
#include "vita/vita_tx.h"

/* Runtime settings declared in config.h.                                 */
uint16_t g_cp_port       = CP_PORT_DEFAULT;
uint16_t g_vita_src_port = VITA_SRC_PORT_DEFAULT;
char     g_device_name[64] = DEVICE_NAME;
double   g_u8_zero_point = RTL_U8_ZERO_POINT_DEFAULT;

/* =========================================================================
 * Signal handling
 * ========================================================================= */
static volatile sig_atomic_t g_shutdown = 0;
static void sig_handler(int sig) { (void)sig; g_shutdown = 1; }

/* =========================================================================
 * Instance name: SLC-XXYYZZ from the last three octets of an interface MAC.
 *
 * This is the identity the client stores the radio under (§6.2), and it
 * has been MAC-derived since the first drop, so it stays MAC-derived.
 * Avahi publishes on every interface; the interface named here is only
 * where the name comes from.  Without -i the first non-loopback interface
 * that has a hardware address is used, so a Pi with wlan0 and no eth0
 * still gets a stable name.
 * ========================================================================= */
static bool read_mac(const char *iface, uint8_t mac[6]) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    unsigned m[6];
    int n = fscanf(f, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
    fclose(f);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
    /* all-zero is what lo and some virtual interfaces report */
    return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

static bool pick_mac(const char *iface, uint8_t mac[6], char *used, size_t cap) {
    if (iface && iface[0]) {
        snprintf(used, cap, "%s", iface);
        return read_mac(iface, mac);
    }
    static const char *preferred[] = { "eth0", "end0", "wlan0", NULL };
    for (int i = 0; preferred[i]; i++) {
        if (read_mac(preferred[i], mac)) {
            snprintf(used, cap, "%s", preferred[i]);
            return true;
        }
    }
    DIR *d = opendir("/sys/class/net");
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while (!ok && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        if (read_mac(e->d_name, mac)) {
            /* IFNAMSIZ-1 is the longest an interface name can be. */
            snprintf(used, cap, "%.15s", e->d_name);
            ok = true;
        }
    }
    closedir(d);
    return ok;
}

/* =========================================================================
 * Usage
 * ========================================================================= */
static void usage(const char *prog) {
    fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"  -p, --port PORT          control plane TCP port (default %d)\n"
"      --rx-port PORT       VITA49 RX source UDP port (default %d)\n"
"  -i, --iface IFACE        interface whose MAC derives the SLC-XXYYZZ\n"
"                           instance name (default: first interface with a\n"
"                           hardware address). mDNS itself is published on\n"
"                           every interface by avahi-daemon.\n"
"      --name NAME          override the mDNS instance / device_name\n"
"      --rx-auth MODE       \"none\" (default): the rx function is open, as\n"
"                           SDR-SLC-CP-1.0 §26 expects of a receiver.\n"
"                           \"psk\": require psk-hmac-sha256 authentication\n"
"                           for every command addressing rx.\n"
"      --keys-file PATH     pre-shared keys, one \"<key_id> <psk>\" per line\n"
"                           (default " SLC_DEFAULT_KEYS_FILE "). Populate it\n"
"                           with: sudo " SLC_DAEMON_NAME " keygen\n"
"\n"
"Sampling mode (choose one; default: normal):\n"
"      --normal             normal tuner path, %.0f kHz - %.0f MHz\n"
"      --direct-i           direct sampling, I-branch, %.0f kHz - %.0f MHz\n"
"      --direct-q           direct sampling, Q-branch, %.0f kHz - %.0f MHz\n"
"\n"
"Hardware defaults (applied at startup; the client may override):\n"
"      --bias-tee           power the bias tee (%.1f V) at startup.\n"
"                           §13.3 makes 0.0 V the startup default, so this\n"
"                           is an override.\n"
"      --bias-tee-persist   also re-apply the startup bias tee value after\n"
"                           a client disconnects, instead of dropping to\n"
"                           0.0 V. Deviates from §13.3; for masthead LNAs\n"
"                           that must stay powered.\n"
"      --agc                enable hardware AGC at startup (default: off)\n"
"      --ppm N              PPM frequency correction (default: 0)\n"
"      --gain DB            initial RF gain in dB (default: %.1f dB)\n"
"      --u8-zero-point V    ADC code value that is zero signal, declared as\n"
"                           iq_formats[].zero_point (default %.1f; the\n"
"                           RTL2832U midpoint, not 128). Measure with a\n"
"                           terminated input: the mean of the raw u8 stream.\n"
"\n"
"      --no-context-packets do not emit VITA-A/1.0-CTX packets. They are on\n"
"                           by default (SDR-SLC-VITA-1.0 Release 1.2 §7).\n"
"      --mdns BACKEND       \"builtin\" (default): the daemon's own mDNS\n"
"                           responder, no dependencies. \"avahi\": publish\n"
"                           through avahi-daemon, as sdr-slc-bladerfd does;\n"
"                           only in a build made with make MDNS=avahi.\n"
"      --no-mdns            do not publish over mDNS\n"
"      --syslog             log to syslog instead of stderr\n"
"  -v, --verbose            repeatable: info -> debug -> trace\n"
"  -h, --help               this text\n"
"\n"
"Subcommands:\n"
"  %s keygen [--key-id ID] [--keys-file PATH]\n"
"                           generate a pre-shared key for the rx function\n"
"                           and append it to the key file\n"
"\n"
"Protocol: SDR-SLC-CP-1.0 Release " SLC_CP_RELEASE ", SDR-SLC-VITA-1.0 Release "
SLC_VITA_RELEASE "; functions rx (Tier %d).\n",
        prog, CP_PORT_DEFAULT, VITA_SRC_PORT_DEFAULT,
        RTL_FREQ_MIN_NORMAL / 1e3,  RTL_FREQ_MAX_NORMAL / 1e6,
        RTL_FREQ_MIN_DIRECT / 1e3,  RTL_FREQ_MAX_DIRECT / 1e6,
        RTL_FREQ_MIN_DIRECT / 1e3,  RTL_FREQ_MAX_DIRECT / 1e6,
        RTL_BIAS_TEE_VOLTS,
        RTL_DEFAULT_GAIN / 10.0,
        RTL_U8_ZERO_POINT_DEFAULT,
        prog, RX_TIER);
}

/* Authentication is on and nothing can satisfy it.  Said once, loudly,
 * with the command that fixes it.                                       */
static void warn_rx_locked(const char *keys_file) {
    LOG_WARN("main", "rx is LOCKED: --rx-auth psk requires authentication "
             "(SDR-SLC-CP-1.0 §26) and no pre-shared key is configured in "
             "%s. Every command addressing rx will be refused with "
             "\"unauthorized\".", keys_file);
    LOG_WARN("main", "to unlock it, generate a key on this machine:");
    LOG_WARN("main", "    sudo " SLC_DAEMON_NAME " keygen");
    LOG_WARN("main", "then enter the key_id and psk it prints into the client "
             "and restart the daemon. To run without authentication, drop "
             "--rx-auth psk.");
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
int main(int argc, char *argv[]) {
    if (argc > 1 && !strcmp(argv[1], "keygen"))
        return auth_keygen_main(argc - 2, argv + 2, SLC_DEFAULT_KEYS_FILE);

    const char     *iface        = NULL;
    const char     *name_arg     = NULL;
    sampling_mode_t samp_mode    = SAMPLING_NORMAL;
    bool            bias_tee     = false;
    bool            bias_persist = false;
    bool            agc          = false;
    bool            ctx_packets  = true;
    bool            no_mdns      = false;
    const char     *mdns_backend = "builtin";
    bool            use_syslog   = false;
    bool            rx_auth      = false;
    int             verbosity    = LL_INFO;
    int             ppm          = 0;
    int             gain_tenth   = RTL_DEFAULT_GAIN;
    char            keys_file[512];
    snprintf(keys_file, sizeof(keys_file), "%s", SLC_DEFAULT_KEYS_FILE);

    static const struct option opts[] = {
        { "port",               required_argument, 0, 'p'  },
        { "rx-port",            required_argument, 0, 1001 },
        { "iface",              required_argument, 0, 'i'  },
        { "name",               required_argument, 0, 1002 },
        { "rx-auth",            required_argument, 0, 1003 },
        { "keys-file",          required_argument, 0, 1004 },
        { "normal",             no_argument,       0, 1010 },
        { "direct-i",           no_argument,       0, 1011 },
        { "direct-q",           no_argument,       0, 1012 },
        { "bias-tee",           no_argument,       0, 1013 },
        { "bias-tee-persist",   no_argument,       0, 1014 },
        { "agc",                no_argument,       0, 1015 },
        { "ppm",                required_argument, 0, 1016 },
        { "gain",               required_argument, 0, 1017 },
        { "u8-zero-point",      required_argument, 0, 1018 },
        { "no-context-packets", no_argument,       0, 1020 },
        { "no-mdns",            no_argument,       0, 1021 },
        { "mdns",               required_argument, 0, 1023 },
        { "syslog",             no_argument,       0, 1022 },
        { "verbose",            no_argument,       0, 'v'  },
        { "help",               no_argument,       0, 'h'  },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:i:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'p':  g_cp_port = (uint16_t)atoi(optarg); break;
        case 1001: g_vita_src_port = (uint16_t)atoi(optarg); break;
        case 'i':  iface = optarg; break;
        case 1002: name_arg = optarg; break;
        case 1003:
            if (!strcmp(optarg, "psk") || !strcmp(optarg, "psk-hmac-sha256")) {
                rx_auth = true;
            } else if (!strcmp(optarg, "none")) {
                rx_auth = false;
            } else {
                fprintf(stderr, "--rx-auth must be psk or none\n");
                return 2;
            }
            break;
        case 1004: snprintf(keys_file, sizeof(keys_file), "%s", optarg); break;
        case 1010: samp_mode = SAMPLING_NORMAL;   break;
        case 1011: samp_mode = SAMPLING_DIRECT_I; break;
        case 1012: samp_mode = SAMPLING_DIRECT_Q; break;
        case 1013: bias_tee = true; break;
        case 1014: bias_tee = true; bias_persist = true; break;
        case 1015: agc = true; break;
        case 1016: ppm = atoi(optarg); break;
        case 1017: gain_tenth = (int)(atof(optarg) * 10.0); break;
        case 1018:
            g_u8_zero_point = atof(optarg);
            if (g_u8_zero_point < 96.0 || g_u8_zero_point > 160.0) {
                fprintf(stderr, "--u8-zero-point %s: expected a code value "
                                "near the ADC midpoint (96..160)\n", optarg);
                return 2;
            }
            break;
        case 1020: ctx_packets = false; break;
        case 1021: no_mdns = true; break;
        case 1023: mdns_backend = optarg; break;
        case 1022: use_syslog = true; break;
        case 'v':  verbosity++; break;
        case 'h':  usage(argv[0]); return 0;
        default:   usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        usage(argv[0]);
        return 2;
    }

    log_init(verbosity, use_syslog);

    /* The interface is resolved below; the backend can be checked now so a
     * typo, or an --mdns avahi against a builtin-only binary, fails before
     * the dongle is opened. */
    if (!no_mdns && mdns_set_backend(mdns_backend, NULL) != 0) {
        fprintf(stderr, "--mdns %s: not available in this build "
                        "(available: %s)\n", mdns_backend,
                mdns_backends_available());
        return 2;
    }

    static const char *mode_names[] = {"normal", "direct-i", "direct-q"};
    LOG_INFO("main", SLC_DAEMON_NAME " %s starting (SDR-SLC-CP-1.0 Release %s, "
             "SDR-SLC-VITA-1.0 Release %s, schema_version %d, functions rx "
             "Tier %d)", SLC_DAEMON_VERSION, SLC_CP_RELEASE, SLC_VITA_RELEASE,
             CAP_SCHEMA_VERSION, RX_TIER);
    LOG_INFO("main", "mode=%s bias_tee=%s agc=%s ppm=%d gain=%.1f dB",
             mode_names[samp_mode], bias_tee ? "on" : "off",
             agc ? "on" : "off", ppm, gain_tenth / 10.0);

    /* §26: keys are read once.  A missing file is the first-run state, not
     * an error; a malformed one is, because a half-read key file is worse
     * than none.                                                        */
    g_rx_auth = rx_auth;
    memset(&g_keys, 0, sizeof(g_keys));
    if (rx_auth) {
        int nk = auth_keyring_load(&g_keys, keys_file);
        if (nk < 0) {
            LOG_ERR("main", "refusing to start with an unreadable or malformed "
                    "key file; fix or remove %s", keys_file);
            return 1;
        }
        if (nk == 0)
            warn_rx_locked(keys_file);
        else
            LOG_INFO("main", "rx requires psk-hmac-sha256 authentication; %d "
                     "key%s loaded from %s", nk, nk == 1 ? "" : "s", keys_file);
    }

    /* Instance name.  --name wins; otherwise SLC-XXYYZZ from a MAC.  The
     * interface the MAC came from is also where the builtin responder
     * announces (its IPv4 goes in the A record); -i sets both.          */
    char mdns_iface[32] = "";
    if (iface) snprintf(mdns_iface, sizeof(mdns_iface), "%s", iface);
    if (name_arg && name_arg[0]) {
        snprintf(g_device_name, sizeof(g_device_name), "%s", name_arg);
        for (char *p = g_device_name; *p; p++)
            if (*p == ' ' || *p == '\t') *p = '-';
    } else {
        uint8_t mac[6];
        char used[32] = "?";
        if (pick_mac(iface, mac, used, sizeof(used))) {
            snprintf(g_device_name, sizeof(g_device_name), "SLC-%02X%02X%02X",
                     mac[3], mac[4], mac[5]);
            snprintf(mdns_iface, sizeof(mdns_iface), "%s", used);
            LOG_INFO("main", "instance name %s from %s", g_device_name, used);
        } else {
            uint32_t pid = (uint32_t)getpid();
            snprintf(g_device_name, sizeof(g_device_name), "SLC-%06X",
                     pid & 0xFFFFFF);
            LOG_WARN("main", "no interface with a hardware address%s%s; "
                     "instance name %s is derived from the PID and will NOT "
                     "be stable across restarts. Use --name.",
                     iface ? " " : "", iface ? iface : "", g_device_name);
        }
    }

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (bias_persist)
        LOG_WARN("main", "--bias-tee-persist: bias tee will be re-applied "
                 "after a client disconnects. This deviates from "
                 "SDR-SLC-CP-1.0 §13.3, which requires 0.0 V.");

    /* 1. Open RTL-SDR device */
    if (rtl_bridge_init(samp_mode, bias_tee ? RTL_BIAS_TEE_VOLTS : 0.0,
                        agc, ppm) < 0) {
        LOG_ERR("main", "Failed to open RTL-SDR device %d", RTL_DEVICE_INDEX);
        LOG_ERR("main", "Is a dongle plugged in?  Try: rtl_test -t");
        return 1;
    }
    /* --gain was parsed and logged by every previous drop and applied by
     * none of them; the bridge always came up at RTL_DEFAULT_GAIN.      */
    if (!agc && gain_tenth != RTL_DEFAULT_GAIN)
        rtl_set_gain(gain_tenth / 10.0);

    g_bias_tee_persist = bias_persist;
    vita_set_context_packets(ctx_packets);

    /* adc_overload (§21.3) is detected in the USB callback thread and
     * published as an event by the control plane.                       */
    rtl_set_overload_handler(cp_on_adc_overload);

    /* 2. VITA UDP socket */
    if (vita_tx_init() < 0) {
        LOG_ERR("main", "Failed to initialise VITA sender on udp/%u",
                g_vita_src_port);
        rtl_bridge_destroy(); return 1;
    }
    vita_tx_start();

    /* 3. SLC-CP TCP server */
    if (cp_server_start() < 0) {
        LOG_ERR("main", "Failed to start CP server on tcp/%u", g_cp_port);
        vita_tx_stop(); vita_tx_destroy(); rtl_bridge_destroy(); return 1;
    }

    /* 4. mDNS (§6).  Same interface as sdr-slc-bladerfd; the builtin
     * responder by default, Avahi with --mdns avahi in an MDNS=avahi build. */
    if (!no_mdns) {
        mdns_set_backend(mdns_backend, mdns_iface);
        mdns_txt_t txt = {
            .name      = g_device_name,
            .proto     = "sdr-slc",
            .proto_ver = PROTO_VERSION,
            .functions = "rx",
            .hw        = HW_REVISION,
            .fw        = SLC_FW_STRING,     /* §6.2 <implementation>/<version> */
            .minhz     = rtl_freq_min(),
            .maxhz     = rtl_freq_max(),
        };
        if (mdns_start(g_device_name, g_cp_port, &txt) != 0)
            LOG_WARN("main", "mDNS publication failed; the daemon still serves "
                     "the control plane on tcp/%u for direct connections",
                     g_cp_port);
    }

    LOG_INFO("main", "%s ready: %s, mode %s, control tcp/%u, vita udp/%u, "
             "mdns %s, rx auth %s", g_device_name, MODEL_NAME,
             mode_names[samp_mode], g_cp_port, g_vita_src_port,
             no_mdns ? "off" : mdns_backend,
             rx_auth ? AUTH_METHOD_PSK : "none");
    LOG_INFO("main", "  ref level: %.1f dBm at %.1f dB gain (nominal, +/-%.0f dB)",
             rtl_reference_level_dbm(g_gain_tenth_db / 10.0),
             g_gain_tenth_db / 10.0, RTL_REF_LEVEL_ACCURACY_DB);
    LOG_INFO("main", "  u8 zero point: %.2f (declared in iq_formats[]; "
             "the client subtracts it)", g_u8_zero_point);
    if (bias_tee)
        LOG_WARN("main", "  BIAS TEE:  ENABLED — voltage on SMA connector");

    while (!g_shutdown) sleep(1);

    LOG_INFO("main", "shutting down");
    if (!no_mdns) mdns_stop();
    cp_server_stop();
    vita_tx_stop();
    rtl_bridge_force_stop();
    vita_tx_destroy();
    rtl_bridge_destroy();
    LOG_INFO("main", "shutdown complete");
    return 0;
}
