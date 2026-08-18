## fix(mdns): prevent NULL netif crash when WiFi STA disconnects under PPP

**Forgejo**: Fixes #18

### Problem

When running PPP-over-serial alongside WiFi STA, a WiFi disconnect tears
down the STA netif. The mDNS stack (ESPmDNS) is bound to that netif and
gets destroyed with it. If `mdnsStarted` is still `true`, the next call
to `initInterfaces()` skips `MDNS.begin()` — and the subsequent
`MDNS.end()` call crashes with a NULL pointer dereference inside the mDNS
server object.

Observed on M5StickC (ESP32-PICO-D4) running PPP+WiFi simultaneously
under DDP flood load, where WiFi STA drops and reconnects frequently.

### Fix

Set `mdnsStarted = false` in the `WIFI_EVENT_STA_DISCONNECTED` handler.
This ensures `initInterfaces()` calls `MDNS.begin()` on the next connect,
re-initialising mDNS cleanly against the new netif.

One-liner. The variable declaration (`WLED_GLOBAL bool mdnsStarted`) is
added to `wled.h` since upstream doesn't track mDNS init state at all —
it just calls `MDNS.begin()` unconditionally in `initInterfaces()`, which
happens to work when WiFi is the only transport but falls over when PPP
is also present and the netif gets recycled.

### Files changed

- `wled00/network.cpp` — add `mdnsStarted = false;` in `WIFI_EVENT_STA_DISCONNECTED` handler
- `wled00/wled.h` — add `WLED_GLOBAL bool mdnsStarted _INIT(false);` declaration

### Testing

Tested on ESP32 (M5StickC) with PPP+WiFi coexistence. WiFi STA
disconnect/reconnect cycle no longer crashes. mDNS resolves correctly
after reconnect. Verified the fix survives DDP flood (500+ fps) with
concurrent WiFi roaming.

### Related

Companion to a guard in ESPmDNS itself — `MDNS.end()` should also NULL-check
`_mdns_server` before dereferencing, but that's an upstream arduino-esp32 fix.
This WLED-side fix is sufficient and self-contained.