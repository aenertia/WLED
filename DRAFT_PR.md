## fix(ESPmDNS): guard end() against NULL _mdns_server

**Forgejo**: Fixes #19

**Target repo**: espressif/arduino-esp32
**WLED workaround**: wled00/network.cpp — mdnsStarted flag prevents calling MDNS.end() when begin() was never called

### Problem

`ESPmDNS::end()` dereferences `_mdns_server` without checking for NULL.
When `begin()` was never called (e.g. PPP is the only active netif and
WiFi never associated), `_mdns_server` is NULL and `end()` crashes.

Observed on ESP32 with PPP+WiFi coexistence where WiFi STA drops before
mDNS is initialised.

### Proposed fix (for espressif/arduino-esp32)

In `libraries/ESPmDNS/src/ESPmDNS.cpp`, `ESPmDNS::end()`:

```diff
 void ESPmDNS::end() {
+  if (!_mdns_server) return;
   mdns_free();
   _mdns_server = NULL;
 }
```

### WLED-side workaround

`wled00/network.cpp` tracks `mdnsStarted` and avoids calling `MDNS.end()`
when `begin()` was never called. This is the workaround; the arduino-esp32
fix is the correct solution.

### Testing

Tested on ESP32-PICO-D4 (M5StickC) with PPP+WiFi. WiFi STA disconnect
no longer crashes when mDNS was never initialised.