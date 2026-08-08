# WLED Build Targets — All MCU Variants

## Build Results (2026-08-09)

All environments compile successfully on WLED 17.0.0-devV5.

### Size Report

| Tier | Environment | MCU | Bin Size | Flash % | RAM % | Partition | Status |
|------|-------------|-----|----------|---------|-------|-----------|--------|
| 1 | m5stickc_ppp | ESP32 | 1,143,840 | ~60% | ~30% | big (4MB) | OK |
| 1 | m5stickc_ppp_tft | ESP32 | 1,143,840 | ~60% | ~30% | big (4MB) | OK |
| 1 | m5stickc_pico | ESP32 | 1,143,840 | ~60% | ~30% | big (4MB) | OK |
| 1 | m5stickc_pico_tft | ESP32 | 1,143,840 | ~60% | ~30% | big (4MB) | OK |
| **2** | **atoms3_lean** | **ESP32-S3** | **1,134,240** | **54.1%** | — | large (8MB) | **OK** |
| **2** | **atoms3u_lean** | **ESP32-S3** | **1,138,784** | **54.3%** | — | large (8MB) | **OK** |
| **2** | **stamps3_lean** | **ESP32-S3** | **1,134,240** | **54.1%** | — | large (8MB) | **OK** |
| **2** | **esp32s3_8m_lean** | **ESP32-S3** | **1,134,304** | **54.1%** | — | large (8MB) | **OK** |
| 3 | esp32dev_ppp | ESP32 | 1,143,840 | 60.2% | 30.5% | big (4MB) | OK |
| 3 | esp32_wrover_ppp | ESP32+PSRAM | 1,144,400 | 60.2% | 20.5% | big (4MB) | OK |
| 4 | esp32c3_ppp | ESP32-C3 | 1,144,144 | 72.7% | 27.3% | default (4MB) | OK |

### Key Findings

1. **ESP32-S3 lean builds are ~10KB smaller than ESP32 classic PPP builds** (1,134K vs 1,144K)
   - S3 omits PPP stack overhead since it uses native USB instead
   - S3 has 8MB flash (large partition) → only 54% used vs 60% on 4MB classic

2. **AtomS3U is 4.5KB larger than AtomS3** (1,138,784 vs 1,134,240)
   - AtomS3U includes OPI PSRAM support (`qio_opi` memory type)
   - AtomS3/StampS3/generic S3 use `qio_qspi` (no PSRAM)

3. **ESP32-C3 is the tightest fit** at 72.7% flash on 4MB default partition
   - Still has ~430KB headroom — adequate for lean builds
   - RISC-V code is slightly larger than Xtensa for the same functionality

4. **WROVER (PSRAM) uses less RAM than esp32dev** (20.5% vs 30.5%)
   - PSRAM offloads heap allocations; firmware binary is only 560 bytes larger

5. **All Tier 1 M5StickC PPP/Pico variants are identical size** (1,143,840)
   - TFT flags don't add code — they're compile-time stubs (Wave 4 work)

## Architecture

### Tier 1: M5StickC + Pico Bridge (existing, unchanged)
- ESP32 classic (Xtensa LX6, 240MHz, 4MB flash)
- PPP over UART to Pi Pico for USB-Ethernet
- 4 variants: FTDI PPP, FTDI PPP+TFT, Pico bridge, Pico bridge+TFT

### Tier 2: ESP32-S3 Single-MCU (NEW — key targets)
- ESP32-S3 (Xtensa LX7, 240MHz, 8MB flash)
- Native USB-OTG → CDC-ECM/NCM eliminates Pico bridge (Phase C)
- Current builds: lean WLED + ARGB passthrough over WiFi
- 4 variants: AtomS3, AtomS3U (USB-A!), StampS3, generic S3 devkit

### Tier 3: ESP32 Classic PPP (NEW)
- ESP32 (Xtensa LX6, 240MHz, 4MB flash)
- PPP over UART — same as Tier 1 but for generic boards
- 2 variants: esp32dev, WROVER (with PSRAM)

### Tier 4: ESP32-C3 Minimal (NEW)
- ESP32-C3 (RISC-V, 160MHz, 4MB flash)
- PPP over UART — needs Pico bridge
- 1 variant: esp32-c3-devkitm-1

## Build Notes

### Required PlatformIO fixes discovered during build

1. **ESP32-S3 boards need `board_build.arduino.memory_type`**
   - `m5stack-atoms3` and `m5stack-stamps3` board JSONs lack this field
   - Without it: `sdkconfig.h: No such file or directory`
   - Fix: set `qio_qspi` (no PSRAM) or `qio_opi` (OPI PSRAM) explicitly

2. **ESP32 classic needs `board_build.flash_mode = dio`**
   - `[common]` section sets `flash_mode = dout` but bootloader only has `dio`/`qio` variants
   - Without it: `bootloader_dout_40m.elf not found`
   - Fix: explicitly set `board_build.flash_mode = dio`

3. **ESP32 classic needs `board_build.arduino.memory_type = dio_qspi`**
   - Same sdkconfig.h resolution issue as S3
   - Fix: set `dio_qspi` matching the `dio` flash mode

4. **WROVER board ID**: `esp-wrover-kit` (not `esp32-wrover-kit`)

### Pin Assignments (to verify on hardware)

| Board | DATA_PINS | BTNPIN | ARGB_RX_PIN | Notes |
|-------|-----------|--------|-------------|-------|
| AtomS3 | 5 | 41 | 6 | GPIO5 = Grove port data |
| AtomS3U | 5 | 41 | 6 | Same as AtomS3, USB-A form factor |
| StampS3 | 1 | 0 | 2 | Minimal breakout |
| ESP32-S3 devkit | 48 | — | 47 | Generic, WS2812 on GPIO48 |
| ESP32dev PPP | 16 | — | 17 | Classic GPIO16/17 pair |
| ESP32-C3 PPP | 8 | — | 9 | GPIO8/9 pair |

**⚠ Pin assignments are educated guesses — verify on actual hardware before deployment.**

## Next Steps

- [ ] Phase C: USB CDC-ECM/NCM on S3 targets (eliminate Pico bridge)
- [ ] Verify pin assignments on physical boards
- [ ] Add WROVER_PPP unique release name (currently overwrites ESP32_PPP)
- [ ] Consider ESP32-C6 target (skipped — may be unstable in WLED)
