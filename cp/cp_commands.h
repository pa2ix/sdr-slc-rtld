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

/* Send an error response.  code must be one of the §15 registry values.  */
void send_error(Session *s, const char *id,
                const char *code, const char *message);

/* Send an asynchronous event.  params_json is the params object fragment. */
void send_event(Session *s, const char *event, const char *params_json);

/* ---- Command handlers -------------------------------------------------- */
void cmd_hello            (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
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
void cmd_get_location     (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);
void cmd_not_supported    (Session *s, const char *id, jsmntok_t *t, int ntok, const char *js);

#endif /* CP_COMMANDS_H */
