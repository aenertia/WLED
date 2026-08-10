# feat: PPP-over-serial network transport (`WLED_USE_PPP`)

Fixes #NNN (link to the feature request issue)

## Background

WLED's network abstraction (`WLEDNetworkClass`) supports WiFi and Ethernet via `esp_netif`. This PR adds PPP as a third transport option — standard IP networking over a serial UART link. The host runs `pppd`, the ESP32 acts as PPP server, and all WLED network services (HTTP, WebSocket, DDP, E1.31, mDNS) work transparently over the tunnel.

Follows the established `WLED_USE_ETHERNET` pattern: `isPPP()` alongside `isEthernet()`, purely additive guards, zero WiFi code changes. WiFi and PPP coexist — both `esp_netif` interfaces are active simultaneously when both are configured.

Use case: USB-tethered WLED devices (PC ARGB controllers, wired installations, kiosks) where WiFi is unavailable, unreliable, or undesirable.

## Changes

1. **`wled_ppp.cpp` / `wled_ppp.h` (new)**: PPP transport driver
   - ESP-IDF `esp_netif` PPP in server mode via `ppp_listen()`
   - UART RX task (8KB stack, pinned to core 0, 1ms poll interval)
   - IPCP static address assignment (169.254.7.1 / 169.254.7.2, configurable)
   - Reconnection via `IP_EVENT_PPP_LOST_IP` (the only reliable reconnect signal — `PPPERR_CONNECT` returns early in ESP-IDF's `on_ppp_status_changed()`, skipping event delivery)
   - Conditional `Serial.end()` only when `PPP_UART_NUM == UART_NUM_0`

2. **`Network.cpp` / `Network.h`**: Network abstraction extension
   - Add `isPPP()` — returns `ppp_connected` volatile flag
   - Extend `isConnected()`: `WiFi || Ethernet || isPPP()`
   - Extend `localIP()`, `subnetMask()`, `gatewayIP()` for PPP netif

3. **`wled.cpp` / `wled.h`**: Startup integration
   - `initPPP()` called after WiFi init (not instead of — coexistence)
   - `handleConnection()` checks PPP link alongside WiFi
   - `serialCanRX`/`serialCanTX` blocked when PPP owns UART0
   - Include `wled_ppp.h` behind `#ifdef WLED_USE_PPP`

4. **`cfg.cpp`**: PPP config persistence in `/cfg.json`

5. **`json.cpp`**: PPP connection status in `/json/info`

## Overhead

| When `WLED_USE_PPP` undefined | When defined + PPP active |
|-------------------------------|---------------------------|
| Flash: +0 | Flash: +20 KB |
| Heap: +0 | Heap: +16 KB |
| CPU: +0% | CPU: +5-10% (core 0 only) |
| LED impact: none | LED impact: none (core 1) |

## Testing

- **Compiled**: ESP32 (`m5stickc_ppp`), verified `esp32dev` stock build unaffected
- **Hardware tested**: M5StickC (ESP32-PICO-D4, FTDI FT232) at 1.5Mbps
- **Verified**: pppd LCP+IPCP negotiation, ping 4.9ms, web dashboard, WebSocket, DDP, JSON API, mDNS `wled.local`, Home Assistant native integration, auto-reconnection on timeout
- **Coexistence**: `isPPP()` guards compile cleanly alongside WiFi path; both netifs register in lwIP without conflict

## Build Configuration (platformio_override.ini example)

```ini
[env:my_ppp_board]
extends = esp32
build_flags = ${common.build_flags} ${esp32.build_flags}
  -D WLED_USE_PPP
  -D PPP_BAUD=1500000
  ; Optional: use UART1 instead of UART0
  ; -D PPP_UART_NUM=UART_NUM_1
  ; -D PPP_TX_PIN=32
  ; -D PPP_RX_PIN=33
custom_sdkconfig =
  CONFIG_LWIP_PPP_SUPPORT=y
  CONFIG_LWIP_PPP_SERVER_SUPPORT=y
```

## ESP-IDF PPP Learnings (for future contributors)

Documenting a few non-obvious behaviors discovered during implementation:

- `PPPERR_CONNECT` case in `on_ppp_status_changed()` returns early, skipping `NETIF_PPP_STATUS` event posting — use `IP_EVENT_PPP_LOST_IP` for reconnection instead
- `PPP_NOTIFY_PHASE` is not compiled in pioarduino pre-built libs — phase events don't fire
- `ppp_listen()` without `ppp_set_silent()` actively sends LCP ConfReq (same behavior as `ppp_connect()`)
- `uart_read_bytes()` blocks for full timeout when buffer > available data — use 1ms timeout, not 100ms
- PPP HDLC deframer handles garbage bytes — no `uart_flush_input()` needed before PPP start
