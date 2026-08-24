# WLED — M5StickC PPP+WiFi Fork

> **Branch**: `dev/ppp-wifi` | **Base**: [Aircoookie/WLED](https://github.com/Aircoookie/WLED) upstream main
> **Hardware**: M5StickC (ESP32-PICO-D4, 4MB flash, no PSRAM)
> **Transport**: PPP-over-serial (USB) + WiFi STA/AP simultaneously
> **Status**: Active development — 5 PRs submitted (1 merged), DDP compression + per-segment routing validated on device

This is my personal fork of WLED targeting the M5StickC as a PC ARGB controller. The goal is a USB-connected LED controller that speaks DDP, HTTP, and mDNS over a PPP link rather than relying on WiFi association for every session. WiFi still works — you get both simultaneously — but PPP over the CP2104 USB-serial gives you a reliable, always-on network interface without the join/leave cycle.

The immediate ancestor is [m5stickCLED](https://github.com/aenertia/m5stickCLED), which started as a quick hack to get the M5StickC talking to a PC ARGB header. That project proved the concept but was too far off WLED's architecture to merge anything useful back. This fork starts from upstream WLED and adds transport, display, and hardware support as proper bus and usermod abstractions, so the interesting bits can eventually go upstream.

The M5StickC is a genuinely odd choice for this — 4MB flash, no PSRAM, three usable GPIOs after the TFT and mic take their cut. It works because the constraints force good decisions, and because I had a pile of them already.

---

## Why PPP-over-serial?

The M5StickC connects to the host PC via USB serial (CP2104). That cable is already there for flashing — PPP turns it into a full network interface at 1.5Mbaud without burning any GPIOs on a CS line or worrying about WiFi association state.

PPP gives you a real IP stack. Everything WLED supports over Ethernet or WiFi — HTTP API, DDP, mDNS, OTA — works identically over PPP. The host runs `pppd` and gets a link-local address; WLED responds on 169.254.7.1. No DHCP server, no router, no association timeout.

The pattern follows WLED's existing Ethernet support ([PR #5697](https://github.com/wled/WLED/pull/5697)): register a second `esp_netif_t`, let lwIP handle routing across both interfaces. PPP is purely additive — WiFi keeps working, and WLED doesn't know or care which interface a packet arrives on.

The WiFi coexistence work also hit a real Arduino-ESP32 bug: calling `WiFi.mode(WIFI_STA)` before `WiFi.begin()` loses `WIFI_EVENT_STA_START` on some core versions ([arduino-esp32 #8796](https://github.com/espressif/arduino-esp32/issues/8796)). Our init order — `WiFi.onEvent()` then `WiFi.mode()` — avoids it.

For DDP specifically: the [DDP spec](http://www.3waylabs.com/ddp/) is simple enough that compressed variants (delta+RLE) fit inside the extension bytes. Sparse LED animations compress 20:1 over the raw protocol, which makes 30fps at full 160×80 TFT resolution feasible on a 1.5Mbps serial link.

---

## Hardware

### M5StickC

| Component | Detail |
|-----------|--------|
| SoC | ESP32-PICO-D4, dual-core Xtensa LX6, 240MHz |
| SRAM | 520KB (no PSRAM) |
| Flash | 4MB embedded |
| PMIC | AXP192 on I2C Wire1 (SDA=21, SCL=22, addr 0x34) |
| TFT | ST7735S 80×160, SPI (MOSI=15, CLK=13, CS=5, DC=23, RST=18) |
| Mic | SPM1423 PDM (CLK=GPIO0, DATA=GPIO34), powered by AXP192 LDOio0 |
| Buttons | A=GPIO37, B=GPIO39, Power=AXP192 |
| USB-Serial | CP2104 on UART0 (TX=GPIO1, RX=GPIO3) |
| Usable GPIOs | G0 (strapping!), G26, G36 — three, after everything else takes its cut |

The M5StickC has a 80×160 TFT that WLED now treats as a pixel matrix, which is either clever or deeply cursed depending on your perspective.

### AXP192 Power Rails

| Output | Powers | Voltage | Register |
|--------|--------|---------|----------|
| DCDC1 | ESP32 core | 3.3V | 0x12 bit 0 |
| LDO2 | TFT backlight | 3.0V | 0x12 bit 2, voltage 0x28 high nibble |
| LDO3 | TFT logic | 3.0V | 0x12 bit 3, voltage 0x28 low nibble |
| LDOio0 | Mic (SPM1423) | 2.8V | 0x90=0x02 (LDO mode), 0x91=0xA0 |

### GPIO0 Strapping Interaction

The SPM1423 mic CLK line is GPIO0, which is also the ESP32 boot strapping pin. If AXP192 LDOio0 is left floating (reg 0x90=0x07 default), the unpowered mic pulls GPIO0 low and the chip boots into download mode every time.

`initAXP192()` in `bus_manager.cpp` sets LDOio0 early in boot, before any I2C bus initialisation that could hang. This is also the fix for the GPIO issue I filed against upstream in 2019 ([WLED#179](https://github.com/Aircoookie/WLED/issues/179)) — took a while to get back to it.

### Variants

- **M5StickC Plus**: ST7789V2 135×240 display, same AXP192, same SoC
- **M5StickC Plus2**: AXP192 removed, ESP32-PICO-V3-02, 8MB flash, 2MB PSRAM, GPIO4 HOLD pin for power

---

## What's in This Fork

- **PPP-over-serial transport** (`WLED_USE_PPP`) — ESP32 as PPP server on UART0 at 1.5Mbaud. Host runs `pppd`. Link-local addresses: device=169.254.7.1, host=169.254.7.2. PPP RX task pinned to Core 1 to avoid `lock_tcpip_core` priority inversion with WiFi.

- **WiFi + PPP simultaneous** — both `esp_netif_t` interfaces registered and active. WLED HTTP, DDP, and mDNS work on either interface. No mutual exclusion, no interface priority.

- **SPI display as WLED pixel matrix** (`BusSPIMatrix`, `TYPE_SPI_MATRIX`) — any SPI display supported by TFT_eSPI (ST7735, ST7789, ILI9341, ILI9486, SSD1351 OLED, and more) mapped as a virtual pixel matrix. WLED treats it as a standard 2D bus: effects, segments, presets, DDP, the lot. Which is either clever or deeply cursed, depending on your perspective.

  The M5StickC's 80×160 TFT maps to 40×80 virtual pixels at 2× integer scale. Each virtual pixel becomes a 2×2 physical block — no floating-point, no interpolation, no heap allocation per pixel. DMA ping-pong buffers overlap SPI transfer with pixel conversion; dirty-row tracking skips unchanged strips entirely.

  The bus is generic — board-specific code is cleanly separated via build flags:
  ```
  WLED_ENABLE_SPI_MATRIX          ← enables the bus (any SPI display, any board)
    SPI_MATRIX_W=40               ← virtual panel width  (mandatory, no default)
    SPI_MATRIX_H=80               ← virtual panel height (mandatory, no default)
  WLED_SPI_MATRIX_AXP192          ← M5StickC board support: compiles AXP192 PMIC init
  WLED_SPI_MATRIX_BOARD_INIT=initAXP192  ← wires PMIC init into bus constructor
  ```

  Common integer-scale configurations (non-integer scale is safe but leaves dead pixels at edges):

  | Panel | Physical | Virtual W×H | Scale |
  |-------|----------|-------------|-------|
  | M5StickC ST7735S | 80×160 | 40×80 | 2×2 |
  | M5StickC+ ST7789V2 | 135×240 | 45×80 | 3×3 |
  | SSD1351 1.5" OLED | 128×128 | 32×32 | 4×4 |
  | ILI9341 2.8" / CYD | 240×320 | 40×80 | 6×4 |
  | ILI9486/ILI9488 3.5" Pi | 320×480 | 40×60 | 8×8 |
  | ST7796 4" Pi | 320×480 | 80×120 | 4×4 |
  | SSD1963 5" Pi | 480×800 | 60×100 | 8×8 |

  AXP192 boot guard prevents I2C hangs from blocking boot when this is the default bus type. The bus constructor calls a generic `WLED_SPI_MATRIX_BOARD_INIT` hook — other boards plug in their own PMIC or backlight init, or omit the flag entirely.

- **DDP per-segment targeting** — dual-mode DDP routing replaces the old `useMainSegmentOnly` boolean:
  - Mode A: DDP `destination` byte (1–32) routes to segment 0–31, channel offset is segment-relative
  - Mode B: `ddpEligibleMask` bitmask distributes a flat pixel stream across eligible segments

- **Compressed DDP** — six codec types in `handleDDPPacket()`: Delta+RLE (0x10), RLE keyframe (0x20), Transform (0x30), Delta-only (0x40), Tuple-RLE (0x50), Planar-RLE (0x60). Uses the DDP C bit (`dataType & 0x80`) as the compression signal. ~95% bandwidth reduction on sparse patterns, 62:1 on solid content. Python sender in `tools/ddp_bench.py`. Full spec in [`docs/ddp-readme.md`](docs/ddp-readme.md).

- **Mixed-segment realtime** — internal effects and DDP can run on separate segments simultaneously. `service()` show is gated on `!rtFrozenSegs`; bus push is owned by `showFrozenSegs()` on DDP PUSH cadence, compositing both effect and DDP pixels atomically. Validated with Ghost Rider on seg0 + DDP twinkle on seg1.

- **Auto-ceiling DDP rate** — `ddpCurrentSafeFps` computed from bus show times each loop iteration. Rate limiter gates DDP at `min(ddpMaxFps, ddpCurrentSafeFps)`. `/diag` exposes `ddpSlots`, `totalElig`, `eligMask`, `frozen` bitmask, per-bus `showUs`.

- **Bus skip-show + DDP realtime fast path** — when a segment's bus has no changes (DDP frozen-segment check), `show()` is skipped entirely. TFT SPI DMA is the bottleneck at ~8ms per frame; skipping unchanged buses pushes sustained DDP throughput from 45fps to 119fps.

- **Wave 3 heap optimisation** — three changes recovered significant heap on a 520KB-SRAM, no-PSRAM device:
  - Segment-scoped `ddpPrevFrame` (−8.4 KB vs. global allocation)
  - Runtime TFT DMA deallocation when TFT bus is removed (+29 KB free)
  - Dirty-row partial render (loopLag 17ms → 0–3ms)

- **AXP192 boot guard** — `initAXP192()` called from `WLED::setup()` before `beginStrip()`. Probes I2C before writing, idempotent via `s_axp192_ready` flag. Fixes the GPIO0 strapping interaction with the SPM1423 mic.

- **AudioReactive with SPM1423 PDM mic** — `m5stickc_ppp_wifi_mic` build variant. Key fix: `i2s_set_clk()` after `i2s_driver_install()` kills PDM mode on IDF 5.x (produces all-zero samples). Fixed with `if (!(_config.mode & I2S_MODE_PDM))` guard.

- **Pi Pico USB-ECM/NAT bridge** — CDC-NCM (USB Ethernet) on the host-facing port, PPP-over-UART to the M5StickC. Zero-config networking: host sees a USB Ethernet adapter, Pico handles PPP negotiation. Uses [pico-sdk](https://github.com/raspberrypi/pico-sdk).

- **ARGB motherboard header passthrough** via RMT — reads 3-pin ARGB header signal, re-times via ESP32 RMT peripheral, drives WS2812B output. Relates to [CorsairLightingProtocol](https://github.com/Legion2/CorsairLightingProtocol) for protocol context.

- **Scrolling text improvements** — anti-aliased font rendering, trail artefact fix on segment wrap.

---

## Build Environments

| Environment | PPP | WiFi | Mic | Notes |
|-------------|-----|------|-----|-------|
| `m5stickc_ppp_wifi` | Yes | Yes | No | Day-to-day dev build. 70.9% flash. |
| `m5stickc_ppp_wifi_mic` | Yes | Yes | Yes | Full-feature. AudioReactive + SPM1423 PDM. |
| `m5stickc_ppp` | Yes | No | No | PPP-only. Max heap. |
| `m5stickc_lean` | No | Yes | No | WiFi-only. Upstream compatibility testing. |
| `m5stickc_pico` | Yes (via Pico) | No | No | PPP via Pi Pico on UART1. Console debug available. |

---

## Quick Start

### Build

Systems with Python >3.12 (e.g. RHEL 10, Fedora 42) need pyenv 3.11 — PlatformIO doesn't support newer interpreters yet.

```bash
cd ~/build/WLED
git checkout dev/ppp-wifi

# If your system Python is >3.12, use pyenv 3.11 explicitly:
PLATFORMIO_CORE_DIR=~/.platformio \
  ~/.pyenv/versions/3.11.9/bin/python3.11 -m platformio run -e m5stickc_ppp_wifi

# Otherwise:
pio run -e m5stickc_ppp_wifi
```

The board variant symlink must exist (the board JSON says `m5stick_c`, the framework ships `m5stack_stickc`):
```bash
ln -s ~/.platformio/packages/framework-arduinoespressif32/variants/m5stack_stickc \
      ~/.platformio/packages/framework-arduinoespressif32/variants/m5stick_c
```

### Flash

Always merge into a combined binary first. The M5StickC needs `--flash-mode dio` and `--no-stub` — higher baud rates fail at 86%, `qio` mode doesn't boot.

```bash
esptool --chip esp32 merge-bin -o /tmp/wled_combined.bin \
  --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x1000  .pio/build/m5stickc_ppp_wifi/bootloader.bin \
  0x8000  .pio/build/m5stickc_ppp_wifi/partitions.bin \
  0xe000  ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/m5stickc_ppp_wifi/firmware.bin

esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --no-stub \
  write_flash -z --flash-size 4MB 0x0 /tmp/wled_combined.bin
```

esptool's RTS reset reboots the device after flash — no manual power cycle needed.

**NVS note**: NVS persists bus config across flashes. If the device already has a saved config, changing `DEFAULT_LED_TYPE` in build flags won't take effect until you either erase NVS or POST new config via `/json/cfg`.

### PPP Setup

After flashing, start `pppd` on the host:

```bash
sudo pppd /dev/ttyUSB0 1500000 noauth nodetach local nocrtscts \
  novj nodeflate nobsdcomp noaccomp nopcomp lcp-echo-interval 0 \
  mru 1500 mtu 1500 169.254.7.2:169.254.7.1
```

WLED is then accessible at `http://169.254.7.1`. DDP to port 4048, mDNS as `wled.local` on the PPP interface.

Add a sudoers rule to avoid the `sudo` prompt every time:
```
# /etc/sudoers.d/pppd
yourusername ALL=(root) NOPASSWD: /usr/bin/pppd
```

---

## Performance

Measured on M5StickC (ESP32-PICO-D4), `m5stickc_ppp_wifi`, Wave 3 heap optimisations active.

| Scenario | FPS | loopLag | Free Heap |
|----------|-----|---------|-----------|
| TFT on, DDP realtime, Wave 3 | 43 | 0–3 ms | 59 KB |
| TFT off (runtime dealloc) | 43 | 0–3 ms | 88.7 KB |
| Bus skip-show, frozen segments | 119 | <1 ms | 59 KB |
| DDP raw (800 LEDs, 1.5Mbps) | 40 | — | — |
| DDP delta+RLE, sparse pattern | ~800 ceiling | — | — |

**DDP compression** (delta+RLE at 40fps, 800 LEDs):

| Pattern | Compression | Wire bandwidth |
|---------|------------|----------------|
| Rainbow (worst case) | 0% | 93.7 KB/s |
| Solid pulse (uniform) | 13–21% RLE | 77.8 KB/s |
| Sparse twinkle (2% change) | 95% delta+RLE | 4.6 KB/s |

The bus skip-show path (45fps → 119fps) requires the DDP sender to hold frozen segments steady — any change to a segment's pixels triggers its bus `show()` again. For ARGB header sync where one segment is the "source" and another mirrors it, this works well.

---

## PR Candidates

5 PRs submitted upstream (1 merged, 1 closed, 3 open). All live on `pr/*` topic branches rebased onto upstream `main`:

| Phase | Branches | Description |
|-------|----------|-------------|
| Bug fixes | `pr/mdns-ppp-crash-fix`, `pr/chunked-json-fix` | mDNS crash on PPP netif removal, chunked JSON response fix |
| DDP | `pr/ddp-rle-codec`, `pr/ddp-compressed-receiver`, `pr/ddp-compressed` | Delta+RLE codec, receiver integration, sender tooling |
| Effects | `pr/effects-fade-snap`, `pr/effects-deferred-fade` | Fade snap on preset change, deferred fade-in |
| Text | `pr/text-aa-fonts`, `pr/text-drop-shadow` | Anti-aliased font rendering, drop shadow |
| Hardware/transport | `pr/argb-passthrough`, `pr/tft-bus-matrix`, `pr/ppp-transport`, `pr/slip-transport` | ARGB RMT passthrough, TFT bus matrix, PPP transport, SLIP transport |
| Upstream component fixes | `pr/arduino-esp32-mdns-guard`, `pr/arduino-esp32-netif-lazy-init`, `pr/esp-idf-lcp-echo-docs` | arduino-esp32 mDNS guard, netif lazy init fix, LCP echo docs |
| Bus | `pr/bus-skip-show` | Bus skip-show fast path for frozen segments |

See [PR-TRACKING.md](PR-TRACKING.md) for branch-by-branch status, commit ranges, and submission readiness.

---

## Upstream

This fork tracks [Aircoookie/WLED](https://github.com/Aircoookie/WLED) main. The `main` branch of this repo is a clean rebase of upstream — no fork-specific commits. All fork work is on `dev/ppp-wifi`.

Upstream remote: `https://github.com/Aircoookie/WLED.git`

---

## Licence

[EUPL v1.2](LICENSE) — same as upstream WLED.

Fork additions by Joel Wirāmu Pauling ([@aenertia](https://github.com/aenertia)).
