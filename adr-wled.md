# ADR: WLED USB PC ARGB Controller (M5StickC)

**Status**: Draft (Rev 2 — PPP-over-serial architecture)
**Date**: 2026-08-08
**Target Hardware**: M5StickC (K016-C, ESP32-PICO-D4)
**Upstream**: WLED v16.0.1 (https://github.com/wled/WLED)
**Forgejo**: https://git.awa.3d.ae.net.nz/aenertia/wled-pccontroller
**Build Host**: koero (crossbuild distrobox or PlatformIO native)

## 1. Context & Motivation

PC ARGB (addressable RGB) lighting is controlled by motherboard headers or dedicated USB controllers (Corsair Commander Pro, NZXT HUE 2, Razer Chroma). All commercial solutions use proprietary USB HID protocols and closed firmware. Open-source alternatives exist on WiFi (WLED over E1.31/DDP) but there is no good **USB-tethered ARGB controller** that combines WLED's mature effects engine, web dashboard, and full protocol stack with direct PC connection over USB.

**The gap**: WLED runs on ESP32 with a full effects engine (200+ effects), web dashboard, WebSocket live updates, segment control, JSON API, and realtime pixel protocols (DDP, E1.31, Art-Net). But it only works over WiFi. We want the **entire WLED experience over USB** — web dashboard in a browser, OpenRGB pixel streaming via DDP, JSON API via HTTP — by tunneling IP over the USB serial link using PPP.

**Core insight**: Rather than reimplementing WLED's HTTP/WebSocket/DDP interfaces as custom serial protocols, we run PPP (Point-to-Point Protocol) over the USB UART. The host PC sees a standard network interface (`ppp0`). WLED's stock web server, WebSocket handler, and DDP/E1.31 UDP receivers all work transparently — zero custom protocol code. TCP for control (HTTP, WebSocket), UDP for signalling (DDP pixel data, E1.31).

**Why M5StickC**: Compact form factor (48×24×14mm), built-in USB (FTDI FT232 at 1.5 Mbps), built-in 80×160 LCD for status display, two buttons for local control, 95mAh battery for graceful shutdown, Grove + HAT connectors for LED data output. The ESP32-PICO-D4 SoC has 8 RMT channels for WS2812B driving and dual cores for concurrent PPP + LED output.

## 2. Goals

1. **Full WLED over USB** — web dashboard, WebSocket, JSON API, DDP/E1.31 realtime — all via PPP-over-serial, no WiFi required
2. **OpenRGB compatibility via DDP** — OpenRGB's preferred WLED protocol, now over the PPP tunnel instead of WiFi
3. **Zero custom serial protocols** — no Adalight byte parsing, no custom JSON framing. All traffic is standard IP (TCP + UDP) over PPP
4. **Complete WLED web dashboard** — browse to `http://10.0.0.1` from the host PC, get the full effects UI, color picker, segments, presets
5. **WLED effects engine preserved** — all 200+ effects, segments, palettes, presets run locally when not in DDP realtime mode
6. **Minimal hardware** — M5StickC + level shifter + standard 3-pin ARGB connectors, powered by external 5V
7. **Upstream-rebasing fork** — thin patch set following WLED's existing Ethernet (`WLED_USE_ETHERNET`) pattern

## 3. Hardware Platform

### M5StickC (K016-C) Key Specs

| Parameter | Value |
|---|---|
| SoC | ESP32-PICO-D4 (dual-core LX6, 240MHz) |
| Flash | 4 MB (in-package SiP) |
| PSRAM | **None** |
| SRAM | 520 KB (~200-280 KB free heap) |
| USB chip | **FTDI FT232** (not CP2104, not CH9102) |
| Max baud | **1,500,000** (1.5 Mbps) |
| Display | ST7735S 80×160 TFT (SPI: G15, G13, G23, G18, G5) |
| Buttons | A=G37, B/Power=G39 |
| IMU/PMU | MPU6886+AXP192 on I2C (G21/G22) |
| Battery | 95 mAh LiPo |

### Available GPIOs for LED Data

| GPIO | Connector | RMT-capable | Recommended |
|---|---|---|---|
| **G26** | HAT header | Yes | **Primary LED data output** |
| G32 | Grove (Yellow) | Yes | Secondary channel |
| G33 | Grove (White) | Yes | Secondary channel |
| G36 | HAT header | **No (input-only)** | Cannot use |
| G0 | HAT header | Yes but **strapping pin** | **Avoid** |

### Wiring Architecture

```
M5StickC HAT Header          74AHCT125             LED Strip / ARGB Fan
+----------------+          +------------+         +------------------+
| G26 (3.3V) ----+--------->| 1A -> 1Y   |-------->| DIN (5V logic)   |
| GND -----------+--------->| GND        |-------->| GND              |
| 5V  -----------+---> VCC  | VCC (5V) <-+--- 5V  | 5V <------------ | External
+----------------+          +------------+         +------------------+  5V PSU
```

**Critical**: M5StickC outputs 3.3V logic. WS2812B V_IH = 3.5V at VDD=5V. A **74AHCT125 level shifter is mandatory** for reliable operation. The HCT family accepts 3.3V as logic high (threshold ~1.4V).

**Power**: External 5V PSU powers the LED strip directly. M5StickC's USB 5V (500mA total input via AXP192) cannot source meaningful LED current. Share only GND between M5StickC and LED PSU.

### Throughput Budget (PPP over 1.5 Mbps Serial)

PPP HDLC overhead: ~8% (framing, FCS, ACCM byte escaping). Effective: **~1.38 Mbps / 172 KB/s**.

| Traffic Type | Size | Frequency | Bandwidth | % of 172 KB/s |
|---|---|---|---|---|
| Dashboard initial load | ~55 KB gzipped | Once | Burst: ~0.32s | — |
| WebSocket state updates | ~3 KB JSON | Max 1/sec | 3 KB/s | 1.7% |
| WS live LED preview (300 LEDs) | ~900 B | 25 fps | 22.5 KB/s | 13% |
| DDP pixel data (300 LEDs, 30fps) | ~928 B | 30 fps | 27.8 KB/s | 16% |
| **Combined (all active)** | | | **~53.3 KB/s** | **31%** |

69% headroom remaining. **Serial is not the bottleneck.** WS2812B refresh at 300 LEDs takes ~9ms (30us/LED), capping real fps at ~110 regardless of transport speed.

**TCP round-trip note**: At 1.5 Mbps, TCP segment + ACK RTT is ~20ms. Web UI initial load involves ~5-6 HTTP round trips → expect **2-4 seconds** for full dashboard. After that, WebSocket is sub-millisecond. Subsequent visits use ETag 304 caching — near-instant.

## 4. Architecture Decisions

### Decision 1: Transport — PPP-over-Serial (IP Tunnel)

**Chosen**: PPP (Point-to-Point Protocol) over the USB UART, creating a standard IP link between the ESP32 and host PC. All WLED traffic — HTTP, WebSocket, DDP, E1.31 — flows over standard TCP/UDP through the PPP tunnel.

```
┌────────────────────────┐   USB Serial (1.5 Mbps)   ┌─────────────────────────┐
│ M5StickC (ESP32)       │◄══════════════════════════►│ Host PC                 │
│                        │   FTDI FT232               │                         │
│ PPP Server             │   PPP HDLC frames          │ pppd → ppp0 interface   │
│ esp_netif PPP          │                            │ IP: 10.0.0.2            │
│ IP: 10.0.0.1           │                            │                         │
│                        │                            │ Browser → 10.0.0.1      │
│ AsyncWebServer :80     │◄── TCP (HTTP/WebSocket) ──│ OpenRGB → DDP 10.0.0.1  │
│ DDP :4048 (UDP)        │◄── UDP (realtime pixels) ─│ curl → /json/state      │
│ E1.31 :5568 (UDP)      │◄── UDP (realtime pixels) ─│                         │
│ Effects engine         │                            │ Auto-connect via udev + │
│                        │                            │ systemd on USB plug-in  │
│ G26 → 74AHCT125 → LEDs│                            └─────────────────────────┘
└────────────────────────┘
```

**Protocol split**:
- **TCP** for control: HTTP (web UI, JSON API), WebSocket (live state sync) — reliable, ordered
- **UDP** for signalling: DDP (port 4048), E1.31/sACN (port 5568) — low-latency pixel streaming, dropped frame beats late frame

**Why PPP, not SLIP**: SLIP was removed from ESP-IDF `esp_netif` core in v5.1 (Aug 2022). PPP is first-class in ESP-IDF, battle-tested in every ESP32 cellular project, has LCP/IPCP auto-negotiation, FCS error detection, VJ header compression, and works natively on Linux (`pppd`), macOS (`pppd`), and Windows (built-in DUN). See [`refs/ppp-serial-tunnel.md`](refs/ppp-serial-tunnel.md).

**Why not custom serial protocols**: Tunneling IP eliminates ALL custom protocol work. No Adalight byte parsing, no serial JSON framing, no baud rate switching, no serial buffer management. WLED's stock HTTP server, WebSocket handler, and DDP/E1.31 receivers all work unmodified.

**Rejected alternatives**:
- **Adalight over serial** (original ADR Rev 1): Works for pixel data but loses web dashboard, WebSocket, and requires custom JSON serial framing for API access. DDP over PPP is strictly superior.
- **SLIP**: Dead in ESP-IDF. No `esp_netif` integration since v5.1. No Windows support since XP.
- **CDC-ECM (USB Ethernet)**: Ideal but requires native USB (ESP32-S2/S3). M5StickC uses FTDI UART bridge. Candidate for v2 hardware.
- **Corsair Lighting Protocol**: Requires USB HID, impossible through FTDI UART bridge.

### Decision 2: WiFi Stack — Replaced by PPP (Not Removed)

**Chosen**: Don't call `WiFi.begin()`. Create a PPP netif instead. Keep the HTTP server, WebSocket, mDNS, and all network services alive — they bind to `INADDR_ANY` via lwIP and work on any `esp_netif` interface.

**Key discovery**: WLED already supports non-WiFi networking via `WLED_USE_ETHERNET`. The `WLEDNetworkClass` abstracts transport:

```cpp
// Existing code in Network.cpp
bool WLEDNetworkClass::isConnected() {
    return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED)
        || isEthernet();   // ← Already supports non-WiFi!
}
```

**Our change follows the identical pattern** — add `isPPP()` alongside `isEthernet()`:

```cpp
bool WLEDNetworkClass::isConnected() {
    return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED)
        || isEthernet()
        || isPPP();        // ← Our addition
}
```

ESPAsyncWebServer binds to `0.0.0.0` via lwIP's `tcp_bind(INADDR_ANY)` — **zero WiFi dependency** at the socket level. It serves on any `esp_netif` interface that has an IP. Verified in source: `AsyncServer::AsyncServer(port)` → `tcp_bind(_pcb, &local_addr, _port)`.

**Build flags still set** (memory savings for unused features):
```
-D WLED_USE_PPP                  ; Our new flag
-D WLED_DISABLE_ALEXA
-D WLED_DISABLE_HUESYNC
-D WLED_DISABLE_MQTT
-D WLED_DISABLE_OTA
-D WLED_DISABLE_ESPNOW
-D WLED_DISABLE_INFRARED
-D WLED_DISABLE_LOXONE
-D WLED_DISABLE_ADALIGHT         ; Safe to disable — DDP replaces Adalight
-D WLED_DISABLE_2D
-D WLED_DISABLE_BROWNOUT_DET
```

**Note**: `-D WLED_DISABLE_ADALIGHT` is now safe because we're NOT using Adalight serial. All pixel data goes via DDP/E1.31 UDP over PPP. This disables `handleSerial()` entirely, which is correct — PPP owns the UART.

### Decision 3: PPP Netif Implementation

**Chosen**: Use ESP-IDF's native `esp_netif` PPP API in server mode (`ppp_listen()`).

**Kconfig requirements** (in `sdkconfig.defaults`):
```
CONFIG_LWIP_PPP_SUPPORT=y
CONFIG_LWIP_PPP_SERVER_SUPPORT=y
```

**ESP32 acts as PPP server** (assigns IPs):
- ESP32 IP: `10.0.0.1`
- Host IP: `10.0.0.2`
- `ppp_passive = true` → waits for host `pppd` to connect

**UART setup**: UART0 at 1,500,000 baud, 8N1, no flow control. PPP RX task feeds `esp_netif_receive()`. Transmit callback calls `uart_write_bytes()`.

**PPP initialization timing**: LCP/IPCP negotiation takes 1-3 seconds. WLED's `server.begin()` is called after `interfacesInited` transitions to true (existing code at `wled.cpp:931`). The `IP_EVENT_PPP_GOT_IP` event handler sets `interfacesInited = true`, matching the Ethernet pattern.

### Decision 4: Usermod vs Core Patch

**Chosen**: Hybrid. Core patches follow the Ethernet precedent. Usermod for M5StickC hardware.

- **Core patches** (~80 lines, following `WLED_USE_ETHERNET` pattern):
  - `Network.h` / `Network.cpp`: Add `isPPP()`, extend `isConnected()`, `localIP()`, `subnetMask()`
  - `wled.cpp`: PPP netif creation in `WLED::setup()`, skip WiFi init when `WLED_USE_PPP`
  - `wled.h`: `WLED_USE_PPP` flag, PPP netif handle global

- **Usermod** (`usermods/m5stickc_ppp/`):
  - PPP UART setup (baud rate, pin config, RX task)
  - M5StickC display: IP address, effect name, brightness, FPS, PPP link status
  - Button A (G37): cycle effects or brightness
  - AXP192 battery monitoring and clean shutdown
  - Optional: Button B (G39) long-press for config reset

### Decision 5: LED Output Configuration

**Chosen**: Single channel on G26 (HAT header), configurable via build defaults.

```ini
build_flags =
  -D DATA_PINS=26
  -D DEFAULT_LED_COUNT=60
  -D DEFAULT_LED_TYPE=TYPE_WS2812_RGB
```

**Rationale**: G26 is the best available GPIO — RMT-capable, no conflicts with internal peripherals, physically accessible on the HAT header. G32/G33 (Grove port) are reserved as optional secondary channels for multi-channel builds.

**Multi-channel future**: WLED supports multiple buses. A PCB adapter could break out G26 + G32 + G33 as three independent ARGB channels, each driving up to ~200 LEDs.

## 5. OpenRGB Integration

### How It Works — DDP over PPP

OpenRGB's preferred WLED protocol is **DDP** (Distributed Display Protocol) over UDP. With PPP, this works exactly as it does over WiFi — OpenRGB targets the ESP32's IP address:

1. Flash firmware on M5StickC, connect USB
2. Host auto-connects via `pppd` (systemd/udev, see §6)
3. Configure OpenRGB `OpenRGB.json`:

```json
{
    "DDPDevices": {
        "devices": [{
            "name": "WLED M5StickC USB",
            "ip": "10.0.0.1",
            "port": 4048,
            "num_leds": 60
        }]
    }
}
```

4. OpenRGB sends DDP UDP packets → PPP tunnel → WLED's stock `handleDDPPacket()` → LEDs update

**Also supported** (no config changes, WLED stock):
- **E1.31/sACN** (UDP port 5568) — configure in OpenRGB's E1.31 settings
- **Art-Net** (UDP port 6454) — if preferred

### Web Dashboard Access

Browse to `http://10.0.0.1` — full WLED dashboard with effects picker, color wheel, segments, presets, settings. WebSocket live preview works.

### JSON API Access

```bash
# Query state
curl http://10.0.0.1/json/state

# Set brightness
curl -X POST http://10.0.0.1/json -d '{"bri":128}'

# List effects
curl http://10.0.0.1/json/eff

# List palettes
curl http://10.0.0.1/json/pal
```

Standard HTTP — any tool, any language, no custom serial framing.

## 6. Build & Host Environment

### PlatformIO Environment

File: `platformio_override.ini`

```ini
[env:m5stickc_ppp]
extends = esp32_idf_V5
board = m5stick-c
platform = ${esp32_idf_V5.platform}
board_build.partitions = tools/WLED_ESP32_4MB_256KB_FS.csv
build_flags = ${common.build_flags} ${esp32_idf_V5.build_flags}
  -D WLED_RELEASE_NAME=\"M5StickC_PPP\"
  ; --- PPP-over-serial mode ---
  -D WLED_USE_PPP
  -D PPP_BAUD=1500000
  -D PPP_OUR_IP=\"10.0.0.1\"
  -D PPP_THEIR_IP=\"10.0.0.2\"
  ; --- Disable unused modules ---
  -D WLED_DISABLE_ALEXA
  -D WLED_DISABLE_HUESYNC
  -D WLED_DISABLE_INFRARED
  -D WLED_DISABLE_MQTT
  -D WLED_DISABLE_OTA
  -D WLED_DISABLE_ESPNOW
  -D WLED_DISABLE_LOXONE
  -D WLED_DISABLE_ADALIGHT       ; DDP replaces Adalight — PPP owns UART
  -D WLED_DISABLE_2D
  -D WLED_DISABLE_BROWNOUT_DET
  ; --- Hardware ---
  -D DATA_PINS=26
  -D DEFAULT_LED_COUNT=60
  -D DEFAULT_LED_TYPE=TYPE_WS2812_RGB
  ; --- M5StickC display/buttons ---
  -D M5STICKC_PPP_CONTROLLER
  -DARDUINO_USB_CDC_ON_BOOT=0
  ; --- ESP-IDF PPP support ---
  -D CONFIG_LWIP_PPP_SUPPORT=1
  -D CONFIG_LWIP_PPP_SERVER_SUPPORT=1
lib_deps = ${esp32_idf_V5.lib_deps}
  m5stack/M5StickC@^0.2.9
custom_usermods = m5stickc_ppp
monitor_speed = 1500000
```

### Build on koero

```bash
# Option A: PlatformIO on host
ssh koero
cd ~/workspace/wled-pccontroller
pio run -e m5stickc_ppp

# Option B: distrobox crossbuild
ssh koero
distrobox enter crossbuild-openaeos
cd /work/wled-pccontroller
pio run -e m5stickc_ppp

# Flash via USB (disconnect pppd first)
sudo systemctl stop wled-ppp@ttyUSB0.service
pio run -e m5stickc_ppp --target upload --upload-port /dev/ttyUSB0
```

PlatformIO and ESP-IDF toolchains install into koero's workspace NVMe (`/var/mnt/koero/workspace/buildcache/`) via the `XDG_CACHE_HOME` redirect.

### Host-Side PPP Setup

**Linux (one-shot)**:
```bash
sudo pppd /dev/ttyUSB0 1500000 noauth local nocrtscts nodetach \
    10.0.0.2:10.0.0.1
# Then: http://10.0.0.1 in browser
```

**Linux (auto-connect on USB plug-in)**:

```ini
# /etc/udev/rules.d/99-wled-ppp.rules
ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", \
    ATTRS{idProduct}=="6001", SYMLINK+="wled-serial", \
    TAG+="systemd", ENV{SYSTEMD_WANTS}="wled-ppp@%k.service"
# Ignore ModemManager for this device
ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", \
    ATTRS{idProduct}=="6001", ENV{ID_MM_DEVICE_IGNORE}="1"
```

```ini
# /etc/systemd/system/wled-ppp@.service
[Unit]
Description=PPP link to WLED ESP32 on /dev/%i
After=dev-%i.device
BindsTo=dev-%i.device

[Service]
Type=simple
ExecStart=/usr/sbin/pppd /dev/%i 1500000 noauth local nocrtscts \
    10.0.0.2:10.0.0.1 persist maxfail 0 holdoff 3
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Plug in M5StickC → udev creates symlink + starts pppd → `ppp0` interface up → browse to `http://10.0.0.1`. Unplug → pppd stops, interface gone.

**macOS**: `sudo pppd /dev/tty.usbserial-* 1500000 noauth local nocrtscts nodetach`

**Windows**: WSL2 with `pppd`, or create a Direct Cable Connection (DUN) via Network Settings.

## 7. Repository Strategy

### Fork Structure

```
wled/WLED (upstream)
  └── aenertia/wled-pccontroller (forgejo fork)
       ├── main                    ← tracks upstream main
       ├── ppp-controller          ← our patches (rebases on main)
       └── releases/m5stickc-v*    ← tagged release builds
```

### Patch Set (minimal diff from upstream — follows Ethernet pattern)

| File | Change | Lines |
|---|---|---|
| `wled00/src/dependencies/network/Network.h` | Add `isPPP()` declaration | ~3 |
| `wled00/src/dependencies/network/Network.cpp` | Implement `isPPP()`, extend `isConnected()`, `localIP()` | ~30 |
| `wled00/wled.cpp` | PPP netif creation in `setup()`, skip WiFi init when `WLED_USE_PPP` | ~40 |
| `wled00/wled.h` | `WLED_USE_PPP` flag, PPP netif handle | ~5 |
| `platformio_override.ini` | M5StickC PPP board environment | new file |
| `usermods/m5stickc_ppp/` | UART PPP setup, display, buttons, PMU | new directory |

Target: **~80 lines of core changes**, rest in usermod. Follows the exact same pattern as `WLED_USE_ETHERNET`. Rebasable on upstream `main` with minimal conflicts because it's additive (`|| isPPP()` alongside `|| isEthernet()`).

## 8. Implementation Phases

### Phase 1: Bootstrap — Stock WLED on M5StickC

1. Clone upstream WLED v16.0.1, push to Forgejo `aenertia/wled-pccontroller`
2. Create `platformio_override.ini` with M5StickC board environment (WiFi still enabled)
3. Verify stock WLED builds and flashes for M5StickC
4. Confirm web dashboard works over WiFi (baseline — proves hardware + build chain work)
5. Confirm LED output on G26 with a WS2812B strip via level shifter

**Exit criteria**: M5StickC running stock WLED, web dashboard accessible over WiFi, LEDs driven on G26.

### Phase 2: PPP Netif — IP-over-Serial

1. Enable `CONFIG_LWIP_PPP_SUPPORT` and `CONFIG_LWIP_PPP_SERVER_SUPPORT` in sdkconfig
2. Implement PPP netif creation in WLED `setup()` — UART0 at 1.5 Mbps, server mode, static IPs
3. Add `isPPP()` to `WLEDNetworkClass` (following `isEthernet()` pattern)
4. Extend `isConnected()`, `localIP()`, `subnetMask()` to include PPP interface
5. Hook `IP_EVENT_PPP_GOT_IP` to trigger `interfacesInited = true`
6. Disable WiFi init when `WLED_USE_PPP` is set
7. Test: `sudo pppd /dev/ttyUSB0 1500000 noauth local nocrtscts nodetach` → browse `http://10.0.0.1`

**Exit criteria**: WLED web dashboard accessible over PPP serial link. WebSocket live updates work. JSON API responds to `curl`.

### Phase 3: DDP Pixel Streaming over PPP

1. Configure OpenRGB with DDP device at `10.0.0.1:4048`
2. Verify realtime pixel control from OpenRGB → DDP UDP → PPP → WLED → LEDs
3. Measure latency and throughput — target <20ms input-to-photon for 60 LEDs
4. Test E1.31/sACN as alternative protocol
5. Verify WLED effects engine runs locally when DDP is not active (automatic — `realtimeMode` idle)

**Exit criteria**: OpenRGB controls LEDs via DDP over PPP. WLED effects run when OpenRGB is disconnected.

### Phase 4: M5StickC Usermod

1. Implement display usermod (IP address, PPP link status, effect name, brightness, FPS)
2. Button A (G37): short press = next effect, long press = brightness cycle
3. AXP192 battery monitoring, low-battery warning on display
4. Optional: Button B (G39) long-press for config reset

**Exit criteria**: Display shows useful status, buttons provide local control without PC.

### Phase 5: Host Integration & Polish

1. Write udev rule + systemd template unit for auto-connect on USB plug-in
2. Write README with wiring diagram, `pppd` setup, OpenRGB DDP config
3. Build and test on physical ARGB fans (Corsair HD120, generic WS2812B strip)
4. Test with level shifter and without (document reliability difference)
5. Create tagged release with pre-built `.bin` for direct flash
6. Test on Linux (koero/z20) and macOS hosts

**Exit criteria**: Plug-and-play — USB plug-in → auto pppd → browser dashboard + OpenRGB DDP. End-to-end working.

## 9. What PPP Eliminates

Everything from the original serial-only design (ADR Rev 1) that is now unnecessary:

| Was Needed (Rev 1) | Now Replaced By | Notes |
|---|---|---|
| Custom serial JSON API (`req` field routing) | `curl http://10.0.0.1/json` | Stock WLED HTTP API |
| Adalight serial reception | DDP over UDP over PPP | Stock WLED, OpenRGB preferred method |
| TPM2 serial reception | E1.31 over UDP over PPP | Stock WLED |
| Serial baud rate switching (`0xB0-0xB7`) | Fixed 1.5 Mbps PPP link | No runtime switching needed |
| Serial RX buffer management | PPP + lwIP handle framing | Kernel-level buffering |
| `wled-serial-cli` companion tool | `curl` / any HTTP client | Standard HTTP tools |
| Custom serial multiplexer | IP handles multiplexing | TCP + UDP coexist natively |
| `serialCanRX`/`serialCanTX` bypass | Not applicable | PPP owns the UART, no serial handler |
| `WLED_DISABLE_WIFI` ifdef surgery | `WLED_USE_PPP` (additive, like Ethernet) | Follow existing pattern |

**Net effect**: Zero custom serial protocols. The firmware change is purely additive — a new transport option alongside WiFi and Ethernet.

## 10. Future: v2 Hardware (ESP32-S3 + CDC-ECM)

The M5StickC's FTDI UART bridge limits us to PPP (IP-over-serial). The ideal v2 uses an **ESP32-S3** with native USB-OTG for **CDC-ECM** (USB Ethernet):

| Aspect | v1 (M5StickC + PPP) | v2 (ESP32-S3 + CDC-ECM) |
|---|---|---|
| Transport | PPP over UART (1.5 Mbps) | USB Ethernet (12 Mbps Full Speed) |
| Host setup | `pppd` + udev rule | Zero — OS sees USB Ethernet adapter |
| Dashboard load | 2-4 seconds | <0.5 seconds |
| Driver | FTDI VCP + PPP | Built-in CDC-ECM (Linux/macOS native) |
| Windows | Needs `pppd` or WSL2 | RNDIS auto-detected |
| Candidate boards | — | M5AtomS3, M5Stamp S3, generic ESP32-S3 DevKit |

**The firmware architecture is identical** — `esp_netif` with AsyncWebServer on `INADDR_ANY`. Only the transport layer changes (PPP netif → USB-ECM netif). Same `WLEDNetworkClass` extension pattern.

## 11. Risk Assessment

| Risk | Impact | Mitigation |
|---|---|---|
| PPP init before HTTP server bind | Medium | Use `IP_EVENT_PPP_GOT_IP` event to trigger `interfacesInited` (Ethernet pattern) |
| WLED WiFi status checks gate functionality | Medium | Follow `isEthernet()` precedent — already proven in WLED codebase |
| 3.3V logic unreliable without level shifter | Medium | Document as mandatory; 74AHCT125 in wiring diagram |
| PPP negotiation latency (1-3s at startup) | Low | Acceptable — device is tethered, not latency-critical at boot |
| ModemManager steals serial port | Low | udev rule: `ENV{ID_MM_DEVICE_IGNORE}="1"` |
| Upstream WLED refactors break patches | Low | Additive changes (`|| isPPP()`) have minimal conflict surface |
| TCP RTT over serial (web UI not instant) | Low | 2-4s initial load, then WebSocket; ETag caching on revisits |
| M5StickC discontinued | Low | Firmware portable to any ESP32 board; v2 targets ESP32-S3 |

## 12. References

Detailed reference documents in `refs/`:

| File | Contents |
|---|---|
| [`refs/m5stickc-hardware.md`](refs/m5stickc-hardware.md) | GPIO pinout, FTDI FT232, RMT channels, power limits, 3.3V/5V analysis |
| [`refs/wled-architecture.md`](refs/wled-architecture.md) | WLED source map, serial handler, JSON API keys, build flags, Ethernet pattern |
| [`refs/openrgb-serial-protocols.md`](refs/openrgb-serial-protocols.md) | Adalight/TPM2 byte formats, OpenRGB DDP config, protocol feasibility matrix |
| [`refs/pc-argb-ecosystem.md`](refs/pc-argb-ecosystem.md) | ARGB connectors, WS2812B specs, commercial controllers, power architecture |
| [`refs/ppp-serial-tunnel.md`](refs/ppp-serial-tunnel.md) | PPP vs SLIP, ESP-IDF PPP API, eppp_link, host pppd setup, throughput analysis |

### External References

| Resource | URL |
|---|---|
| WLED Repository | https://github.com/wled/WLED |
| WLED JSON API Docs | https://kno.wled.ge/interfaces/json-api/ |
| WLED DDP Interface | https://kno.wled.ge/interfaces/ddp/ |
| WLED Compiling Guide | https://kno.wled.ge/advanced/compiling-wled/ |
| WLED Ethernet Support | https://kno.wled.ge/advanced/ethernet-compatible/ |
| ESP-IDF PPP API | https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif.html |
| Espressif eppp_link | https://github.com/espressif/esp-protocols/tree/master/components/eppp_link |
| M5StickC Product Page | https://docs.m5stack.com/en/core/m5stickc |
| M5StickC Schematic | https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/schematic/Core/M5StickC/20191118__StickC_A04_3110_Schematic_Rebuild_PinMap.pdf |
| ESP32 RMT Docs | https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/rmt.html |
| OpenRGB Project | https://openrgb.org / https://gitlab.com/CalcProgrammer1/OpenRGB |
| OpenRGB DDP Config | See `DDPDevices` in OpenRGB settings |
| WLED Compatible Hardware | https://kno.wled.ge/basics/compatible-hardware/ |
| WLED Compatible LED Strips | https://kno.wled.ge/basics/compatible-led-strips/ |
