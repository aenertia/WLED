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

## Lean Build Analysis (Task 4)

### m5stickc_lean (IDF v4, all non-essential modules disabled)
- firmware.bin: 1,106,080 bytes (1.05 MB)
- Flash: 58.2% of 1,900,544 (1.81MB OTA partition)
- RAM: 23.8% (78,088 / 327,680)
- Headroom: ~775 KB per OTA slot

### Size Comparison: Stock vs Lean
| Metric | Stock m5stickc | Lean m5stickc | Delta |
|--------|---------------|---------------|-------|
| firmware.bin | 1,265,216 B | 1,106,080 B | **-159,136 B (-155 KB)** |
| Flash usage | 66.2% | 58.2% | -8.0 pp |
| Headroom | 620 KB | 775 KB | +155 KB |
| RAM | 80,984 B (24.7%) | 78,088 B (23.8%) | -2,896 B (-2.8 KB) |

### Disabled Modules (combined savings: 155 KB flash, 2.8 KB RAM)
- WLED_DISABLE_ALEXA — Alexa/Hue emulation
- WLED_DISABLE_HUESYNC — Philips Hue sync
- WLED_DISABLE_INFRARED — IR remote receiver
- WLED_DISABLE_MQTT — MQTT client
- WLED_DISABLE_OTA — ArduinoOTA (HTTP OTA via /update still works)
- WLED_DISABLE_ESPNOW — ESP-NOW mesh
- WLED_DISABLE_LOXONE — Loxone home automation
- WLED_DISABLE_2D — 2D matrix effects
- WLED_DISABLE_BROWNOUT_DET — brownout detector
- WLED_DISABLE_ADALIGHT — serial LED protocol (freed for PPP UART)

### Flash Budget After Lean
With 775 KB headroom, PPP stack (~30-50 KB estimated) fits comfortably even
with additional usermod code. WiFi stack removal (Wave 2) would save another
~40-60 KB but requires source changes.

## PPP Feasibility Analysis (Task 5)

### IDF Version Confirmed
- Arduino-ESP32: framework-arduinoespressif32 (PlatformIO prebuilt)
- ESP-IDF version: **4.4.8** (MAJOR=4, MINOR=4, PATCH=8)

### PPP Header Availability: ✅ FOUND
- `esp_netif_ppp.h` exists at: `tools/sdk/esp32/include/esp_netif/include/esp_netif_ppp.h`
- lwip PPP headers exist: `netif/ppp/*.h` (ccp, chap, eap, ecp, fsm, ipcp, etc.)
- `PPP_SERVER` macro defined in `ppp_opts.h` (conditional on `PPP_SUPPORT`)

### PPP Server API: ⚠️ CLIENT-ONLY in esp_netif
- `esp_netif_ppp.h` provides: `esp_netif_ppp_set_auth()`, `esp_netif_ppp_set_params()`, `esp_netif_ppp_get_params()`
- NO server-mode API (`ppp_listen`, `ppp_passive`, `ppp_set_server`) in esp_netif headers
- lwip's `ppp_opts.h` has `PPP_SERVER` support at the lwip level, but esp_netif doesn't expose it

### Critical Blocker: Prebuilt liblwip.a has PPP DISABLED
- `sdkconfig` in Arduino-ESP32 SDK: `# CONFIG_LWIP_PPP_SUPPORT is not set`
- `liblwip.a` contains zero PPP symbols (verified via nm)
- `-D CONFIG_LWIP_PPP_SUPPORT=1` as a build flag has NO EFFECT — it only affects
  preprocessor guards in headers, but the actual lwip object code is already compiled
  without PPP and linked from the prebuilt static library

### PPP Enablement Paths (Wave 2 Options)

**Option A: Patch liblwip.a (Recommended for PoC)**
- Rebuild lwip from Arduino-ESP32 source with `CONFIG_LWIP_PPP_SUPPORT=y`
- Replace the prebuilt `liblwip.a` in the framework package
- Pros: No framework migration needed, surgical change
- Cons: Fragile, must redo on framework updates

**Option B: Switch to ESP-IDF framework (Recommended for production)**
- Change PlatformIO framework from `arduino` to `espidf` (or `arduino+espidf`)
- Use `sdkconfig.defaults` with `CONFIG_LWIP_PPP_SUPPORT=y` and `CONFIG_LWIP_PPP_SERVER_SUPPORT=y`
- This triggers a full IDF build that compiles lwip from source with PPP enabled
- Pros: Proper, maintainable, enables sdkconfig control
- Cons: May break Arduino-dependent WLED code, significant migration effort

**Option C: Raw lwip PPP API bypass**
- Use lwip's `pppapi.h` / `pppos.h` directly instead of esp_netif abstraction
- Still blocked by prebuilt liblwip.a — same rebuild requirement as Option A
- But avoids the missing esp_netif server API problem

**Option D: Upgrade to IDF v5 / Arduino-ESP32 3.x**
- Arduino-ESP32 3.x uses IDF v5.x which may have PPP compiled in by default
- Would also get the newer esp_netif PPP server API if available
- Cons: WLED v16 may not support Arduino-ESP32 3.x yet

### Recommendation
Start with **Option A** (patch liblwip.a) for the PPP PoC, plan migration to
**Option B** (ESP-IDF framework) for production. The 775 KB flash headroom from
the lean build provides ample space for PPP stack overhead.

## IDF v5 Migration Path (discovered post-Wave 1)

### Critical Finding: Upstream WLED main already migrated to IDF v5

Our v16.0.1 tag is behind. Upstream `main` has `[esp32_idf_V5]` as the DEFAULT:
- ESP-IDF 5.3.4
- arduino-esp32 v3.1.10
- Tasmota platform `2026.02.30`
- New shared RMT driver (`WLED_USE_SHARED_RMT`)
- NeoPixelBus CORE3 branch for IDF v5 compatibility

### Key insight: `framework = arduino` gives full IDF access

Arduino-esp32 v3.x IS an IDF component. All ESP-IDF APIs (`esp_netif_*`, `uart_*`, PPP) are directly available. No need for dual framework (`arduino, espidf`).

### sdkconfig.defaults works with framework = arduino

The Tasmota platform processes `sdkconfig.defaults` even in Arduino-only mode. This means:
- `CONFIG_LWIP_PPP_SUPPORT=y` → lwIP compiles PPP from source
- `CONFIG_LWIP_PPP_SERVER_SUPPORT=y` → server mode available
- No prebuilt `liblwip.a` blocker (IDF v5 compiles everything from source)

### Wave 2 plan: Rebase onto upstream main

1. `git fetch upstream && git rebase upstream/main`
2. Resolve conflicts in `platformio_override.ini` (our custom envs)
3. Update `platformio_override.ini` to extend `esp32_idf_V5` instead of `esp32_idf_V4`
4. Add `sdkconfig.defaults` with PPP + tuning flags
5. Build and verify M5StickC on IDF v5
6. PPP should just work (lwIP compiled from source with PPP enabled)

### V5 build flag changes needed
- Add `-D WLED_USE_SHARED_RMT` (inherited from V5 base)
- Add `-D ESP32_ARDUINO_NO_RGB_BUILTIN` (inherited from V5 base)
- NeoPixelBus CORE3 branch (inherited from V5 lib_deps)
- lib_ignore NeoESP32RmtHI (inherited from V5)

## Wave 2 Findings

### IDF v5 Rebase
- Upstream WLED main rebased cleanly — only 1 conflict (our AGENTS.md, two versions)
- `esp32_idf_V5` section has 72 references in platformio.ini
- V4 section retained for backward compat

### IDF v5 Build Sizes (vs V4 baseline)
| Env | V4 | V5 | Delta |
|-----|----|----|-------|
| m5stickc | 1,265,216 B | 1,284,496 B | +19,280 B (+1.5%) |
| lean | 1,106,080 B | 1,136,992 B | +30,912 B (+2.8%) |
| ppp | — | 1,135,792 B | (based on lean + PPP) |

V5 slightly larger due to shared RMT driver, DMX input, NeoPixelBus CORE3.

### PPP Implementation
- 59 PPP symbols confirmed in firmware.elf
- `wled_ppp.h` + `wled_ppp.cpp`: ~200 lines of new code
- Follows isEthernet() pattern exactly
- PPP server mode: 169.254.7.1 (ESP32) / 169.254.7.2 (host)
- UART0 at 1.5Mbps, server waits for host pppd
- IP_EVENT_PPP_GOT_IP triggers interfacesInited → web server starts

### sdkconfig.defaults
Works with framework=arduino on Tasmota platform (IDF 5.3.4).
lwIP compiles from source with PPP enabled. Key options:
- CONFIG_LWIP_PPP_SUPPORT=y
- CONFIG_LWIP_PPP_SERVER_SUPPORT=y
- CONFIG_LWIP_PPP_PAP_SUPPORT=y
- LCP echo keepalive enabled

### Variant Symlink Issue (pioarduino V5)
pioarduino V5 renamed m5stick_c variant to m5stack_stickc.
Need to create symlink in the variant directory for build to find board files.

### TFT Display as WLED Segment (ADR addition)
- BusHub75Matrix is the exact pattern — non-LED display as pixel output bus
- Type IDs 72-79 are unused — perfect slot for BusTFTMatrix
- 20x40 virtual pixels (4x upscale to 80x160) = 800 pixels, ~7KB total memory
- SPI (TFT) and RMT (LEDs) are independent peripherals — no conflicts
- LovyanGFX preferred for M5StickC (native board defs, DMA support)
- This is novel — no existing TFT-as-bus in WLED community
