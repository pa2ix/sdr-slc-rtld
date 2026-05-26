/*
 * JSMN — Jasmine JSON tokeniser
 * Original: Serge Zaitsev <zaitsev.serge@gmail.com>
 * License: MIT
 *
 * Embedded verbatim (pinned to jsmn v1.1.0 behaviour).
 * This is a zero-allocation, non-recursive tokeniser that operates
 * entirely on the caller-supplied buffer.
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT    = 1,
    JSMN_ARRAY     = 2,
    JSMN_STRING    = 3,
    JSMN_PRIMITIVE = 4  /* number, boolean, null */
} jsmntype_t;

typedef enum {
    JSMN_ERROR_NOMEM = -1, /* Not enough tokens */
    JSMN_ERROR_INVAL = -2, /* Invalid character */
    JSMN_ERROR_PART  = -3  /* Partial JSON — more bytes expected */
} jsmnerr_t;

typedef struct {
    jsmntype_t type;
    int        start; /* First char in string (inclusive) */
    int        end;   /* Last char in string (exclusive)  */
    int        size;  /* Number of child tokens           */
#ifdef JSMN_PARENT_LINKS
    int parent;
#endif
} jsmntok_t;

typedef struct {
    unsigned int pos;       /* offset in the JSON string */
    unsigned int toknext;   /* next token to allocate    */
    int          toksuper;  /* superior token node, -1   */
} jsmn_parser;

void jsmn_init(jsmn_parser *parser);
int  jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
                jsmntok_t *tokens, unsigned int num_tokens);

#ifdef JSMN_IMPL   /* included by jsmn.c only */

#include <string.h>

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser,
                                    jsmntok_t *tokens,
                                    size_t num_tokens) {
    if (parser->toknext >= num_tokens) return NULL;
    jsmntok_t *tok = &tokens[parser->toknext++];
    tok->start = tok->end = -1;
    tok->size = 0;
#ifdef JSMN_PARENT_LINKS
    tok->parent = -1;
#endif
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type,
                             int start, int end) {
    token->type  = type;
    token->start = start;
    token->end   = end;
    token->size  = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                  size_t len, jsmntok_t *tokens,
                                  size_t num_tokens) {
    int start = (int)parser->pos;
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char c = js[parser->pos];
        switch (c) {
        case ':': case ',': case ']': case '}': case ' ':
        case '\t': case '\r': case '\n':
            goto found;
        }
        if (c < 32 || c >= 127) return JSMN_ERROR_INVAL;
    }
found:
    if (tokens == NULL) { parser->pos--; return 0; }
    jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
    if (!token) { parser->pos = (unsigned int)start; return JSMN_ERROR_NOMEM; }
    jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
#ifdef JSMN_PARENT_LINKS
    token->parent = parser->toksuper;
#endif
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js,
                               size_t len, jsmntok_t *tokens,
                               size_t num_tokens) {
    int start = (int)parser->pos++;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '\"') {
            if (tokens == NULL) return 0;
            jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (!token) { parser->pos = (unsigned int)start; return JSMN_ERROR_NOMEM; }
            jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
#ifdef JSMN_PARENT_LINKS
            token->parent = parser->toksuper;
#endif
            return 0;
        }
        if (c == '\\' && parser->pos + 1 < len) {
            parser->pos++;
            switch (js[parser->pos]) {
            case '\"': case '/': case '\\': case 'b': case 'f':
            case 'r': case 'n': case 't': break;
            case 'u':
                parser->pos++;
                for (int i = 0; i < 4 && parser->pos < len; i++, parser->pos++) {
                    char h = js[parser->pos];
                    if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                          (h >= 'a' && h <= 'f')))
                        return JSMN_ERROR_INVAL;
                }
                parser->pos--;
                break;
            default: return JSMN_ERROR_INVAL;
            }
        }
    }
    return JSMN_ERROR_PART;
}

void jsmn_init(jsmn_parser *parser) {
    parser->pos      = 0;
    parser->toknext  = 0;
    parser->toksuper = -1;
}

int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned int num_tokens) {
    int       r;
    int       i;
    jsmntok_t *token;
    int        count = (int)parser->toknext;

    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char      c      = js[parser->pos];
        jsmntype_t type;

        switch (c) {
        case '{': case '[':
            count++;
            if (tokens == NULL) break;
            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (!token) return JSMN_ERROR_NOMEM;
            if (parser->toksuper != -1) {
                jsmntok_t *t = &tokens[parser->toksuper];
#ifdef JSMN_STRICT
                if (t->type == JSMN_OBJECT) return JSMN_ERROR_INVAL;
#endif
                t->size++;
#ifdef JSMN_PARENT_LINKS
                token->parent = parser->toksuper;
#endif
            }
            token->type = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
            token->start = (int)parser->pos;
            parser->toksuper = (int)(parser->toknext - 1);
            break;

        case '}': case ']':
            type = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
            if (tokens == NULL) break;
#ifdef JSMN_PARENT_LINKS
            if (parser->toknext < 1) return JSMN_ERROR_INVAL;
            token = &tokens[parser->toknext - 1];
            for (;;) {
                if (token->start != -1 && token->end == -1) {
                    if (token->type != type) return JSMN_ERROR_INVAL;
                    token->end = (int)parser->pos + 1;
                    parser->toksuper = token->parent;
                    break;
                }
                if (token->parent == -1) break;
                token = &tokens[token->parent];
            }
#else
            for (i = (int)parser->toknext - 1; i >= 0; i--) {
                token = &tokens[i];
                if (token->start != -1 && token->end == -1) {
                    if (token->type != type) return JSMN_ERROR_INVAL;
                    parser->toksuper = -1;
                    token->end = (int)parser->pos + 1;
                    break;
                }
            }
            if (i == -1) return JSMN_ERROR_INVAL;
            for (; i >= 0; i--) {
                token = &tokens[i];
                if (token->start != -1 && token->end == -1) {
                    parser->toksuper = i;
                    break;
                }
            }
#endif
            break;

        case '\"':
            r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
            if (r < 0) return r;
            count++;
            if (parser->toksuper != -1 && tokens != NULL)
                tokens[parser->toksuper].size++;
            break;

        case '\t': case '\r': case '\n': case ' ': break;

        case ':':
            parser->toksuper = (int)(parser->toknext - 1);
            break;

        case ',':
            if (tokens != NULL && parser->toksuper != -1 &&
                tokens[parser->toksuper].type != JSMN_ARRAY &&
                tokens[parser->toksuper].type != JSMN_OBJECT) {
#ifdef JSMN_PARENT_LINKS
                parser->toksuper = tokens[parser->toksuper].parent;
#else
                for (i = (int)parser->toknext - 1; i >= 0; i--) {
                    if (tokens[i].type == JSMN_ARRAY ||
                        tokens[i].type == JSMN_OBJECT) {
                        if (tokens[i].start != -1 && tokens[i].end == -1) {
                            parser->toksuper = i;
                            break;
                        }
                    }
                }
#endif
            }
            break;

#ifdef JSMN_STRICT
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case 't': case 'f': case 'n':
            if (tokens != NULL && parser->toksuper != -1) {
                jsmntok_t *t = &tokens[parser->toksuper];
                if (t->type == JSMN_OBJECT ||
                    (t->type == JSMN_STRING && t->size != 0))
                    return JSMN_ERROR_INVAL;
            }
#else
        default:
#endif
            r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
            if (r < 0) return r;
            count++;
            if (parser->toksuper != -1 && tokens != NULL)
                tokens[parser->toksuper].size++;
            break;

#ifdef JSMN_STRICT
        default:
            return JSMN_ERROR_INVAL;
#endif
        }
    }

    if (tokens != NULL) {
        for (i = (int)parser->toknext - 1; i >= 0; i--) {
            if (tokens[i].start != -1 && tokens[i].end == -1)
                return JSMN_ERROR_PART;
        }
    }
    return count;
}

#endif /* JSMN_IMPL */

#ifdef __cplusplus
}
#endif
#endif /* JSMN_H */
