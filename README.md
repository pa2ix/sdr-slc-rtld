<p align="center">
  <img src="https://raw.githubusercontent.com/pa2ix/sdr-slc/main/docs/images/SDR_SLC_Icon_V2_no_spectrum.png" width="320">
</p>

# sdr-slc-rtld

**SDR-SLC RTL-SDR Daemon** — a Linux daemon that bridges an RTL-SDR dongle to the SDR-SLC protocol suite.

It implements the full server side of the SDR-SLC stack:

- **SLC-DISC** — mDNS/DNS-SD service announcement so clients discover the device automatically
- **SLC-CP v1.0** — JSON/TCP control plane for capabilities, tuning, gain, and stream lifecycle
- **VITA-SLC** — VITA-49 UDP I/Q stream transport including Context packets

The daemon is the reference implementation of a Tier S(RX) conformant server as defined in SDR-SLC-CP.

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

The daemon requires the **RTL-SDR Blog fork** of librtlsdr, which adds support for bias tee and RTL-SDR V4 hardware. The Debian/Ubuntu package (`librtlsdr-dev`) may work but may lack bias tee support depending on version.

**Install from source (recommended):**

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

**Verify the dongle is detected:**

```bash
rtl_test -t
```

### USB permissions (udev rule)

To run the daemon without root, add a udev rule:

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
sudo apt install gcc make
```

GCC 7 or later is required (C11 + `stdatomic.h`). GCC 10+ is recommended.

---

## Building

```bash
git clone https://github.com/pa2ix/sdr-slc-rtld.git
cd sdr-slc-rtld
make
```

**Debug build** (with AddressSanitizer and UBSan):

```bash
make debug
```

**Install to `/usr/local/bin`:**

```bash
sudo make install
```

---

## Running

### Basic usage

```bash
./sdr-slc-rtld
```

The daemon starts, opens the RTL-SDR dongle, announces itself via mDNS, and listens for SLC-CP connections on TCP port 4620.

### Command-line options

```
./sdr-slc-rtld [options]

Network:
  -i <iface>        Network interface for mDNS announcement (default: eth0)
  --name <name>     Human-readable device name shown in mDNS and hello response
                    Default: SLC-XXYYZZ derived from the interface MAC address

Sampling mode (mutually exclusive; default: --normal):
  --normal          Normal R820T2 tuner path. Range: 500 kHz – 1766 MHz
  --direct-i        Direct sampling via I-branch. Range: 100 kHz – 30 MHz
  --direct-q        Direct sampling via Q-branch. Range: 100 kHz – 30 MHz

Hardware defaults (applied at startup; the client may override via set_control):
  --bias-tee        Enable bias tee on startup (default: off)
                    WARNING: applies +5V DC to the SMA antenna connector
  --agc             Enable hardware AGC on startup (default: off)
  --ppm <n>         Frequency correction in PPM (default: 0)
  --gain <dB>       Initial RF gain in dB (default: 20.0 dB)
```

### Examples

```bash
# Wideband VHF/UHF with custom name
./sdr-slc-rtld -i eth0 --name "Shack Dongle"

# HF direct sampling on I-branch with PPM correction
./sdr-slc-rtld -i eth0 --name "HF Receiver" --direct-i --ppm -14

# VHF with bias tee for an external LNA
./sdr-slc-rtld -i eth0 --bias-tee --gain 15.0

# Multiple dongles on the same machine (use different ports — edit config.h)
./sdr-slc-rtld -i eth0 --name "VHF-UHF"
./sdr-slc-rtld -i eth0 --name "HF-40m" --direct-i
```

### Logging

The daemon logs to stderr. Set `LOG_LEVEL` in `config.h` before building:

```c
#define LOG_LEVEL  LOG_LEVEL_DEBUG   // ERR / WARN / INFO / DEBUG
```

---

## Running as a systemd service

```bash
sudo make install-service
sudo systemctl enable --now sdr-slc-rtld
sudo journalctl -u sdr-slc-rtld -f
```

Edit `/etc/systemd/system/sdr-slc-rtld.service` to add command-line options:

```ini
ExecStart=/usr/local/bin/sdr-slc-rtld -i eth0 --name "Shack Dongle"
```

---

## What the daemon implements

### SLC-DISC — Service Discovery

The daemon announces itself on the local network using raw mDNS multicast (no libavahi dependency). Any SLC-CP client that uses DNS-SD to browse for `_sdr-slc._tcp.local.` will discover the device automatically.

The mDNS TXT record includes:

| Field | Example | Description |
|---|---|---|
| `name` | `Shack-Dongle` | Device name (from `--name` or MAC-derived) |
| `sampling` | `normal` | `normal`, `direct-i`, or `direct-q` |
| `minhz` | `500000` | Minimum frequency for current mode |
| `maxhz` | `1766000000` | Maximum frequency for current mode |
| `role` | `rx` | Always `rx` for this daemon |
| `proto` | `sdr-slc` | Protocol identifier |
| `stream` | `vita-49-slc` | Stream format |

### SLC-CP — Control Plane (Tier S(RX) conformant)

Implemented commands:

| Command | Description |
|---|---|
| `hello` | Protocol version negotiation. Supports protocol version 1.0. |
| `ping` | Liveness check. |
| `get_capabilities` | Full hardware capability report including frequency range, sample rates, preferred rate, preferred gain, and all controls with typed descriptors. |
| `get_status` | Current hardware state: frequency, sample rate, streaming status, all control values. |
| `set_frequency` | Tune the centre frequency. Returns the actual applied frequency (hardware snaps to nearest). |
| `set_sample_rate` | Set sample rate from the supported list. Rejected while streaming. |
| `set_control` | Set a named hardware control. See controls table below. |
| `get_control` | Read the current value of a named control. |
| `open_stream` | Configure a VITA-49 RX stream session. Returns stream ID, source port, format, and `signal_type`. |
| `close_stream` | Release a stream session. |
| `start_rx` | Begin streaming. Emits a VITA-49 Context packet before data flow, then fires the `stream_state` event. |
| `stop_rx` | Halt streaming. Session remains open for restart. |
| `get_clock_status` | Returns `free_running` — RTL-SDR has no PTPv2 or GPS clock. |
| `get_location` | Returns no-fix — no GPS module. |
| All TX commands | Return `not_supported`. |

**Hardware controls:**

| Control | Type | Range | Description |
|---|---|---|---|
| `rf_gain` | float | 0 – ~49.6 dB | RF gain. Snapped to nearest hardware step. Ignored while AGC is active. |
| `agc` | boolean | `true` / `false` | Hardware automatic gain control. Overrides `rf_gain` when active. |
| `bias_tee` | boolean | `true` / `false` | +5V DC on SMA connector for external LNA power. V3/V4 only. |
| `ppm_correction` | integer | −100 to +100 | Oscillator frequency correction in PPM. |

**Frequency ranges by sampling mode:**

| Mode | Flag | Range |
|---|---|---|
| Normal tuner | `--normal` | 500 kHz – 1766 MHz |
| Direct sampling I | `--direct-i` | 100 kHz – 30 MHz |
| Direct sampling Q | `--direct-q` | 100 kHz – 30 MHz |

**Preferred sample rate:** 2048000 SPS (normal mode) / 1024000 SPS (direct sampling).

### VITA-SLC — I/Q Stream Transport

The daemon implements the VITA-A/1.0 constrained profile:

- **IF Data packets** — VITA-49 packet type 0x1, Class ID present, TSI=UTC seconds, TSF=picoseconds. Payload: interleaved int16 big-endian IQ (converted from RTL-SDR native u8 using empirical 127.4 DC offset to eliminate centre-frequency spike).
- **Context packets** — VITA-49 packet type 0x4, emitted once before the first IF Data packet per `start_rx`. Carries the Payload Format field describing the signal type (`complex_iq` in normal mode, `real` in direct sampling mode). Clients that parse Context packets know the signal type from the stream itself without relying on the SLC-CP `signal_type` field.
- **Packet size:** 256 IQ pairs per packet (1052 bytes on wire for int16). This exactly divides the librtlsdr default callback buffer (262144 bytes = 512 packets per callback) with zero remainder.
- **Transmission:** all packets from a single librtlsdr callback are dispatched in a single `sendmmsg()` call — one kernel transition, one softirq cycle, no inter-packet scheduling gaps.
- **Timestamps:** derived from sample count and a wall-clock anchor captured at `start_rx`. No `time()` calls in the hot path.

### IQ format

The RTL-SDR delivers 8-bit unsigned offset-binary samples (u8). The daemon converts these to int16 for VITA-49:

```
int16 = (sample − 127.4) / 128.0 × 32767
```

The 127.4 constant matches RTL-TCP and eliminates the DC spike caused by the RTL2832U ADC hardware bias.

---

## Client-side UDP buffer

The daemon sends 512 VITA-49 packets (~538 KB) in a single burst every 64ms. The default macOS UDP receive buffer (196 KB) is too small to absorb this. Set a larger receive buffer in your client application before binding the UDP socket:

```
SO_RCVBUF = 8 × 1024 × 1024  (8 MB)
```

On macOS, also increase the system-wide limit if needed:

```bash
sudo sysctl -w net.inet.udp.recvspace=8388608
sudo sysctl -w kern.ipc.maxsockbuf=16777216
```

On Linux:

```bash
sudo sysctl -w net.core.rmem_max=8388608
```

---

## Source tree

```
sdr-slc-rtld/
├── main.c               Entry point, signal handling, CLI argument parsing
├── config.h             Compile-time constants, logging macros
├── Makefile
├── disc/
│   ├── disc.c/h         SLC-DISC: raw mDNS/DNS-SD responder (no libavahi)
├── cp/
│   ├── cp_server.c/h    TCP accept loop
│   ├── cp_session.c/h   Per-client session state machine and framing
│   └── cp_commands.c/h  All SLC-CP command handlers
├── vita/
│   ├── vita_tx.c/h      VITA-A/1.0 IF Data + Context packet builder and sender
├── rtl/
│   ├── rtl_bridge.c/h   librtlsdr integration, hardware control
└── json/
    ├── jsmn.c/h         Embedded JSMN tokeniser (zero allocation)
    └── json_builder.c/h Stack-allocated JSON serialiser
```

---

## Protocol suite

The full SDR-SLC specification documents are published in a separate repository:

[https://github.com/pa2ix/sdr-slc](https://github.com/pa2ix/sdr-slc)

---

## Known limitations

- RX only. TX commands return `not_supported`.
- Single client per daemon instance. Multiple simultaneous CP connections are accepted but only one may stream at a time.
- Free-running clock. VITA-49 timestamps are derived from wall clock + sample count, not from PTPv2 or GPS. Clients should treat timestamps as relative.
- No TLS. The control plane has no authentication or encryption. Suitable for trusted local networks only. Do not expose port 4620 to the internet.
- Direct sampling mode reduces usable bandwidth by half (real-valued signal, mirrored spectrum).

---

## License

Source code: MIT License

---

## Author

Ivo van Ling, PA2IX  
IXChange BV  
[https://pa2ix.vanling.net](https://pa2ix.vanling.net)
