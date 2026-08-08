# WLED USB Controller — Build Learnings

Accumulated findings from iteration wave 1. Update as new discoveries are made.

## Build System

### IDF Version: V4, not V5
WLED v16.0.1 `platformio.ini` has both `esp32_idf_V4` and `esp32_idf_V5` sections, but the V5 section in this version references a Tasmota fork that resolves to IDF v4 internally. The stock `esp32dev` env uses `esp32_idf_V4` explicitly. Our `platformio_override.ini` extends `esp32_idf_V4`.

**Implication for PPP**: PPP server support (`CONFIG_LWIP_PPP_SERVER_SUPPORT`) must be verified against the IDF v4 SDK that PlatformIO actually downloads, not the upstream ESP-IDF v5 docs we referenced in the ADR. The API surface may differ.

### platformio_override.ini is .gitignored
WLED's `.gitignore` includes `platformio_override.ini`. Must use `git add -f` to force-track it in our fork.

### Web UI Build Requires Node.js
`pio-scripts/build_ui.py` runs `npm ci && npm run build` (which calls `node tools/cdata.js`). This generates 8 `html_*.h` header files (~770KB total) containing gzipped PROGMEM web assets. Node.js v22.23.1 on koero host handles this.

### Build Times
- First build (with toolchain download): ~116 seconds
- Cached rebuild (m5stickc after esp32dev): ~36 seconds
- Full clean rebuild: expect ~2 minutes

## Flash Budget

### esp32dev (IDF v4, stock WLED)
- firmware.bin: 1,304,704 bytes (1.24 MB)
- Flash: 82.5% of 1,572,864 (1.5MB OTA partition)
- RAM: 24.9% (81,512 / 327,680)
- Headroom: 268 KB

### m5stickc (IDF v4, stock WLED, m5stick-c board)
- firmware.bin: 1,265,216 bytes (1.21 MB)
- Flash: 66.2% of 1,810,432 (1.72MB OTA partition — bigger partition table)
- RAM: 24.7% (80,984 / 327,680)
- Headroom: ~642 KB per OTA slot
- **Smaller than esp32dev** because m5stick-c board uses different partition layout

### Key flash consumers (from linker map)
- Effects engine (FX.cpp): largest single contributor
- Web UI (html_*.h PROGMEM): ~135KB gzipped
- ArduinoJson: ~30KB
- AsyncWebServer + AsyncTCP: ~40KB
- WiFi stack: ~40-60KB (target for removal when PPP replaces WiFi)

## Hardware

### M5StickC Board ID
PlatformIO has native `m5stick-c` board definition. No custom board JSON needed.

### GPIO Allocation Confirmed
- G26 (HAT): LED data output — works with RMT, no conflicts
- G37: Button A — active low, internal pull-up
- G0: **DO NOT USE** — strapping pin, can prevent boot
- G36: **input-only** — cannot drive LEDs
- G32/G33 (Grove): available for secondary LED channels

### USB Serial
- FTDI FT232 — official baud rates: 115200, 250000, 500000, 750000, 1500000
- 921600 is NOT in FTDI official list (commonly used elsewhere but may be unreliable)
- For PPP: target 1500000 baud (max supported)

## Infrastructure

### SSH Host Key Rotation
Forgejo (`[git.awa.3d.ae.net.nz]:2222`) SSH host key rotated at some point. koero had stale known_hosts entry. Fixed by removing old key and adding new ed25519 key via `ssh-keyscan -p 2222 172.16.1.132` (hostname keyscan failed, IP worked).

### pip Dependency Conflicts on koero (non-blocking)
`pip3 install --user platformio` shows dependency conflicts with `command-line-assistant` (wants older markdown/requests/sqlalchemy) and `ipatests` (missing polib/pytest_multihost). These are pre-existing RHEL package conflicts and do NOT affect PlatformIO.

### Avahi on koero
- `avahi-daemon` was already running
- `nss-mdns` was NOT installed (needed for .local resolution) — installed in Task 1
- `allow-point-to-point=no` was default — changed to `yes` for PPP mDNS

## PPP Considerations (for Wave 2)

### IDF v4 PPP Support
Need to verify whether the Arduino-ESP32 2.0.18 (IDF v4) framework includes:
- `esp_netif_ppp.h`
- `CONFIG_LWIP_PPP_SERVER_SUPPORT` Kconfig option
- `ppp_listen()` function

The ESP-IDF v5 docs (referenced in refs/ppp-serial-tunnel.md) may not apply directly. The IDF v4 PPP API surface could be different or missing server support.

### Alternative: WLED v16 may have V5 builds
Check if WLED v16.0.1 has working IDF v5 environments (`esp32_idf_V5` section). If the V5 section works for M5StickC, PPP server support is guaranteed. IDF v4 PPP support is the risk item.

### UART0 Contention
PPP needs exclusive UART0 access. WLED_DISABLE_ADALIGHT disables the serial handler, freeing UART0 for PPP. This is confirmed safe — DDP over UDP replaces Adalight for pixel data.

## Build Warnings (all upstream, non-blocking)

1. `audioreactive/audio_source.h`: ADC API deprecated (adc_digi_pattern_table_t, adc_gpio_init)
2. `GifDecoder_Impl.h:181`: variable shadowing
3. `esp32-hal-i2c-slave.c:547`: variable shadowing
4. PSRAM not defined message (expected — M5StickC has no PSRAM)
