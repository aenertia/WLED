# TFT Display as WLED Segment — Architecture Reference

## Concept

Use the M5StickC's built-in ST7735S 80x160 TFT as a WLED output target.
Each WLED "pixel" maps to a block of physical TFT pixels (4x4 or 5x5).
Effects, animations, and DDP data render on the screen alongside physical LED strips.

## WLED Bus Architecture (our extension point)

WLED uses a polymorphic bus hierarchy:
```
Bus (abstract base)
├── BusDigital      — WS2812B, SK6812 (via NeoPixelBus/RMT)
├── BusPwm          — Analog PWM
├── BusOnOff        — Binary relay
├── BusNetwork      — DDP/Art-Net/E131 over UDP
├── BusHub75Matrix  — HUB75 LED panels (I2S DMA) ← OUR MODEL
└── BusTFTMatrix    — TFT display output (NEW, SPI DMA)
```

Key virtual interface:
```cpp
virtual void     show()                                  = 0;
virtual void     setPixelColor(unsigned pix, uint32_t c) = 0;
virtual uint32_t getPixelColor(unsigned pix) const       { return 0; }
virtual bool     canShow() const                         { return true; }
```

BusManager dispatches by pixel index — bus just needs `containsPixel(pix)`.

## Type ID Slot

Types 72-79 are UNUSED between HUB75 (64-71) and virtual/network (80-95):
```cpp
#define TYPE_TFT_MATRIX_MIN  72
#define TYPE_TFT_MATRIX      72
#define TYPE_TFT_MATRIX_MAX  79
```

## Memory Budget — Why We Downscale

ESP32-PICO-D4 (no PSRAM): ~160-200KB usable SRAM after WLED.
MAX_LEDS without PSRAM: 1536. MAX_LED_MEMORY: 32KB.

| Resolution | Pixels | Seg buf (4B/px) | LED buf (3B/px) | RGB565 FB (2B/px) | Total |
|---|---|---|---|---|---|
| 80x160 native | 12,800 | 50 KB | 37.5 KB | 25 KB | ~114 KB IMPOSSIBLE |
| 40x80 (÷2) | 3,200 | 12.5 KB | 9.4 KB | 6.25 KB | ~28 KB TIGHT |
| **20x40 (÷4)** | **800** | **3.1 KB** | **2.3 KB** | **1.6 KB** | **~7 KB OK** |
| 16x32 (÷5) | 512 | 2 KB | 1.5 KB | 1 KB | ~4.5 KB OK |

**Recommended: 20x40 virtual pixels** (4x upscale to fill 80x160 physical).
800 pixels — well under the 1536 MAX_LEDS limit.
User confirmed: "single addressable pixels are unreadable anyway, blocks of 4 is fine."

## SPI + RMT Coexistence

No conflicts — different ESP32 peripherals:
- RMT: WS2812B LED output (G26)
- SPI: TFT display (G15 MOSI, G13 CLK, G5 CS, G23 DC, G18 RST)
- Proven by existing ST7789/TTGO usermods running alongside NeoPixelBus

## Implementation Pattern (follows BusHub75Matrix)

1. Double-buffered: CRGB array + dirty-bit tracking
2. `setPixelColor()`: write to buffer, set dirty bit
3. `show()`: iterate dirty pixels, draw scaled `fillRect()` to sprite, DMA push
4. Register in BusManager::add() under isTFT() type check
5. 2D matrix config in WLED UI maps linear pixels to grid automatically

## Build Flags

```ini
-D WLED_ENABLE_TFT_MATRIX
-D TFT_VIRTUAL_W=20
-D TFT_VIRTUAL_H=40
```

## Library Choice

LovyanGFX preferred over TFT_eSPI for M5StickC:
- Native M5StickC board definitions
- Better DMA support (`pushSpriteDMA()` for non-blocking transfers)
- Or skip sprite entirely and use direct `fillRect()` calls (saves 25KB framebuffer)

## Existing Precedent

- BusHub75Matrix: exact pattern (non-LED display as bus, dirty-bit buffer, show() flush)
- EleksTube IPS usermod: reads segment colors for display tinting (closest concept)
- ST7789_display usermod: SPI pin allocation alongside WLED (proves coexistence)
- NO existing TFT-as-bus implementation in WLED — this is novel
