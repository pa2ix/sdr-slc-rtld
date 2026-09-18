/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "json_builder.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

static void jb_putc(JB *jb, char c) {
    if (jb->overflow) return;
    if (jb->pos + 1 >= jb->cap) { jb->overflow = true; return; }
    jb->buf[jb->pos++] = c;
    jb->buf[jb->pos]   = '\0';
}

static void jb_puts(JB *jb, const char *s, size_t len) {
    if (jb->overflow) return;
    if (jb->pos + len >= jb->cap) { jb->overflow = true; return; }
    memcpy(jb->buf + jb->pos, s, len);
    jb->pos += len;
    jb->buf[jb->pos] = '\0';
}

/* Emit a comma before a value/key if this level already has content */
static void jb_sep(JB *jb) {
    if (jb->depth > 0 && (jb->need_comma & (1u << jb->depth)))
        jb_putc(jb, ',');
    /* Mark this level as needing a comma for the next element */
    if (jb->depth >= 0)
        jb->need_comma |= (1u << jb->depth);
}

/* -------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void jb_init(JB *jb, char *buf, size_t cap) {
    jb->buf        = buf;
    jb->cap        = cap;
    jb->pos        = 0;
    jb->overflow   = false;
    jb->need_comma = 0;
    jb->depth      = 0;
    if (cap > 0) buf[0] = '\0';
}

void jb_obj_begin(JB *jb) {
    jb_sep(jb);
    jb_putc(jb, '{');
    jb->depth++;
    /* Clear comma state for the new level */
    jb->need_comma &= ~(1u << jb->depth);
}

void jb_obj_end(JB *jb) {
    jb->need_comma &= ~(1u << jb->depth);
    jb->depth--;
    jb_putc(jb, '}');
}

void jb_arr_begin(JB *jb) {
    jb_sep(jb);
    jb_putc(jb, '[');
    jb->depth++;
    jb->need_comma &= ~(1u << jb->depth);
}

void jb_arr_end(JB *jb) {
    jb->need_comma &= ~(1u << jb->depth);
    jb->depth--;
    jb_putc(jb, ']');
}

/* Emit a JSON string, escaping special characters */
void jb_str(JB *jb, const char *s) {
    jb_sep(jb);
    jb_putc(jb, '"');
    if (!s) { jb_puts(jb, "\"", 1); return; }
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { jb_puts(jb, "\\\"", 2); }
        else if (c == '\\') { jb_puts(jb, "\\\\", 2); }
        else if (c == '\n') { jb_puts(jb, "\\n",  2); }
        else if (c == '\r') { jb_puts(jb, "\\r",  2); }
        else if (c == '\t') { jb_puts(jb, "\\t",  2); }
        else if (c < 0x20)  {
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            jb_puts(jb, esc, 6);
        } else {
            jb_putc(jb, (char)c);
        }
    }
    jb_putc(jb, '"');
}

void jb_strn(JB *jb, const char *s, size_t len) {
    /* NUL-terminate a slice, then reuse jb_str */
    char tmp[256];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    jb_str(jb, tmp);
}

void jb_u32(JB *jb, uint32_t v) {
    jb_sep(jb);
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%u", v);
    jb_puts(jb, tmp, (size_t)n);
}

void jb_u64(JB *jb, uint64_t v) {
    jb_sep(jb);
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    jb_puts(jb, tmp, (size_t)n);
}

void jb_i32(JB *jb, int32_t v) {
    jb_sep(jb);
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%d", v);
    jb_puts(jb, tmp, (size_t)n);
}

void jb_f32(JB *jb, double v, int decimals) {
    jb_sep(jb);
    char fmt[8];
    snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), fmt, v);
    jb_puts(jb, tmp, (size_t)n);
}

void jb_bool(JB *jb, bool v) {
    jb_sep(jb);
    if (v) jb_puts(jb, "true",  4);
    else   jb_puts(jb, "false", 5);
}

void jb_null(JB *jb) {
    jb_sep(jb);
    jb_puts(jb, "null", 4);
}

void jb_raw(JB *jb, const char *raw) {
    jb_sep(jb);
    jb_puts(jb, raw, strlen(raw));
}

/* Key: emits string + colon, suppresses the separator that the value
 * would normally emit (the key already added the separator).            */
void jb_key(JB *jb, const char *k) {
    jb_str(jb, k);   /* jb_str calls jb_sep internally            */
    jb_putc(jb, ':');
    /* The next value call must NOT add a comma — the key owns this slot.
     * We achieve this by clearing the comma-needed bit for this level.  */
    jb->need_comma &= ~(1u << jb->depth);
}

/* ---- Convenience key+value ------------------------------------------ */

void jb_kstr(JB *jb, const char *k, const char *v) {
    jb_key(jb, k); jb_str(jb, v);
}

void jb_ku64(JB *jb, const char *k, uint64_t v) {
    jb_key(jb, k); jb_u64(jb, v);
}

void jb_ki32(JB *jb, const char *k, int32_t v) {
    jb_key(jb, k); jb_i32(jb, v);
}

void jb_kf32(JB *jb, const char *k, double v, int decimals) {
    jb_key(jb, k); jb_f32(jb, v, decimals);
}

void jb_kbool(JB *jb, const char *k, bool v) {
    jb_key(jb, k); jb_bool(jb, v);
}

void jb_knull(JB *jb, const char *k) {
    jb_key(jb, k); jb_null(jb);
}
