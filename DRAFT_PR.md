# perf(ws): skip serializeInfo() on broadcast updates to save heap

**Forgejo**: Fixes #23

## Summary

WebSocket broadcast updates currently serialize both `state` and `info` JSON blocks to every connected client on every state change. The `info` block is ~4-6KB of mostly-static data (version string, MAC address, WiFi details, filesystem stats) that clients only need on initial connect. Subsequent updates are state-only — this matches what the WLED app actually consumes.

On constrained devices like the ESP32-PICO-D4 (520KB SRAM, no PSRAM), this unnecessary serialization burns heap on every brightness change, effect switch, or palette update.

## What changed

In `sendDataWs()`, the `serializeInfo()` call is now guarded by `if (client)`:

- **`client != nullptr`** (initial connect response): full `state` + `info` JSON, unchanged behaviour.
- **`client == nullptr`** (broadcast to all connected clients): state-only JSON, saving 4-6KB of heap allocation per broadcast.

This is a 3-line functional change in `wled00/ws.cpp`.

## Impact

- **Heap saved**: ~4-6KB per WebSocket broadcast (measured on ESP32-PICO-D4 with WiFi+PPP active).
- **No UI regression**: the WLED app and web UI request info on connect and don't rely on broadcast info updates.
- **Backwards compatible**: clients that do need periodic info can still call `/json/info` or reconnect.

## Testing

- Verified on M5StickC (ESP32-PICO-D4, 4MB flash, no PSRAM) running WiFi+PPP dual-stack.
- Confirmed WLED app receives full state+info on connect, state-only on subsequent updates.
- Heap headroom improved from ~40KB to ~46KB during active WebSocket streaming.