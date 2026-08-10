# Feature Request: PPP-over-Serial Transport — Full WLED Over USB/UART

## The Gap

WLED supports two network transports: WiFi and Ethernet. Both require wireless radios or dedicated PHY hardware. There's a third category of deployment — USB-tethered devices — where the ESP32 is physically connected to a host via USB serial (FTDI, CP2104, CH340, or native USB-CDC) and the user wants the **full WLED experience** (web dashboard, WebSocket, DDP, E1.31, mDNS, JSON API) over that wired link.

Today the only serial option is Adalight/TPM2 — pixel-only protocols that give you LED data streaming but lose the entire web stack. You can't browse to a dashboard, you can't use Home Assistant's native WLED integration, you can't run OpenRGB DDP alongside local effects. PPP fills this gap: standard IP networking over the serial link, enabling every WLED network feature to work transparently over USB.

## Use Cases

- **PC ARGB controller** — ESP32 inside a PC case, USB to motherboard, driving addressable fans/strips via WLED with OpenRGB DDP + dashboard access. No WiFi needed or wanted inside a metal case.
- **Kiosk/signage** — wired installation where WiFi is unreliable or prohibited (i.e retail, hospitality, healthcare RF-restricted environments)
- **Art installations** — permanent wired setups where WiFi congestion from 50+ ESP32s on the same channel makes deployment painful
- **Development/testing** — deterministic wired link for CI/automation without WiFi variability

## Proposed Implementation

Add `WLED_USE_PPP` as a build flag following the existing `WLED_USE_ETHERNET` pattern. ESP32 acts as PPP server via ESP-IDF's native `esp_netif` PPP driver, assigns link-local IPs via IPCP. Host connects with standard `pppd` (Linux/macOS) or Windows DUN — zero custom tools.

### Architecture

```
ESP32 (WLED)                              Host PC
  PPP server (esp_netif)                    pppd client
  IP: 169.254.7.1        ← UART →          IP: 169.254.7.2
  mDNS: wled.local                          browse http://wled.local
  HTTP :80 / WS :80                         Home Assistant integration
  DDP :4048 / E1.31 :5568                   OpenRGB pixel streaming
```

### Coexistence with WiFi

PPP runs **alongside** WiFi, not instead of it. Both `esp_netif` interfaces are active simultaneously. PPP is purely additive — `isPPP()` guards alongside `isEthernet()` in `Network.cpp`, `initPPP()` called after WiFi init in `setup()`. When both interfaces have IPs, PPP takes routing priority (`route_prio = 128` > WiFi's `100`).

### Overhead (WiFi + PPP active simultaneously)

| Resource | With PPP | Without PPP | Delta |
|----------|----------|-------------|-------|
| Flash | 1,299 KB | 1,279 KB | **+20 KB** |
| Free heap | ~145 KB | ~160 KB | **-16 KB** |
| CPU (core 0) | 19-32% | 10-20% | **+5-10%** |
| CPU (core 1 — LEDs) | unaffected | — | **0%** |
| LED limits | unchanged | — | **none** |

When `WLED_USE_PPP` is not defined: **zero cost** — all code `#ifdef` gated.

### Files Changed (~367 lines, 9 files)

| File | Change | Lines |
|------|--------|-------|
| `wled_ppp.cpp` (new) | PPP init, UART RX task, esp_netif PPP server, reconnection | +184 |
| `wled_ppp.h` (new) | Config defaults (baud, pins, IPs), public API | +40 |
| `Network.cpp` | Add `isPPP()`, extend `isConnected()`, `localIP()` | +30 |
| `Network.h` | `isPPP()` declaration | +5 |
| `wled.cpp` | `initPPP()` after WiFi init, serial ownership guard | +15 |
| `wled.h` | Include `wled_ppp.h` | +3 |
| `cfg.cpp` | PPP config persistence | +11 |
| `json.cpp` | PPP status in `/json/info` | +7 |

### ESP-IDF PPP Support

ESP-IDF's PPP is first-class infrastructure — it's the foundation of every ESP32 cellular/modem project. `ppp_listen()` server mode, IPCP address assignment, LCP echo keepalives, VJ header compression, automatic reconnection via `IP_EVENT_PPP_LOST_IP`. Battle-tested across millions of deployed devices.

Requires `CONFIG_LWIP_PPP_SUPPORT=y` and `CONFIG_LWIP_PPP_SERVER_SUPPORT=y` in sdkconfig — these are per-environment settings in `platformio_override.ini`, not global changes.

### Serial Port Ownership

When PPP is active on a UART, Adalight/TPM2 serial reception must be disabled on that UART. The implementation sets `serialCanRX = false` / `serialCanTX = false` when `PPP_UART_NUM == UART_NUM_0`. When PPP uses UART1 or UART2, Serial/debug on UART0 is unaffected. This also addresses #5652 (serial RX noise when Adalight is unwanted).

### Host-Side Setup

```bash
# Linux (one-shot)
sudo pppd /dev/ttyUSB0 1500000 noauth local nocrtscts nodetach

# macOS
sudo pppd /dev/tty.usbserial-* 1500000 noauth local nocrtscts nodetach

# Auto-connect via udev + systemd (Linux)
# udev rule triggers systemd template unit on USB plug-in
```

IPCP auto-assigns 169.254.7.2 to the host — no manual IP config needed.

## Related Issues

- #4990 — WiFi Toggle (users want WiFi-less operation)
- #5762 — WLED-AP active on Ethernet (users want wired-only)
- #1382 — Bluetooth transport request (5+ years open — demand for non-WiFi)
- #5652 — Serial RX noise / disable serial (PPP addresses this)

## Related PRs

- #5650 — Independent Ethernet/WiFi IP + primary netif selection (lwIP multi-netif infrastructure)
- #5697 — ESP32-P4 ethernet-only build (WLED_ETHERNET_ONLY_BUILD precedent)

## Working Implementation

I have a working fork with PPP transport verified on M5StickC (ESP32-PICO-D4) over FTDI at 1.5Mbps:
- Ping 4.9ms avg, 0% loss
- Full web dashboard loads and renders
- WebSocket bidirectional (effects, palettes, brightness)
- DDP pixel streaming (OpenRGB → wled.local:4048)
- Home Assistant native integration (HTTP JSON + WebSocket, no MQTT)
- Auto-reconnection on LCP timeout via IP_EVENT_PPP_LOST_IP

Happy to submit a PR if there's interest. The diff is ~367 lines following the Ethernet pattern — purely additive, no WiFi code changes.
