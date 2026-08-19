# TFT Display as WLED Pixel Matrix Output Bus (BusSPIMatrix)

**Forgejo**: Fixes #14

## Summary

Adds BusSPIMatrix — a new bus type that drives an ST7735S/ST7789 TFT display as a WLED pixel matrix. The display appears as a standard 2D LED matrix: effects, segments, presets, and DDP all work identically.

## Changes

| File | Description |
|------|-------------|
| `bus_spi_matrix.h` | TFT_eSPI configuration defines and includes |
| `bus_manager.cpp` | BusSPIMatrix class implementation + AXP192 PMIC init |
| `bus_manager.h` | BusSPIMatrix class declaration, `isSPIMatrix()` type check |
| `const.h` | `TYPE_SPI_MATRIX` (72) and range constants |
| `wled.cpp` | Early `initAXP192()` call before `beginStrip()` |
| `cfg.cpp` | `SPI_MATRIX_W/H` matrix panel defaults on fresh NVS |
| `wled.h` | Minimal `rtFrozenSegs` for DDP-aware row range calculation |

## Build Flags

The bus uses a layered flag system so board-specific code is cleanly separated from the generic bus:

```
# Generic — enables the bus class (any SPI display, any board)
-D WLED_ENABLE_SPI_MATRIX
-D SPI_MATRIX_W=40        # virtual panel width  (mandatory, no default)
-D SPI_MATRIX_H=80        # virtual panel height (mandatory, no default)

# M5StickC board support — compiles AXP192 PMIC code
-D WLED_SPI_MATRIX_AXP192
-D WLED_SPI_MATRIX_BOARD_INIT=initAXP192
```

`SPI_MATRIX_W` and `SPI_MATRIX_H` have no defaults — omitting them is a compile error. This forces explicit configuration and prevents silent M5StickC-specific values leaking into other boards.

### Common integer-scale configurations

| Panel | TFT_WIDTH×HEIGHT | SPI_MATRIX_W×H | Scale |
|-------|-----------------|-----------------|-------|
| M5StickC ST7735S | 80×160 | W=40 H=80 | 2×2 |
| M5StickC+ ST7789V2 | 135×240 | W=45 H=80 | 3×3 |
| SSD1351 1.5" OLED | 128×128 | W=32 H=32 | 4×4 |
| TTGO T-Display | 135×240 | W=27 H=48 | 5×5 |
| ILI9341 2.8" / CYD | 240×320 | W=40 H=80 | 6×4 |
| ILI9486/ILI9488 3.5" Pi | 320×480 | W=40 H=60 | 8×8 |
| ST7796 4" Pi | 320×480 | W=80 H=120 | 4×4 |
| SSD1963 5" Pi | 480×800 | W=60 H=100 | 8×8 |

Non-integer scale is safe (no crash) but leaves dead pixels at right/bottom edges. A compile-time `#warning` fires when `TFT_WIDTH % SPI_MATRIX_W != 0`.

## Key Design Decisions

### Board-init hook (`WLED_SPI_MATRIX_BOARD_INIT`)
The bus constructor calls `WLED_SPI_MATRIX_BOARD_INIT()` if defined, and marks the bus invalid if it returns false. For M5StickC this is `initAXP192`. Other boards define their own function or omit the flag entirely (constructor skips the hook). This keeps AXP192 code out of the generic bus path.

### AXP192 Early Init (M5StickC only)
The M5StickC uses an AXP192 PMIC. Without early init, the mic's unpowered CLK line pulls GPIO0 LOW (a strapping pin), forcing download mode. `initAXP192()` runs before `beginStrip()` and is idempotent. Compiled only when `WLED_SPI_MATRIX_AXP192` is defined.

### DMA Ping-Pong Buffers
Two DMA strip buffers alternate to overlap SPI transfer with pixel conversion. Buffer size is computed at init time from available DMA heap with a configurable cap (`SPI_MATRIX_DMA_BUDGET`, default 16KB).

### Lazy Buffer Allocation
DMA and snapshot buffers are allocated on first active `show()`, not in the constructor. This saves ~28KB when the TFT segment is off at boot.

### Integer Scaling
4x integer scaling maps virtual pixels to physical TFT pixels (e.g., 40x80 virtual → 160x80 physical). No floating-point, no interpolation — each virtual pixel becomes a `scaleX × scaleY` block.

## Hardware Tested

- M5StickC (ESP32-PICO-D4, ST7735S 80×160)
- M5StickC Plus (ESP32-PICO-D4, ST7789V2 135×240)

## Notes

- Build flag is opt-in; no impact on standard WLED builds
- No dependencies on PPP, SLIP, or ARGB features
## Related upstream issues

| Issue/PR | Repo | Title | Relevance |
|----------|------|-------|-----------|
| [#2197](https://github.com/wled/WLED/issues/2197) | Aircoookie/WLED | Framebuffer::GFX for 2D matrix output (closed) | Prior discussion of TFT-as-matrix; this PR implements the concept cleanly |
| [#1963](https://github.com/wled/WLED/issues/1963) | Aircoookie/WLED | Touch display support (closed/stale) | Related hardware integration pattern |
| [#4375](https://github.com/wled/WLED/issues/4375) | Aircoookie/WLED | TTGO-T-Display usermod failure (closed) | TFT display integration pain points this PR addresses at the bus level |
