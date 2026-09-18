# sdr-slc-rtld

SDR-SLC bridge for an RTL-SDR dongle: mDNS discovery, the JSON control plane
over TCP, VITA-49 IQ over UDP. One function block, `rx`, at Tier 2.
Reference implementation named by SDR-SLC-CP-1.0 Release 1.3 and
SDR-SLC-VITA-1.0 Release 1.2. Its sibling, `sdr-slc-bladerfd`, drives a
bladeRF 2.0 micro and shares this daemon's `common/` code, configuration
layout and command-line conventions.

This is example / reference code, published to help you build your own
SDR-SLC device: do as you like with it. It is MIT-licensed (see LICENSE).
By Ivo van Ling (PA2IX).

## Build

    sudo apt install build-essential pkg-config librtlsdr-dev
    make
    sudo make install        # binary, systemd unit, /etc/sdr-slc/rtld.conf

Discovery uses the daemon's own mDNS responder by default and needs nothing
else. To publish through avahi-daemon instead (what sdr-slc-bladerfd does):

    sudo apt install libavahi-client-dev
    make MDNS=avahi          # both backends compiled in
    SLC_ARGS=-v --mdns avahi # in rtld.conf

## Run

    sdr-slc-rtld -v                       # normal tuner mode, 500 kHz - 1766 MHz
    sdr-slc-rtld -v --direct-q            # HF via direct sampling, 100 kHz - 30 MHz
    sdr-slc-rtld --help                   # every option

As a service, arguments live in `/etc/sdr-slc/rtld.conf`:

    SLC_ARGS=-v --direct-q
    sudo systemctl enable --now sdr-slc-rtld

Control plane on tcp/4620, VITA-49 from udp/4621, both settable
(`--port`, `--rx-port`). The instance name `SLC-XXYYZZ` derives from a MAC
address and is the identity the client stores the radio under; `--name`
overrides it.

## Authentication (optional)

A receiver does not require it and the default is off. To lock the rx
function behind a pre-shared key (SDR-SLC-CP-1.0 §26):

    sudo sdr-slc-rtld keygen              # prints key_id + psk once
    SLC_ARGS=-v --rx-auth psk             # in rtld.conf, then restart

Keys are one `<key_id> <psk>` per line in `/etc/sdr-slc/rtld.keys`; delete
a line to revoke that client.

## Test

    make check        # builds the no-dongle stub, runs all suites

    test/test_slc_cp_r13.py    control plane vs SDR-SLC-CP-1.0 R1.3, rx Tier 2
    test/test_auth.py          §26 authentication and keygen
    test/test_vita_context.py  Context and IF Data packets on the wire, R1.2
    test/test_mdns.py          builtin responder: announce, browse, resolve, goodbye

Every check names the clause it tests. The stub (`test/rtlsdr_stub.c`)
models the tuner's discrete gain table and the clock divider that keeps the
RTL2832U from reaching every rate exactly, so `actual_sample_rate_sps` is
exercised.

## Layout

    main.c          arguments, keygen dispatch, startup, mDNS TXT
    config.h        version/release numbers, ports, hardware tables,
                    Context packet constants, calibration constant
    cp/             control plane: server, sessions (§17 ownership, §21
                    events, §26 gating), command handlers
    vita/           IF Data + Context packet sender, callback-driven
    rtl/            librtlsdr integration, ADC level, Reference Level
    common/         auth, log, mdns (dispatcher + builtin responder +
                    optional Avahi backend) - shared with sdr-slc-bladerfd
    json/           jsmn parser, small builder
    test/           suites and the librtlsdr stub
    docs/           capabilities-rtl.json, status-rtl.json (reference)

## Calibration

The u8 stream's zero point (the ADC code that is zero signal) is declared
in `iq_formats[].zero_point`, 127.4 by default; `--u8-zero-point` sets it
for a dongle that measures differently (mean of the raw stream with the
input terminated). The client subtracts it; assuming 128 leaves a DC spike.

`reference_level_dbm` is nominal: `RTL_REF_LEVEL_DBM_AT_0DB` in `config.h`,
-10 dBm for a stock R820T2, ±8 dB advertised. Characterise your dongle
against a signal generator and edit that one number.


## License

MIT — see [`LICENSE`](LICENSE). In short: example / reference code, do as you
like with it, keep the copyright notice. (c) 2026 Ivo van Ling (PA2IX).

Bundled and external components keep their own licences:

- `json/jsmn.*` — jsmn tokeniser, (c) Serge Zaitsev (MIT), embedded verbatim.
- **librtlsdr is GPL-2.0-or-later.** You may license this daemon's own source
  under MIT, but a binary built by linking librtlsdr is a combined work covered
  by the GPL, so anyone distributing compiled binaries must do so under GPL-2.0
  terms. Publishing this source under MIT is unaffected. (Summary, not legal advice.)
- Avahi (optional mDNS backend) is LGPL; linking it does not affect the licence.
