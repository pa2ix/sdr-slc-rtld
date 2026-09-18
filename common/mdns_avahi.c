/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* mdns_avahi.c - the Avahi backend: sdr-slc-bladerfd src/mdns.c with the
 * three entry points renamed so the dispatcher in mdns.c can choose it.
 * Compiled only with -DSLC_MDNS_AVAHI (make MDNS=avahi or MDNS=both). */
#include "mdns.h"
#include "log.h"
#include "../config.h"   /* SLC_DAEMON_NAME */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#define SLC_SERVICE_TYPE "_sdr-slc._tcp"

static AvahiThreadedPoll *g_poll;
static AvahiClient       *g_client;
static AvahiEntryGroup   *g_group;

static char     g_instance[64];
static uint16_t g_port;
static bool     g_published;

/* Owned copies: Avahi may re-publish long after mdns_start() returned. */
static char g_txt[8][160];
static int  g_ntxt;

static void publish_service(AvahiClient *c);

static void group_cb(AvahiEntryGroup *g, AvahiEntryGroupState state, void *ud)
{
    (void)ud;
    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        g_published = true;
        LOGI(T_MDNS, "published \"%s\" as %s.local. on port %u",
             g_instance, SLC_SERVICE_TYPE, g_port);
        break;

    case AVAHI_ENTRY_GROUP_COLLISION:
        /* Deliberately NOT renaming with avahi_alternative_service_name():
         * the instance name is the client's durable identity for this radio.
         * A collision almost always means a previous instance of this daemon
         * has not released the name yet, or two daemons share a serial. */
        g_published = false;
        LOGE(T_MDNS, "instance name \"%s\" collided on the network. Not "
                     "renaming - the client treats this name as the radio's "
                     "identity. Check for another " SLC_DAEMON_NAME " holding this "
                     "name, then restart.", g_instance);
        break;

    case AVAHI_ENTRY_GROUP_FAILURE:
        g_published = false;
        LOGE(T_MDNS, "entry group failed: %s",
             avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
        break;

    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
    default:
        break;
    }
}

static void publish_service(AvahiClient *c)
{
    if (!g_group) {
        g_group = avahi_entry_group_new(c, group_cb, NULL);
        if (!g_group) {
            LOGE(T_MDNS, "avahi_entry_group_new(): %s",
                 avahi_strerror(avahi_client_errno(c)));
            return;
        }
    }
    if (!avahi_entry_group_is_empty(g_group))
        return;

    AvahiStringList *sl = NULL;
    for (int i = 0; i < g_ntxt; i++)
        sl = avahi_string_list_add(sl, g_txt[i]);

    int rc = avahi_entry_group_add_service_strlst(
        g_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, g_instance,
        SLC_SERVICE_TYPE, NULL, NULL, g_port, sl);
    avahi_string_list_free(sl);
    if (rc < 0) {
        LOGE(T_MDNS, "add_service(%s): %s", g_instance, avahi_strerror(rc));
        return;
    }

    rc = avahi_entry_group_commit(g_group);
    if (rc < 0)
        LOGE(T_MDNS, "entry_group_commit(): %s", avahi_strerror(rc));
}

static void client_cb(AvahiClient *c, AvahiClientState state, void *ud)
{
    (void)ud;
    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        publish_service(c);
        break;

    case AVAHI_CLIENT_FAILURE:
        g_published = false;
        LOGE(T_MDNS, "avahi client failure: %s",
             avahi_strerror(avahi_client_errno(c)));
        break;

    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
        /* Host name is being re-resolved; drop and re-add on the way back. */
        g_published = false;
        if (g_group)
            avahi_entry_group_reset(g_group);
        break;

    case AVAHI_CLIENT_CONNECTING:
        LOGI(T_MDNS, "waiting for avahi-daemon");
        break;
    }
}

int mdns_avahi_start(const char *instance, uint16_t port, const mdns_txt_t *txt)
{
    snprintf(g_instance, sizeof g_instance, "%s", instance);
    g_port = port;

    g_ntxt = 0;
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "name=%s", txt->name);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "proto=%s", txt->proto);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "proto_ver=%s", txt->proto_ver);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "functions=%s", txt->functions);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "hw=%s", txt->hw);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "fw=%s", txt->fw);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "minhz=%llu",
             (unsigned long long)txt->minhz);
    snprintf(g_txt[g_ntxt++], sizeof g_txt[0], "maxhz=%llu",
             (unsigned long long)txt->maxhz);

    LOGI(T_MDNS, "TXT: functions=%s hw=%s fw=%s minhz=%llu maxhz=%llu",
         txt->functions, txt->hw, txt->fw,
         (unsigned long long)txt->minhz, (unsigned long long)txt->maxhz);

    g_poll = avahi_threaded_poll_new();
    if (!g_poll) {
        LOGE(T_MDNS, "avahi_threaded_poll_new() failed");
        return -1;
    }

    int err = 0;
    g_client = avahi_client_new(avahi_threaded_poll_get(g_poll),
                                AVAHI_CLIENT_NO_FAIL, client_cb, NULL, &err);
    if (!g_client) {
        LOGE(T_MDNS, "avahi_client_new(): %s", avahi_strerror(err));
        avahi_threaded_poll_free(g_poll);
        g_poll = NULL;
        return -1;
    }

    if (avahi_threaded_poll_start(g_poll) < 0) {
        LOGE(T_MDNS, "avahi_threaded_poll_start() failed");
        mdns_avahi_stop();
        return -1;
    }
    return 0;
}

void mdns_avahi_stop(void)
{
    if (g_poll)
        avahi_threaded_poll_stop(g_poll);
    if (g_group) {
        avahi_entry_group_free(g_group);
        g_group = NULL;
    }
    if (g_client) {
        avahi_client_free(g_client);
        g_client = NULL;
    }
    if (g_poll) {
        avahi_threaded_poll_free(g_poll);
        g_poll = NULL;
    }
    if (g_published)
        LOGI(T_MDNS, "withdrew \"%s\"", g_instance);
    g_published = false;
}

bool mdns_avahi_published(void) { return g_published; }
