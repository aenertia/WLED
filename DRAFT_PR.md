# PPP-over-Serial Network Transport (WLED_USE_PPP)

**Forgejo**: Fixes #15

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
## Related upstream issues

These upstream issues demonstrate community demand for non-WiFi and serial transport options:

| Issue/PR | Repo | Title | Relevance |
|----------|------|-------|-----------|
| [#4990](https://github.com/wled/WLED/issues/4990) | Aircoookie/WLED | WiFi Toggle — disable WiFi entirely | PPP enables WiFi-free operation; addresses this use case |
| [#5762](https://github.com/wled/WLED/issues/5762) | Aircoookie/WLED | AP still active when Ethernet connected | PPP follows the same dual-transport pattern as Ethernet |
| [#5614](https://github.com/wled/WLED/issues/5614) | Aircoookie/WLED | WLED reboots when no WiFi available | PPP provides a stable transport independent of WiFi |
| [#1382](https://github.com/wled/WLED/issues/1382) | Aircoookie/WLED | ESP32 Bluetooth — non-WiFi demand | Long-standing demand for non-WiFi transports |
| [#5652](https://github.com/wled/WLED/issues/5652) | Aircoookie/WLED | Serial RX noise / Adalight truncation | PPP over serial is more robust than raw serial protocols |
| [#4662](https://github.com/wled/WLED/issues/4662) | Aircoookie/WLED | USB_CDC UART conflict | PPP on UART0 avoids CDC conflicts |
| [#5501](https://github.com/wled/WLED/issues/5501) | Aircoookie/WLED | Adalight fragile on slow links | PPP with proper framing is more reliable |
| [PR #5650](https://github.com/wled/WLED/pull/5650) | Aircoookie/WLED | Ethernet/WiFi IP config (open v17.0) | PPP follows the same dual-transport architecture |
| [PR #5697](https://github.com/wled/WLED/pull/5697) | Aircoookie/WLED | ESP32-P4 Ethernet-only (closed) | Prior art for non-WiFi transport integration |
| [PR #5667](https://github.com/wled/WLED/pull/5667) | Aircoookie/WLED | RTL8201 Ethernet (open v16.1) | Coordinate: PPP and Ethernet share the same netif abstraction |
