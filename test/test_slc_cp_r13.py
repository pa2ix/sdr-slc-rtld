#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# sdr-slc-rtld — example / reference code, published so you can build your
# own SDR-SLC device. Do as you like with it; see LICENSE. Ivo van Ling (PA2IX).

"""
Conformance checks for SDR-SLC-CP-1.0 Release 1.3 (block rx, Tier 2).

Drives the daemon over a real TCP control connection and asserts the wire
format against the specification, section by section.  Spawns the stub
build itself, so no dongle is needed:

    make test-stub
    python3 test/test_slc_cp_r13.py ./sdr-slc-rtld-stub

Every check names the clause it is checking, so a failure points at the
paragraph rather than at this file.  The Release 1.2 checks are all still
here (§1.1: nothing in 1.3 changes the receive path); the Release 1.3
section at the end covers what did change.
"""
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slc  # noqa: E402

PORT = 4740
UDP_PORT = 60000

FAILURES = []
CHECKS = 0


def check(cond, clause, what):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(f"{clause}: {what}")
        print(f"  FAIL  [{clause}] {what}")
    else:
        print(f"  ok    [{clause}] {what}")


def CP():
    return slc.CP(PORT)


HELLO = {"client_name": "r13-conformance", "client_version": "1.0",
         "supported_protocol_versions": ["1.0"]}


def section(title):
    print(f"\n== {title}")


# ---------------------------------------------------------------------------
def test_envelope():
    section("Envelope — hello, framing, hello-first (§10, §24.1)")
    c = CP()

    r = c.call("get_status")
    check(r["status"] == "error" and r["error"]["code"] == "invalid_request",
          "§10.1", "command before hello is rejected with invalid_request")

    r = c.call("hello", HELLO)
    check(r["status"] == "ok", "§10.1", "hello succeeds")
    check(r["result"]["selected_protocol_version"] == "1.0",
          "§10.1", "selected_protocol_version returned")
    check(r["result"]["protocol_name"] == "SDR-SLC Control Plane",
          "§10.1", "protocol_name is the fixed literal")

    r = c.call("no_such_command")
    check(r["status"] == "error" and r["error"]["code"] == "unknown_command",
          "§24.1", "unimplemented command returns unknown_command")

    # An unknown field must be ignored, not rejected.
    c.n += 1
    msg = {"type": "request", "id": "extra-1", "command": "ping",
           "params": {}, "future_field": {"a": 1}}
    raw = json.dumps(msg).encode()
    c.s.sendall(struct.pack(">I", len(raw)) + raw)
    m = c._read_frame()
    check(m.get("id") == "extra-1" and m["status"] == "ok",
          "§9.4", "unknown top-level field is silently ignored")

    c.close()
    return True


def test_no_tx_commands():
    section("tx and codec absent (§11.1, §24.1)")
    c = CP()
    c.call("hello", HELLO)

    for cmd in ("start_tx", "stop_tx", "set_tx_power", "get_tx_capabilities",
                "get_vswr", "auto_tune"):
        r = c.call(cmd, {})
        check(r["status"] == "error" and
              r["error"]["code"] == "unknown_command",
              "§24.1", f"{cmd} returns unknown_command (not not_supported)")

    for cmd in ("set_clock", "get_location", "set_location"):
        r = c.call(cmd, {})
        check(r["status"] == "error" and
              r["error"]["code"] == "unknown_command",
              "§5.2", f"Tier 3 command {cmd} returns unknown_command")

    r = c.call("set_frequency", {"function": "tx", "frequency_hz": 14100000})
    check(r["status"] == "error" and r["error"]["code"] == "not_supported",
          "§12", "function:'tx' on a shared command returns not_supported")
    c.close()
    return True


def test_capabilities():
    section("Capabilities (§11, §13.2, §14.3)")
    c = CP()
    c.call("hello", HELLO)
    r = c.call("get_capabilities")
    caps = r["result"]["capabilities"]

    check(caps["schema"] == "sdrslc.capabilities", "§11.1", "schema present")
    check(caps["schema_version"] == 2, "§11.1", "schema_version is 2")
    check("rx" in caps, "§11.1", "rx key present")
    check("tx" not in caps, "§11.1",
          "tx key absent (R1.1 supported:false object dropped)")
    check("codec" not in caps, "§11.1", "codec key absent")
    check("streaming_protocols" not in caps, "§14.3",
          "top-level streaming_protocols replaced by per-block transports")

    rx = caps["rx"]
    check(rx.get("tier") in (1, 2, 3), "§4.4", "rx declares a tier")
    check("frequency_ranges_hz" in rx, "§24.2", "frequency_ranges_hz present")
    check(isinstance(rx.get("sample_rates_sps"), list), "§24.2",
          "sample_rates_sps is a discrete list")
    check("sample_rate_min_hz" not in rx and "sample_rate_max_hz" not in rx,
          "§13.2", "no min/max sample rate range alongside the list")
    check("vita_port" not in rx, "§1.2",
          "vita_port gone (renamed source_port, moved into transports[])")
    check("samples_per_packet" not in rx, "§14.2",
          "samples_per_packet moved into the transport descriptor")
    check("channels" not in rx, "§12.3",
          "single-path device omits channels entirely")

    tr = rx.get("transports")
    check(isinstance(tr, list) and len(tr) >= 1, "§14.3",
          "transports[] advertised")
    v = tr[0]
    check(v["protocol"] == "vita49", "§14.3", "vita49 transport advertised")
    check("source_port" in v, "§14.4",
          "transport declares source_port (device sends from it)")
    check("samples_per_packet" in v, "§14.2",
          "samples_per_packet is inside the transport descriptor")

    check(rx.get("access") == "exclusive", "§17.2",
          "rx declares an access model")

    names = {ctl["name"]: ctl for ctl in rx["controls"]}
    check("rf_gain" in names, "§24.2", "rf_gain published")
    check(all("type" in ctl for ctl in rx["controls"]), "§11.3",
          "every control descriptor carries a type")
    check(names["rf_gain"]["type"] == "float", "§11.3", "rf_gain is float")

    if rx.get("tier", 1) >= 2:
        for std in ("agc", "bias_tee", "ppm_correction", "direct_sampling"):
            check(std in names, "§24.3", f"Tier 2 standard control {std}")
        check(names["bias_tee"]["type"] == "float", "§11.3",
              "bias_tee is a float in volts, not the R1.1 boolean")
        check("allowed_values" in names["bias_tee"], "§11.3",
              "bias_tee publishes allowed_values")
        check(names["direct_sampling"]["type"] == "enum", "§11.3",
              "direct_sampling is an enum")
        check(set(names["direct_sampling"]["values"]) ==
              {"off", "i_branch", "q_branch"}, "§11.3",
              "direct_sampling values are off/i_branch/q_branch")
        for f in ("preferred_sample_rate_sps", "preferred_gain_db",
                  "reference_level_dbm_at_preferred_gain"):
            check(f in rx, "§24.3", f"Tier 2 hint {f} published")

    c.close()
    return caps


def test_function_addressing(caps):
    section("Function addressing (§12)")
    c = CP()
    c.call("hello", HELLO)

    r = c.call("set_frequency", {"direction": "rx", "frequency_hz": 14100000})
    check(r["status"] == "error" and
          r["error"]["code"] == "missing_parameter",
          "§12.1", "R1.1 'direction' is not accepted; missing_parameter")

    r = c.call("set_frequency", {"function": "rx", "frequency_hz": 14100000})
    check(r["status"] == "ok", "§13.1", "set_frequency with function:'rx'")
    check(r["result"].get("function") == "rx", "§13.1",
          "response carries function, not direction")
    check("direction" not in r["result"], "§12.1",
          "response does not carry direction")
    check(r["result"]["applied_frequency_hz"] == 14100000, "§13.1",
          "applied_frequency_hz reported")

    fmax = caps["rx"]["frequency_ranges_hz"][0]["max"]
    r = c.call("set_frequency", {"function": "rx", "frequency_hz": fmax + 10**6})
    check(r["status"] == "error" and
          r["error"]["code"] == "unsupported_frequency",
          "§22", "out-of-range frequency gives unsupported_frequency")

    r = c.call("set_sample_rate", {"function": "rx", "sample_rate_sps": 3})
    check(r["status"] == "error" and
          r["error"]["code"] == "unsupported_sample_rate",
          "§13.2", "rate absent from the published list is refused")

    rate = caps["rx"]["sample_rates_sps"][0]
    r = c.call("set_sample_rate", {"function": "rx", "sample_rate_sps": rate})
    check(r["status"] == "ok" and r["result"]["function"] == "rx",
          "§13.2", "set_sample_rate accepts a published rate")

    r = c.call("set_control", {"function": "rx", "channel": "a",
                               "name": "rf_gain", "value": 20.0})
    check(r["status"] == "error", "§12.3",
          "naming a channel on a device that publishes none is refused")
    c.close()


def test_controls():
    section("Controls (§11.3, §13.3)")
    c = CP()
    c.call("hello", HELLO)

    caps = c.call("get_capabilities")["result"]["capabilities"]
    gain = [x for x in caps["rx"]["controls"] if x["name"] == "rf_gain"][0]
    av = gain.get("allowed_values")
    check(isinstance(av, list) and len(av) >= 10 and av == sorted(av), "§11.3",
          f"rf_gain publishes allowed_values from the tuner ({len(av) if av else 0} steps)")
    check(av and av[0] == gain["min"] and av[-1] == gain["max"], "§11.3",
          "rf_gain min/max bracket the list")
    check(av and gain["default"] in av, "§11.3",
          f"rf_gain default {gain['default']} is on the list")
    check(caps["rx"]["preferred_gain_db"] == gain["default"], "§19.1",
          "preferred_gain_db equals the rf_gain default")
    check("step" not in gain, "§11.3", "no step alongside allowed_values")

    r = c.call("set_control", {"function": "rx", "name": "rf_gain",
                               "value": 22.9})
    check(r["status"] == "ok", "§13.3", "rf_gain float accepted")
    check(r["result"].get("applied_value") == 22.9, "§13.3",
          "applied_value is the listed value, unquantised")
    r = c.call("set_control", {"function": "rx", "name": "rf_gain",
                               "value": 24.0})
    check(r["status"] == "error" and r["error"]["code"] == "invalid_parameter",
          "§13.3", "a gain not in allowed_values is invalid_parameter")

    r = c.call("set_control", {"function": "rx", "name": "bias_tee",
                               "value": True})
    check(r["status"] == "error" and
          r["error"]["code"] == "invalid_parameter",
          "§11.3", "bias_tee rejects the R1.1 boolean form")

    r = c.call("set_control", {"function": "rx", "name": "bias_tee",
                               "value": 3.3})
    check(r["status"] == "error" and
          r["error"]["code"] == "invalid_parameter",
          "§13.3", "float with allowed_values must match exactly")

    r = c.call("set_control", {"function": "rx", "name": "bias_tee",
                               "value": 0.0})
    check(r["status"] == "ok", "§13.3", "bias_tee 0.0 V accepted")

    r = c.call("set_control", {"function": "rx", "name": "direct_sampling",
                               "value": "i_branch"})
    check(r["status"] == "ok", "§11.3", "direct_sampling enum accepted")
    r = c.call("get_control", {"function": "rx", "name": "direct_sampling"})
    check(r["result"]["value"] == "i_branch", "§13.4",
          "get_control reflects the applied enum value")
    r = c.call("get_capabilities")
    fr = r["result"]["capabilities"]["rx"]["frequency_ranges_hz"][0]
    check(fr["max"] == 30000000, "§11",
          "capabilities track the direct sampling branch")
    c.call("set_control", {"function": "rx", "name": "direct_sampling",
                           "value": "off"})

    r = c.call("set_control", {"function": "rx", "name": "no_such_control",
                               "value": 1})
    check(r["status"] == "error" and r["error"]["code"] == "not_supported",
          "§24.2", "unpublished control name returns not_supported")
    c.close()


def test_stream(caps):
    section("Stream session (§14) and events (§21)")
    c = CP()
    c.call("hello", HELLO)
    c.call("set_frequency", {"function": "rx", "frequency_hz": 14100000})

    # R1.1 top-level transport parameters must not work any more.
    r = c.call("open_stream", {"function": "rx", "protocol": "vita49",
                               "destination": {"ip": "127.0.0.1",
                                               "port": 60000},
                               "samples_per_packet": 256})
    check(r["status"] == "error" and
          r["error"]["code"] == "missing_parameter",
          "§14.2", "R1.1 top-level transport parameters are refused")

    r = c.call("open_stream", {"function": "rx", "protocol": "carrier_pigeon",
                               "vita": {"destination": {"ip": "127.0.0.1",
                                                        "port": 60000}}})
    check(r["status"] == "error" and
          r["error"]["code"] == "unsupported_stream_protocol",
          "§14.3", "protocol absent from transports[] is refused")

    r = c.call("open_stream", {
        "function": "rx", "protocol": "vita49",
        "vita": {"protocol_version": "VITA-A/1.0",
                 "format": {"encoding": "unsigned", "bits_per_component": 8,
                            "container_bits": 8, "packing": "interleaved",
                            "byte_order": "big_endian"},
                 "destination": {"ip": "127.0.0.1", "port": 60000}}})
    check(r["status"] == "ok", "§14.1", "open_stream with grouped transport")
    res = r["result"]
    sid = res["stream_id"]
    check(res.get("function") == "rx", "§14.1", "result carries function")
    check("vita" in res, "§14.2",
          "result groups transport parameters under 'vita'")
    for leaked in ("destination", "samples_per_packet", "format", "source"):
        check(leaked not in res, "§14.2",
              f"'{leaked}' does not appear at the top level of the result")
    v = res["vita"]
    for f in ("source", "destination", "samples_per_packet", "stream_id",
              "packet_class", "timestamp_mode", "format"):
        check(f in v, "§14.1", f"vita.{f} returned")
    check(v["destination"]["port"] == 60000, "§14.4",
          "device echoes the client-chosen destination port")
    check(v["format"]["label"] == "u8", "§14.1",
          "negotiated format echoed back")

    r = c.call("open_stream", {"function": "rx", "protocol": "vita49",
                               "vita": {"destination": {"ip": "127.0.0.1",
                                                        "port": 60001}}})
    check(r["status"] == "error" and
          r["error"]["code"] == "stream_already_open",
          "§22", "second open on the same session gives stream_already_open")

    r = c.call("start_rx", {"stream_id": sid})
    check(r["status"] == "ok" and r["result"]["streaming"] is True,
          "§14.5", "start_rx begins streaming")
    check(r["result"].get("function") == "rx", "§14.5",
          "start_rx result carries function")

    c.drain_events(0.5)
    st = [e for e in c.events if e["event"] == "stream_state"]
    check(len(st) >= 1, "§21.2", "stream_state emitted")
    check(st[-1]["params"].get("function") == "rx", "§21.1",
          "stream_state carries function, not direction")
    check("direction" not in st[-1]["params"], "§21.1",
          "stream_state does not carry direction")

    r = c.call("get_status")
    rx = r["result"]["rx"]
    check("tx" not in r["result"], "§18.1",
          "get_status has no tx object on a receive-only device")
    check(rx["streaming"] is True, "§18.1", "streaming reported")
    check(rx.get("control_state") == "owned_by_me", "§17.3",
          "control_state reported for the exclusive resource")
    for f in ("actual_sample_rate_sps", "reference_level_dbm",
              "adc_overload", "adc_level_dbfs", "lo_locked"):
        check(f in rx, "§24.3", f"Tier 2 status field {f} present")
    check("lo_locked" in rx and rx["lo_locked"] is None, "§18.2",
          "unavailable field is present as null, not absent")
    hm = rx.get("hardware_monitors")
    check(isinstance(hm, dict), "§18.3", "hardware_monitors always present")
    check({"pa_temperature_celsius", "pa_supply_voltage_v",
           "pa_drain_current_ma"} <= set(hm), "§18.3",
          "the three pa_* channels are present (null on a receiver)")
    check(rx["adc_level_dbfs"] is not None, "§18.1",
          "adc_level_dbfs measured while streaming")

    # Gain change while streaming: control_changed and a fresh Context.
    c.events.clear()
    c.call("set_control", {"function": "rx", "name": "rf_gain",
                           "value": 29.7})
    c.drain_events(0.4)
    check("control_changed" in c.event_names(), "§21.2",
          "control_changed emitted on a gain change")

    r = c.call("get_status")
    ref_hi = r["result"]["rx"]["reference_level_dbm"]
    c.call("set_control", {"function": "rx", "name": "rf_gain", "value": 12.5})
    r = c.call("get_status")
    ref_lo = r["result"]["rx"]["reference_level_dbm"]
    check(ref_lo > ref_hi, "§19",
          "Reference Level rises when gain falls (it tracks gain)")

    r = c.call("stop_rx", {"stream_id": sid})
    check(r["status"] == "ok" and r["result"]["streaming"] is False,
          "§14.5", "stop_rx stops the stream, session stays open")
    r = c.call("start_rx", {"stream_id": sid})
    check(r["status"] == "ok", "§14.5",
          "stream restarts without reopening")
    c.call("stop_rx", {"stream_id": sid})

    r = c.call("close_stream", {"stream_id": sid})
    check(r["status"] == "ok" and r["result"]["closed"] is True,
          "§14.6", "close_stream releases the session")
    r = c.call("start_rx", {"stream_id": sid})
    check(r["status"] == "error" and
          r["error"]["code"] == "stream_not_found",
          "§22", "stream_id is invalid after close_stream")
    c.close()


def test_exclusive_access():
    section("Access control and ownership (§17)")
    a = CP(); a.call("hello", HELLO)
    b = CP(); b.call("hello", HELLO)

    r = a.call("open_stream", {"function": "rx", "protocol": "vita49",
                               "vita": {"destination": {"ip": "127.0.0.1",
                                                        "port": 60010}}})
    check(r["status"] == "ok", "§17.2", "first client claims the rx channel")
    sid = r["result"]["stream_id"]

    r = b.call("open_stream", {"function": "rx", "protocol": "vita49",
                               "vita": {"destination": {"ip": "127.0.0.1",
                                                        "port": 60011}}})
    check(r["status"] == "error" and r["error"]["code"] == "in_use",
          "§17.2", "second client is refused with in_use")

    r = b.call("set_frequency", {"function": "rx", "frequency_hz": 7100000})
    check(r["status"] == "error" and r["error"]["code"] == "in_use",
          "§17.1", "non-owner cannot retune the channel out from under it")

    r = b.call("get_status")
    check(r["result"]["rx"]["control_state"] == "owned_by_other",
          "§17.3", "non-owner sees control_state owned_by_other")

    r = a.call("get_status")
    check(r["result"]["rx"]["control_state"] == "owned_by_me",
          "§17.3", "owner sees control_state owned_by_me")

    # §14/§17.2: control is released implicitly on loss of the connection.
    a.close()
    time.sleep(0.5)
    r = b.call("get_status")
    check(r["result"]["rx"]["control_state"] == "free",
          "§17.2", "control released implicitly when the owner disconnects")

    r = b.call("open_stream", {"function": "rx", "protocol": "vita49",
                               "vita": {"destination": {"ip": "127.0.0.1",
                                                        "port": 60011}}})
    check(r["status"] == "ok", "§17.2",
          "the channel is claimable again after the owner goes away")
    check(r["result"]["stream_id"] != sid, "§24.1",
          "stream IDs are not reused")
    b.close()


def test_id_correlation():
    section("Response correlation (§9.5)")
    c = CP()
    c.call("hello", HELLO)
    ids = []
    for i in range(6):
        ids.append(c.send("ping", {}, msg_id=f"burst-{i}"))
    got = []
    for _ in range(6):
        m = c._read_frame()
        if m.get("type") == "event":
            continue
        got.append(m["id"])
    check(sorted(got) == sorted(ids), "§9.5",
          "every request in a burst gets its own id echoed back")
    c.close()


def test_release_13():
    section("Release 1.3 alignment (§1.2, §6.2, §10.1, §14.6, §20, §21)")
    c = CP()
    r = c.call("hello", HELLO)
    fw = r["result"].get("firmware_version", "")
    check(fw.startswith("sdr-slc-rtld/"), "§10.1",
          f"firmware_version is <implementation>/<version> ({fw})")
    check("spec_release" not in r["result"], "§10.1",
          "no spec_release in hello (it was never a defined field)")
    check("auth" not in r["result"], "§26.2",
          "hello carries no auth object when no function requires it")

    # §10.1: release numbers are not protocol versions, and the refusal
    # says so.
    d = CP()
    r = d.call("hello", {"client_name": "x", "client_version": "1",
                         "supported_protocol_versions": ["1.3"]})
    check(r["status"] == "error" and
          r["error"]["code"] == "unsupported_protocol_version",
          "§10.1", "offering only a release number is refused")
    check("revision" in r["error"]["message"].lower() or
          "release" in r["error"]["message"].lower(),
          "§10.1", "and the message explains why")
    d.close()

    caps = c.call("get_capabilities")["result"]["capabilities"]
    check(caps["schema_version"] == 2, "§11.2", "schema_version stays 2")
    check(caps["rx"].get("auth") == "none", "§26.1",
          "rx.auth is none by default (a receiver has no reason for it)")
    check("shares_sample_clock_with" not in caps["rx"], "§12.4",
          "no shares_sample_clock_with on a single-block device")
    check("turnaround_mode" not in caps and "full_duplex" not in caps,
          "§1.2", "no transmitter-only fields at the top level")

    # §18.3 (A1): six standard monitor channels, number or null.
    hm = c.call("get_status")["result"]["rx"]["hardware_monitors"]
    for ch in ("pa_temperature_celsius", "pa_supply_voltage_v", "pa_drain_current_ma",
               "board_temperature_celsius", "supply_voltage_v", "total_current_ma"):
        check(ch in hm and (hm[ch] is None or isinstance(hm[ch], (int, float))),
              "§18.3", f"hardware_monitors.{ch} present ({hm.get(ch)})")
    check(all(hm[k] is None for k in ("pa_temperature_celsius", "pa_supply_voltage_v",
                                       "pa_drain_current_ma")), "§18.3",
          "pa_* channels are null on a receiver")
    check(set(hm) <= {"pa_temperature_celsius", "pa_supply_voltage_v",
                      "pa_drain_current_ma", "board_temperature_celsius",
                      "supply_voltage_v", "total_current_ma"}, "§18.3",
          "no unprefixed non-standard channels")

    # §11.3 addendum: the u8 zero point is declared, not assumed.
    u8 = [f for f in caps["rx"]["iq_formats"] if f["encoding"] == "unsigned"]
    check(u8 and abs(u8[0].get("zero_point", 0) - 127.4) < 1e-6, "§11.3",
          f"u8 iq_format declares zero_point 127.4 (got {u8[0].get('zero_point') if u8 else None})")

    # §20: honest host clock, not null.
    r = c.call("get_clock_status")["result"]
    check(r["source"] == "free_running" and r["synced"] is False, "§20",
          "source free_running, synced false")
    check(isinstance(r["epoch_s"], int) and abs(r["epoch_s"] - time.time()) < 5,
          "§20", "epoch_s is the host clock, within 5 s of the test host")
    check(isinstance(r["utc_time"], str) and r["utc_time"].endswith("Z"),
          "§20", "utc_time is an ISO-8601 UTC string")

    # §14.6, §5.1: open_stream result shape.
    r = c.call("open_stream", {
        "function": "rx", "protocol": "vita49",
        "vita": {"protocol_version": "VITA-A/1.0",
                 "destination": {"ip": "127.0.0.1", "port": UDP_PORT}}})
    res = r["result"]
    sid = res["stream_id"]
    check(res.get("signal_type") in ("complex_iq", "real_i_branch",
                                     "real_q_branch"),
          "§14.6", f"signal_type in open_stream result ({res.get('signal_type')})")
    check(res["vita"].get("protocol_version") == "VITA-A/1.0", "§14.3",
          "agreed protocol_version echoed")
    check(res["vita"].get("timestamp_mode") == "utc_picoseconds", "§5.1",
          "timestamp_mode is utc_picoseconds")
    check(abs(res["vita"]["format"].get("zero_point", 0) - 127.4) < 1e-6, "§11.3",
          "open_stream echoes zero_point in vita.format")

    # §21.2: stream_state carries reason.
    c.events.clear()
    c.call("start_rx", {"stream_id": sid})
    c.drain_events(0.3)
    st = [e for e in c.events if e["event"] == "stream_state"]
    check(st and st[-1]["params"].get("reason") == "client_request", "§21.2",
          "stream_state on start_rx has reason client_request")
    c.events.clear()
    c.call("stop_rx", {"stream_id": sid})
    c.drain_events(0.3)
    st = [e for e in c.events if e["event"] == "stream_state"]
    check(st and st[-1]["params"].get("streaming") is False and
          st[-1]["params"].get("reason") == "client_request", "§21.2",
          "stream_state on stop_rx has reason client_request")

    # §14.8: close while active stops first, with a stream_state.
    c.call("start_rx", {"stream_id": sid})
    c.drain_events(0.2)
    c.events.clear()
    r = c.call("close_stream", {"stream_id": sid})
    c.drain_events(0.3)
    st = [e for e in c.events if e["event"] == "stream_state"]
    check(r["status"] == "ok" and st and
          st[-1]["params"].get("streaming") is False, "§14.8",
          "close_stream on an active stream stops it and says so")

    # §14.3: a protocol_version outside transports[] is refused.
    r = c.call("open_stream", {
        "function": "rx", "protocol": "vita49",
        "vita": {"protocol_version": "VITA-A/1.0-TX",
                 "destination": {"ip": "127.0.0.1", "port": UDP_PORT}}})
    check(r["status"] == "error" and
          r["error"]["code"] == "unsupported_stream_protocol", "§14.3",
          "unadvertised protocol_version is unsupported_stream_protocol")
    r = c.call("get_status")
    check(r["result"]["rx"]["control_state"] == "free", "§17.2",
          "a refused open_stream does not leave the channel claimed")

    # §21.2 control_lost: an observer sees the owner's stream stop when the
    # owner's connection drops mid-stream.
    o = CP(); o.call("hello", HELLO)
    r = c.call("open_stream", {
        "function": "rx", "protocol": "vita49",
        "vita": {"destination": {"ip": "127.0.0.1", "port": UDP_PORT}}})
    sid = r["result"]["stream_id"]
    c.call("start_rx", {"stream_id": sid})
    o.drain_events(0.2)
    o.events.clear()
    c.close()
    o.drain_events(0.6)
    st = [e for e in o.events if e["event"] == "stream_state"]
    check(st and st[-1]["params"].get("streaming") is False and
          st[-1]["params"].get("reason") == "control_lost", "§21.2",
          "observer receives stream_state reason control_lost when the "
          "owner drops")
    check("control_revoked" in o.event_names(), "§21.2",
          "and control_revoked, since the channel is free again")
    o.close()

    # §24.1 / §22: not_supported vs unknown_command for the tx set.
    e = CP(); e.call("hello", HELLO)
    for cmd in ("start_tx", "stop_tx", "set_tx_power", "get_vswr",
                "auto_tune", "get_tx_capabilities", "get_timestamps"):
        r = e.call(cmd, {"function": "tx"})
        check(r["status"] == "error" and r["error"]["code"] == "unknown_command",
              "§24.1", f"{cmd} is unknown_command on a receive-only device")
    e.close()


def test_zero_point_option():
    section("u8 zero point is configurable (--u8-zero-point)")
    d = slc.spawn(slc.binary_from_argv(), PORT + 20, extra=["--u8-zero-point", "127.35"])
    try:
        c = slc.CP(PORT + 20)
        c.call("hello", HELLO)
        caps = c.call("get_capabilities")["result"]["capabilities"]
        u8 = [f for f in caps["rx"]["iq_formats"] if f["encoding"] == "unsigned"][0]
        check(abs(u8["zero_point"] - 127.35) < 1e-6, "§11.3",
              "configured zero_point appears in capabilities")
        r = c.call("open_stream", {"function": "rx", "protocol": "vita49",
                                   "vita": {"destination": {"ip": "127.0.0.1", "port": UDP_PORT}}})
        check(abs(r["result"]["vita"]["format"]["zero_point"] - 127.35) < 1e-6, "§11.3",
              "and in the open_stream echo")
        c.close()
    finally:
        slc.stop(d)


def main():
    print("SDR-SLC-CP-1.0 Release 1.3 conformance checks (rx Tier 2)")
    daemon = slc.spawn(slc.binary_from_argv(), PORT)
    try:
        test_envelope()
        test_no_tx_commands()
        caps = test_capabilities()
        test_function_addressing(caps)
        test_controls()
        test_stream(caps)
        test_exclusive_access()
        test_id_correlation()
        test_release_13()
        test_zero_point_option()
    except Exception as exc:  # noqa: BLE001
        print(f"\nABORTED: {exc!r}")
        FAILURES.append(f"exception: {exc!r}")
    finally:
        slc.stop(daemon)

    print(f"\n{CHECKS - len(FAILURES)}/{CHECKS} checks passed")
    if FAILURES:
        print("\nFailures:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
