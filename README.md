<p align="center">
  <img src="https://raw.githubusercontent.com/pa2ix/sdr-slc/main/docs/images/SDR_SLC_Icon_V2_no_spectrum.png" width="320">
</p>

# sdr-slc-rtld

**SDR-SLC RTL-SDR Daemon** — a Linux daemon that bridges an RTL-SDR dongle to the SDR-SLC protocol suite.

It implements the server side of the SDR-SLC stack:

- **SLC-DISC** — mDNS/DNS-SD service announcement, so clients discover the device automatically
- **SLC-CP** — JSON/TCP control plane for capabilities, tuning, gain, and stream lifecycle
- **VITA-SLC** — VITA-49 UDP I/Q stream transport, including Context packets

It is the reference RTL-SDR implementation of the `rx` function block at **Tier 2**, conformant to **SDR-SLC-CP-1.0 Release 1.3** and **SDR-SLC-VITA-1.0 Release 1.2**. Its sibling `sdr-slc-bladerfd` drives a Nuand bladeRF and shares this daemon's `common/` code, configuration layout, and command-line conventions.

This is example / reference code, published to help you build your own SDR-SLC device: do as you like with it. It is MIT-licensed (see LICENSE). By Ivo van Ling (PA2IX).

---

## Hardware

| Hardware | Notes |
|---|---|
| **RTL-SDR Blog V3 / V4** | Fully supported. Bias tee requires V3 or later. |
| Any RTL2832U + R820T/R820T2 dongle | Supported. Bias tee returns `not_supported` if unavailable in librtlsdr. |
| Raspberry Pi 3B / 3B+ / 4 / 5 | Tested. Pi 4 recommended for sustained 2 MSPS streaming. |
| Any Linux host with USB 2.0 | Should work. Tested on Raspberry Pi OS (Bookworm, 64-bit). |

> **Ethernet strongly recommended.** WiFi with Power Save Mode enabled causes packet bursts and audio gaps. Disable PSM with `sudo iwconfig wlan0 power off` if you must use WiFi.

---

## Prerequisites

### librtlsdr

The daemon links **librtlsdr**. The **RTL-SDR Blog fork** is recommended, as it adds bias-tee and RTL-SDR V4 support; the Debian/Ubuntu package (`librtlsdr-dev`) works but may lack bias-tee support depending on version.

**Install the fork from source (recommended):**

```bash
sudo apt install git cmake build-essential libusb-1.0-0-dev pkg-config
git clone https://github.com/rtlsdrblog/rtl-sdr-blog.git
cd rtl-sdr-blog
mkdir build && cd build
cmake .. -DINSTALL_UDEV_RULES=ON
make -j$(nproc)
sudo make install
sudo ldconfig
```

Verify the dongle is detected:

```bash
rtl_test -t
```

> **Note on licensing:** librtlsdr is GPL-2.0-or-later. You may license this daemon's own source under MIT (as it is), but a *binary* built by linking librtlsdr is a combined work covered by the GPL. See the License section.

### USB permissions (udev rule)

To run without root, add a udev rule:

```bash
sudo tee /etc/udev/rules.d/20-rtlsdr.rules << 'EOF'
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", GROUP="plugdev", MODE="0664"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2832", GROUP="plugdev", MODE="0664"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG plugdev $USER
```

Log out and back in for the group change to take effect.

### Blacklist the DVB kernel module

The kernel DVB driver claims the dongle before librtlsdr can. Blacklist it permanently:

```bash
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtlsdr.conf
sudo modprobe -r dvb_usb_rtl28xxu 2>/dev/null || true
```

### Build tools

```bash
sudo apt install build-essential pkg-config librtlsdr-dev
```

GCC 7 or later is required (C11 + `stdatomic.h`); GCC 10+ is recommended.

---

## Building

```bash
git clone https://github.com/pa2ix/sdr-slc-rtld.git
cd sdr-slc-rtld
make
sudo make install        # installs the binary, the systemd unit, and /etc/sdr-slc/rtld.conf
```

Discovery uses the daemon's own built-in mDNS responder by default and needs nothing else. To publish through `avahi-daemon` instead (what `sdr-slc-bladerfd` does):

```bash
sudo apt install libavahi-client-dev
make MDNS=avahi          # both backends compiled in; select at runtime with --mdns avahi
```

---

## Running

### Basic usage

```bash
sdr-slc-rtld -v
```

The daemon opens the RTL-SDR dongle, announces itself via mDNS, and listens for control-plane connections on TCP port 4620. VITA-49 I/Q is sent from UDP port 4621.

### Command-line options

```
  -p, --port PORT          control plane TCP port (default 4620)
      --rx-port PORT       VITA49 RX source UDP port (default 4621)
  -i, --iface IFACE        interface whose MAC derives the SLC-XXYYZZ name
      --name NAME          override the mDNS instance / device_name
      --rx-auth MODE       "none" (default) or "psk" — require §26 auth for rx
      --keys-file PATH     pre-shared keys, one "<key_id> <psk>" per line
      --normal             normal tuner path, 500 kHz - 1766 MHz (default)
      --direct-i           direct sampling, I-branch, 100 kHz - 30 MHz
      --direct-q           direct sampling, Q-branch, 100 kHz - 30 MHz
      --bias-tee           power the bias tee at startup
      --bias-tee-persist   also re-apply the startup bias tee value after re-open
      --agc                enable hardware AGC at startup (default: off)
      --ppm N              PPM frequency correction (default: 0)
      --gain DB            initial RF gain in dB (default: 19.7)
      --u8-zero-point V    ADC code that is zero signal (default: 127.4)
      --no-context-packets do not emit VITA-A/1.0-CTX packets
      --mdns BACKEND       "builtin" (default) or "avahi" (needs an MDNS=avahi build)
      --no-mdns            do not publish over mDNS
      --syslog             log to syslog instead of stderr
  -v, --verbose            repeatable: info -> debug -> trace
  -h, --help               full option list

  sdr-slc-rtld keygen [--key-id ID] [--keys-file PATH]   # generate a pre-shared key
```

### Examples

```bash
# Wideband VHF/UHF on eth0 with a custom name
sdr-slc-rtld -i eth0 --name "Shack Dongle" -v

# HF via direct sampling (Q-branch) with PPM correction
sdr-slc-rtld -i eth0 --direct-q --ppm -14 -v

# Bias tee on for an external LNA, fixed gain
sdr-slc-rtld --bias-tee --gain 15 -v
```

### Logging

`-v` is repeatable: `-v` for info, `-vv` for debug, `-vvv` for trace. Use `--syslog` to log to syslog instead of stderr (useful under systemd).

---

## Authentication (optional)

A receiver does not require authentication, and the default is off. To lock the `rx` function behind a pre-shared key (SDR-SLC-CP-1.0 §26):

```bash
sudo sdr-slc-rtld keygen              # prints key_id + psk once
```

Then enable it (in `/etc/sdr-slc/rtld.conf`, or on the command line) and restart:

```
SLC_ARGS=-v --rx-auth psk
```

Keys live one `<key_id> <psk>` per line in `/etc/sdr-slc/rtld.keys`; delete a line to revoke that client. Enter the printed pair in the client. Ten failed attempts in a minute close the connection.

---

## Running as a systemd service

`make install` installs the unit. Put your arguments in `/etc/sdr-slc/rtld.conf`:

```
SLC_ARGS=-v --direct-q
```

```bash
sudo systemctl enable --now sdr-slc-rtld
sudo journalctl -u sdr-slc-rtld -f
```

---

## What the daemon implements

### SLC-DISC — Service Discovery

The daemon announces itself as `_sdr-slc._tcp.local.` on the local network — using its own mDNS responder by default, or `avahi-daemon` in an `MDNS=avahi` build with `--mdns avahi`. Browse for it with:

```bash
avahi-browse -rt _sdr-slc._tcp
```

The TXT record carries:

| Field | Example | Description |
|---|---|---|
| `name` | `Shack-Dongle` | Device name (from `--name` or MAC-derived) |
| `proto` | `sdr-slc` | Protocol identifier |
| `proto_ver` | `1.0` | Protocol version |
| `functions` | `rx` | Function blocks offered (always `rx` here) |
| `hw` | `RTL-SDR-v4` | Hardware identifier |
| `fw` | `sdr-slc-rtld/1.0` | `<implementation>/<version>` — matches `hello`'s `firmware_version` |
| `minhz` / `maxhz` | `500000` / `1766000000` | Frequency range for the current mode |

The instance name `SLC-XXYYZZ` derives from the interface MAC and is the identity the client stores the radio under; `--name` overrides it.

### SLC-CP — Control Plane (rx Tier 2)

| Command | Description |
|---|---|
| `hello` | Protocol version negotiation. Supports version 1.0. |
| `ping` | Liveness check. |
| `get_capabilities` | Full capability report: frequency range, sample rates, preferred rate/gain, and all controls with typed descriptors. |
| `get_status` | Current hardware state: frequency, sample rate, streaming status, control values. |
| `set_frequency` | Tune the centre frequency. Returns the actual applied frequency. |
| `set_sample_rate` | Set a sample rate from the supported list. Rejected while streaming. |
| `set_control` / `get_control` | Set/read a named hardware control (see below). |
| `open_stream` / `close_stream` | Configure / release a VITA-49 RX stream session. |
| `start_rx` / `stop_rx` | Begin / halt streaming. `start_rx` emits a Context packet, then fires the `stream_state` event. |
| `get_clock_status` | Returns `free_running` — RTL-SDR has no PTPv2 or GPS clock. |
| `get_location` | Returns no-fix — no GPS module. |
| `authenticate` | §26 PSK exchange, when `--rx-auth psk` is enabled. |
| All TX commands | Return `not_supported`. |

**Hardware controls:**

| Control | Type | Range | Description |
|---|---|---|---|
| `rf_gain` | float | 0 – 49.6 dB | RF gain, snapped to the nearest hardware step. Ignored while AGC is active. |
| `agc` | boolean | true / false | Hardware AGC. Overrides `rf_gain` when active. |
| `bias_tee` | boolean | true / false | +5 V on the SMA connector for an external LNA. V3/V4 only. |
| `ppm_correction` | integer | −100 to +100 | Oscillator frequency correction in PPM. |
| `direct_sampling` | enum | normal / direct-i / direct-q | Sampling-mode selector (also set at startup with `--normal` / `--direct-i` / `--direct-q`). |

**Frequency ranges by sampling mode:**

| Mode | Flag | Range |
|---|---|---|
| Normal tuner | `--normal` | 500 kHz – 1766 MHz |
| Direct sampling I | `--direct-i` | 100 kHz – 30 MHz |
| Direct sampling Q | `--direct-q` | 100 kHz – 30 MHz |

**Sample rates:** 250000, 1024000, 1536000, 1800000, 2048000, 2400000 SPS. **Preferred:** 2048000 SPS.

### VITA-SLC — I/Q Stream Transport

The daemon implements the VITA-A/1.0 constrained profile:

- **IF Data packets** — VITA-49 type 0x1, Class ID present, TSI = UTC seconds, TSF = picoseconds. Payload: interleaved big-endian int16 IQ.
- **Context packets** — VITA-49 type 0x4, emitted once before the first IF Data packet per `start_rx`, carrying the Payload Format field (signal type: `complex_iq` in normal mode, `real` in direct sampling). Disable with `--no-context-packets`.
- **Packet size:** 256 IQ pairs per packet. This exactly divides the librtlsdr default callback buffer (512 packets per callback) with no remainder.
- **Transmission:** all packets from one librtlsdr callback are sent in a single `sendmmsg()` — one kernel transition, no inter-packet gaps.
- **Timestamps:** derived from sample count plus a wall-clock anchor captured at `start_rx`; no `time()` calls in the hot path.

### IQ format

The RTL-SDR delivers 8-bit unsigned offset-binary samples (u8), converted to int16 for VITA-49:

```
int16 = (sample − zero_point) / 128.0 × 32767
```

`zero_point` defaults to 127.4 (matching RTL-TCP; eliminates the RTL2832U DC spike). Override per-dongle with `--u8-zero-point` — the mean of the raw stream with the input terminated.

---

## Client-side UDP buffer

The daemon sends a burst of VITA-49 packets per callback. The default macOS UDP receive buffer (196 KB) is too small to absorb this; set a larger receive buffer in your client before binding the UDP socket:

```
SO_RCVBUF = 8 × 1024 × 1024   (8 MB)
```

On macOS, also raise the system limits if needed:

```bash
sudo sysctl -w net.inet.udp.recvspace=8388608
sudo sysctl -w kern.ipc.maxsockbuf=16777216
```

On Linux:

```bash
sudo sysctl -w net.core.rmem_max=8388608
```

---

## Calibration

`reference_level_dbm` is nominal: characterise your dongle against a signal generator and edit the reference-level constant in `config.h` (about −10 dBm for a stock R820T2, ±8 dB advertised). The u8 zero point is `--u8-zero-point` as described above.

---

## Testing

```bash
make check        # builds the no-dongle stub and runs every suite
```

| Suite | Covers |
|---|---|
| `test/test_slc_cp_r13.py` | control plane vs SDR-SLC-CP-1.0 Release 1.3, rx Tier 2 |
| `test/test_auth.py` | §26 authentication and keygen |
| `test/test_vita_context.py` | Context and IF Data packets on the wire, Release 1.2 |
| `test/test_mdns.py` | built-in responder: announce, browse, resolve, goodbye |

The stub (`test/rtlsdr_stub.c`) models the tuner's discrete gain table and the clock divider, so no dongle is needed to run the suites.

---

## Source tree

```
sdr-slc-rtld/
├── main.c          arguments, keygen dispatch, startup, mDNS TXT
├── config.h        ports, hardware tables, Context constants, calibration
├── Makefile
├── common/         auth, log, mdns (dispatcher + builtin responder +
│                   optional Avahi backend) — shared with sdr-slc-bladerfd
├── cp/             control plane: server, sessions, command handlers
├── vita/           VITA-A/1.0 IF Data + Context packet builder and sender
├── rtl/            librtlsdr integration, hardware control, ADC level
├── json/           jsmn tokeniser (embedded) + stack-allocated builder
├── test/           conformance suites and the librtlsdr stub
└── docs/           capabilities-rtl.json, status-rtl.json (reference)
```

---

## Protocol suite

The full SDR-SLC specification documents are published separately:
[https://github.com/pa2ix/sdr-slc](https://github.com/pa2ix/sdr-slc)

---

## Known limitations

- **RX only.** TX commands return `not_supported`.
- **One active stream.** Multiple control-plane connections are accepted, but only one may stream at a time.
- **Free-running clock.** VITA-49 timestamps come from wall clock + sample count, not PTPv2 or GPS; treat them as relative.
- **Optional PSK auth, no TLS.** The control plane can require a §26 pre-shared key but is not encrypted. Suitable for trusted local networks only — do not expose port 4620 to the internet.
- **Direct sampling** halves usable bandwidth (real-valued signal, mirrored spectrum).

---

## License

MIT — see [`LICENSE`](LICENSE). In short: example / reference code, do as you like with it, keep the copyright notice. (c) 2026 Ivo van Ling (PA2IX).

Bundled and external components keep their own licences:

- `json/jsmn.*` — jsmn tokeniser, (c) Serge Zaitsev (MIT), embedded verbatim.
- **librtlsdr is GPL-2.0-or-later.** You may license this daemon's own source under MIT, but a binary built by linking librtlsdr is a combined work covered by the GPL, so anyone distributing compiled binaries must do so under GPL-2.0 terms. Publishing this source under MIT is unaffected. (Summary, not legal advice.)
- Avahi (optional mDNS backend) is LGPL; linking it does not affect the licence.

---

## Author

Ivo van Ling, PA2IX
IXChange BV
[https://pa2ix.vanling.net](https://pa2ix.vanling.net)
