/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "cp_commands.h"
#include "cp_session.h"
#include "../json/json_builder.h"
#include "../rtl/rtl_bridge.h"
#include "../vita/vita_tx.h"
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <math.h>

/* =========================================================================
 * SLC-CP command handlers — SDR-SLC-CP-1.0 Release 1.3
 *
 * This device implements the envelope plus one function block: rx, at
 * Tier 2.  There is no tx block and no codec block, so their capability
 * keys are absent (§11.1) and their commands answer unknown_command
 * (§24.1) rather than being stubbed out.
 *
 * Release 1.3 changed nothing on the receive path of a Tier 1 device
 * (§1.1).  What it did change here: the fw string (§6.2, §10.1),
 * stream_state carries reason (§21.2), get_clock_status reports the host
 * clock honestly (§20), and the §26 authenticate command exists so an
 * operator can lock the receiver if they want to.
 * ========================================================================= */

/* Wall clock, for §20.  gmtime_r rather than the logger's helper so this
 * file stays independent of it.                                          */
static void utc_now(char *iso, size_t cap, long long *epoch_s) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(iso, cap, "%s.%03dZ", stamp, (int)(ts.tv_nsec / 1000000L));
    *epoch_s = (long long)ts.tv_sec;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* =========================================================================
 * Response helpers
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Escape a string for inclusion in a JSON string literal.
 *
 * The request id is chosen by the client (§9.1: "any unique string you
 * choose") and is echoed verbatim into every response.  An id containing
 * a quote or a backslash would otherwise emit malformed JSON, and since
 * the frame length is computed from the finished buffer the client would
 * see a valid-length frame with unparseable content.  §8.2 also forbids
 * unescaped control characters inside JSON strings.
 * --------------------------------------------------------------------- */
static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *esc = NULL;
        char ubuf[8];
        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        default:
            if (*p < 0x20) {
                snprintf(ubuf, sizeof(ubuf), "\\u%04x", *p);
                esc = ubuf;
            }
            break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= cap) break;
            memcpy(out + o, esc, n);
            o += n;
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
}

void send_ok(Session *s, const char *id, const char *result_json) {
    static _Thread_local char buf[MAX_MSG_BYTES];
    char idbuf[288];
    json_escape(id, idbuf, sizeof(idbuf));
    snprintf(buf, sizeof(buf),
        "{\"type\":\"response\",\"id\":\"%s\","
        "\"status\":\"ok\",\"result\":%s}",
        idbuf, result_json ? result_json : "{}");
    send_framed(s, buf);
}

void send_error(Session *s, const char *id,
                const char *code, const char *message) {
    char buf[1024];
    char idbuf[288], msgbuf[512];
    json_escape(id, idbuf, sizeof(idbuf));
    json_escape(message, msgbuf, sizeof(msgbuf));
    snprintf(buf, sizeof(buf),
        "{\"type\":\"response\",\"id\":\"%s\","
        "\"status\":\"error\","
        "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        idbuf, code, msgbuf);
    send_framed(s, buf);
}

void send_event(Session *s, const char *event, const char *params_json) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"event\",\"event\":\"%s\",\"params\":%s}",
        event, params_json ? params_json : "{}");
    send_framed(s, buf);
}

/* =========================================================================
 * Parameter helpers
 * ========================================================================= */

static int find_params(const char *js, jsmntok_t *t, int ntok) {
    (void)ntok;
    return tok_find_key(js, t, 0, "params");
}

/* -------------------------------------------------------------------------
 * Function addressing [§12]
 *
 * Release 1.1 called this field `direction` and gave it two values.
 * Release 1.2 renamed it to `function`, because it always addressed a
 * function block rather than a signal direction, and the value space now
 * includes identifiers such as `codec` for which the old name is simply
 * wrong.  There is no compatibility shim: a Release 1.1 client sending
 * `direction` gets missing_parameter, which is exactly what §12.1 says
 * should happen.
 *
 * Returns the params token index on success, or -1 having already sent
 * the appropriate error.
 * --------------------------------------------------------------------- */
static int require_function(Session *s, const char *id,
                            const char *js, jsmntok_t *t, int ntok) {
    int p = find_params(js, t, ntok);
    if (p < 0) {
        send_error(s, id, "missing_parameter", "params required");
        return -1;
    }

    int f = tok_find_key(js, t, p, "function");
    if (f < 0) {
        /* A Release 1.1 client will land here.  Name the field it should
         * be sending — the error is otherwise very hard to read from the
         * client side.                                                   */
        if (tok_find_key(js, t, p, "direction") >= 0) {
            send_error(s, id, "missing_parameter",
                       "function required (Release 1.2 renamed direction "
                       "to function; see SDR-SLC-CP-1.0 12.1)");
        } else {
            send_error(s, id, "missing_parameter", "function required");
        }
        return -1;
    }

    if (tok_eq(js, &t[f], "rx")) return p;

    /* §12: a device returns not_supported for a function it does not
     * implement.  tx and codec are named here only so the message is
     * useful; any unknown identifier gets the same answer.              */
    send_error(s, id, "not_supported",
               "This device implements the rx function block only");
    return -1;
}

/* §12.3: a device that omits channels has exactly one channel, and an
 * omitted channel parameter addresses it.  A client that names a channel
 * anyway is addressing something that does not exist.                   */
static bool channel_ok(Session *s, const char *id,
                       const char *js, jsmntok_t *t, int p) {
    int c = tok_find_key(js, t, p, "channel");
    if (c < 0) return true;
    send_error(s, id, "invalid_parameter",
               "This device has a single signal path and publishes no "
               "channels; omit the channel parameter");
    return false;
}

/* §17: the rx channel is exclusive — one tuned frequency, one gain.  A
 * client that does not hold it may read state but may not change it.   */
static bool may_configure(Session *s, const char *id) {
    if (!cp_rx_owned_by_other(s)) return true;
    send_error(s, id, "in_use",
               "The rx channel is held by another client");
    return false;
}

/* =========================================================================
 * Hardware monitors [§18.3, amendment A1]
 *
 * A receive-only dongle has no PA, so the three pa_* channels are null.
 * The host it runs on may know its own temperature and supply, and those
 * go in the standard board_temperature_celsius and supply_voltage_v
 * channels: on a Raspberry Pi the SoC temperature is
 * /sys/class/thermal/thermal_zone0/temp (millidegrees) on every model, and
 * the Pi 5 firmware exposes the 5 V rail as a hwmon named rpi_volt.  Any
 * other Linux host with the same files gets the same treatment; a host
 * without them reports null, which §18.3 allows.
 * ========================================================================= */
static bool read_sysfs_number(const char *path, double scale, double *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    double v;
    int n = fscanf(f, "%lf", &v);
    fclose(f);
    if (n != 1) return false;
    *out = v * scale;
    return true;
}

static bool read_board_temperature(double *out) {
    return read_sysfs_number("/sys/class/thermal/thermal_zone0/temp", 1e-3, out)
        && *out > -40.0 && *out < 150.0;
}

/* The hwmon named rpi_volt (Pi 5 firmware), in0_input in millivolts.
 * Nothing else is trusted to be the supply.                             */
static bool read_supply_voltage(double *out) {
    for (int i = 0; i < 16; i++) {
        char path[96], name[64] = "";
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof(name), f)) name[0] = '\0';
        fclose(f);
        if (strncmp(name, "rpi_volt", 8) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/in0_input", i);
        if (read_sysfs_number(path, 1e-3, out)) return *out > 0.0 && *out < 30.0;
    }
    return false;
}

/* Formats a nullable number: "41.5" or "null". */
static const char *fmt_nullable(char *buf, size_t cap, bool have, double v) {
    if (have) snprintf(buf, cap, "%.1f", v);
    else      snprintf(buf, cap, "null");
    return buf;
}

/* =========================================================================
 * Capabilities [§11]
 * ========================================================================= */

/* The tuner's gain table, as the R820T2 reports it: 29 discrete steps
 * from 0.0 to 49.6 dB, unevenly spaced.  Published as allowed_values so a
 * client never offers a gain the tuner cannot set.  The default is the
 * table entry nearest RTL_DEFAULT_GAIN, because a default that is not on
 * the list is a value the tuner would silently round (20.0 is not a
 * R820T2 step; 19.7 is).                                                 */
static int    s_gains[64];
static int    s_ngains = -1;
static double s_gain_default;

static void load_gain_table(void) {
    if (s_ngains >= 0) return;
    s_ngains = rtl_get_gain_list(s_gains, 64);
    if (s_ngains <= 0) { s_ngains = 0; s_gain_default = RTL_DEFAULT_GAIN / 10.0; return; }
    int best = s_gains[0];
    for (int i = 1; i < s_ngains; i++)
        if (abs(s_gains[i] - RTL_DEFAULT_GAIN) < abs(best - RTL_DEFAULT_GAIN))
            best = s_gains[i];
    s_gain_default = best / 10.0;
}

double cp_default_gain_db(void) { load_gain_table(); return s_gain_default; }

static void build_capabilities_json(char *out, size_t cap) {
    /* Gain list from the tuner itself — never hardcoded.               */
    load_gain_table();
    double gain_min = (s_ngains > 0) ? s_gains[0]             / 10.0 : 0.0;
    double gain_max = (s_ngains > 0) ? s_gains[s_ngains - 1]  / 10.0 : 49.6;
    char gains_arr[400] = "[";
    for (int i = 0; i < s_ngains; i++) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s%.1f", i ? "," : "", s_gains[i] / 10.0);
        strncat(gains_arr, tmp, sizeof(gains_arr) - strlen(gains_arr) - 1);
    }
    strncat(gains_arr, "]", sizeof(gains_arr) - strlen(gains_arr) - 1);

    uint64_t        freq_min   = rtl_freq_min();
    uint64_t        freq_max   = rtl_freq_max();
    int             rate_count = 0;
    const uint32_t *rates      = rtl_rate_list(&rate_count);
    uint32_t        pref_rate  = rtl_preferred_rate();
    double          pref_gain  = s_gain_default;

    /* Sample rates.  §13.2 and §24.2: the discrete list is normative and
     * a device SHALL NOT additionally advertise a min/max range.  R1.1
     * published both, which left an independent client unable to tell
     * which one governed; sample_rate_min_hz and sample_rate_max_hz are
     * gone from this response for that reason.                          */
    char rates_arr[160] = "[";
    for (int i = 0; i < rate_count; i++) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s%u", i ? "," : "", rates[i]);
        strncat(rates_arr, tmp, sizeof(rates_arr) - strlen(rates_arr) - 1);
    }
    strncat(rates_arr, "]", sizeof(rates_arr) - strlen(rates_arr) - 1);

    /* Controls.  §11.3: every descriptor carries a type, and bias_tee is
     * a float in volts with allowed_values — not the boolean it was in
     * R1.1.  direct_sampling is a standard Tier 2 enum, so it is settable
     * at runtime here rather than only from the command line.           */
    char controls[1200];
    snprintf(controls, sizeof(controls),
        "["
        "{\"name\":\"rf_gain\",\"type\":\"float\",\"unit\":\"dB\","
         "\"min\":%.1f,\"max\":%.1f,\"default\":%.1f,"
         "\"allowed_values\":%s},"
        "{\"name\":\"agc\",\"type\":\"boolean\",\"default\":false},"
        "{\"name\":\"bias_tee\",\"type\":\"float\",\"unit\":\"V\","
         "\"min\":0.0,\"max\":%.1f,\"step\":%.1f,\"default\":0.0,"
         "\"allowed_values\":[0.0,%.1f]},"
        "{\"name\":\"ppm_correction\",\"type\":\"integer\","
         "\"min\":-100,\"max\":100,\"step\":1,\"default\":0},"
        "{\"name\":\"direct_sampling\",\"type\":\"enum\","
         "\"values\":[\"off\",\"i_branch\",\"q_branch\"],"
         "\"default\":\"off\"}"
        "]",
        gain_min, gain_max, pref_gain, gains_arr,
        RTL_BIAS_TEE_VOLTS, RTL_BIAS_TEE_VOLTS, RTL_BIAS_TEE_VOLTS);

    /* §11.1: a key is present for each implemented function block and
     * absent for every other.  R1.1 required a tx object with
     * supported:false; R1.2 drops it, because absence is now the single
     * uniform signal and a client has to handle absence anyway for blocks
     * it has never heard of.                                            */
    snprintf(out, cap,
        "{"
        "\"schema\":\"sdrslc.capabilities\","
        "\"schema_version\":%d,"
        "\"rx\":{"
          "\"tier\":%d,"
          "\"access\":\"exclusive\","
          /* §26.1: the method required to address rx, or none.  Off by
           * default; --rx-auth psk turns it on.                          */
          "\"auth\":\"%s\","
          "\"frequency_ranges_hz\":[{\"min\":%llu,\"max\":%llu}],"
          "\"sample_rates_sps\":%s,"
          "\"preferred_sample_rate_sps\":%u,"
          "\"preferred_gain_db\":%.1f,"
          "\"reference_level_dbm_at_preferred_gain\":%.1f,"
          "\"reference_level_accuracy_db\":%.1f,"
          "\"transports\":["
            "{\"protocol\":\"vita49\",\"protocol_version\":\"VITA-A/1.0\","
             "\"source_port\":%d,\"samples_per_packet\":%u}"
          "],"
          /* Single native format: unsigned 8-bit, exactly what the
           * RTL2832U ADC produces (SDR-SLC-CP-1.0 §5.1 advertises
           * this same format for the reference RTL-SDR bridge).  Sending
           * u8 halves the wire bandwidth versus int16 and lets the sender
           * ship raw ADC bytes with no per-sample conversion.
           *
           * zero_point (§11.3 addendum, VITA §5.1): the code value that
           * is zero signal.  The RTL2832U ADC is centred at ~127.4, not
           * 128; a 0.6 LSB shift is not representable in 8 bits, so the
           * daemon declares it and the client subtracts it before
           * scaling.  Assuming 128 leaves a coherent DC term of about
           * -47 dBFS - a spike at the capture centre.                    */
          "\"iq_formats\":["
            "{\"encoding\":\"unsigned\",\"bits_per_component\":8,"
             "\"container_bits\":8,\"packing\":\"interleaved\","
             "\"byte_order\":\"big_endian\",\"zero_point\":%.2f,"
             "\"native\":true,\"supported\":true,\"label\":\"u8\"}"
          "],"
          "\"controls\":%s"
        "}"
        "}",
        CAP_SCHEMA_VERSION,
        RX_TIER,
        cp_rx_requires_auth() ? AUTH_METHOD_PSK : "none",
        (unsigned long long)freq_min, (unsigned long long)freq_max,
        rates_arr,
        pref_rate,
        pref_gain,
        rtl_reference_level_dbm(pref_gain),
        RTL_REF_LEVEL_ACCURACY_DB,
        g_vita_src_port,
        g_samples_per_pkt,
        g_u8_zero_point,
        controls
    );
}

/* =========================================================================
 * Envelope [§10, §11]
 * ========================================================================= */

/* ---- hello ---- */
void cmd_hello(Session *s, const char *id,
               jsmntok_t *t, int ntok, const char *js) {
    int p = find_params(js, t, ntok);
    if (p < 0) { send_error(s, id, "missing_parameter", "params required"); return; }

    int vers_idx = tok_find_key(js, t, p, "supported_protocol_versions");
    if (vers_idx < 0 || t[vers_idx].type != JSMN_ARRAY) {
        send_error(s, id, "missing_parameter",
                   "hello requires supported_protocol_versions, an array of "
                   "version strings such as [\"1.0\"]");
        return;
    }

    /* §10.1: select the highest mutually supported version.  We speak
     * exactly one.  Release numbers (1.1, 1.2, 1.3) are document
     * revisions and are named in the refusal, because a client that
     * offers them is making an understandable mistake.                 */
    bool supports = false, offered_release = false;
    int count = t[vers_idx].size;
    int vi = vers_idx + 1;
    for (int i = 0; i < count; i++) {
        if (tok_eq(js, &t[vi], PROTO_VERSION)) supports = true;
        else if (tok_eq(js, &t[vi], "1.1") || tok_eq(js, &t[vi], "1.2") ||
                 tok_eq(js, &t[vi], "1.3")) offered_release = true;
        int skip = t[vi].size; vi++;
        while (skip-- > 0) { skip += t[vi].size; vi++; }
    }
    if (!supports) {
        send_error(s, id, "unsupported_protocol_version",
                   offered_release
                   ? "No common protocol version: this device speaks \"1.0\". "
                     "Release numbers (1.1, 1.2, 1.3) are document revisions, "
                     "not protocol versions (SDR-SLC-CP-1.0 10.1)"
                   : "No common protocol version; this device speaks \"1.0\"");
        return;
    }

    int cn = tok_find_key(js, t, p, "client_name");
    if (cn >= 0) tok_str(js, &t[cn], s->client_name, sizeof(s->client_name));
    else         snprintf(s->client_name, sizeof(s->client_name), "(unnamed)");

    s->state = SESSION_IDLE;

    /* §26.2: the auth object is present if and only if a function
     * requires authentication.  The nonce is fresh per connection.     */
    char auth_obj[160] = "";
    if (cp_rx_requires_auth()) {
        if (!auth_make_nonce(s->nonce, s->nonce_hex)) {
            LOG_ERR("cp", "no random source for the auth nonce; rx stays "
                    "locked on this connection");
            s->nonce_valid = false;
            s->nonce_hex[0] = '\0';
        } else {
            s->nonce_valid = true;
        }
        snprintf(auth_obj, sizeof(auth_obj),
                 ",\"auth\":{\"methods\":[\"%s\"],\"nonce\":\"%s\"}",
                 AUTH_METHOD_PSK, s->nonce_hex);
    }

    /* §10.1: hello deliberately says nothing about what the device is.
     * The extra fields below are informational and clients ignore any
     * they do not know.  firmware_version is "<implementation>/<version>",
     * the same string discovery carries in fw=.                        */
    char result[800];
    snprintf(result, sizeof(result),
        "{\"protocol_name\":\"%s\","
        "\"selected_protocol_version\":\"%s\","
        "\"device_name\":\"%s\","
        "\"manufacturer\":\"%s\","
        "\"model\":\"%s\","
        "\"hardware_revision\":\"%s\","
        "\"firmware_version\":\"%s\"%s}",
        PROTO_NAME, PROTO_VERSION, g_device_name, MANUFACTURER,
        MODEL_NAME, HW_REVISION, SLC_FW_STRING, auth_obj);
    send_ok(s, id, result);
    LOG_INFO("cp", "hello from %s — protocol %s (release %s)%s",
             s->client_name, PROTO_VERSION, SLC_CP_RELEASE,
             cp_rx_requires_auth() ? ", challenge issued" : "");
}

/* ---- authenticate [§26] ---- */
#define AUTH_MAX_FAILURES     10
#define AUTH_WINDOW_NS        60000000000ull   /* one minute */

void cmd_authenticate(Session *s, const char *id,
                      jsmntok_t *t, int ntok, const char *js) {
    if (!cp_rx_requires_auth()) {
        /* Nothing on this device is locked.  Say so rather than fail: a
         * client that authenticates unconditionally should not be
         * punished for it.                                              */
        send_ok(s, id, "{\"authenticated\":true,\"functions\":[]}");
        return;
    }

    int p = find_params(js, t, ntok);
    int mi = (p >= 0) ? tok_find_key(js, t, p, "method")   : -1;
    int ki = (p >= 0) ? tok_find_key(js, t, p, "key_id")   : -1;
    int ri = (p >= 0) ? tok_find_key(js, t, p, "response") : -1;
    if (mi < 0 || ri < 0) {
        send_error(s, id, "missing_parameter",
                   "authenticate requires \"method\" and \"response\"; "
                   "\"key_id\" defaults to \"default\"");
        return;
    }
    char method[48], key_id[AUTH_KEY_ID_MAX], resp[80];
    tok_str(js, &t[mi], method, sizeof(method));
    tok_str(js, &t[ri], resp,   sizeof(resp));
    if (ki >= 0) tok_str(js, &t[ki], key_id, sizeof(key_id));
    else         snprintf(key_id, sizeof(key_id), "default");

    /* §26.3: rate-limit failed attempts.  Ten a minute is ample.       */
    uint64_t now = mono_ns();
    if (now - s->auth_window_ns > AUTH_WINDOW_NS) {
        s->auth_window_ns = now;
        s->auth_failures  = 0;
    }

    /* §26.3: a failed attempt SHALL NOT reveal whether the method, the
     * key identifier or the MAC was at fault - every failure path funnels
     * through the same reply.  An unconfigured device fails the same way
     * (refuse rather than accept any key).                              */
    bool ok = s->nonce_valid &&
              strcmp(method, AUTH_METHOD_PSK) == 0 &&
              g_keys.n > 0 &&
              auth_verify(&g_keys, key_id, s->nonce, AUTH_NONCE_BYTES, resp);

    if (!ok) {
        s->auth_failures++;
        if (g_keys.n == 0)
            LOG_WARN("cp", "authenticate from %s refused: no pre-shared key "
                     "is configured, so rx is locked. Run: sudo "
                     SLC_DAEMON_NAME " keygen", s->client_name);
        else
            LOG_WARN("cp", "authenticate from %s failed (key_id \"%s\", "
                     "attempt %u this minute)", s->client_name, key_id,
                     s->auth_failures);

        /* §26.3: never reuse a nonce after a failure.  Issue a fresh one
         * in details.nonce; if none can be made, the client must
         * reconnect.                                                    */
        char details[96] = "{}";
        if (auth_make_nonce(s->nonce, s->nonce_hex)) {
            s->nonce_valid = true;
            snprintf(details, sizeof(details), "{\"nonce\":\"%s\"}",
                     s->nonce_hex);
        } else {
            s->nonce_valid = false;
        }
        char idbuf[288], buf[640];
        json_escape(id, idbuf, sizeof(idbuf));
        snprintf(buf, sizeof(buf),
            "{\"type\":\"response\",\"id\":\"%s\",\"status\":\"error\","
            "\"error\":{\"code\":\"auth_failed\","
            "\"message\":\"authentication was rejected\","
            "\"details\":%s}}", idbuf, details);
        send_framed(s, buf);

        if (s->auth_failures >= AUTH_MAX_FAILURES)
            s->close_after_reply = true;
        return;
    }

    s->rx_unlocked = true;
    /* A nonce is single-use: a replayed authenticate on this connection
     * would otherwise still verify.                                     */
    s->nonce_valid = false;
    LOG_INFO("cp", "%s authenticated for rx with key_id \"%s\"",
             s->client_name, key_id);
    send_ok(s, id, "{\"authenticated\":true,\"functions\":[\"rx\"]}");
}

/* §26.1 gate.  A command that names function:"rx" in params, and the
 * three stream-handle commands (which can only refer to an rx stream on
 * this device).  A command naming a function the device lacks is not
 * gated: it answers not_supported as before, which tells the client
 * something true instead of demanding a key for nothing.               */
bool cmd_addresses_rx(const char *cmd, jsmntok_t *t, int ntok, const char *js) {
    if (strcmp(cmd, "start_rx") == 0 || strcmp(cmd, "stop_rx") == 0 ||
        strcmp(cmd, "close_stream") == 0)
        return true;
    if (strcmp(cmd, "set_frequency") && strcmp(cmd, "set_sample_rate") &&
        strcmp(cmd, "set_control") && strcmp(cmd, "open_stream"))
        return false;
    int p = find_params(js, t, ntok);
    if (p < 0) return false;
    int f = tok_find_key(js, t, p, "function");
    return f >= 0 && tok_eq(js, &t[f], "rx");
}

/* ---- get_capabilities ---- */
void cmd_get_capabilities(Session *s, const char *id,
                          jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    char caps[6144];
    build_capabilities_json(caps, sizeof(caps));
    char result[6272];
    snprintf(result, sizeof(result), "{\"capabilities\":%s}", caps);
    send_ok(s, id, result);
}

/* ---- ping [RX-T2] ---- */
void cmd_ping(Session *s, const char *id,
              jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    send_ok(s, id, "{}");
}

/* =========================================================================
 * Status [§18]
 * ========================================================================= */

/* ---- get_status [RX-T2] ---- */
void cmd_get_status(Session *s, const char *id,
                    jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;

    /* §18.2: fields listed as nullable are present with a JSON null when
     * unavailable, never absent, so a client can parse unconditionally. */
    double adc_dbfs;
    char adc_buf[32];
    if (rtl_get_adc_level_dbfs(&adc_dbfs))
        snprintf(adc_buf, sizeof(adc_buf), "%.1f", adc_dbfs);
    else
        snprintf(adc_buf, sizeof(adc_buf), "null");

    static const char *ds_names[] = {"off", "i_branch", "q_branch"};

    double temp = 0, volt = 0;
    char   temp_buf[24], volt_buf[24];
    bool   have_temp = read_board_temperature(&temp);
    bool   have_volt = read_supply_voltage(&volt);

    char result[1024];
    snprintf(result, sizeof(result),
        "{\"rx\":{"
          "\"enabled\":true,"
          "\"streaming\":%s,"
          "\"control_state\":\"%s\","
          "\"frequency_hz\":%" PRIu64 ","
          "\"sample_rate_sps\":%u,"
          "\"actual_sample_rate_sps\":%u,"
          "\"reference_level_dbm\":%.1f,"
          "\"adc_overload\":%s,"
          "\"adc_level_dbfs\":%s,"
          /* The R820T2 exposes no PLL lock indicator, so this is null
           * rather than an invented true.  §24.3 asks for the field, not
           * for a value the hardware cannot supply.                     */
          "\"lo_locked\":null,"
          "\"controls\":{"
            "\"rf_gain\":%.1f,"
            "\"agc\":%s,"
            "\"bias_tee\":%.1f,"
            "\"ppm_correction\":%d,"
            "\"direct_sampling\":\"%s\""
          "},"
          /* §18.3 (A1): the object is always present at Tier 2 with all
           * six standard channels, null where there is no sensor.  The
           * pa_* channels are always null here - there is no PA.       */
          "\"hardware_monitors\":{"
            "\"pa_temperature_celsius\":null,"
            "\"pa_supply_voltage_v\":null,"
            "\"pa_drain_current_ma\":null,"
            "\"board_temperature_celsius\":%s,"
            "\"supply_voltage_v\":%s,"
            "\"total_current_ma\":null"
          "}"
        "}}",
        (s->state == SESSION_STREAMING) ? "true" : "false",
        cp_rx_control_state(s),
        g_freq_hz,
        g_rate_requested_sps,
        g_rate_sps,
        rtl_reference_level_dbm(g_gain_tenth_db / 10.0),
        g_adc_overload ? "true" : "false",
        adc_buf,
        g_gain_tenth_db / 10.0,
        g_agc_enabled ? "true" : "false",
        g_bias_tee_volts,
        g_ppm_correction,
        ds_names[g_sampling_mode],
        fmt_nullable(temp_buf, sizeof(temp_buf), have_temp, temp),
        fmt_nullable(volt_buf, sizeof(volt_buf), have_volt, volt)
    );
    send_ok(s, id, result);
}

/* ---- get_clock_status [RX-T2] ---- */
void cmd_get_clock_status(Session *s, const char *id,
                           jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    /* §20 (Release 1.3): a device with any time source at all - here the
     * host's NTP-or-whatever clock - stamps its best estimate of UTC under
     * TSI=01 and says so honestly: source free_running, synced false, and
     * the actual host time.  Release 1.2 had this daemon report null here
     * while stamping UTC on the wire, which contradicted itself.       */
    char iso[40];
    long long epoch;
    utc_now(iso, sizeof(iso), &epoch);
    char result[256];
    snprintf(result, sizeof(result),
        "{"
        "\"source\":\"free_running\","
        "\"synced\":false,"
        "\"accuracy_us\":null,"
        "\"utc_time\":\"%s\","
        "\"epoch_s\":%lld,"
        "\"ptp\":{\"available\":false},"
        "\"gps\":{\"available\":false}"
        "}", iso, epoch);
    send_ok(s, id, result);
}

/* =========================================================================
 * Configuration [§13]
 * ========================================================================= */

/* ---- set_frequency [RX-T1] ---- */
void cmd_set_frequency(Session *s, const char *id,
                       jsmntok_t *t, int ntok, const char *js) {
    int p = require_function(s, id, js, t, ntok);
    if (p < 0) return;
    if (!channel_ok(s, id, js, t, p)) return;
    if (!may_configure(s, id)) return;

    int fi = tok_find_key(js, t, p, "frequency_hz");
    if (fi < 0) {
        send_error(s, id, "missing_parameter", "frequency_hz required");
        return;
    }

    char fstr[32];
    tok_str(js, &t[fi], fstr, sizeof(fstr));
    errno = 0;
    uint64_t freq = strtoull(fstr, NULL, 10);
    if (errno == ERANGE || freq < 1) {
        send_error(s, id, "invalid_parameter", "frequency_hz out of range");
        return;
    }

    if (freq < rtl_freq_min() || freq > rtl_freq_max()) {
        send_error(s, id, "unsupported_frequency",
                   "Frequency outside the published range for this mode");
        return;
    }

    uint32_t applied = rtl_set_frequency(freq);
    if (applied == 0) {
        send_error(s, id, "device_error", "Frequency set failed"); return;
    }
    s->frequency_hz = applied;

    char result[128];
    snprintf(result, sizeof(result),
             "{\"function\":\"rx\",\"applied_frequency_hz\":%u}", applied);
    send_ok(s, id, result);

    /* §13.1: the device MAY emit frequency_changed once the synthesiser
     * has settled.  Everyone listening to this receiver wants it.       */
    char ev[128];
    snprintf(ev, sizeof(ev),
             "{\"function\":\"rx\",\"frequency_hz\":%u}", applied);
    cp_broadcast_event("frequency_changed", ev);
}

/* ---- set_sample_rate [RX-T1] ---- */
void cmd_set_sample_rate(Session *s, const char *id,
                         jsmntok_t *t, int ntok, const char *js) {
    int p = require_function(s, id, js, t, ntok);
    if (p < 0) return;
    if (!channel_ok(s, id, js, t, p)) return;
    if (!may_configure(s, id)) return;

    if (s->state == SESSION_STREAMING || s->state == SESSION_STREAM_OPEN) {
        send_error(s, id, "stream_reconfiguration_required",
                   "Close the stream before changing the sample rate");
        return;
    }

    int ri = tok_find_key(js, t, p, "sample_rate_sps");
    if (ri < 0) {
        send_error(s, id, "missing_parameter", "sample_rate_sps required");
        return;
    }

    char rstr[16];
    tok_str(js, &t[ri], rstr, sizeof(rstr));
    uint32_t rate = (uint32_t)strtoul(rstr, NULL, 10);

    int rate_count = 0;
    const uint32_t *rates = rtl_rate_list(&rate_count);
    bool valid = false;
    for (int i = 0; i < rate_count; i++)
        if (rates[i] == rate) { valid = true; break; }
    if (!valid) {
        send_error(s, id, "unsupported_sample_rate",
                   "Rate not present in the published sample_rates_sps");
        return;
    }

    uint32_t applied = rtl_set_sample_rate(rate);
    if (applied == 0) {
        send_error(s, id, "device_error", "Sample rate set failed"); return;
    }
    s->sample_rate_sps = applied;

    char result[160];
    snprintf(result, sizeof(result),
             "{\"function\":\"rx\",\"applied_sample_rate_sps\":%u}", rate);
    send_ok(s, id, result);

    char ev[192];
    snprintf(ev, sizeof(ev),
             "{\"function\":\"rx\",\"sample_rate_sps\":%u,"
             "\"actual_sample_rate_sps\":%u}", rate, applied);
    cp_broadcast_event("sample_rate_changed", ev);
}

/* -------------------------------------------------------------------------
 * direct_sampling — mode switch helper
 *
 * Switching the direct sampling branch changes both the frequency range
 * and the usable sample rate list, so anything now out of range has to be
 * brought back inside it and the client has to be told.
 * --------------------------------------------------------------------- */
static void apply_mode_change_side_effects(void) {
    /* Frequency */
    uint64_t fmin = rtl_freq_min(), fmax = rtl_freq_max();
    if (g_freq_hz < fmin || g_freq_hz > fmax) {
        uint64_t target = (g_freq_hz < fmin) ? fmin : fmax;
        uint32_t applied = rtl_set_frequency(target);
        if (applied) {
            char ev[128];
            snprintf(ev, sizeof(ev),
                     "{\"function\":\"rx\",\"frequency_hz\":%u}", applied);
            cp_broadcast_event("frequency_changed", ev);
        }
    }

    /* Sample rate */
    int rate_count = 0;
    const uint32_t *rates = rtl_rate_list(&rate_count);
    bool ok = false;
    for (int i = 0; i < rate_count; i++)
        if (rates[i] == g_rate_requested_sps) { ok = true; break; }
    if (!ok) {
        uint32_t want = rtl_preferred_rate();
        uint32_t applied = rtl_set_sample_rate(want);
        if (applied) {
            char ev[192];
            snprintf(ev, sizeof(ev),
                     "{\"function\":\"rx\",\"sample_rate_sps\":%u,"
                     "\"actual_sample_rate_sps\":%u}", want, applied);
            cp_broadcast_event("sample_rate_changed", ev);
        }
    }

    /* §13.3: on a direct_sampling change the device emits a Context
     * packet carrying the updated Payload Format before the next IF Data
     * packet.  Nothing is streaming here (the command is refused while a
     * stream is open) but the stored word still has to move.            */
    vita_update_payload_format(sampling_mode_payload_fmt(g_sampling_mode));
    vita_send_context_all();
}

/* ---- set_control [RX-T1 for rf_gain, RX-T2 for the rest] ---- */
void cmd_set_control(Session *s, const char *id,
                     jsmntok_t *t, int ntok, const char *js) {
    int p = require_function(s, id, js, t, ntok);
    if (p < 0) return;
    if (!channel_ok(s, id, js, t, p)) return;
    if (!may_configure(s, id)) return;

    int ni = tok_find_key(js, t, p, "name");
    int vi = tok_find_key(js, t, p, "value");
    if (ni < 0 || vi < 0) {
        send_error(s, id, "missing_parameter", "name and value required");
        return;
    }

    char name[32], valstr[64];
    tok_str(js, &t[ni], name, sizeof(name));
    tok_str(js, &t[vi], valstr, sizeof(valstr));
    LOG_DBG("cp", "set_control function=rx name='%s' value='%s'",
            name, valstr);

    char result[160];
    char ev[192];

    /* ---- rf_gain (float) [RX-T1] ---- */
    if (strcmp(name, "rf_gain") == 0) {
        if (t[vi].type != JSMN_PRIMITIVE || tok_is_bool(js, &t[vi])) {
            send_error(s, id, "invalid_parameter",
                       "rf_gain requires a numeric value"); return;
        }
        /* §11.3: when agc is true the hardware manages rf_gain and
         * set_control for rf_gain is ignored.                           */
        if (g_agc_enabled) {
            LOG_INFO("cp", "rf_gain ignored — AGC is active");
            snprintf(result, sizeof(result),
                     "{\"function\":\"rx\",\"name\":\"rf_gain\","
                     "\"applied_value\":%.1f}", rtl_get_gain());
            send_ok(s, id, result);
            return;
        }
        double val = strtod(valstr, NULL);
        /* §13.3: a float control with allowed_values takes one of them.
         * The tuner would round anything else, and a client that read
         * capabilities has the list; one that did not learns it here. */
        load_gain_table();
        bool on_list = false;
        for (int i = 0; i < s_ngains; i++)
            if (fabs(val - s_gains[i] / 10.0) < 0.05) { on_list = true; break; }
        if (!on_list) {
            send_error(s, id, "invalid_parameter",
                       "rf_gain must be one of the tuner's steps listed in "
                       "capabilities.rx.controls[rf_gain].allowed_values");
            return;
        }
        double applied = rtl_set_gain(val);
        s->rf_gain_db = applied;
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"rf_gain\","
                 "\"applied_value\":%.1f}", applied);
        send_ok(s, id, result);

        /* §13.3 and §24.3: a gain change means a new Reference Level, and
         * the Context packet carrying it has to reach the client before
         * the next IF Data packet — otherwise the S-meter reads the old
         * calibration against new samples.                              */
        vita_send_context_all();

        snprintf(ev, sizeof(ev),
                 "{\"function\":\"rx\",\"name\":\"rf_gain\",\"value\":%.1f}",
                 applied);
        cp_broadcast_event("control_changed", ev);
        return;

    /* ---- agc (boolean) [RX-T2] ---- */
    } else if (strcmp(name, "agc") == 0) {
        if (!tok_is_bool(js, &t[vi])) {
            send_error(s, id, "invalid_parameter",
                       "agc requires true or false"); return;
        }
        bool en = tok_is_true(js, &t[vi]);
        /* §13.3: when agc returns to false the device restores the last
         * client-set rf_gain.  rtl_set_agc() does that from
         * g_gain_tenth_db, which is only ever written by an explicit
         * rf_gain set.                                                  */
        rtl_set_agc(en);
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"agc\","
                 "\"applied_value\":%s}", en ? "true" : "false");
        send_ok(s, id, result);

        vita_send_context_all();
        snprintf(ev, sizeof(ev),
                 "{\"function\":\"rx\",\"name\":\"agc\",\"value\":%s}",
                 en ? "true" : "false");
        cp_broadcast_event("control_changed", ev);
        return;

    /* ---- bias_tee (float, volts) [RX-T2] ---- */
    } else if (strcmp(name, "bias_tee") == 0) {
        if (t[vi].type != JSMN_PRIMITIVE || tok_is_bool(js, &t[vi])) {
            /* R1.1 published bias_tee as a boolean.  Say so, because an
             * old client sending true will otherwise see only a bare
             * invalid_parameter.                                        */
            send_error(s, id, "invalid_parameter",
                       "bias_tee is a float in volts (0.0 or 5.0), not a "
                       "boolean");
            return;
        }
        double volts = strtod(valstr, NULL);
        /* §13.3: for float controls with allowed_values the value SHALL
         * match one of the listed values exactly.                       */
        if (volts != 0.0 && volts != RTL_BIAS_TEE_VOLTS) {
            send_error(s, id, "invalid_parameter",
                       "bias_tee accepts only the published "
                       "allowed_values");
            return;
        }
        if (rtl_set_bias_tee(volts) != 0) {
            send_error(s, id, "not_supported",
                       "bias_tee not supported by this hardware"); return;
        }
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"bias_tee\","
                 "\"applied_value\":%.1f}", g_bias_tee_volts);
        send_ok(s, id, result);

        snprintf(ev, sizeof(ev),
                 "{\"function\":\"rx\",\"name\":\"bias_tee\","
                 "\"value\":%.1f}", g_bias_tee_volts);
        cp_broadcast_event("control_changed", ev);
        return;

    /* ---- ppm_correction (integer) [RX-T2] ---- */
    } else if (strcmp(name, "ppm_correction") == 0) {
        if (t[vi].type != JSMN_PRIMITIVE || tok_is_bool(js, &t[vi])) {
            send_error(s, id, "invalid_parameter",
                       "ppm_correction requires an integer"); return;
        }
        int ppm = (int)strtol(valstr, NULL, 10);
        if (ppm < -100 || ppm > 100) {
            send_error(s, id, "invalid_parameter",
                       "ppm_correction out of range [-100,100]"); return;
        }
        rtl_set_ppm(ppm);
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"ppm_correction\","
                 "\"applied_value\":%d}", g_ppm_correction);
        send_ok(s, id, result);

        snprintf(ev, sizeof(ev),
                 "{\"function\":\"rx\",\"name\":\"ppm_correction\","
                 "\"value\":%d}", g_ppm_correction);
        cp_broadcast_event("control_changed", ev);
        return;

    /* ---- direct_sampling (enum) [RX-T2] ---- */
    } else if (strcmp(name, "direct_sampling") == 0) {
        if (t[vi].type != JSMN_STRING) {
            send_error(s, id, "invalid_parameter",
                       "direct_sampling requires a string from the "
                       "published values"); return;
        }
        sampling_mode_t mode;
        if      (tok_eq(js, &t[vi], "off"))      mode = SAMPLING_NORMAL;
        else if (tok_eq(js, &t[vi], "i_branch")) mode = SAMPLING_DIRECT_I;
        else if (tok_eq(js, &t[vi], "q_branch")) mode = SAMPLING_DIRECT_Q;
        else {
            send_error(s, id, "invalid_parameter",
                       "direct_sampling must be off, i_branch or q_branch");
            return;
        }

        if (mode == g_sampling_mode) {
            snprintf(result, sizeof(result),
                     "{\"function\":\"rx\",\"name\":\"direct_sampling\","
                     "\"applied_value\":\"%s\"}", valstr);
            send_ok(s, id, result);
            return;
        }

        /* Switching branches moves the frequency range and the sample
         * rate list underneath an open stream, so the stream has to be
         * reopened.  §13.2 already defines that answer for the analogous
         * sample rate case.                                             */
        if (s->stream_open) {
            send_error(s, id, "stream_reconfiguration_required",
                       "Close the stream before changing direct_sampling");
            return;
        }

        if (rtl_set_direct_sampling(mode) != 0) {
            send_error(s, id, "device_error",
                       "direct_sampling change failed"); return;
        }

        static const char *ds_names[] = {"off", "i_branch", "q_branch"};
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"direct_sampling\","
                 "\"applied_value\":\"%s\"}", ds_names[mode]);
        send_ok(s, id, result);

        snprintf(ev, sizeof(ev),
                 "{\"function\":\"rx\",\"name\":\"direct_sampling\","
                 "\"value\":\"%s\"}", ds_names[mode]);
        cp_broadcast_event("control_changed", ev);

        apply_mode_change_side_effects();

        /* The published frequency range and sample rate list have just
         * changed, so anything a client cached from get_capabilities is
         * now stale.                                                    */
        cp_broadcast_event("warning",
            "{\"code\":\"capabilities_changed\",\"message\":"
            "\"direct_sampling changed; re-read get_capabilities for the "
            "current frequency range and sample rates\"}");
        return;

    } else {
        /* §24.2: return not_supported for control names other than those
         * published.                                                    */
        send_error(s, id, "not_supported", "Unknown control name"); return;
    }
}

/* ---- get_control [RX-T2] ---- */
void cmd_get_control(Session *s, const char *id,
                     jsmntok_t *t, int ntok, const char *js) {
    int p = require_function(s, id, js, t, ntok);
    if (p < 0) return;
    if (!channel_ok(s, id, js, t, p)) return;

    int ni = tok_find_key(js, t, p, "name");
    if (ni < 0) {
        send_error(s, id, "missing_parameter", "name required"); return;
    }

    char name[32];
    tok_str(js, &t[ni], name, sizeof(name));
    char result[160];

    static const char *ds_names[] = {"off", "i_branch", "q_branch"};

    if (strcmp(name, "rf_gain") == 0) {
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"rf_gain\","
                 "\"value\":%.1f}", rtl_get_gain());
    } else if (strcmp(name, "agc") == 0) {
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"agc\","
                 "\"value\":%s}", g_agc_enabled ? "true" : "false");
    } else if (strcmp(name, "bias_tee") == 0) {
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"bias_tee\","
                 "\"value\":%.1f}", g_bias_tee_volts);
    } else if (strcmp(name, "ppm_correction") == 0) {
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"ppm_correction\","
                 "\"value\":%d}", g_ppm_correction);
    } else if (strcmp(name, "direct_sampling") == 0) {
        snprintf(result, sizeof(result),
                 "{\"function\":\"rx\",\"name\":\"direct_sampling\","
                 "\"value\":\"%s\"}", ds_names[g_sampling_mode]);
    } else {
        send_error(s, id, "not_supported", "Unknown control name"); return;
    }
    send_ok(s, id, result);
}

/* =========================================================================
 * Stream session management [§14]
 * ========================================================================= */

/* ---- open_stream [RX-T1] ---- */
void cmd_open_stream(Session *s, const char *id,
                     jsmntok_t *t, int ntok, const char *js) {
    int p = require_function(s, id, js, t, ntok);
    if (p < 0) return;
    if (!channel_ok(s, id, js, t, p)) return;

    if (s->stream_open) {
        send_error(s, id, "stream_already_open",
                   "This session already holds an rx stream; call "
                   "close_stream first");
        return;
    }

    /* §17.2: for an exclusive resource open_stream either succeeds or
     * fails with in_use.  One tuner, one tuned frequency.               */
    if (!cp_rx_claim(s)) {
        send_error(s, id, "in_use",
                   "The rx channel is held by another client");
        return;
    }

    int proto_i = tok_find_key(js, t, p, "protocol");
    if (proto_i < 0) {
        cp_rx_release(s);
        send_error(s, id, "missing_parameter", "protocol required"); return;
    }
    if (!tok_eq(js, &t[proto_i], "vita49")) {
        cp_rx_release(s);
        /* §14.3: unsupported_stream_protocol names a protocol absent from
         * the function's transports[].                                  */
        send_error(s, id, "unsupported_stream_protocol",
                   "Only vita49 is present in capabilities.rx.transports");
        return;
    }

    /* §14.2: transport parameters live inside an object named after the
     * transport, and SHALL NOT appear at the top level.  A Release 1.1
     * client puts destination, format and samples_per_packet at the top
     * level, so name the change rather than answering a bare
     * missing_parameter.                                                */
    int vi = tok_find_key(js, t, p, "vita");
    if (vi < 0 || t[vi].type != JSMN_OBJECT) {
        cp_rx_release(s);
        if (tok_find_key(js, t, p, "destination") >= 0) {
            send_error(s, id, "missing_parameter",
                       "Transport parameters belong inside a vita object "
                       "since Release 1.2; see SDR-SLC-CP-1.0 14.2");
        } else {
            send_error(s, id, "missing_parameter",
                       "vita transport object required");
        }
        return;
    }

    /* §14.3: the only receive profile advertised is VITA-A/1.0.  A client
     * may omit protocol_version (it then gets the advertised one) but may
     * not ask for another.  The value is echoed in the result so the
     * client sees what was agreed.                                      */
    int pv = tok_find_key(js, t, vi, "protocol_version");
    if (pv >= 0 && !tok_eq(js, &t[pv], "VITA-A/1.0")) {
        cp_rx_release(s);
        send_error(s, id, "unsupported_stream_protocol",
                   "capabilities.rx.transports advertises VITA-A/1.0 only");
        return;
    }

    /* §11.3 addendum: echo zero_point for unsigned encodings only; it is
     * meaningless for twos_complement and is omitted there.            */
    char zero_point_field[40] = "";

    /* Format.  The daemon advertises exactly one format — u8 — so u8 is
     * both the default (absence means the native one) and the only value
     * that can be accepted.  A client that explicitly asks for anything
     * else gets unsupported_format, per §22 / iq_formats.                */
    iq_format_t fmt = IQ_FMT_U8;
    int fmt_i = tok_find_key(js, t, vi, "format");
    if (fmt_i >= 0 && t[fmt_i].type == JSMN_OBJECT) {
        int lbl  = tok_find_key(js, t, fmt_i, "label");
        int enc  = tok_find_key(js, t, fmt_i, "encoding");
        int bits = tok_find_key(js, t, fmt_i, "bits_per_component");

        bool matched = false;
        if (lbl >= 0 && tok_eq(js, &t[lbl], "u8")) {
            matched = true;                              /* explicit u8   */
        } else if (enc >= 0 && bits >= 0) {
            if (tok_eq(js, &t[enc], "unsigned") && tok_eq(js, &t[bits], "8"))
                matched = true;                          /* u8 by fields  */
        } else if (enc >= 0 && tok_eq(js, &t[enc], "unsigned")) {
            matched = true;                              /* u8 by encoding */
        }

        if (!matched) {
            cp_rx_release(s);
            send_error(s, id, "unsupported_format",
                       "This device streams unsigned 8-bit IQ only; see "
                       "capabilities.rx.iq_formats");
            return;
        }
    }

    /* Destination — §14.4.  The client picks it; the device never
     * assumes a fixed or default client port.                           */
    int dest_i = tok_find_key(js, t, vi, "destination");
    if (dest_i < 0 || t[dest_i].type != JSMN_OBJECT) {
        cp_rx_release(s);
        send_error(s, id, "missing_parameter",
                   "vita.destination required"); return;
    }
    int ip_i   = tok_find_key(js, t, dest_i, "ip");
    int port_i = tok_find_key(js, t, dest_i, "port");
    if (ip_i < 0 || port_i < 0) {
        cp_rx_release(s);
        send_error(s, id, "missing_parameter",
                   "vita.destination.ip and .port required"); return;
    }
    char ip_str[INET_ADDRSTRLEN], port_str[8];
    tok_str(js, &t[ip_i],   ip_str,   sizeof(ip_str));
    tok_str(js, &t[port_i], port_str, sizeof(port_str));

    struct in_addr dest_addr;
    if (inet_pton(AF_INET, ip_str, &dest_addr) != 1) {
        cp_rx_release(s);
        send_error(s, id, "invalid_parameter",
                   "vita.destination.ip invalid"); return;
    }
    unsigned long dp = strtoul(port_str, NULL, 10);
    if (dp == 0 || dp > 65535) {
        cp_rx_release(s);
        send_error(s, id, "invalid_parameter",
                   "vita.destination.port invalid"); return;
    }
    uint16_t dest_port = (uint16_t)dp;

    alloc_stream_id(s->stream_id, sizeof(s->stream_id), &s->vita_stream_id);
    s->dest_ip     = dest_addr;
    s->dest_port   = dest_port;
    s->iq_fmt      = fmt;
    s->stream_open = true;
    s->state       = SESSION_STREAM_OPEN;

    /* Device source IP as seen on the control connection.              */
    char src_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        if (getsockname(s->fd, (struct sockaddr *)&addr, &alen) == 0)
            inet_ntop(AF_INET, &addr.sin_addr, src_ip, sizeof(src_ip));
    }

    const char *fmt_label  = (fmt == IQ_FMT_INT16) ? "int16" : "u8";
    const char *fmt_enc    = (fmt == IQ_FMT_INT16)
                             ? "twos_complement" : "unsigned";
    int         fmt_bits   = (fmt == IQ_FMT_INT16) ? 16 : 8;
    /* u8 is the device's native format now (§5.1).                       */
    bool        fmt_native = (fmt == IQ_FMT_U8);
    const char *sig_type   = sampling_mode_signal_type(g_sampling_mode);
    if (fmt == IQ_FMT_U8)
        snprintf(zero_point_field, sizeof(zero_point_field),
                 "\"zero_point\":%.2f,", g_u8_zero_point);

    /* §14.2: the response mirrors the request — everything transport
     * specific inside the vita object, nothing at the top level.  The
     * field R1.1 called vita_port is source_port in capabilities and
     * appears here as vita.source, because it is where the device sends
     * from and never a destination (§14.4).                             */
    char result[1400];
    snprintf(result, sizeof(result),
        "{"
        "\"stream_id\":\"%s\","
        "\"function\":\"rx\","
        "\"sample_rate_sps\":%u,"
        "\"signal_type\":\"%s\","
        "\"vita\":{"
          "\"protocol_version\":\"VITA-A/1.0\","
          "\"source\":{\"ip\":\"%s\",\"port\":%d},"
          "\"destination\":{\"ip\":\"%s\",\"port\":%u},"
          "\"samples_per_packet\":%u,"
          "\"stream_id\":%u,"
          "\"packet_class\":\"if_data\","
          /* §5.1: UTC seconds under TSI=01, picoseconds under TSF=10. */
          "\"timestamp_mode\":\"utc_picoseconds\","
          "\"format\":{"
            "\"encoding\":\"%s\","
            "\"bits_per_component\":%d,"
            "\"container_bits\":%d,"
            "\"packing\":\"interleaved\","
            "\"byte_order\":\"big_endian\",%s"
            "\"native\":%s,\"supported\":true,\"label\":\"%s\""
          "}"
        "}"
        "}",
        s->stream_id,
        g_rate_sps,
        sig_type,
        src_ip, g_vita_src_port,
        ip_str, dest_port,
        g_samples_per_pkt,
        s->vita_stream_id,
        fmt_enc, fmt_bits, fmt_bits,
        zero_point_field,
        fmt_native ? "true" : "false", fmt_label
    );
    send_ok(s, id, result);
    LOG_INFO("cp", "stream %s opened -> %s:%u (fmt=%s signal=%s)",
             s->stream_id, ip_str, dest_port, fmt_label, sig_type);

    cp_broadcast_event_except(s, "control_revoked",
        "{\"function\":\"rx\",\"control_state\":\"owned_by_other\"}");
}

/* ---- start_rx [RX-T1] ---- */
void cmd_start_rx(Session *s, const char *id,
                  jsmntok_t *t, int ntok, const char *js) {
    int p  = find_params(js, t, ntok);
    int si = (p >= 0) ? tok_find_key(js, t, p, "stream_id") : -1;
    if (si < 0) {
        send_error(s, id, "missing_parameter", "stream_id required"); return;
    }

    char sid[32];
    tok_str(js, &t[si], sid, sizeof(sid));
    if (!s->stream_open || strcmp(sid, s->stream_id) != 0) {
        send_error(s, id, "stream_not_found", "stream_id not found"); return;
    }
    if (s->state == SESSION_STREAMING) {
        send_ok(s, id, "{\"function\":\"rx\",\"streaming\":true}"); return;
    }

    uint32_t pf_word0 = sampling_mode_payload_fmt(g_sampling_mode);
    s->vita_token = vita_session_add(
        s->stream_id, s->vita_stream_id,
        s->dest_ip, s->dest_port,
        s->iq_fmt, pf_word0);

    if (s->vita_token < 0) {
        /* §17.5: a device returns busy when resource limits prevent
         * serving a request, and clients treat busy as retryable.       */
        send_error(s, id, "busy",
                   "No free stream slot; retry shortly"); return;
    }

    if (rtl_bridge_start() < 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
        send_error(s, id, "device_error",
                   "Failed to start RTL async read"); return;
    }

    s->state = SESSION_STREAMING;

    /* The Context packet was emitted by vita_session_add() above, before
     * any data could flow for this stream — see §24.3 and the note in
     * vita_session_add().                                               */
    send_ok(s, id, "{\"function\":\"rx\",\"streaming\":true}");

    /* §21.2: every stream_state says why.  A start or stop command is
     * client_request; the other reasons are for the device acting alone. */
    char ev[192];
    snprintf(ev, sizeof(ev),
             "{\"stream_id\":\"%s\",\"streaming\":true,\"function\":\"rx\","
             "\"reason\":\"client_request\"}", s->stream_id);
    send_event(s, "stream_state", ev);
    LOG_INFO("cp", "stream %s started (signal=%s)",
             s->stream_id, sampling_mode_signal_type(g_sampling_mode));
}

/* ---- stop_rx [RX-T1] ---- */
void cmd_stop_rx(Session *s, const char *id,
                 jsmntok_t *t, int ntok, const char *js) {
    int p  = find_params(js, t, ntok);
    int si = (p >= 0) ? tok_find_key(js, t, p, "stream_id") : -1;
    if (si < 0) {
        send_error(s, id, "missing_parameter", "stream_id required"); return;
    }

    char sid[32];
    tok_str(js, &t[si], sid, sizeof(sid));
    if (!s->stream_open || strcmp(sid, s->stream_id) != 0) {
        send_error(s, id, "stream_not_found", "stream_id not found"); return;
    }
    if (s->state != SESSION_STREAMING) {
        send_ok(s, id, "{\"function\":\"rx\",\"streaming\":false}"); return;
    }

    if (s->vita_token >= 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
    }
    rtl_bridge_stop();
    rtl_reset_adc_level();

    /* §14.5: the session stays open and can be restarted without
     * reopening.                                                        */
    s->state = SESSION_STREAM_OPEN;

    send_ok(s, id, "{\"function\":\"rx\",\"streaming\":false}");

    char ev[192];
    snprintf(ev, sizeof(ev),
             "{\"stream_id\":\"%s\",\"streaming\":false,\"function\":\"rx\","
             "\"reason\":\"client_request\"}", s->stream_id);
    send_event(s, "stream_state", ev);
    LOG_INFO("cp", "stream %s stopped", s->stream_id);
}

/* ---- close_stream [RX-T1] ---- */
void cmd_close_stream(Session *s, const char *id,
                      jsmntok_t *t, int ntok, const char *js) {
    int p = find_params(js, t, ntok);
    int si = (p >= 0) ? tok_find_key(js, t, p, "stream_id") : -1;
    if (si < 0) {
        send_error(s, id, "missing_parameter", "stream_id required"); return;
    }

    char sid[32];
    tok_str(js, &t[si], sid, sizeof(sid));
    if (!s->stream_open || strcmp(sid, s->stream_id) != 0) {
        send_error(s, id, "stream_not_found", "stream_id not found"); return;
    }

    if (s->vita_token >= 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
    }
    bool was_streaming = (s->state == SESSION_STREAMING);
    if (was_streaming) {
        rtl_bridge_stop();
        rtl_reset_adc_level();
    }

    char closed_id[32];
    snprintf(closed_id, sizeof(closed_id), "%s", s->stream_id);
    s->stream_open = false;
    s->state       = SESSION_IDLE;
    memset(s->stream_id, 0, sizeof(s->stream_id));

    /* §14.8: any access control held through the stream is released. */
    cp_rx_release(s);

    send_ok(s, id, "{\"closed\":true}");
    if (was_streaming) {
        /* §14.8: "the device SHALL stop it first" - and a stop emits
         * stream_state (§14.7).                                         */
        char ev[192];
        snprintf(ev, sizeof(ev),
                 "{\"stream_id\":\"%s\",\"streaming\":false,"
                 "\"function\":\"rx\",\"reason\":\"client_request\"}",
                 closed_id);
        send_event(s, "stream_state", ev);
    }
    LOG_INFO("cp", "stream closed");
}
