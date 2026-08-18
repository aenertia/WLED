# SLIP-over-Serial Transport (WLED_USE_SLIP)

**Forgejo**: Fixes #8

## Summary

Adds SLIP (Serial Line Internet Protocol) as an alternative serial network transport for WLED. SLIP is simpler than PPP — no negotiation, no authentication — but requires static IP configuration on both ends.

## Use Case

Lightweight wired connectivity for ESP32 devices without WiFi, using a direct serial connection to a host computer. Suitable for development, testing, or permanent installations where WiFi is unavailable or undesirable.

## Changes

| File | Description |
|------|-------------|
| `wled_slip.cpp` | SLIP framer: UART init, packet encode/decode, netif integration |
| `wled_slip.h` | Public API: `initSLIP()`, `slip_connected` flag, IP constants |

## Build Flag

```
-D WLED_USE_SLIP
```

## Technical Notes

- ESP-IDF v5.1 removed SLIP from `esp_netif`. This implementation uses a custom SLIP framer over UART, bypassing the removed driver.
- SLIP owns UART0 exclusively — serial debug output is disabled when SLIP is active.
- Static IP addressing: device gets `SLIP_OUR_IP`, host gets `SLIP_THEIR_IP`.
- Mutually exclusive with WiFi (SLIP takes full UART0 ownership).

## Priority

Low — PPP is preferred for new deployments (dynamic IP, link negotiation, error detection). SLIP is provided as a minimal fallback for constrained environments.

## Testing

- Tested on ESP32 with `slattach` on Linux host
- HTTP API, WebSocket, and OTA all functional over SLIP link