#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>

#include "config.h"
#include "disc/disc.h"
#include "cp/cp_server.h"
#include "rtl/rtl_bridge.h"
#include "vita/vita_tx.h"

/* =========================================================================
 * Signal handling
 * ========================================================================= */
static volatile sig_atomic_t g_shutdown = 0;
static void sig_handler(int sig) { (void)sig; g_shutdown = 1; }

/* =========================================================================
 * Usage
 * ========================================================================= */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Network:\n"
        "  -i <iface>        Network interface for mDNS (default: %s)\n"
        "  --name <name>     Device name shown in mDNS and hello response\n"
        "                    (default: SLC-XXYYZZ derived from MAC address)\n"
        "\n"
        "Sampling mode (choose one; default: normal):\n"
        "  --normal          Normal tuner path, %.0f kHz – %.0f MHz\n"
        "  --direct-i        Direct sampling, I-branch, %.0f kHz – %.0f MHz\n"
        "  --direct-q        Direct sampling, Q-branch, %.0f kHz – %.0f MHz\n"
        "\n"
        "Hardware defaults (applied at startup; client may override):\n"
        "  --bias-tee        Enable bias tee on startup (default: off)\n"
        "  --agc             Enable hardware AGC on startup (default: off)\n"
        "  --ppm <n>         PPM frequency correction (default: 0)\n"
        "  --gain <dB>       Initial RF gain in dB (default: %.1f dB)\n"
        "\n"
        "Protocol:\n"
        "  CP port:          %d (fixed)\n"
        "  VITA UDP port:    %d (fixed)\n"
        "\n",
        prog,
        MDNS_ANNOUNCE_IFACE,
        RTL_FREQ_MIN_NORMAL / 1e3,  RTL_FREQ_MAX_NORMAL / 1e6,
        RTL_FREQ_MIN_DIRECT / 1e3,  RTL_FREQ_MAX_DIRECT / 1e6,
        RTL_FREQ_MIN_DIRECT / 1e3,  RTL_FREQ_MAX_DIRECT / 1e6,
        RTL_DEFAULT_GAIN / 10.0,
        CP_PORT, VITA_SRC_PORT);
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
int main(int argc, char *argv[]) {
    const char     *iface       = MDNS_ANNOUNCE_IFACE;
    const char     *device_name = NULL;   /* NULL = use MAC-derived default */
    sampling_mode_t samp_mode   = SAMPLING_NORMAL;
    bool            bias_tee    = false;
    bool            agc         = false;
    int             ppm         = 0;
    /* gain_tenth_db: -1 means "use RTL_DEFAULT_GAIN" */
    int             gain_tenth  = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iface = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            device_name = argv[++i];
        } else if (strcmp(argv[i], "--normal") == 0) {
            samp_mode = SAMPLING_NORMAL;
        } else if (strcmp(argv[i], "--direct-i") == 0) {
            samp_mode = SAMPLING_DIRECT_I;
        } else if (strcmp(argv[i], "--direct-q") == 0) {
            samp_mode = SAMPLING_DIRECT_Q;
        } else if (strcmp(argv[i], "--bias-tee") == 0) {
            bias_tee = true;
        } else if (strcmp(argv[i], "--agc") == 0) {
            agc = true;
        } else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc) {
            ppm = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
            gain_tenth = (int)(atof(argv[++i]) * 10.0);
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* gain_tenth < 0 means use compile-time default */
    if (gain_tenth < 0) gain_tenth = RTL_DEFAULT_GAIN;

    static const char *mode_names[] = {"normal","direct-i","direct-q"};
    LOG_INFO("main", "SDR-SLC RTL-SDR daemon starting");
    LOG_INFO("main", "iface=%s CP_PORT=%d VITA_PORT=%d",
             iface, CP_PORT, VITA_SRC_PORT);
    LOG_INFO("main", "mode=%s bias_tee=%s agc=%s ppm=%d gain=%.1f dB",
             mode_names[samp_mode],
             bias_tee ? "on" : "off",
             agc      ? "on" : "off",
             ppm, gain_tenth / 10.0);

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 1. Open RTL-SDR device */
    if (rtl_bridge_init(samp_mode, bias_tee, agc, ppm) < 0) {
        LOG_ERR("main", "Failed to open RTL-SDR device %d", RTL_DEVICE_INDEX);
        LOG_ERR("main", "Is a dongle plugged in?  Try: rtl_test -t");
        return 1;
    }

    /* 2. VITA TX socket */
    if (vita_tx_init() < 0) {
        LOG_ERR("main", "Failed to initialise VITA TX");
        rtl_bridge_destroy(); return 1;
    }

    /* 3. RTL async read: started on-demand by start_rx */
    LOG_INFO("main", "RTL device ready — async read starts on first start_rx");

    /* 4. VITA TX thread (no-op — callback driven) */
    vita_tx_start();

    /* 5. SLC-CP TCP server */
    if (cp_server_start() < 0) {
        LOG_ERR("main", "Failed to start CP server on port %d", CP_PORT);
        vita_tx_stop(); vita_tx_destroy(); rtl_bridge_destroy(); return 1;
    }

    /* 6. mDNS */
    if (disc_init(iface, CP_PORT, device_name) < 0) {
        LOG_WARN("main", "mDNS init failed — discovery disabled");
    } else {
        disc_announce();
        if (disc_start() < 0)
            LOG_WARN("main", "mDNS responder thread failed");
    }

    LOG_INFO("main", "daemon ready");
    LOG_INFO("main", "  mode:      %s", mode_names[samp_mode]);
    LOG_INFO("main", "  mDNS:      _sdr-slc._tcp.local. on %s", iface);
    LOG_INFO("main", "  CP:        TCP port %d", CP_PORT);
    LOG_INFO("main", "  VITA UDP:  source port %d", VITA_SRC_PORT);
    if (bias_tee)
        LOG_WARN("main", "  BIAS TEE:  ENABLED — voltage on SMA connector");
    LOG_INFO("main", "Press Ctrl-C to stop.");

    /* Main loop */
    while (!g_shutdown) sleep(1);

    LOG_INFO("main", "shutdown signal received");

    disc_stop();
    cp_server_stop();
    vita_tx_stop();
    rtl_bridge_force_stop();
    vita_tx_destroy();
    rtl_bridge_destroy();

    LOG_INFO("main", "shutdown complete");
    return 0;
}
