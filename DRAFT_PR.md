# PPP-over-Serial Network Transport (WLED_USE_PPP)

## Summary

Adds PPP-over-serial as a network transport alongside WiFi. The ESP32 acts as a PPP server on UART at 1.5Mbaud; the host runs `pppd` and gets a link-local address (169.254.7.2). WLED responds on 169.254.7.1.

Everything WLED supports over WiFi works identically over PPP: HTTP API, DDP, mDNS, OTA. PPP is purely additive — WiFi keeps working.

## Changes

| File | Description |
|------|-------------|
| `wled_ppp.cpp` | PPP engine: UART init, esp_netif PPP driver, RX task, link management |
| `wled_ppp.h` | Public API: `initPPP()`, `ppp_connected`, `ppp_netif_uart` |
| `Network.cpp` | PPP-aware `localIP()`, `isConnected()`, `isPPP()` |
| `Network.h` | `isPPP()` declaration |
| `wled.cpp` | PPP+WiFi coexistence in `setup()`, PPP handling in `handleConnection()`, mDNS skip under `WLED_PPP_WIFI` |
| `wled.h` | PPP includes, debug counters |

## Build Flags

```
-D WLED_USE_PPP           # Enable PPP transport
-D WLED_USE_PPP_UART      # Use UART for PPP (vs BLE)
-D WLED_PPP_WIFI          # PPP+WiFi coexistence mode
-D PPP_UART_NUM=0         # UART number (0 = UART0)
-D PPP_BAUD=1500000       # Baud rate
```

## Operating Modes

### PPP+WiFi (`WLED_PPP_WIFI`)
Both transports active. PPP inits first (before WiFi driver), WiFi STA starts only if credentials are saved. mDNS disabled to avoid crash in `igmp_leavegroup_netif`.

### PPP-Exclusive (`WLED_USE_PPP` without `WLED_PPP_WIFI`)
No WiFi at all. Manual `esp_netif_init()` + `esp_event_loop_create_default()` bootstrap since Arduino's WiFi stack isn't initialized.

## Host-Side Setup

```bash
sudo pppd /dev/ttyUSB0 1500000 noauth nodetach local nocrtscts \
  novj nodeflate nobsdcomp noaccomp nopcomp lcp-echo-interval 0 \
  mru 1500 mtu 1500 169.254.7.2:169.254.7.1 &
```

## Testing

- M5StickC (ESP32-PICO-D4) at 1.5Mbaud
- 10+ minute soak tests with DDP streaming, 0 drops
- HTTP API, WebSocket, OTA all verified over PPP link

## Notes

- Build flag is opt-in; no impact on standard WLED builds
- PPP RX task pinned to Core 1 to avoid `lock_tcpip_core` priority inversion
- `esp_netif_init()` called by `initPPP()` before WiFi to prevent lazy-init race
