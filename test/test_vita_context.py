#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# sdr-slc-rtld — example / reference code, published so you can build your
# own SDR-SLC device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).

"""
Wire-level checks on the VITA-A/1.0-CTX Context packet, SDR-SLC-VITA-1.0
Release 1.2 §7, and on the IF Data header that follows it (§6).

Verifies what cannot be seen from the control plane alone:

  CP §24.3   a Context packet is the FIRST UDP packet after start_rx
  VITA §7.1  24 bytes, header 0x4000_0006 | count<<16
  VITA §7.3  CIF0 0x81008000 on the first packet (bit 31 changed, bit 24
             Reference Level, bit 15 Payload Format), 0x01008000 when
             nothing moved
  VITA §7.4  Reference Level Q9.7 in the low half-word, high half zero
  VITA §7.5  Payload Format word 0 computed for u8: 0x300001C7 complex
  VITA §7.6  a gain change produces a fresh Context packet before the
             next IF Data packet, with the change bit set
  VITA §6.1  IF Data: TSI=01 (UTC), TSF=10, Class ID 0x402814 / 0x0001
  VITA §6.4  the integer timestamp is host UTC, not seconds-since-start

    python3 test/test_vita_context.py ./sdr-slc-rtld-stub
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slc  # noqa: E402

PORT = 4780
UDP_PORT = 60050
FAILURES = []
HELLO = {"client_name": "ctx-check", "client_version": "1",
         "supported_protocol_versions": ["1.0"]}


def check(cond, clause, what):
    if cond:
        print(f"  ok    [{clause}] {what}")
    else:
        FAILURES.append(f"{clause}: {what}")
        print(f"  FAIL  [{clause}] {what}")


def pkt_type(pkt):
    return struct.unpack(">I", pkt[:4])[0] >> 28


def decode_context(pkt):
    """(hdr, stream_id, cif0, reference_level_dbm, ref_word, pf0, pf1)"""
    w = struct.unpack(">6I", pkt[:24])
    raw = w[3] & 0xFFFF
    val = raw - 65536 if raw & 0x8000 else raw
    return w[0], w[1], w[2], val / 128.0, w[3], w[4], w[5]


def next_context(u, limit=50000):
    for _ in range(limit):
        p, _a = u.recvfrom(65535)
        if pkt_type(p) == 4:
            return p
    return None


def open_udp():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    u.bind(("127.0.0.1", UDP_PORT))
    u.settimeout(5)
    return u


def main_checks(binary):
    d = slc.spawn(binary, PORT)
    u = open_udp()
    try:
        c = slc.CP(PORT)
        c.call("hello", HELLO)
        c.call("set_control", {"function": "rx", "name": "rf_gain", "value": 19.7})
        r = c.call("open_stream", {
            "function": "rx", "protocol": "vita49",
            "vita": {"destination": {"ip": "127.0.0.1", "port": UDP_PORT}}})
        sid = r["result"]["stream_id"]
        vita_sid = r["result"]["vita"]["stream_id"]

        t_start = time.time()
        c.call("start_rx", {"stream_id": sid})

        first, _ = u.recvfrom(65535)
        check(pkt_type(first) == 4, "CP §24.3",
              "first UDP packet after start_rx is a Context packet "
              f"(got packet type {pkt_type(first)})")

        if pkt_type(first) == 4:
            hdr, s_id, cif0, ref, ref_word, pf0, pf1 = decode_context(first)
            check(len(first) == 24, "§7.1", f"Context packet is 24 bytes (got {len(first)})")
            check(hdr & 0xFFFF == 6, "§7.2", f"header size field is 6 words (got {hdr & 0xFFFF})")
            check(hdr & 0x0FF00000 == 0, "§7.2",
                  "no Class ID, no TSI, no TSF in the Context header")
            check(hdr >> 16 & 0xF == 0, "§7.2", "Context packet count starts at 0")
            check(s_id == vita_sid, "§9", "Context stream_id matches the one from open_stream")
            check(cif0 == 0x81008000, "§7.3",
                  f"CIF0 is 0x81008000 on the first packet (got 0x{cif0:08X})")
            check(ref_word >> 16 == 0, "§7.4", "Reference Level high half-word is zero")
            check(abs(ref - (-29.7)) < 0.6, "§7.4",
                  f"Reference Level {ref:.2f} dBm at 19.7 dB gain (nominal -10 dBm at 0 dB)")
            check(pf0 == 0x300001C7, "§7.5",
                  f"Payload Format word 0 is 0x300001C7 for complex u8 (got 0x{pf0:08X})")
            check(pf1 == 0, "§7.5", "Payload Format word 1 is zero")

        nxt, _ = u.recvfrom(65535)
        check(pkt_type(nxt) == 1, "CP §24.3", "IF Data packets follow the Context packet")
        if pkt_type(nxt) == 1:
            w = struct.unpack(">7I", nxt[:28])
            check((w[0] >> 27) & 1 == 1, "§6.1", "IF Data: Class ID present")
            check((w[0] >> 22) & 3 == 1, "§6.1", "IF Data: TSI=01 (UTC)")
            check((w[0] >> 20) & 3 == 2, "§6.1", "IF Data: TSF=10 (picoseconds)")
            check(w[2] == 0x00402814 and w[3] == 0x00000001, "§6.3",
                  "Class ID OUI 0x402814, ICC 0, PCC 1")
            check(w[0] & 0xFFFF == 7 + (len(nxt) - 28) // 4, "§6.1",
                  "packet size field counts header and payload words")
            check(abs(w[4] - t_start) < 5, "§6.4.1",
                  f"Timestamp Integer is host UTC (epoch {w[4]}), not seconds since start")
            check(w[1] == vita_sid, "§6.2", "IF Data stream_id matches")
            check((len(nxt) - 28) % 4 == 0, "§6.5", "u8 payload is a whole number of words")

        # §6.4.2: consecutive stamps advance by exactly spp*1e12/fs (integer),
        # across USB buffer boundaries too.  200 packets spans several.
        fs = r["result"]["sample_rate_sps"]
        spp = r["result"]["vita"]["samples_per_packet"]
        step = spp * 10 ** 12 // fs
        prev = None
        bad = 0
        n = 0
        while n < 200:
            p, _ = u.recvfrom(65535)
            if pkt_type(p) != 1:
                continue
            w = struct.unpack(">7I", p[:28])
            tot = w[4] * 10 ** 12 + ((w[5] << 32) | w[6])
            cnt = (w[0] >> 16) & 0xF
            if prev is not None and (prev[1] + 1) & 0xF == cnt and abs(tot - prev[0] - step) > 2:
                bad += 1
            prev = (tot, cnt)
            n += 1
        check(bad == 0, "§6.4.2",
              f"consecutive stamps advance by {step} ps (+/-2) over 200 packets ({bad} off)")

        # §7.6: a gain change must produce a fresh Context packet, changed.
        c.call("set_control", {"function": "rx", "name": "rf_gain", "value": 40.2})
        p = next_context(u)
        check(p is not None, "§7.6", "a gain change emits a new Context packet")
        if p:
            hdr, _s, cif0, ref2, _rw, _pf0, _pf1 = decode_context(p)
            check(cif0 == 0x81008000, "§7.3",
                  f"change bit set after the gain change (got 0x{cif0:08X})")
            check(ref2 < -45.0, "§7.4",
                  f"Reference Level fell to {ref2:.2f} dBm when gain rose to 40.2 dB")
            check(hdr >> 16 & 0xF == 1, "§7.2", "Context packet count incremented independently")

        # Same gain again: a Context packet goes out, and nothing changed.
        c.call("set_control", {"function": "rx", "name": "rf_gain", "value": 40.2})
        p = next_context(u)
        if p:
            _h, _s, cif0, _r, _rw, _p0, _p1 = decode_context(p)
            check(cif0 == 0x01008000, "§7.3",
                  f"change bit clear when Reference Level and format are unchanged "
                  f"(got 0x{cif0:08X})")

        c.call("stop_rx", {"stream_id": sid})
        c.call("close_stream", {"stream_id": sid})
        c.close()
    finally:
        u.close()
        slc.stop(d)


def no_context_checks(binary):
    print("\n-- --no-context-packets")
    d = slc.spawn(binary, PORT + 2, extra=["--no-context-packets"])
    u = open_udp()
    try:
        c = slc.CP(PORT + 2)
        c.call("hello", HELLO)
        r = c.call("open_stream", {
            "function": "rx", "protocol": "vita49",
            "vita": {"destination": {"ip": "127.0.0.1", "port": UDP_PORT}}})
        sid = r["result"]["stream_id"]
        c.call("start_rx", {"stream_id": sid})
        first, _ = u.recvfrom(65535)
        check(pkt_type(first) == 1, "opt-out",
              "with --no-context-packets the first packet is IF Data")
        c.call("stop_rx", {"stream_id": sid})
        c.close()
    finally:
        u.close()
        slc.stop(d)


def main():
    binary = slc.binary_from_argv()
    print("VITA-A/1.0-CTX Context packet wire checks (SDR-SLC-VITA-1.0 R1.2)")
    try:
        main_checks(binary)
        no_context_checks(binary)
    except Exception as exc:  # noqa: BLE001
        print(f"\nABORTED: {exc!r}")
        FAILURES.append(f"exception: {exc!r}")
    if FAILURES:
        print("\nFailures:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("\nall Context packet checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
