## fix(WiFi): call esp_netif_init() before netif creation in WiFiGeneric

**Target repo**: espressif/arduino-esp32
**WLED workaround**: wled00/wled_ppp.cpp — initPPP() calls esp_netif_init() itself

### Problem

Arduino's WiFi stack lazily initialises `esp_netif_init()` and
`esp_event_loop_create_default()` on the first WiFi call. When PPP or
Ethernet creates a netif before WiFi is initialised, these calls haven't
happened yet — the netif creation silently fails.

### Proposed fix (for espressif/arduino-esp32)

In `libraries/WiFi/src/WiFiGeneric.cpp`, ensure `esp_netif_init()` and
`esp_event_loop_create_default()` are called before any netif creation,
not lazily on first WiFi use.

### WLED-side workaround

`wled00/wled_ppp.cpp` `initPPP()` calls `esp_netif_init()` and
`esp_event_loop_create_default()` itself before creating the PPP netif.
This is the workaround; the arduino-esp32 fix ensures correct ordering
for all transports.

### Testing

Tested on ESP32-PICO-D4 with PPP transport initialised before WiFi.begin().
PPP netif creates successfully without the workaround when the fix is applied.
