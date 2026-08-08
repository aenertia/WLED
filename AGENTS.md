# WLED USB PC ARGB Controller — AI Agent Context

## Project

Fork of [WLED v16.0.1](https://github.com/wled/WLED) adding PPP-over-serial transport for USB-tethered PC ARGB control. The M5StickC (ESP32-PICO-D4) acts as a USB ARGB controller — plug in, browse to `http://wled.local`, control LEDs.

**Forgejo**: https://git.awa.3d.ae.net.nz/aenertia/wled
**ADR**: `adr-wled.md` — full architecture decisions
**Reference docs**: `refs/` — hardware specs, protocol analysis, PPP tunnel design

## Architecture (PPP-over-Serial)

```
M5StickC (ESP32)  ←— USB Serial (PPP, 1.5Mbps) —→  Host PC (pppd)
  IP: 169.254.7.1                                     IP: 169.254.7.2
  mDNS: wled.local                                     browse wled.local
  HTTP :80 (dashboard)                                 OpenRGB → DDP :4048
  DDP :4048 (pixel data)                               curl → /json/state
  G26 → 74AHCT125 → WS2812B LEDs
```

- **TCP** for control: HTTP web UI, WebSocket live sync, JSON API
- **UDP** for signalling: DDP pixel data (OpenRGB), E1.31/sACN
- **Zero custom serial protocols** — all traffic is standard IP over PPP
- Follows WLED's existing `WLED_USE_ETHERNET` pattern (`isPPP()` alongside `isEthernet()`)

## Build Environment

**Build host**: koero (172.16.1.124, RHEL 10.2, HPE DL360 Gen10)
**ALL builds on koero NVMe** — never build on NFS.

| Path | Purpose |
|---|---|
| `/var/mnt/koero/workspace/wled/` | Repo clone (NVMe) |
| `/var/mnt/koero/workspace/buildcache/platformio/` | PlatformIO core + toolchains |
| `/var/mnt/koero/workspace/buildcache/npm/` | Node.js package cache |
| `/tmp` | 16G tmpfs (transient build temps) |
| `/var/tmp` | NVMe-backed (bind to workspace) |

**Toolchain**:
- PlatformIO Core 6.1.19 (`~/.local/bin/pio`)
- PLATFORMIO_CORE_DIR → NVMe (set in `~/.bash_profile`)
- Node.js v22.23.1, npm 10.9.8 (needed for WLED web UI build via `build_ui.py`)
- Framework: Migrating to Arduino-ESP32 v3.1.10 (IDF v5.3.4) — Tasmota platform 2026.02.30
- Toolchain: xtensa-esp32 13.2 (IDF v5.3.4 default)

**Build command**:
```bash
ssh koero "bash -l -c 'cd /var/mnt/koero/workspace/wled && pio run -e m5stickc'"
```

## Hardware: M5StickC (K016-C)

| Param | Value |
|---|---|
| SoC | ESP32-PICO-D4 (dual-core LX6, 240MHz) |
| Flash | 4MB (no PSRAM) |
| USB | FTDI FT232 (max 1.5Mbps) |
| LED data | G26 (HAT header, RMT-capable) via 74AHCT125 level shifter |
| Button A | G37 (active low) |
| Display | ST7735S 80x160 TFT (SPI) |

## Critical Warnings

- `esp32_idf_V5` exists in upstream main (rebase needed) in this WLED version. Use `esp32_idf_V4`.
- **`platformio_override.ini` is .gitignored** by WLED. Use `git add -f` to commit it.
- **Unset proxy before podman/pip/git on koero**: `unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy`
- **SSH to Forgejo uses port 2222**: `ssh://git@git.awa.3d.ae.net.nz:2222/aenertia/wled.git`
- **3.3V logic → 5V WS2812B**: 74AHCT125 level shifter mandatory. GPIO outputs 3.3V, WS2812B V_IH = 3.5V.
- **ModemManager steals serial ports**: udev rule `ENV{ID_MM_DEVICE_IGNORE}="1"` for FTDI VID 0403.

## Key Files

| File | Purpose |
|---|---|
| `adr-wled.md` | Architecture decision record (Rev 2, PPP) |
| `PLAN-iteration-1.md` | Wave 1 iteration plan |
| `platformio_override.ini` | M5StickC board environment |
| `refs/m5stickc-hardware.md` | GPIO, USB chip, RMT, power specs |
| `refs/wled-architecture.md` | WLED source map, serial handler, JSON API |
| `refs/ppp-serial-tunnel.md` | ESP-IDF PPP API, host pppd setup, throughput |
| `refs/openrgb-serial-protocols.md` | Adalight/DDP/TPM2 protocols, OpenRGB config |
| `refs/pc-argb-ecosystem.md` | ARGB connectors, WS2812B, commercial controllers |

## Host-Side Setup (one-time, for wled.local resolution)

```bash
sudo dnf install -y nss-mdns
sudo sed -i 's/^#allow-point-to-point=no/allow-point-to-point=yes/' /etc/avahi/avahi-daemon.conf
sudo systemctl restart avahi-daemon
```

## Infrastructure

- **Forgejo**: git.awa.3d.ae.net.nz (port 2222 SSH, HTTPS)
- **Primary server**: awa.3d.ae.net.nz (RHEL 10.2, 37 containers, NFS server)
- **Build host**: koero.3d.ae.net.nz (RHEL 10.2, NVMe workspace)
- **Domain**: 3d.ae.net.nz / Kerberos 3D.AE.NET.NZ
- **NFS**: awa exports /vol/nfs_final → koero mounts at /var/mnt/awa
- See `3d-docs` repo for full infrastructure documentation
