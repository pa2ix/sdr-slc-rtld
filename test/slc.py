# SPDX-License-Identifier: MIT
#
# sdr-slc-rtld — example / reference code, published so you can build your
# own SDR-SLC device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).

"""
slc.py - shared plumbing for the test suites.

Mirrors tests/slc.py in sdr-slc-bladerfd: every suite spawns its own
daemon (the no-dongle stub build) on private ports with --no-mdns, and
gets the client half of the SDR-SLC-CP-1.0 §26 exchange so a suite that
enables --rx-auth psk can authenticate like a real client.
"""

import hashlib
import hmac
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

KEY_ID = "test-client"
PSK = "test-psk-0123456789abcdef0123456789abcdef0123456789abcdef"

_keyfile = None


def keys_file():
    """Path to a key file holding KEY_ID/PSK. Created once per process."""
    global _keyfile
    if _keyfile is None:
        fd, path = tempfile.mkstemp(prefix="slc-test-", suffix=".keys")
        os.write(fd, f"# test keys\n{KEY_ID} {PSK}\n".encode())
        os.close(fd)
        _keyfile = path
    return _keyfile


def binary_from_argv(default="./sdr-slc-rtld-stub"):
    return sys.argv[1] if len(sys.argv) > 1 else default


def spawn(binary, port, rx_port=None, extra=(), keys=True, mdns=False,
          stderr=subprocess.DEVNULL):
    """Start a daemon on port (control) and rx_port (VITA source), with
    --no-mdns unless mdns=True. keys=False points --keys-file at a path that
    does not exist, i.e. the first-run state. The key file is only read with
    --rx-auth psk."""
    if rx_port is None:
        rx_port = port + 1
    args = [binary, "--port", str(port), "--rx-port", str(rx_port),
            "--name", f"SLC-TEST{port}"]
    if not mdns:
        args.append("--no-mdns")
    if keys:
        args += ["--keys-file", keys_file()]
    else:
        args += ["--keys-file", os.path.join(tempfile.gettempdir(),
                                             f"slc-test-absent-{port}.keys")]
    args += list(extra)
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=stderr)
    # wait for the control port rather than sleeping a fixed time
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            if proc.poll() is not None:
                raise RuntimeError(f"daemon exited with {proc.returncode}")
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("daemon did not open its control port")


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def mac_for(nonce_hex, psk=PSK):
    """§26.2: response = hex( HMAC-SHA256( psk, hex_decode(nonce) ) )."""
    return hmac.new(psk.encode(), bytes.fromhex(nonce_hex),
                    hashlib.sha256).hexdigest()


def auth_params(hello_result, key_id=KEY_ID, psk=PSK):
    return {"method": "psk-hmac-sha256", "key_id": key_id,
            "response": mac_for(hello_result["auth"]["nonce"], psk)}


class CP:
    """One SLC-CP control connection: length-prefixed JSON (§8.3)."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.n = 0
        self.events = []

    def send(self, command, params=None, msg_id=None):
        self.n += 1
        mid = msg_id or f"m{self.n}"
        msg = {"type": "request", "id": mid, "command": command,
               "params": params if params is not None else {}}
        raw = json.dumps(msg).encode()
        self.s.sendall(struct.pack(">I", len(raw)) + raw)
        return mid

    def _read_frame(self):
        hdr = b""
        while len(hdr) < 4:
            c = self.s.recv(4 - len(hdr))
            if not c:
                raise EOFError("connection closed")
            hdr += c
        (n,) = struct.unpack(">I", hdr)
        body = b""
        while len(body) < n:
            c = self.s.recv(n - len(body))
            if not c:
                raise EOFError("connection closed")
            body += c
        return json.loads(body)

    def call(self, command, params=None):
        mid = self.send(command, params)
        deadline = time.time() + 5
        while time.time() < deadline:
            m = self._read_frame()
            if m.get("type") == "event":
                self.events.append(m)
                continue
            if m.get("id") == mid:
                return m
        raise TimeoutError(f"no response to {command}")

    def drain_events(self, seconds=0.4):
        self.s.settimeout(seconds)
        try:
            while True:
                m = self._read_frame()
                if m.get("type") == "event":
                    self.events.append(m)
        except (socket.timeout, TimeoutError, OSError):
            pass
        finally:
            self.s.settimeout(5)
        return self.events

    def event_names(self):
        return [e.get("event") for e in self.events]

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass
