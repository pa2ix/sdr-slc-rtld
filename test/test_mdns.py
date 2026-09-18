#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# sdr-slc-rtld — example / reference code, published so you can build your
# own SDR-SLC device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).

"""
Discovery checks on the builtin mDNS responder (SDR-SLC-CP-1.0 §6, §24.1).

Starts the stub with --mdns builtin on the loopback interface, sends a
PTR query for _sdr-slc._tcp.local. to 224.0.0.251:5353, and decodes the
answer: PTR -> SRV/TXT/A, the eight TXT keys of §6.1, fw of the §6.2 form.
Then a direct SRV query for the instance, and the goodbye on shutdown.

Multicast on lo is not available everywhere (some containers); if the
query socket cannot join the group the suite reports that and skips.

    python3 test/test_mdns.py ./sdr-slc-rtld-stub
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slc  # noqa: E402

PORT = 4790
NAME = "SLC-MDNSTEST"
GROUP, MPORT = "224.0.0.251", 5353
FAILURES = []


def check(cond, clause, what):
    if cond:
        print(f"  ok    [{clause}] {what}")
    else:
        FAILURES.append(f"{clause}: {what}")
        print(f"  FAIL  [{clause}] {what}")


def encode_name(name):
    out = b""
    for label in name.strip(".").split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\x00"


def query(qname, qtype):
    return struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0) + encode_name(qname) + \
        struct.pack(">HH", qtype, 1)


def read_name(buf, pos):
    labels, jumped, end = [], False, None
    for _ in range(64):
        l = buf[pos]
        if l == 0:
            pos += 1
            break
        if l & 0xC0 == 0xC0:
            target = struct.unpack(">H", buf[pos:pos + 2])[0] & 0x3FFF
            if not jumped:
                end = pos + 2
            jumped, pos = True, target
            continue
        labels.append(buf[pos + 1:pos + 1 + l].decode())
        pos += 1 + l
    return ".".join(labels), (end if jumped else pos)


def parse_answers(buf):
    qd, an = struct.unpack(">HH", buf[4:8])
    pos = 12
    for _ in range(qd):
        _n, pos = read_name(buf, pos)
        pos += 4
    out = []
    for _ in range(an):
        name, pos = read_name(buf, pos)
        rtype, rcls, ttl, rdlen = struct.unpack(">HHIH", buf[pos:pos + 10])
        pos += 10
        rdata = buf[pos:pos + rdlen]
        rec = {"name": name, "type": rtype, "ttl": ttl, "flush": bool(rcls & 0x8000)}
        if rtype == 12:
            rec["target"], _ = read_name(buf, pos)
        elif rtype == 33:
            rec["port"] = struct.unpack(">H", rdata[4:6])[0]
            rec["target"], _ = read_name(buf, pos + 6)
        elif rtype == 16:
            txt, p = {}, 0
            while p < len(rdata):
                l = rdata[p]
                k, _, v = rdata[p + 1:p + 1 + l].decode().partition("=")
                txt[k] = v
                p += 1 + l
            rec["txt"] = txt
        elif rtype == 1:
            rec["addr"] = socket.inet_ntoa(rdata)
        pos += rdlen
        out.append(rec)
    return out


def listener():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", MPORT))
    mreq = socket.inet_aton(GROUP) + socket.inet_aton("127.0.0.1")
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    s.settimeout(3)
    return s


def collect(s, want, seconds=1.5):
    """Gather responses (QR=1) until one satisfies want(records)."""
    deadline = time.time() + seconds
    seen = []
    while time.time() < deadline:
        try:
            buf, _ = s.recvfrom(4096)
        except socket.timeout:
            break
        if len(buf) < 12 or not buf[2] & 0x80:
            continue
        try:
            recs = parse_answers(buf)
        except Exception:  # noqa: BLE001
            continue
        seen.extend(recs)
        if want(recs):
            return recs
    return seen


def main():
    binary = slc.binary_from_argv()
    print("mDNS builtin responder checks (SDR-SLC-CP-1.0 §6)")
    try:
        s = listener()
    except OSError as e:
        print(f"  skip  multicast on loopback unavailable here ({e}); "
              "nothing checked")
        return 0

    proc = slc.spawn(binary, PORT, mdns=True,
                     extra=["--mdns", "builtin", "-i", "lo", "--name", NAME])
    try:
        # The startup announcement.
        recs = collect(s, lambda r: any(x["type"] == 12 for x in r), 3)
        check(any(x["type"] == 12 and x["target"].startswith(NAME) for x in recs),
              "§6", "startup announcement carries a PTR to the instance")

        # A browse query, as a client sends it.
        s.sendto(query("_sdr-slc._tcp.local.", 12), (GROUP, MPORT))
        recs = collect(s, lambda r: any(x["type"] == 16 for x in r))
        types = {x["type"] for x in recs}
        check({12, 33, 16, 1} <= types, "RFC 6763 §12.1",
              f"PTR query answered with PTR, SRV, TXT and A (got {sorted(types)})")
        srv = next((x for x in recs if x["type"] == 33), None)
        check(srv and srv["port"] == PORT, "§6",
              f"SRV names the control port {PORT}")
        check(srv and srv["target"].startswith(NAME) and srv["flush"], "RFC 6762 §10.2",
              "SRV target is <instance>.local with cache-flush set")
        a = next((x for x in recs if x["type"] == 1), None)
        check(a and a["addr"] == "127.0.0.1", "§6", "A record carries the interface address")
        txt = next((x["txt"] for x in recs if x["type"] == 16), {})
        for k in ("name", "proto", "proto_ver", "functions", "hw", "fw", "minhz", "maxhz"):
            check(k in txt, "§6.1", f"TXT has {k}")
        check(txt.get("functions") == "rx", "§6.1", "functions=rx")
        check(txt.get("fw", "").startswith("sdr-slc-rtld/"), "§6.2",
              f"fw is <implementation>/<version> ({txt.get('fw')})")
        check(txt.get("name") == NAME and txt.get("proto") == "sdr-slc", "§6.1",
              "name and proto as published")
        check("role" not in txt and "tx" not in txt, "§6.1", "no R1.1 role/rx/tx keys")
        check(srv and srv["ttl"] == 120, "RFC 6762 §10", "SRV record TTL 120")

        # Resolve query for the instance's SRV, answered with SRV and A.
        s.sendto(query(f"{NAME}._sdr-slc._tcp.local.", 33), (GROUP, MPORT))
        recs = collect(s, lambda r: any(x["type"] == 33 for x in r))
        check(any(x["type"] == 33 for x in recs) and any(x["type"] == 1 for x in recs),
              "RFC 6763 §12.2", "SRV query for the instance answered with SRV and A")

        # Meta-query.
        s.sendto(query("_services._dns-sd._udp.local.", 12), (GROUP, MPORT))
        recs = collect(s, lambda r: any(x["type"] == 12 for x in r))
        check(any(x["type"] == 12 for x in recs), "RFC 6763 §9",
              "service type enumeration query answered")

        # Goodbye on shutdown.
        proc.terminate()
        recs = collect(s, lambda r: any(x["type"] == 12 and x["ttl"] == 0 for x in r), 3)
        check(any(x["ttl"] == 0 for x in recs), "RFC 6762 §10.1",
              "goodbye (TTL 0) sent on shutdown")
    except Exception as exc:  # noqa: BLE001
        print(f"\nABORTED: {exc!r}")
        FAILURES.append(f"exception: {exc!r}")
    finally:
        slc.stop(proc)
        s.close()

    if FAILURES:
        print("\nFailures:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("\nall mDNS checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
