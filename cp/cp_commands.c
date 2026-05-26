#include "cp_commands.h"
#include "cp_session.h"
#include "../json/json_builder.h"
#include "../rtl/rtl_bridge.h"
#include "../vita/vita_tx.h"
#include "../disc/disc.h"
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
 * Response helpers
 * ========================================================================= */

void send_ok(Session *s, const char *id, const char *result_json) {
    char buf[MAX_MSG_BYTES];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"response\",\"id\":\"%s\","
        "\"status\":\"ok\",\"result\":%s}",
        id, result_json ? result_json : "{}");
    send_framed(s->fd, buf);
}

void send_error(Session *s, const char *id,
                const char *code, const char *message) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"response\",\"id\":\"%s\","
        "\"status\":\"error\","
        "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        id, code, message);
    send_framed(s->fd, buf);
}

void send_event(Session *s, const char *event, const char *params_json) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"event\",\"event\":\"%s\",\"params\":%s}",
        event, params_json ? params_json : "{}");
    send_framed(s->fd, buf);
}

/* =========================================================================
 * Utility
 * ========================================================================= */

static int find_params(const char *js, jsmntok_t *t, int ntok) {
    (void)ntok;
    return tok_find_key(js, t, 0, "params");
}

static bool require_rx_direction(Session *s, const char *id,
                                  const char *js, jsmntok_t *t, int ntok) {
    int p = find_params(js, t, ntok);
    if (p < 0) { send_error(s, id, "missing_parameter", "params required"); return false; }
    int d = tok_find_key(js, t, p, "direction");
    if (d < 0) { send_error(s, id, "missing_parameter", "direction required"); return false; }
    if (tok_eq(js, &t[d], "rx")) return true;
    if (tok_eq(js, &t[d], "tx")) {
        send_error(s, id, "not_supported", "TX not present on this hardware");
        return false;
    }
    send_error(s, id, "invalid_parameter", "direction must be rx or tx");
    return false;
}

/* =========================================================================
 * Capabilities — built dynamically based on sampling mode
 * ========================================================================= */

static void build_capabilities_json(char *out, size_t cap) {
    /* Gain list */
    int gains[64];
    int ngains = rtl_get_gain_list(gains, 64);
    double gain_min = (ngains > 0) ? gains[0]          / 10.0 : 0.0;
    double gain_max = (ngains > 0) ? gains[ngains - 1] / 10.0 : 49.6;

    /* Frequency range and sample rates by mode */
    sampling_mode_t mode = g_sampling_mode;
    uint64_t freq_min, freq_max;
    const uint32_t *rates;
    int rate_count;
    uint32_t pref_rate;

    if (mode == SAMPLING_NORMAL) {
        freq_min   = RTL_FREQ_MIN_NORMAL;
        freq_max   = RTL_FREQ_MAX_NORMAL;
        rates      = RTL_SAMPLE_RATES_NORMAL;
        rate_count = RTL_RATE_COUNT_NORMAL;
        pref_rate  = RTL_DEFAULT_RATE_NORMAL;
    } else {
        freq_min   = RTL_FREQ_MIN_DIRECT;
        freq_max   = RTL_FREQ_MAX_DIRECT;
        rates      = RTL_SAMPLE_RATES_DIRECT;
        rate_count = RTL_RATE_COUNT_DIRECT;
        pref_rate  = RTL_DEFAULT_RATE_DIRECT;
    }

    /* Sample rate array */
    char rates_arr[128] = "[";
    for (int i = 0; i < rate_count; i++) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s%u", i ? "," : "", rates[i]);
        strncat(rates_arr, tmp, sizeof(rates_arr) - strlen(rates_arr) - 1);
    }
    strncat(rates_arr, "]", sizeof(rates_arr) - strlen(rates_arr) - 1);

    /* Controls array — rf_gain and agc always present.
     * bias_tee, ppm_correction always present (hardware supports them).
     * direct_sampling only advertised in auto mode — we don't support
     * auto mode, so it is intentionally omitted.                          */
    char controls[512];
    snprintf(controls, sizeof(controls),
        "["
        "{\"name\":\"rf_gain\",\"type\":\"float\",\"unit\":\"dB\","
         "\"min\":%.1f,\"max\":%.1f,\"step\":0.1,\"default\":%.1f},"
        "{\"name\":\"agc\",\"type\":\"boolean\",\"default\":false},"
        "{\"name\":\"bias_tee\",\"type\":\"boolean\",\"default\":false},"
        "{\"name\":\"ppm_correction\",\"type\":\"integer\","
         "\"min\":-100,\"max\":100,\"step\":1,\"default\":0}"
        "]",
        gain_min, gain_max, RTL_DEFAULT_GAIN / 10.0);

    snprintf(out, cap,
        "{"
        "\"schema\":\"sdrslc.capabilities\","
        "\"schema_version\":1,"
        "\"full_duplex\":false,"
        "\"turnaround_mode\":\"manual\","
        "\"streaming_protocols\":["
          "{\"name\":\"vita49\",\"versions\":[\"VITA-A/1.0\"],"
           "\"transport\":\"udp\",\"negotiable\":true,"
           "\"formats_ref\":\"capabilities.rx.iq_formats\"}"
        "],"
        "\"rx\":{"
          "\"supported\":true,"
          "\"channels\":1,"
          "\"frequency_ranges_hz\":[{\"min\":%llu,\"max\":%llu}],"
          "\"sample_rates_sps\":%s,"
          "\"sample_rate_min_hz\":%u,"
          "\"sample_rate_max_hz\":%u,"
          "\"preferred_sample_rate_sps\":%u,"
          "\"preferred_gain_db\":%.1f,"
          "\"vita_port\":%d,"
          "\"samples_per_packet\":%u,"
          "\"iq_formats\":["
            "{\"encoding\":\"twos_complement\",\"bits_per_component\":16,"
             "\"container_bits\":16,\"packing\":\"interleaved\","
             "\"byte_order\":\"big_endian\","
             "\"native\":true,\"supported\":true,\"label\":\"int16\"},"
            "{\"encoding\":\"unsigned\",\"bits_per_component\":8,"
             "\"container_bits\":8,\"packing\":\"interleaved\","
             "\"byte_order\":\"big_endian\","
             "\"native\":false,\"supported\":true,\"label\":\"u8\"}"
          "],"
          "\"controls\":%s"
        "},"
        "\"tx\":{"
          "\"supported\":false,"
          "\"channels\":0,"
          "\"frequency_ranges_hz\":[],"
          "\"sample_rates_sps\":[],"
          "\"sample_rate_min_hz\":0,"
          "\"sample_rate_max_hz\":0,"
          "\"vita_port\":0,"
          "\"samples_per_packet\":0,"
          "\"iq_formats\":[],"
          "\"power_min_dbm\":0,"
          "\"power_max_dbm\":0,"
          "\"power_step_dbm\":0"
        "}"
        "}",
        (unsigned long long)freq_min, (unsigned long long)freq_max,
        rates_arr,
        rates[0], rates[rate_count - 1],
        pref_rate,
        RTL_DEFAULT_GAIN / 10.0,
        VITA_SRC_PORT,
        g_samples_per_pkt,
        controls
    );
}

/* =========================================================================
 * Command handlers
 * ========================================================================= */

/* ---- hello ---- */
void cmd_hello(Session *s, const char *id,
               jsmntok_t *t, int ntok, const char *js) {
    int p = find_params(js, t, ntok);
    if (p < 0) { send_error(s, id, "missing_parameter", "params required"); return; }

    int vers_idx = tok_find_key(js, t, p, "supported_protocol_versions");
    bool supports_10 = false;
    if (vers_idx >= 0 && t[vers_idx].type == JSMN_ARRAY) {
        int count = t[vers_idx].size;
        int vi = vers_idx + 1;
        for (int i = 0; i < count; i++) {
            if (tok_eq(js, &t[vi], "1.0") || tok_eq(js, &t[vi], "1.1"))
                supports_10 = true;
            int skip = t[vi].size; vi++;
            while (skip-- > 0) { skip += t[vi].size; vi++; }
        }
    }

    if (!supports_10) {
        send_error(s, id, "unsupported_protocol_version",
                   "No common protocol version"); return;
    }

    s->state = SESSION_IDLE;

    char result[512];
    snprintf(result, sizeof(result),
        "{\"protocol_name\":\"%s\","
        "\"selected_protocol_version\":\"1.0\","
        "\"device_name\":\"%s\","
        "\"manufacturer\":\"%s\"}",
        PROTO_NAME, g_device_name, MANUFACTURER);
    send_ok(s, id, result);
    LOG_INFO("cp", "hello OK");
}

/* ---- ping ---- */
void cmd_ping(Session *s, const char *id,
              jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    send_ok(s, id, "{}");
}

/* ---- get_capabilities ---- */
void cmd_get_capabilities(Session *s, const char *id,
                          jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    char caps[4096];
    build_capabilities_json(caps, sizeof(caps));
    char result[4160];
    snprintf(result, sizeof(result), "{\"capabilities\":%s}", caps);
    send_ok(s, id, result);
}

/* ---- get_status ---- */
void cmd_get_status(Session *s, const char *id,
                    jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    char result[512];
    snprintf(result, sizeof(result),
        "{\"rx\":{"
          "\"enabled\":true,"
          "\"streaming\":%s,"
          "\"frequency_hz\":%" PRIu64 ","
          "\"sample_rate_sps\":%u,"
          "\"power_dbm\":null,"
          "\"controls\":{"
            "\"rf_gain\":%.1f,"
            "\"agc\":%s,"
            "\"bias_tee\":%s,"
            "\"ppm_correction\":%d"
          "}"
        "},"
        "\"tx\":{"
          "\"enabled\":false,\"streaming\":false,"
          "\"frequency_hz\":0,\"sample_rate_sps\":0,\"power_dbm\":null"
        "}}",
        (s->state == SESSION_STREAMING) ? "true" : "false",
        g_freq_hz, g_rate_sps,
        g_gain_tenth_db / 10.0,
        g_agc_enabled      ? "true" : "false",
        g_bias_tee_enabled ? "true" : "false",
        g_ppm_correction
    );
    send_ok(s, id, result);
}

/* ---- set_frequency ---- */
void cmd_set_frequency(Session *s, const char *id,
                       jsmntok_t *t, int ntok, const char *js) {
    if (!require_rx_direction(s, id, js, t, ntok)) return;
    int p = find_params(js, t, ntok);
    int fi = tok_find_key(js, t, p, "frequency_hz");
    if (fi < 0) {
        send_error(s, id, "missing_parameter", "frequency_hz required");
        return;
    }

    char fstr[32];
    tok_str(js, &t[fi], fstr, sizeof(fstr));
    errno = 0;
    uint64_t freq = strtoull(fstr, NULL, 10);
    if (errno == ERANGE || freq < 1 || freq > 1000000000000000ULL) {
        send_error(s, id, "invalid_parameter", "frequency_hz out of range");
        return;
    }

    /* Range check against current mode */
    uint64_t fmin = (g_sampling_mode == SAMPLING_NORMAL)
                    ? RTL_FREQ_MIN_NORMAL : RTL_FREQ_MIN_DIRECT;
    uint64_t fmax = (g_sampling_mode == SAMPLING_NORMAL)
                    ? RTL_FREQ_MAX_NORMAL : RTL_FREQ_MAX_DIRECT;
    if (freq < fmin || freq > fmax) {
        send_error(s, id, "unsupported_frequency",
                   "Frequency outside hardware range for current mode");
        return;
    }

    uint32_t applied = rtl_set_frequency(freq);
    if (applied == 0) {
        send_error(s, id, "device_error", "Frequency set failed"); return;
    }
    s->frequency_hz = applied;
    char result[128];
    snprintf(result, sizeof(result),
             "{\"direction\":\"rx\",\"applied_frequency_hz\":%u}", applied);
    send_ok(s, id, result);
}

/* ---- set_sample_rate ---- */
void cmd_set_sample_rate(Session *s, const char *id,
                         jsmntok_t *t, int ntok, const char *js) {
    if (!require_rx_direction(s, id, js, t, ntok)) return;

    if (s->state == SESSION_STREAMING || s->state == SESSION_STREAM_OPEN) {
        send_error(s, id, "stream_reconfiguration_required",
                   "Close stream before changing sample rate"); return;
    }

    int p = find_params(js, t, ntok);
    int ri = tok_find_key(js, t, p, "sample_rate_sps");
    if (ri < 0) {
        send_error(s, id, "missing_parameter", "sample_rate_sps required");
        return;
    }

    char rstr[16];
    tok_str(js, &t[ri], rstr, sizeof(rstr));
    uint32_t rate = (uint32_t)strtoul(rstr, NULL, 10);

    /* Validate against mode-appropriate list */
    const uint32_t *rates = (g_sampling_mode == SAMPLING_NORMAL)
                            ? RTL_SAMPLE_RATES_NORMAL
                            : RTL_SAMPLE_RATES_DIRECT;
    int rate_count = (g_sampling_mode == SAMPLING_NORMAL)
                     ? RTL_RATE_COUNT_NORMAL : RTL_RATE_COUNT_DIRECT;
    bool valid = false;
    for (int i = 0; i < rate_count; i++)
        if (rates[i] == rate) { valid = true; break; }
    if (!valid) {
        send_error(s, id, "unsupported_sample_rate",
                   "Rate not in supported list"); return;
    }

    uint32_t applied = rtl_set_sample_rate(rate);
    if (applied == 0) {
        send_error(s, id, "device_error", "Sample rate set failed"); return;
    }
    s->sample_rate_sps = applied;
    char result[128];
    snprintf(result, sizeof(result),
             "{\"direction\":\"rx\",\"applied_sample_rate_sps\":%u}", applied);
    send_ok(s, id, result);
}

/* ---- set_control ---- */
void cmd_set_control(Session *s, const char *id,
                     jsmntok_t *t, int ntok, const char *js) {
    if (!require_rx_direction(s, id, js, t, ntok)) return;
    int p = find_params(js, t, ntok);

    int ni = tok_find_key(js, t, p, "name");
    int vi = tok_find_key(js, t, p, "value");
    if (ni < 0 || vi < 0) {
        send_error(s, id, "missing_parameter", "name and value required");
        return;
    }

    char name[32];
    tok_str(js, &t[ni], name, sizeof(name));

    LOG_INFO("cp", "set_control direction=rx name='%s'", name);

    /* Write the raw value token text for diagnostics */
    char valstr[64];
    tok_str(js, &t[vi], valstr, sizeof(valstr));
    LOG_DBG("cp", "set_control raw value token: '%s'", valstr);

    char result[128];

    /* ---- rf_gain (float) ---- */
    if (strcmp(name, "rf_gain") == 0) {
        if (t[vi].type != JSMN_PRIMITIVE) {
            send_error(s, id, "invalid_parameter",
                       "rf_gain requires a numeric value"); return;
        }
        /* AGC interlock: if AGC is on, ignore rf_gain set silently */
        if (g_agc_enabled) {
            LOG_INFO("cp", "rf_gain set ignored — AGC is active");
            snprintf(result, sizeof(result),
                     "{\"direction\":\"rx\",\"name\":\"rf_gain\","
                     "\"applied_value\":%.1f}", rtl_get_gain());
            send_ok(s, id, result);
            return;
        }
        double val = strtod(valstr, NULL);
        if (val < 0.0 || val > 60.0) {
            send_error(s, id, "invalid_parameter",
                       "rf_gain out of range"); return;
        }
        LOG_INFO("cp", "set_control rf_gain requested=%.1f dB", val);
        double applied = rtl_set_gain(val);
        s->rf_gain_db = applied;
        LOG_INFO("cp", "set_control rf_gain applied=%.1f dB", applied);
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"rf_gain\","
                 "\"applied_value\":%.1f}", applied);

    /* ---- agc (boolean) ---- */
    } else if (strcmp(name, "agc") == 0) {
        if (!tok_is_bool(js, &t[vi])) {
            send_error(s, id, "invalid_parameter",
                       "agc requires true or false"); return;
        }
        bool en = tok_is_true(js, &t[vi]);
        rtl_set_agc(en);
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"agc\","
                 "\"applied_value\":%s}", en ? "true" : "false");

    /* ---- bias_tee (boolean) ---- */
    } else if (strcmp(name, "bias_tee") == 0) {
        if (!tok_is_bool(js, &t[vi])) {
            send_error(s, id, "invalid_parameter",
                       "bias_tee requires true or false"); return;
        }
        bool en = tok_is_true(js, &t[vi]);
        if (rtl_set_bias_tee(en) != 0) {
            send_error(s, id, "not_supported",
                       "bias_tee not supported by this hardware"); return;
        }
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"bias_tee\","
                 "\"applied_value\":%s}", en ? "true" : "false");

    /* ---- ppm_correction (integer) ---- */
    } else if (strcmp(name, "ppm_correction") == 0) {
        if (tok_is_bool(js, &t[vi])) {
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
                 "{\"direction\":\"rx\",\"name\":\"ppm_correction\","
                 "\"applied_value\":%d}", g_ppm_correction);

    } else {
        send_error(s, id, "not_supported", "Unknown control name"); return;
    }

    send_ok(s, id, result);
}

/* ---- get_control ---- */
void cmd_get_control(Session *s, const char *id,
                     jsmntok_t *t, int ntok, const char *js) {
    if (!require_rx_direction(s, id, js, t, ntok)) return;
    int p = find_params(js, t, ntok);
    int ni = tok_find_key(js, t, p, "name");
    if (ni < 0) {
        send_error(s, id, "missing_parameter", "name required"); return;
    }

    char name[32];
    tok_str(js, &t[ni], name, sizeof(name));
    char result[128];

    if (strcmp(name, "rf_gain") == 0) {
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"rf_gain\","
                 "\"value\":%.1f}", rtl_get_gain());
    } else if (strcmp(name, "agc") == 0) {
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"agc\","
                 "\"value\":%s}", g_agc_enabled ? "true" : "false");
    } else if (strcmp(name, "bias_tee") == 0) {
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"bias_tee\","
                 "\"value\":%s}", g_bias_tee_enabled ? "true" : "false");
    } else if (strcmp(name, "ppm_correction") == 0) {
        snprintf(result, sizeof(result),
                 "{\"direction\":\"rx\",\"name\":\"ppm_correction\","
                 "\"value\":%d}", g_ppm_correction);
    } else {
        send_error(s, id, "not_supported", "Unknown control name"); return;
    }
    send_ok(s, id, result);
}

/* ---- open_stream ---- */
void cmd_open_stream(Session *s, const char *id,
                     jsmntok_t *t, int ntok, const char *js) {
    int p = find_params(js, t, ntok);
    if (p < 0) {
        send_error(s, id, "missing_parameter", "params required"); return;
    }

    int di = tok_find_key(js, t, p, "direction");
    if (di < 0) {
        send_error(s, id, "missing_parameter", "direction required"); return;
    }
    if (tok_eq(js, &t[di], "tx")) {
        send_error(s, id, "not_supported",
                   "TX not present on this hardware"); return;
    }
    if (!tok_eq(js, &t[di], "rx")) {
        send_error(s, id, "invalid_parameter",
                   "direction must be rx"); return;
    }

    if (s->stream_open) {
        send_error(s, id, "stream_already_open",
                   "An RX stream is already open; call close_stream first");
        return;
    }

    int proto_i = tok_find_key(js, t, p, "protocol");
    if (proto_i < 0 || !tok_eq(js, &t[proto_i], "vita49")) {
        send_error(s, id, "unsupported_stream_protocol",
                   "Only vita49 is supported"); return;
    }

    /* Format — default int16 */
    iq_format_t fmt = IQ_FMT_INT16;
    int fmt_i = tok_find_key(js, t, p, "format");
    if (fmt_i >= 0 && t[fmt_i].type == JSMN_OBJECT) {
        int lbl = tok_find_key(js, t, fmt_i, "label");
        if (lbl >= 0 && tok_eq(js, &t[lbl], "u8")) fmt = IQ_FMT_U8;
        int enc = tok_find_key(js, t, fmt_i, "encoding");
        if (enc >= 0 &&
            !tok_eq(js, &t[enc], "twos_complement") &&
            !tok_eq(js, &t[enc], "unsigned")) {
            send_error(s, id, "unsupported_format",
                       "Only int16 and u8 formats are supported"); return;
        }
    }

    /* Destination */
    int dest_i = tok_find_key(js, t, p, "destination");
    if (dest_i < 0 || t[dest_i].type != JSMN_OBJECT) {
        send_error(s, id, "missing_parameter", "destination required"); return;
    }
    int ip_i   = tok_find_key(js, t, dest_i, "ip");
    int port_i = tok_find_key(js, t, dest_i, "port");
    if (ip_i < 0 || port_i < 0) {
        send_error(s, id, "missing_parameter",
                   "destination.ip and .port required"); return;
    }
    char ip_str[INET_ADDRSTRLEN], port_str[8];
    tok_str(js, &t[ip_i],   ip_str,   sizeof(ip_str));
    tok_str(js, &t[port_i], port_str, sizeof(port_str));

    struct in_addr dest_addr;
    if (inet_pton(AF_INET, ip_str, &dest_addr) != 1) {
        send_error(s, id, "invalid_parameter",
                   "destination.ip invalid"); return;
    }
    uint16_t dest_port = (uint16_t)strtoul(port_str, NULL, 10);
    if (dest_port == 0) {
        send_error(s, id, "invalid_parameter",
                   "destination.port invalid"); return;
    }

    alloc_stream_id(s->stream_id, sizeof(s->stream_id), &s->vita_stream_id);
    s->dest_ip     = dest_addr;
    s->dest_port   = dest_port;
    s->iq_fmt      = fmt;
    s->stream_open = true;
    s->state       = SESSION_STREAM_OPEN;

    /* Device source IP */
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
    bool        fmt_native = (fmt == IQ_FMT_INT16);

    /* signal_type — per CP spec §3 (SLC-CP-1.3) */
    const char *sig_type = sampling_mode_signal_type(g_sampling_mode);

    char result[1200];
    snprintf(result, sizeof(result),
        "{"
        "\"stream_id\":\"%s\","
        "\"direction\":\"rx\","
        "\"protocol\":\"vita49\","
        "\"protocol_version\":\"VITA-A/1.0\","
        "\"source\":{\"ip\":\"%s\",\"port\":%d},"
        "\"destination\":{\"ip\":\"%s\",\"port\":%u},"
        "\"sample_rate_sps\":%u,"
        "\"samples_per_packet\":%u,"
        "\"format\":{"
          "\"encoding\":\"%s\","
          "\"bits_per_component\":%d,"
          "\"container_bits\":%d,"
          "\"packing\":\"interleaved\","
          "\"byte_order\":\"big_endian\","
          "\"native\":%s,\"supported\":true,\"label\":\"%s\""
        "},"
        "\"vita\":{"
          "\"stream_id\":%u,"
          "\"packet_class\":\"if_data\","
          "\"timestamp_mode\":\"integer_and_fractional\""
        "},"
        "\"signal_type\":\"%s\""
        "}",
        s->stream_id,
        src_ip, VITA_SRC_PORT,
        ip_str, dest_port,
        g_rate_sps,
        g_samples_per_pkt,
        fmt_enc, fmt_bits, fmt_bits,
        fmt_native ? "true" : "false", fmt_label,
        s->vita_stream_id,
        sig_type
    );
    send_ok(s, id, result);
    LOG_INFO("cp", "stream %s opened → %s:%u (fmt=%s signal=%s)",
             s->stream_id, ip_str, dest_port, fmt_label, sig_type);
}

/* ---- close_stream ---- */
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
    if (s->state == SESSION_STREAMING) rtl_bridge_stop();

    s->stream_open = false;
    s->state       = SESSION_IDLE;
    memset(s->stream_id, 0, sizeof(s->stream_id));

    send_ok(s, id, "{\"closed\":true}");
    LOG_INFO("cp", "stream closed");
}

/* ---- start_rx ---- */
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
        send_ok(s, id, "{\"direction\":\"rx\",\"streaming\":true}"); return;
    }

    /* Register VITA session with mode-appropriate Payload Format word */
    uint32_t pf_word0 = sampling_mode_payload_fmt(g_sampling_mode);
    s->vita_token = vita_session_add(
        s->stream_id, s->vita_stream_id,
        s->dest_ip, s->dest_port,
        s->iq_fmt, pf_word0);

    if (s->vita_token < 0) {
        send_error(s, id, "device_error", "Too many active streams"); return;
    }

    /* Start RTL hardware */
    if (rtl_bridge_start() < 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
        send_error(s, id, "device_error",
                   "Failed to start RTL async read"); return;
    }

    s->state = SESSION_STREAMING;

    /* Send response first, then Context packet, then data flows.
     * Spec §7.3: Context packet SHALL be sent before first IF Data packet. */
    send_ok(s, id, "{\"direction\":\"rx\",\"streaming\":true}");

    vita_send_context_packet(s->vita_token);

    char ev[128];
    snprintf(ev, sizeof(ev),
             "{\"stream_id\":\"%s\",\"streaming\":true,\"direction\":\"rx\"}",
             s->stream_id);
    send_event(s, "stream_state", ev);
    LOG_INFO("cp", "stream %s started (signal=%s)",
             s->stream_id, sampling_mode_signal_type(g_sampling_mode));
}

/* ---- stop_rx ---- */
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

    if (s->vita_token >= 0) {
        vita_session_remove(s->vita_token);
        s->vita_token = -1;
    }
    rtl_bridge_stop();
    s->state = SESSION_STREAM_OPEN;

    send_ok(s, id, "{\"direction\":\"rx\",\"streaming\":false}");

    char ev[128];
    snprintf(ev, sizeof(ev),
             "{\"stream_id\":\"%s\",\"streaming\":false,\"direction\":\"rx\"}",
             s->stream_id);
    send_event(s, "stream_state", ev);
    LOG_INFO("cp", "stream %s stopped", s->stream_id);
}

/* ---- get_clock_status ---- */
void cmd_get_clock_status(Session *s, const char *id,
                           jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    send_ok(s, id,
        "{"
        "\"source\":\"free_running\","
        "\"synced\":false,"
        "\"accuracy_us\":1000000,"
        "\"utc_time\":null,"
        "\"epoch_s\":null,"
        "\"ptp\":{\"available\":false},"
        "\"gps\":{\"available\":false}"
        "}");
}

/* ---- get_location ---- */
void cmd_get_location(Session *s, const char *id,
                       jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    send_ok(s, id,
        "{\"source\":\"manual\",\"fix\":false,"
        "\"latitude\":0.0,\"longitude\":0.0,\"altitude_m\":0.0,"
        "\"accuracy_m\":0,\"maidenhead\":null,\"satellites\":0}");
}

/* ---- not_supported ---- */
void cmd_not_supported(Session *s, const char *id,
                        jsmntok_t *t, int ntok, const char *js) {
    (void)t; (void)ntok; (void)js;
    send_error(s, id, "not_supported",
               "Feature not implemented on this hardware");
}
