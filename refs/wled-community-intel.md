# WLED Community Intelligence — Serial/PPP/IDF v5

## IDF v5 Migration Status

### PR #4838 — ESP-IDF V5 (MERGED Jul 19, 2026)
- 296 commits by @netmindz merged into main
- IDF 4.4.x / arduino-esp32 v2.0.5 → IDF 5.x / arduino-esp32 v3.x
- Renamed NetworkClass → WLEDNetworkClass (avoid symbol collision)
- New RMT, LEDC, I2S, DMX driver APIs for V5
- Still open: MQTT disabled, IR disabled, audioreactive I2S needs migration
- URL: https://github.com/wled/WLED/pull/4838

### PR #5769 — Update to IDF 5.5.4 (OPEN, approved)
- Tasmota platform 2026.05.50
- ESP32-C5 confirmed working
- URL: https://github.com/wled/WLED/pull/5769

### Issue #5441 — Discussion: move to pure IDF?
- Opened by @DedeHai (WLED Collaborator), backburner
- Pure IDF considered long-term goal, not imminent
- URL: https://github.com/wled/WLED/issues/5441

## WiFi-less / Ethernet-Only Operation

### PR #5697 — ESP32-P4 Ethernet-only (CLOSED, code exists)
**This is our closest precedent.** Introduced:
- `WLED_ETHERNET_ONLY_BUILD` compile-time flag
- Early-returns in initAP(), initConnection(), handleConnection()
- WiFi scan/event hookup wrapped behind the flag
- Fork: https://github.com/petrisy/WLED/tree/p4-eth-upstream-ready
- URL: https://github.com/wled/WLED/pull/5697

### Issue #5762 — AP still active with Ethernet
- Users want WiFi completely disabled when using Ethernet
- Validates our WiFi-less approach
- URL: https://github.com/wled/WLED/issues/5762

## Serial Interface Pain Points

### Issue #5652 — Noise on serial RX / option to disable serial
- Aggressive ADALight monitoring causes stuttering on floating RX
- Validates WLED_DISABLE_ADALIGHT for our PPP use case
- URL: https://github.com/wled/WLED/issues/5652

### Issue #4230 — WLED_DISABLE_ADALIGHT broke settings/sync
- Was broken, now FIXED — safe to use
- URL: https://github.com/wled/WLED/issues/4230

## PPP / USB Networking
**Zero results.** No issues, PRs, or forks for PPP, USB networking, or serial IP tunneling in WLED. We are pioneering this approach.

## Network Refactoring (Active, Aug 2026)
PRs #5774-#5791 by @netmindz — encapsulating global state:
- network.cpp (#5786), wled_server.cpp (#5780), udp.cpp split (#5774)
- Good timing — codebase becoming more modular for our transport swap

## Memory / Flash
### Issue #5518 — Audit JSON_BUFFER_SIZE
- ESP32 no-PSRAM: 32,767 bytes JSON buffer
- Streaming JSON proposed to reduce this
- URL: https://github.com/wled/WLED/issues/5518

## Key Build Flags (from community)
```
-DWLED_ETHERNET_ONLY_BUILD    # Skip WiFi init (from P4 PR)
-DWLED_DISABLE_ADALIGHT       # Free serial for PPP (fixed, safe)
-DWLED_DISABLE_OTA            # No OTA over WiFi
-DWLED_DISABLE_MQTT           # Disabled in V5 anyway
-DWLED_DISABLE_INFRARED       # Disabled in V5 anyway
```
