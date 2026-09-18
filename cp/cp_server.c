/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "cp_server.h"
#include "cp_session.h"
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

static int       s_listen_fd = -1;
static pthread_t s_accept_tid;
static volatile bool s_running = false;

/* Each client session thread is detached — it calls session_destroy() on
 * exit to free its slot.                                                  */
static void *accept_thread(void *arg) {
    (void)arg;
    LOG_INFO("cp", "accept thread started on port %u", g_cp_port);

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = accept(s_listen_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!s_running) break;
            if (errno == EINTR || errno == EAGAIN) continue;
            LOG_WARN("cp", "accept: %s", strerror(errno));
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        LOG_INFO("cp", "new connection from %s", ip_str);

        Session *sess = session_create(client_fd, client_addr.sin_addr);
        if (!sess) {
            LOG_WARN("cp", "session table full, rejecting %s", ip_str);
            close(client_fd);
            continue;
        }

        /* Spawn a detached thread for this session */
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int r = pthread_create(&tid, &attr, cp_session_thread, sess);
        pthread_attr_destroy(&attr);
        if (r != 0) {
            LOG_ERR("cp", "pthread_create: %s", strerror(r));
            session_destroy(sess);
        }
    }

    LOG_INFO("cp", "accept thread exiting");
    return NULL;
}

int cp_server_start(void) {
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        LOG_ERR("cp", "socket: %s", strerror(errno)); return -1;
    }

    int yes = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(g_cp_port)
    };
    if (bind(s_listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERR("cp", "bind: %s", strerror(errno)); return -1;
    }
    if (listen(s_listen_fd, CP_BACKLOG) < 0) {
        LOG_ERR("cp", "listen: %s", strerror(errno)); return -1;
    }

    s_running = true;
    int r = pthread_create(&s_accept_tid, NULL, accept_thread, NULL);
    if (r != 0) {
        LOG_ERR("cp", "pthread_create: %s", strerror(r));
        s_running = false;
        return -1;
    }
    return 0;
}

void cp_server_stop(void) {
    s_running = false;
    if (s_listen_fd >= 0) {
        shutdown(s_listen_fd, SHUT_RDWR);
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    pthread_join(s_accept_tid, NULL);
}
