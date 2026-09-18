/* SPDX-License-Identifier: MIT
 *
 * sdr-slc-rtld — RTL-SDR bridge daemon for the SDR-SLC protocol suite.
 *
 * Example / reference code, published so you can build your own SDR-SLC
 * device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).
 */

/* auth.h - SDR-SLC-CP-1.0 §26 authentication: psk-hmac-sha256.
 *
 * §26.1 requires a device implementing the tx block to implement
 * authentication and to require it for tx by default; the operator may switch
 * it off. The keys live in a file the operator populates with the keygen
 * subcommand, because §26.3 forbids shipping a pre-configured key: a device
 * requiring authentication with no key configured refuses the protected
 * functions rather than accepting any key.
 *
 * Key file format, one key per line:
 *
 *     <key_id> <psk>
 *
 * Blank lines and lines starting with '#' are ignored. The PSK is used as
 * the HMAC key exactly as written, as UTF-8 octets (§26.2), so keygen writes
 * 64 hex characters - something that can be pasted into a client without
 * escaping. Any other printable string works as well.
 */
#ifndef SLC_AUTH_H
#define SLC_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_METHOD_PSK   "psk-hmac-sha256"
#define AUTH_NONCE_BYTES  16                 /* §26.2: at least 16 octets  */
#define AUTH_NONCE_HEX    (AUTH_NONCE_BYTES * 2 + 1)
#define AUTH_MAX_KEYS     32
#define AUTH_KEY_ID_MAX   64
#define AUTH_PSK_MAX      256

typedef struct {
    char key_id[AUTH_KEY_ID_MAX];
    char psk[AUTH_PSK_MAX];
} auth_key_t;

typedef struct {
    auth_key_t keys[AUTH_MAX_KEYS];
    int        n;
    char       path[512];
    bool       loaded;          /* file was readable (even if empty)        */
} auth_keyring_t;

/* Load the key file. Returns the number of keys, 0 if the file is absent or
 * empty (which is the first-run state), -1 on a read or parse error. */
int auth_keyring_load(auth_keyring_t *kr, const char *path);

/* Fresh nonce for one connection: AUTH_NONCE_BYTES random octets, written as
 * lowercase hex into out (AUTH_NONCE_HEX bytes). Returns false if the system
 * random source is unavailable, in which case nothing must be unlocked. */
bool auth_make_nonce(uint8_t *raw, char *hex_out);

/* §26.2: response = hex( HMAC-SHA256( psk, nonce_bytes ) ). Verifies the
 * client's hex MAC against every key with the given key_id, in constant
 * time. Returns true on a match. */
bool auth_verify(const auth_keyring_t *kr, const char *key_id,
                 const uint8_t *nonce, size_t nonce_len,
                 const char *response_hex);

/* Building blocks, exposed for the self-test in tests/ and for keygen. */
void auth_hmac_sha256(const uint8_t *key, size_t keylen,
                      const uint8_t *msg, size_t msglen, uint8_t out[32]);
void auth_hex(const uint8_t *in, size_t n, char *out);   /* out: 2n+1 bytes */

/* The keygen subcommand: generate a key, append it to the key file, print it
 * once. argv points at the arguments after "keygen". Returns a process exit
 * code. */
int auth_keygen_main(int argc, char **argv, const char *default_path);

#endif
