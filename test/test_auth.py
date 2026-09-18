#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# sdr-slc-rtld — example / reference code, published so you can build your
# own SDR-SLC device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).

"""
SDR-SLC-CP-1.0 §26 authentication checks for sdr-slc-rtld.

The receiver does not require authentication by default (§26: "a receiver
has no particular reason to use it"), so this suite starts the daemon
three ways: --rx-auth psk with a key, --rx-auth psk in the first-run state
with no key at all, and the default.  It also exercises the keygen
subcommand end to end: generate a key, start the daemon on that file,
authenticate with the printed secret.

    python3 test/test_auth.py ./sdr-slc-rtld-stub
"""
import os
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slc  # noqa: E402

PORT = 4760
FAILURES = []
CHECKS = 0
HELLO = {"client_name": "auth-check", "client_version": "1.0",
         "supported_protocol_versions": ["1.0"]}


def check(cond, clause, what):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(f"{clause}: {what}")
        print(f"  FAIL  [{clause}] {what}")
    else:
        print(f"  ok    [{clause}] {what}")


def section(t):
    print(f"\n== {t}")


def err(r):
    return r.get("error", {}).get("code") if r["status"] == "error" else None


def with_key(binary):
    section("--rx-auth psk with a configured key (§26.1, §26.2, §26.3)")
    d = slc.spawn(binary, PORT, extra=["--rx-auth", "psk"])
    try:
        a = slc.CP(PORT)
        h = a.call("hello", HELLO)["result"]
        auth = h.get("auth")
        check(isinstance(auth, dict), "§26.2", "hello carries an auth object")
        check(auth and auth.get("methods") == ["psk-hmac-sha256"], "§26.2",
              "methods lists psk-hmac-sha256")
        nonce = auth.get("nonce", "") if auth else ""
        check(len(nonce) >= 32 and all(c in "0123456789abcdef" for c in nonce),
              "§26.2", f"nonce is at least 16 octets of lowercase hex ({len(nonce)//2} octets)")

        b = slc.CP(PORT)
        h2 = b.call("hello", HELLO)["result"]
        check(h2["auth"]["nonce"] != nonce, "§26.2",
              "a second connection gets a different nonce")

        caps = a.call("get_capabilities")["result"]["capabilities"]
        check(caps["rx"].get("auth") == "psk-hmac-sha256", "§26.1",
              "capabilities.rx.auth names the method")

        for cmd, p in (("ping", {}), ("get_status", {}),
                       ("get_clock_status", {}),
                       ("get_control", {"function": "rx", "name": "rf_gain"})):
            r = a.call(cmd, p)
            check(r["status"] == "ok", "§26.1",
                  f"{cmd} is not gated (reading never needs a key)")

        for cmd, p in (("set_frequency", {"function": "rx", "frequency_hz": 100000000}),
                       ("set_sample_rate", {"function": "rx", "sample_rate_sps": 2048000}),
                       ("set_control", {"function": "rx", "name": "rf_gain", "value": 10}),
                       ("open_stream", {"function": "rx", "protocol": "vita49",
                                        "vita": {"destination": {"ip": "127.0.0.1", "port": 60000}}}),
                       ("start_rx", {"stream_id": "rx-1"})):
            r = a.call(cmd, p)
            check(err(r) == "unauthorized", "§26.1",
                  f"{cmd} addressing rx is unauthorized before authenticate")

        r = a.call("set_frequency", {"function": "tx", "frequency_hz": 1})
        check(err(r) == "not_supported", "§12",
              "a function the device lacks still answers not_supported, not unauthorized")

        # Wrong secret.
        r = a.call("authenticate", slc.auth_params(h, psk="not-the-key"))
        check(err(r) == "auth_failed", "§26.3", "wrong psk is auth_failed")
        msg = r["error"]["message"].lower() if err(r) else ""
        check("key" not in msg and "mac" not in msg and "method" not in msg,
              "§26.3", "the refusal does not say which part was wrong")
        fresh = r["error"].get("details", {}).get("nonce") if err(r) else None
        check(fresh and fresh != nonce, "§26.3",
              "auth_failed carries a fresh nonce in details.nonce")

        # The old nonce is dead: a correct MAC over it must fail now.
        r = a.call("authenticate", {"method": "psk-hmac-sha256",
                                    "key_id": slc.KEY_ID,
                                    "response": slc.mac_for(nonce)})
        check(err(r) == "auth_failed", "§26.3",
              "a correct MAC over the previous nonce is refused")
        fresh = r["error"]["details"]["nonce"]

        r = a.call("authenticate", {"method": "psk-hmac-sha256",
                                    "key_id": "nobody",
                                    "response": slc.mac_for(fresh)})
        check(err(r) == "auth_failed", "§26.3", "unknown key_id is auth_failed")
        fresh = r["error"]["details"]["nonce"]

        r = a.call("authenticate", {"method": "psk-hmac-sha256",
                                    "response": slc.mac_for(fresh)})
        check(err(r) == "auth_failed", "§26.2",
              "omitted key_id means \"default\", which is not this key")
        fresh = r["error"]["details"]["nonce"]

        r = a.call("authenticate", {"key_id": slc.KEY_ID})
        check(err(r) == "missing_parameter", "§26.2",
              "authenticate without method/response is missing_parameter")

        # The right one.
        good = {"method": "psk-hmac-sha256", "key_id": slc.KEY_ID,
                "response": slc.mac_for(fresh)}
        r = a.call("authenticate", good)
        check(r["status"] == "ok" and r["result"].get("authenticated") is True,
              "§26.2", "correct response authenticates")
        check(r["result"].get("functions") == ["rx"], "§26.2",
              "result names the unlocked function")
        r = a.call("set_frequency", {"function": "rx", "frequency_hz": 100000000})
        check(r["status"] == "ok", "§26.1", "rx commands work once authenticated")

        # Replay: the nonce was single use, but the session stays unlocked.
        r = a.call("authenticate", good)
        check(err(r) == "auth_failed", "§26.3",
              "replaying the successful response on the same connection fails")
        r = a.call("set_frequency", {"function": "rx", "frequency_hz": 101000000})
        check(r["status"] == "ok", "§26.3",
              "a failed re-authenticate never removes an unlocked function")

        # The other connection is still locked, and can still observe.
        r = b.call("set_frequency", {"function": "rx", "frequency_hz": 1})
        check(err(r) == "unauthorized", "§26.3",
              "authentication is scoped to the TCP session")
        r = a.call("open_stream", {"function": "rx", "protocol": "vita49",
                                   "vita": {"destination": {"ip": "127.0.0.1", "port": 60000}}})
        check(r["status"] == "ok", "§26.1", "authenticated owner opens a stream")
        r = b.call("get_status")
        check(r["result"]["rx"]["control_state"] == "owned_by_other", "§17.4",
              "an unauthenticated observer can still read get_status")

        # Rate limit: ten failures in a minute closes the connection.
        c = slc.CP(PORT)
        hc = c.call("hello", HELLO)["result"]
        closed = False
        n = 0
        try:
            for n in range(1, 13):
                r = c.call("authenticate", slc.auth_params(hc, psk="wrong"))
                if err(r) != "auth_failed":
                    break
                hc = {"auth": {"nonce": r["error"]["details"]["nonce"]}}
        except (EOFError, OSError, TimeoutError):
            closed = True
        check(closed and n >= 10, "§26.3",
              f"connection closed after {n} failed attempts")
        c.close()
        a.close(); b.close()
    finally:
        slc.stop(d)


def locked(binary):
    section("--rx-auth psk with no key configured: locked (§26.3)")
    d = slc.spawn(binary, PORT + 2, extra=["--rx-auth", "psk"], keys=False)
    try:
        a = slc.CP(PORT + 2)
        h = a.call("hello", HELLO)["result"]
        check("auth" in h, "§26.2", "hello still issues a challenge")
        r = a.call("set_frequency", {"function": "rx", "frequency_hz": 1000000})
        check(err(r) == "unauthorized", "§26.3", "rx is unauthorized")
        check("keygen" in r["error"]["message"], "§26.3",
              "and the message names the keygen command")
        r = a.call("authenticate", slc.auth_params(h))
        check(err(r) == "auth_failed", "§26.3",
              "a device with no key refuses a key rather than accepting any")
        r = a.call("get_status")
        check(r["status"] == "ok", "§26.1", "get_status still works")
        a.close()
    finally:
        slc.stop(d)


def default_off(binary):
    section("default: no authentication (§26)")
    d = slc.spawn(binary, PORT + 4)
    try:
        a = slc.CP(PORT + 4)
        h = a.call("hello", HELLO)["result"]
        check("auth" not in h, "§26.2", "hello carries no auth object")
        caps = a.call("get_capabilities")["result"]["capabilities"]
        check(caps["rx"].get("auth") == "none", "§26.1", "rx.auth is none")
        r = a.call("authenticate", {"method": "psk-hmac-sha256", "response": "00"})
        check(r["status"] == "ok" and r["result"].get("functions") == [],
              "§26.3", "authenticate on an open device succeeds and unlocks nothing")
        r = a.call("set_frequency", {"function": "rx", "frequency_hz": 100000000})
        check(r["status"] == "ok", "§26.1", "rx is open")
        a.close()
    finally:
        slc.stop(d)


def keygen(binary):
    section("keygen subcommand (§26.3: no shipped key; operator generates one)")
    path = os.path.join(tempfile.mkdtemp(prefix="slc-keygen-"), "sub", "rtld.keys")
    p = subprocess.run([binary, "keygen", "--keys-file", path, "--key-id",
                        "shack-laptop", "--quiet"], capture_output=True, text=True)
    check(p.returncode == 0, "keygen", f"exits 0 ({p.stderr.strip()})")
    psk = p.stdout.strip()
    check(len(psk) == 64 and all(c in "0123456789abcdef" for c in psk),
          "keygen", "--quiet prints a 64-hex-char secret and nothing else")
    check(os.path.exists(path), "keygen", "creates the parent directory and file")
    mode = stat.S_IMODE(os.stat(path).st_mode) if os.path.exists(path) else 0
    check(mode == 0o600, "keygen", f"key file is mode 0600 (got {oct(mode)})")
    line = open(path).read().strip().split("\n")[-1] if os.path.exists(path) else ""
    check(line == f"shack-laptop {psk}", "keygen", "file line is \"<key_id> <psk>\"")

    p = subprocess.run([binary, "keygen", "--keys-file", path, "--key-id",
                        "shack-laptop"], capture_output=True, text=True)
    check(p.returncode != 0 and "already exists" in p.stderr, "keygen",
          "a duplicate key_id is refused")
    p = subprocess.run([binary, "keygen", "--keys-file", path, "--key-id",
                        "second", "--quiet"], capture_output=True, text=True)
    check(p.returncode == 0 and len(open(path).read().strip().split("\n")) == 2,
          "keygen", "a second key is appended, not overwritten")

    d = slc.spawn(binary, PORT + 6, extra=["--rx-auth", "psk", "--keys-file", path],
                  keys=False)
    try:
        a = slc.CP(PORT + 6)
        h = a.call("hello", HELLO)["result"]
        r = a.call("authenticate", {"method": "psk-hmac-sha256",
                                    "key_id": "shack-laptop",
                                    "response": slc.mac_for(h["auth"]["nonce"], psk)})
        check(r["status"] == "ok" and r["result"].get("functions") == ["rx"],
              "§26.2", "the generated key authenticates against the daemon "
                       "(Python hmac agrees with common/auth.c)")
        a.close()
    finally:
        slc.stop(d)

    # A malformed key file is refused at startup, not half-read.
    bad = path + ".bad"
    open(bad, "w").write("only-an-id\n")
    p = subprocess.run([binary, "--no-mdns", "--port", str(PORT + 8), "--rx-port",
                        str(PORT + 9), "--rx-auth", "psk", "--keys-file", bad],
                       capture_output=True, text=True, timeout=10)
    check(p.returncode == 1, "§26.3", "a malformed key file stops the daemon at startup")


def main():
    binary = slc.binary_from_argv()
    print("SDR-SLC-CP-1.0 §26 authentication checks")
    try:
        with_key(binary)
        locked(binary)
        default_off(binary)
        keygen(binary)
    except Exception as exc:  # noqa: BLE001
        print(f"\nABORTED: {exc!r}")
        FAILURES.append(f"exception: {exc!r}")
    print(f"\n{CHECKS - len(FAILURES)}/{CHECKS} checks passed")
    if FAILURES:
        print("\nFailures:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
