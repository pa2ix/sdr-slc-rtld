#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

/*
 * Minimal JSON builder — writes into a caller-supplied buffer.
 * No heap allocation.  Overflow is tracked; the buffer is always
 * NUL-terminated even when truncated.
 *
 * Usage:
 *   char buf[1024];
 *   JB jb;
 *   jb_init(&jb, buf, sizeof(buf));
 *   jb_obj_begin(&jb);
 *     jb_key(&jb, "type");  jb_str(&jb, "response");
 *     jb_key(&jb, "count"); jb_u64(&jb, 42);
 *   jb_obj_end(&jb);
 *   // use jb.buf, check jb.overflow
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char    *buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;
    /* Comma-insertion state stack (max 16 levels deep) */
    uint16_t need_comma;  /* bit-field: bit N set = level N needs comma */
    int      depth;
} JB;

void jb_init(JB *jb, char *buf, size_t cap);

/* Structural */
void jb_obj_begin(JB *jb);
void jb_obj_end(JB *jb);
void jb_arr_begin(JB *jb);
void jb_arr_end(JB *jb);

/* Values */
void jb_str(JB *jb, const char *s);
void jb_strn(JB *jb, const char *s, size_t len);
void jb_u32(JB *jb, uint32_t v);
void jb_u64(JB *jb, uint64_t v);
void jb_i32(JB *jb, int32_t v);
void jb_f32(JB *jb, double v, int decimals);
void jb_bool(JB *jb, bool v);
void jb_null(JB *jb);
void jb_raw(JB *jb, const char *raw);   /* pre-serialised JSON fragment  */

/* Object key (string followed by colon — next call provides value) */
void jb_key(JB *jb, const char *k);

/* Convenience: key + value in one call */
void jb_kstr(JB *jb, const char *k, const char *v);
void jb_ku64(JB *jb, const char *k, uint64_t v);
void jb_ki32(JB *jb, const char *k, int32_t v);
void jb_kf32(JB *jb, const char *k, double v, int decimals);
void jb_kbool(JB *jb, const char *k, bool v);
void jb_knull(JB *jb, const char *k);

/* JSMN helpers (parsing counterpart) */
#include "jsmn.h"
#include <string.h>

/* Compare a JSMN token to a C string literal */
static inline bool tok_eq(const char *js, const jsmntok_t *t, const char *s) {
    int tlen = t->end - t->start;
    return tlen == (int)strlen(s) &&
           strncmp(js + t->start, s, (size_t)tlen) == 0;
}

/* Copy a JSMN token value into a NUL-terminated buffer */
static inline void tok_str(const char *js, const jsmntok_t *t,
                            char *out, size_t cap) {
    size_t tlen = (size_t)(t->end - t->start);
    if (tlen >= cap) tlen = cap - 1;
    memcpy(out, js + t->start, tlen);
    out[tlen] = '\0';
}

/* Find a key in an object token and return index of its value token.
 * Returns -1 if not found.
 * 'obj' is the index of the JSMN_OBJECT token.                           */
static inline int tok_find_key(const char *js, const jsmntok_t *tokens,
                                int obj, const char *key) {
    int n = tokens[obj].size;
    int i = obj + 1;
    for (int k = 0; k < n; k++) {
        if (tok_eq(js, &tokens[i], key)) return i + 1;
        /* Skip value subtree */
        int skip = 1;
        i++;
        while (skip > 0) {
            skip += tokens[i].size;
            skip--;
            i++;
        }
    }
    return -1;
}

#endif /* JSON_BUILDER_H */

/* ---- Boolean / primitive token helpers -------------------------------- */

/* JSMN treats true/false/null as JSMN_PRIMITIVE alongside numbers.
 * These helpers distinguish them unambiguously.                           */
static inline bool tok_is_true(const char *js, const jsmntok_t *t) {
    return t->type == JSMN_PRIMITIVE &&
           (t->end - t->start) == 4 &&
           strncmp(js + t->start, "true", 4) == 0;
}

static inline bool tok_is_false(const char *js, const jsmntok_t *t) {
    return t->type == JSMN_PRIMITIVE &&
           (t->end - t->start) == 5 &&
           strncmp(js + t->start, "false", 5) == 0;
}

static inline bool tok_is_bool(const char *js, const jsmntok_t *t) {
    return tok_is_true(js, t) || tok_is_false(js, t);
}
