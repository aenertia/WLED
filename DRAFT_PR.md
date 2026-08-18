## fix(json): use Content-Length instead of chunked transfer for /json/fxdata and /json/effects

### Problem

Two related issues on slow/serial transports (PPP at 1.5Mbaud):

1. **Truncated /json/fxdata**: `respondModeData()` uses `sendChunked()` with
   `Connection: close`. On slow links, the TCP FIN from `Connection: close`
   races with the final chunk delivery. The client sees FIN before the last
   chunk arrives — truncated JSON. This is fine on fast WiFi where the kernel
   TCP buffer drains before FIN processing, but PPP serialises everything
   through the same UART — the FIN and final data bytes are literally in the
   same serial stream with no out-of-band signalling.

2. **Deadlocked /json/effects**: The existing `/json/effects` path goes through
   `LockedJsonResponse`, which holds the global JSON buffer lock for the entire
   async TCP send. On constrained devices (M5StickC, 520KB SRAM), the async
   send takes long enough that `sendDataWs()` (called from the main loop on
   state changes) blocks waiting for the same lock. Neither can proceed —
   classic lock-ordering deadlock.

### Fix

1. **Two-pass Content-Length for respondModeData()**: New `measureJSONStringElement()`
   pre-computes exact byte count without writing. First pass measures, second pass
   writes with `request->send(contentType, totalLen, callback)`. The TCP stack
   knows the exact payload size, so it won't FIN until all bytes are ACK'd.

2. **New respondModeNames()**: Serves effect names as a streaming JSON array using
   the same two-pass pattern, bypassing `LockedJsonResponse` entirely. The
   `/json/effects` route now calls `respondModeNames()` directly instead of going
   through `serveJson()`'s lock-based path. Zero heap allocation for the response
   body — just a 256-byte stack buffer per chunk callback.

Both functions follow the same pattern as the existing `respondModeData()` —
streaming callback with `writeJSONStringElement()` — so no new architectural
patterns are introduced.

### Files changed

- `wled00/json.cpp` — add `measureJSONStringElement()`, rewrite `respondModeData()`
  to two-pass Content-Length, add `respondModeNames()`, route `/json/effects`
  through `respondModeNames()`
- `wled00/fcn_declare.h` — add `respondModeNames()` declaration

### Testing

Tested on M5StickC (ESP32-PICO-D4, 520KB SRAM) over PPP at 1.5Mbaud:
- `/json/fxdata` responses complete without truncation (verified with `curl -s | python -m json.tool`)
- `/json/effects` no longer deadlocks under concurrent WebSocket state pushes
- WiFi transport unaffected — Content-Length is strictly better than chunked for
  known-length payloads regardless of transport speed

### Related

Part of the PPP transport hardening series. The underlying issue affects any
slow transport, not just PPP — would also manifest on ESP-NOW bridged HTTP
or any future serial-based transport.
