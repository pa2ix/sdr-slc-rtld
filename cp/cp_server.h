/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#ifndef CP_SERVER_H
#define CP_SERVER_H

/*
 * cp_server — TCP accept loop for the SLC-CP control plane
 *
 * Listens on CP_PORT (4620), accepts connections, creates a Session for
 * each, and spawns a detached cp_session_thread per connection.
 */

/* Start the CP server (creates listening socket, spawns accept thread).
 * Returns 0 on success, -1 on error.                                     */
int  cp_server_start(void);

/* Signal the accept thread to stop and close the listening socket.       */
void cp_server_stop(void);

#endif /* CP_SERVER_H */
