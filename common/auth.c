/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

#include "auth.h"
#include "log.h"
#include "../config.h"   /* SLC_DAEMON_NAME */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------- SHA-256 */

/* FIPS 180-4. Written out here rather than pulling in a TLS library: the
 * daemon has no other use for one, and §26.2 chose HMAC-SHA256 precisely
 * because it is a few hundred lines on a microcontroller. */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->buflen = 0;
}

static void sha256_update(sha256_t *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - s->buflen;
        if (take > n)
            take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p += take;
        n -= take;
        if (s->buflen == 64) {
            sha256_block(s, s->buf);
            s->buflen = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->buflen != 56)
        sha256_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void sha256(const uint8_t *p, size_t n, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, out);
}

/* RFC 2104. */
void auth_hmac_sha256(const uint8_t *key, size_t keylen,
                      const uint8_t *msg, size_t msglen, uint8_t out[32])
{
    uint8_t k[64];
    memset(k, 0, sizeof k);
    if (keylen > 64)
        sha256(key, keylen, k);
    else
        memcpy(k, key, keylen);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner[32];
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, ipad, 64);
    sha256_update(&s, msg, msglen);
    sha256_final(&s, inner);

    sha256_init(&s);
    sha256_update(&s, opad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

void auth_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ------------------------------------------------------------- keyring */

int auth_keyring_load(auth_keyring_t *kr, const char *path)
{
    memset(kr, 0, sizeof *kr);
    snprintf(kr->path, sizeof kr->path, "%s", path);

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        LOGE(T_SLC, "cannot read key file %s: %s", path, strerror(errno));
        return -1;
    }
    kr->loaded = true;

    char line[AUTH_KEY_ID_MAX + AUTH_PSK_MAX + 16];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '#')
            continue;
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1]))
            *--end = '\0';

        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp))
            sp++;
        if (!*sp) {
            LOGE(T_SLC, "%s:%d: expected \"<key_id> <psk>\"", path, lineno);
            fclose(f);
            return -1;
        }
        *sp++ = '\0';
        while (*sp && isspace((unsigned char)*sp))
            sp++;
        if (!*sp) {
            LOGE(T_SLC, "%s:%d: key \"%s\" has an empty secret", path, lineno, p);
            fclose(f);
            return -1;
        }
        if (kr->n >= AUTH_MAX_KEYS) {
            LOGW(T_SLC, "%s:%d: more than %d keys; the rest are ignored",
                 path, lineno, AUTH_MAX_KEYS);
            break;
        }
        if (strlen(p) >= AUTH_KEY_ID_MAX || strlen(sp) >= AUTH_PSK_MAX) {
            LOGE(T_SLC, "%s:%d: key_id or psk too long", path, lineno);
            fclose(f);
            return -1;
        }
        /* Both lengths were checked just above; the precision only keeps
         * -Wformat-truncation quiet. */
        snprintf(kr->keys[kr->n].key_id, AUTH_KEY_ID_MAX, "%.*s",
                 AUTH_KEY_ID_MAX - 1, p);
        snprintf(kr->keys[kr->n].psk, AUTH_PSK_MAX, "%.*s",
                 AUTH_PSK_MAX - 1, sp);
        kr->n++;
    }
    fclose(f);
    return kr->n;
}

bool auth_make_nonce(uint8_t *raw, char *hex_out)
{
    size_t got = 0;
    while (got < AUTH_NONCE_BYTES) {
        ssize_t n = getrandom(raw + got, AUTH_NONCE_BYTES - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        got += (size_t)n;
    }
    auth_hex(raw, AUTH_NONCE_BYTES, hex_out);
    return true;
}

/* §26.2: comparison in constant time. */
static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= (uint8_t)(a[i] ^ b[i]);
    return acc == 0;
}

bool auth_verify(const auth_keyring_t *kr, const char *key_id,
                 const uint8_t *nonce, size_t nonce_len,
                 const char *response_hex)
{
    if (!kr || kr->n == 0 || !response_hex)
        return false;
    if (strlen(response_hex) != 64)
        return false;
    uint8_t given[32];
    for (int i = 0; i < 32; i++) {
        int hi = hexval((unsigned char)response_hex[2 * i]);
        int lo = hexval((unsigned char)response_hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        given[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Every key with a matching id is tried and the result accumulated, so
     * the time taken does not depend on which key matched. */
    bool ok = false;
    for (int i = 0; i < kr->n; i++) {
        if (strcmp(kr->keys[i].key_id, key_id))
            continue;
        uint8_t mac[32];
        auth_hmac_sha256((const uint8_t *)kr->keys[i].psk,
                         strlen(kr->keys[i].psk), nonce, nonce_len, mac);
        ok |= ct_equal(mac, given, 32);
    }
    return ok;
}

/* -------------------------------------------------------------- keygen */

static int mkdir_parent(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';
    if (mkdir(dir, 0755) == 0 || errno == EEXIST)
        return 0;
    fprintf(stderr, "keygen: cannot create %s: %s\n", dir, strerror(errno));
    return -1;
}

int auth_keygen_main(int argc, char **argv, const char *default_path)
{
    const char *path = default_path;
    const char *key_id = "default";
    bool quiet = false;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--keys-file") && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--key-id") && i + 1 < argc) {
            key_id = argv[++i];
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf(
"Usage: " SLC_DAEMON_NAME " keygen [--key-id ID] [--keys-file PATH] [--quiet]\n"
"\n"
"Generates a pre-shared key for SDR-SLC-CP-1.0 §26 (psk-hmac-sha256) and\n"
"appends it to the key file the daemon reads at startup (default %s).\n"
"The secret is printed exactly once. Paste it into the client as the key\n"
"for this device; the key_id (default \"default\") goes alongside it.\n"
"\n"
"  --key-id ID       name for this key, so one client can later be revoked\n"
"                    by deleting its line without disturbing the others\n"
"  --keys-file PATH  write somewhere other than the default\n"
"  --quiet           print only the secret, for scripting\n",
                   default_path);
            return 0;
        } else {
            fprintf(stderr, "keygen: unknown argument %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    for (const char *p = key_id; *p; p++) {
        if (isspace((unsigned char)*p) || *p == '#') {
            fprintf(stderr, "keygen: key_id may not contain whitespace or #\n");
            return 2;
        }
    }
    if (strlen(key_id) >= AUTH_KEY_ID_MAX) {
        fprintf(stderr, "keygen: key_id too long\n");
        return 2;
    }

    auth_keyring_t kr;
    int n = auth_keyring_load(&kr, path);
    if (n < 0)
        return 1;
    for (int i = 0; i < kr.n; i++) {
        if (!strcmp(kr.keys[i].key_id, key_id)) {
            fprintf(stderr, "keygen: a key named \"%s\" already exists in %s; "
                            "delete that line to replace it, or choose another "
                            "--key-id\n", key_id, path);
            return 1;
        }
    }

    uint8_t raw[32];
    size_t got = 0;
    while (got < sizeof raw) {
        ssize_t r = getrandom(raw + got, sizeof raw - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "keygen: getrandom(): %s\n", strerror(errno));
            return 1;
        }
        got += (size_t)r;
    }
    char psk[65];
    auth_hex(raw, sizeof raw, psk);

    if (mkdir_parent(path) != 0)
        return 1;

    /* 0600 on creation: the file holds secrets. An existing file keeps
     * whatever mode the operator gave it. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "keygen: cannot open %s for writing: %s\n",
                path, strerror(errno));
        if (errno == EACCES)
            fprintf(stderr, "        (the default location needs root: "
                            "sudo " SLC_DAEMON_NAME " keygen)\n");
        return 1;
    }
    char line[AUTH_KEY_ID_MAX + 80];
    int len = snprintf(line, sizeof line, "%s %s\n", key_id, psk);
    if (write(fd, line, (size_t)len) != len) {
        fprintf(stderr, "keygen: short write to %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    if (quiet) {
        printf("%s\n", psk);
        return 0;
    }
    printf(
"Generated a pre-shared key for the " SLC_AUTH_FUNCTION " function and appended it to\n"
"  %s\n"
"\n"
"  key_id : %s\n"
"  psk    : %s\n"
"\n"
"Enter both in the client's authentication settings for this device.\n"
"This is the only time the secret is shown; the daemon reads the file,\n"
"it never prints it.\n"
"\n"
"Restart the daemon (or start it) for the key to take effect:\n"
"  sudo systemctl restart " SLC_DAEMON_NAME "\n"
"\n"
"To revoke this key later, delete its line from the file and restart.\n",
           path, key_id, psk);
    return 0;
}
