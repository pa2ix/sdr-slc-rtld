/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#ifndef CP_COMMANDS_H
#define CP_COMMANDS_H

/*
 * cp_commands — one handler function per SLC-CP command.
 *
 * Each handler receives:
 *   s      — the session owning this connection
 *   id     — the request "id" string (echoed in response)
 *   tokens — JSMN token array for the entire message
 *   ntok   — number of tokens
 *   js     — raw JSON string (tokens index into this)
 *
 * Handlers are responsible for:
 *   1. Parsing required/optional params from the tokens array.
 *   2. Validating params and calling send_error() on validation failure.
 *   3. Mutating session state and/or hardware state.
 *   4. Calling send_ok() or send_event() to respond.
 */

#include "cp_session.h"
#include "../json/jsmn.h"

/* ---- Response helpers -------------------------------------------------- */

/* Send a successful response with an arbitrary result object.
 * result_json must be a valid JSON object fragment, e.g. "{\"a\":1}".    */
void send_ok(Session *s, const char *id, const char *result_json);

/* Send an error response.  code must be one of the §22 registry values.  */
void send_error(Session *s, const char *id,
                const char *code, const char *message);

/* Send an asynchronous event.  params_json is the params object fragment. */
void send_event(Session *s, const char *event, const char *params_json);

/* ---- Command handlers -------------------------------------------------- */
void cmd_hello            (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_authenticate     (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_ping             (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_get_capabilities (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_get_status       (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_set_frequency    (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_set_sample_rate  (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_set_control      (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_get_control      (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_open_stream      (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_close_stream     (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_start_rx         (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_stop_rx          (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_get_clock_status (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);

/* True when a command, as sent, addresses the rx function and is therefore
 * gated by §26.1 when rx requires authentication.  hello, authenticate,
 * get_capabilities, ping, get_status, get_clock_status and get_control are
 * never gated (§26.1 exempts the first two; the rest only read).        */
bool cmd_addresses_rx(const char *cmd, jsmntok_t *t, int ntok, const char *js);

/*
 * Removed in Release 1.2:
 *
 *   cmd_get_location / cmd_set_location / cmd_set_clock
 *     RX Tier 3 (§20).  This daemon declares Tier 2, and §5.2 says a
 *     minimal device answers unknown_command for them.  They are absent
 *     from the dispatch table so they do exactly that.
 *
 *   cmd_not_supported
 *     Was wired to the tx commands so they answered not_supported.
 *     §24.1 requires unknown_command for a command that is not
 *     implemented; not_supported is for a function or control that is
 *     addressed but absent.  Dropping the entries gets the right answer.
 */

#endif /* CP_COMMANDS_H */
