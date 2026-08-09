# tft-bus-matrix - Work Plan

## TL;DR (For humans)

**What you'll get:** The M5StickC's built-in 80×160 TFT screen works as a WLED pixel matrix out of the box. Flash `m5stickc_ppp`, power on, connect via PPP — the WLED dashboard shows up and effects render on the screen as a 20×40 pixel grid. No LED strip needed to test. Physical LEDs on G26 are segment 2 (added later via web UI if wanted).

**Why this approach:** Follow the existing BusHub75Matrix pattern exactly — dirty-bit buffer, `setPixelColor()` writes to buffer, `show()` repaints only dirty pixels via SPI `fillRect()`. TFT_eSPI library (already in WLED ecosystem, proven with ST7789 usermod). 20×40 virtual pixels = 800 LEDs, 4×4 upscale to fill 80×160 physical — fits in 7KB RAM on the ESP32-PICO-D4 (no PSRAM).

**What it will NOT do:** No touch input, no UI overlay on TFT, no DMA sprite (direct fillRect is simpler and uses less RAM), no runtime resolution change.

**Effort:** Medium
**Risk:** Low — pattern is proven (BusHub75Matrix), library is proven (TFT_eSPI + ST7789 usermod), SPI+RMT coexistence is proven (different peripherals).
**Decisions to sanity-check:** TFT_eSPI over LovyanGFX (simpler, already in WLED ecosystem). TFT as segment 1 on ALL m5stickc envs (not just _tft variants) — every M5StickC has a screen.

Your next move: approve to begin. `$start-work tft-bus-matrix`

---

> TL;DR (machine): Medium effort, low risk. BusTFTMatrix class (20×40 pixel grid on ST7735S via TFT_eSPI), dirty-bit buffer, OOTB as default segment 1 on m5stickc builds. First flash target: m5stickc_ppp.

## Scope
### Must have
- TYPE_TFT_MATRIX constant in const.h (type ID 72)
- `isTFT()` predicate in Bus class
- BusTFTMatrix class: constructor (SPI init), setPixelColor (buffer + dirty), getPixelColor, show (dirty-only fillRect), cleanup
- Registration in BusManager::add() and getLEDTypesJSONString()
- TFT_eSPI library dependency in platformio_override.ini with ST7735 pin config
- Default bus config: TFT as segment 1 (800 pixels = 20×40), LED strip on G26 as segment 2 (60 LEDs)
- 2D matrix setup: `WLED_DEFAULT_2D` with 20 cols × 40 rows for the TFT segment
- m5stickc_ppp env compiles and boots with TFT showing effects OOTB

### Must NOT have (guardrails, anti-slop, scope boundaries)
- No DMA sprite buffer (saves 25KB RAM; direct fillRect sufficient at 20×40)
- No LovyanGFX (TFT_eSPI is simpler, already proven in WLED)
- No touch/gesture input from TFT
- No OSD/text overlay on TFT (pure pixel matrix)
- No runtime resolution change (fixed 20×40 at compile time)
- No modifications to BusHub75Matrix code (only use as pattern reference)

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: none (no test framework) — compile gate + grep assertions
- Compile: all 11 targets on koero via SSH (same pattern as previous sprints)
- Evidence: .omo/evidence/

## Execution strategy
### Parallel execution waves

**Wave 1 (parallel):** T1-T3 — constants, class definition, library config (different files)
**Wave 2 (sequential):** T4-T5 — class implementation, BusManager registration
**Wave 3 (parallel):** T6-T7 — default config + build env updates
**Wave 4 (gate):** T8 — compile all targets

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| T1 (const.h type IDs) | — | T4, T5 | T2, T3 |
| T2 (bus_manager.h class def) | — | T4, T5 | T1, T3 |
| T3 (TFT_eSPI lib + pin config) | — | T4 | T1, T2 |
| T4 (bus_tft_matrix.h implementation) | T1, T2, T3 | T5, T6 | — |
| T5 (BusManager registration) | T1, T2, T4 | T6 | — |
| T6 (default config: TFT as segment 1) | T5 | T8 | T7 |
| T7 (merge TFT into all m5stickc envs) | T3 | T8 | T6 |
| T8 (compile gate) | T6, T7 | — | — |

## Todos
> Implementation + Test = ONE todo. Never separate.

### Wave 1 — Constants, Class Skeleton, Library (parallel)

- [ ] 1. const.h: add TYPE_TFT_MATRIX constants
  What to do: In wled00/const.h, after `TYPE_HUB75MATRIX_MAX 71` (line ~383), add:
  ```cpp
  #define TYPE_TFT_MATRIX_MIN  72
  #define TYPE_TFT_MATRIX      72  // ST7735/ST7789/ILI9341 SPI TFT as pixel matrix
  #define TYPE_TFT_MATRIX_MAX  79
  ```
  In the Bus class static predicates section of bus_manager.h (near `isHub75`), add:
  ```cpp
  static constexpr bool  isTFT(uint8_t type)       { return (type >= TYPE_TFT_MATRIX_MIN && type <= TYPE_TFT_MATRIX_MAX); }
  ```
  Must NOT change any existing type ID ranges.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T4, T5
  References: const.h:380-383 (HUB75 type range), bus_manager.h:197 (isHub75 predicate), refs/tft-display-segment.md §Type ID Slot
  Acceptance criteria: `grep TYPE_TFT_MATRIX wled00/const.h` returns 3 matches. `grep isTFT wled00/bus_manager.h` returns 1 match.
  Commit: N (batched)

- [ ] 2. bus_manager.h: add BusTFTMatrix class definition
  What to do: After the BusHub75Matrix class (line ~452), add the BusTFTMatrix class under `#ifdef WLED_ENABLE_TFT_MATRIX`. Follow BusHub75Matrix's pattern exactly:
  ```cpp
  #ifdef WLED_ENABLE_TFT_MATRIX
  class BusTFTMatrix : public Bus {
    public:
      BusTFTMatrix(const BusConfig &bc);
      ~BusTFTMatrix() { cleanup(); }
      void setPixelColor(unsigned pix, uint32_t c) override;
      uint32_t getPixelColor(unsigned pix) const override;
      void show() override;
      void setBrightness(uint8_t b) override;
      size_t getPins(uint8_t* pinArray = nullptr) const override;
      void cleanup();
      static std::vector<LEDType> getLEDTypes();
    private:
      uint16_t _panelWidth;
      uint16_t _panelHeight;
      uint8_t _scaleX;
      uint8_t _scaleY;
      uint32_t *_ledBuffer;
      byte *_ledsDirty;
  };
  #endif
  ```
  Must NOT modify BusHub75Matrix. Must be inside `#ifdef WLED_ENABLE_TFT_MATRIX`.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T4, T5
  References: bus_manager.h:418-452 (BusHub75Matrix class for pattern), refs/tft-display-segment.md §Implementation Pattern
  Acceptance criteria: `grep 'class BusTFTMatrix' wled00/bus_manager.h` returns 1 match.
  Commit: N (batched)

- [ ] 3. platformio_override.ini: add TFT_eSPI library dependency and ST7735 pin config
  What to do: In the `[m5stickc_tft_flags]` section (line ~75), add TFT_eSPI build flags for the M5StickC's ST7735S display:
  ```ini
  [m5stickc_tft_flags]
  build_flags =
    -D WLED_ENABLE_TFT_MATRIX
    -D TFT_VIRTUAL_W=20
    -D TFT_VIRTUAL_H=40
    -D USER_SETUP_LOADED
    -D ST7735_DRIVER
    -D TFT_WIDTH=80
    -D TFT_HEIGHT=160
    -D TFT_MOSI=15
    -D TFT_SCLK=13
    -D TFT_CS=5
    -D TFT_DC=23
    -D TFT_RST=18
    -D SPI_FREQUENCY=27000000
    -D TFT_RGB_ORDER=TFT_BGR
  lib_deps =
    bodmer/TFT_eSPI@^2.5.0
  ```
  Also add `${m5stickc_tft_flags.build_flags}` and `${m5stickc_tft_flags.lib_deps}` to the BASE m5stickc_ppp_board section so ALL m5stickc builds get TFT support. Remove the separate `_tft` variant envs (m5stickc_ppp_tft, m5stickc_pico_tft) since TFT is now always-on for m5stickc.
  Must NOT change S3 or esp32dev/wrover/c3 envs (they don't have this TFT).
  Parallelization: Wave 1 | Blocked by: — | Blocks: T4
  References: platformio_override.ini:75-80 (current tft_flags stub), refs/m5stickc-hardware.md:43 (SPI pins), refs/tft-display-segment.md §Build Flags, usermods/ST7789_display (TFT_eSPI precedent)
  Acceptance criteria: `grep TFT_eSPI platformio_override.ini` returns 1+ match. `grep ST7735_DRIVER platformio_override.ini` returns 1 match.
  Commit: N (batched)

### Wave 2 — Implementation (sequential)

- [ ] 4. bus_tft_matrix.h + bus_manager.cpp: full BusTFTMatrix implementation
  What to do: Replace the stub in wled00/bus_tft_matrix.h with the full implementation. Include `<TFT_eSPI.h>` and `<Wire.h>`. Pattern follows BusHub75Matrix exactly:
  
  **Constructor — AXP192 PMIC init FIRST (critical)**: The M5StickC's TFT is powered by the AXP192 PMIC via I2C. LDO2 = backlight, LDO3 = TFT logic power. Without enabling these rails, the TFT's SPI bus is dead. Init sequence:
  ```cpp
  // AXP192 at 0x34 on Wire1 (GPIO21 SDA, GPIO22 SCL)
  Wire1.begin(21, 22);
  Wire1.setClock(400000);
  // Helper: write one byte to AXP192
  auto axpWrite = [](uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(0x34);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
  };
  auto axpRead = [](uint8_t reg) -> uint8_t {
    Wire1.beginTransmission(0x34);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)0x34, (uint8_t)1);
    return Wire1.read();
  };
  axpWrite(0x28, 0xCC);                              // LDO2+LDO3 = 3.0V
  axpWrite(0x12, axpRead(0x12) | 0x0C);              // Enable LDO2 (bit2) + LDO3 (bit3)
  delay(10);                                          // Power rail stabilization
  ```
  THEN init TFT_eSPI, set rotation (1 for landscape or 0 for portrait — M5StickC is 80×160 portrait, rotation=0 keeps it natural), fill black, allocate buffers.
  
  **Constructor continued**: allocate `_ledBuffer` (uint32_t × _len) and `_ledsDirty` (bit array). `_panelWidth = TFT_VIRTUAL_W`, `_panelHeight = TFT_VIRTUAL_H`, `_scaleX = TFT_WIDTH / TFT_VIRTUAL_W` (= 4), `_scaleY = TFT_HEIGHT / TFT_VIRTUAL_H` (= 4).
  
  **setPixelColor**: Same as BusHub75Matrix — compare with buffer, set dirty bit if changed:
  ```cpp
  void BusTFTMatrix::setPixelColor(unsigned pix, uint32_t c) {
    if (!_valid || pix >= _len) return;
    if (_ledBuffer[pix] != c) {
      _ledBuffer[pix] = c;
      setBitInArray(_ledsDirty, pix, true);
    }
  }
  ```
  
  **getPixelColor**: Return from `_ledBuffer[pix]`.
  
  **show**: Iterate dirty pixels, draw scaled fillRect:
  ```cpp
  void BusTFTMatrix::show() {
    if (!_valid) return;
    for (unsigned pix = 0; pix < _len; pix++) {
      if (getBitFromArray(_ledsDirty, pix)) {
        uint32_t c = _ledBuffer[pix];
        uint16_t x = (pix % _panelWidth) * _scaleX;
        uint16_t y = (pix / _panelWidth) * _scaleY;
        uint16_t color565 = ((R(c) & 0xF8) << 8) | ((G(c) & 0xFC) << 3) | (B(c) >> 3);
        _tft->fillRect(x, y, _scaleX, _scaleY, color565);
      }
    }
    setBitArray(_ledsDirty, _len, false);
  }
  ```
  
  **setBrightness**: Map WLED brightness (0-255) to AXP192 LDO2 voltage (2500-3300mV) via register 0x28 upper nibble:
  ```cpp
  void BusTFTMatrix::setBrightness(uint8_t b) {
    _bri = b;
    int vol = map(b, 0, 255, 2500, 3300);
    int val = (vol - 1800) / 100;
    uint8_t reg = axpRead(0x28);
    axpWrite(0x28, (reg & 0x0F) | (val << 4));
  }
  ```
  Store the `axpWrite`/`axpRead` lambdas as static helpers or inline the I2C calls.
  
  **cleanup**: Free buffers, end TFT.
  
  **getLEDTypes**: Return `{{TYPE_TFT_MATRIX, "", PSTR("TFT Matrix (SPI)")}}`.
  
  The TFT_eSPI instance should be a static singleton (one TFT per device). Declare it in bus_tft_matrix.h.
  
  Add the implementation to bus_manager.cpp after the BusHub75Matrix section, inside `#ifdef WLED_ENABLE_TFT_MATRIX`.
  
  Must NOT use DMA sprites (saves 25KB). Must NOT allocate PSRAM (M5StickC has none). Must use `fillRect` for upscaling, not per-pixel `drawPixel`. Must respect `BFRALLOC_NOBYTEACCESS` for `_ledBuffer` (use uint32_t).
  Parallelization: Wave 2 | Blocked by: T1, T2, T3 | Blocks: T5, T6
  References: bus_manager.cpp:820-1231 (BusHub75Matrix implementation — exact pattern), bus_manager.h:418-452 (BusHub75Matrix class), refs/tft-display-segment.md (full design), refs/m5stickc-hardware.md:43 (SPI pins)
  Acceptance criteria: `grep 'BusTFTMatrix::show' wled00/bus_manager.cpp` returns 1 match. `grep fillRect wled00/bus_manager.cpp` returns 1+ match in TFT section. `grep TFT_eSPI wled00/bus_tft_matrix.h` returns 1+ match.
  Commit: N (batched)

- [ ] 5. bus_manager.cpp: register BusTFTMatrix in BusManager::add() and getLEDTypesJSONString()
  What to do: In BusManager::add() (bus_manager.cpp ~1297-1309), add a TFT branch after the Hub75 branch:
  ```cpp
  #ifdef WLED_ENABLE_TFT_MATRIX
  } else if (Bus::isTFT(bc.type)) {
    busses.push_back(make_unique<BusTFTMatrix>(bc));
  #endif
  ```
  In getLEDTypesJSONString() (bus_manager.cpp ~1327-1339), add:
  ```cpp
  #ifdef WLED_ENABLE_TFT_MATRIX
  json += LEDTypesToJson(BusTFTMatrix::getLEDTypes());
  #endif
  ```
  Must NOT modify existing Hub75 or Network bus registration.
  Parallelization: Wave 2 | Blocked by: T1, T2, T4 | Blocks: T6
  References: bus_manager.cpp:1297-1309 (BusManager::add, Hub75 branch pattern), bus_manager.cpp:1327-1339 (getLEDTypesJSONString)
  Acceptance criteria: `grep isTFT wled00/bus_manager.cpp` returns 1+ match. `grep BusTFTMatrix wled00/bus_manager.cpp` returns 3+ matches (add, getLEDTypes, constructor call).
  Commit: N (batched)

### Wave 3 — Default Config (parallel)

- [ ] 6. platformio_override.ini + cfg.cpp: configure TFT as default segment 1 OOTB
  What to do: For the m5stickc builds, set the default bus config so TFT is segment 1 and LED strip is segment 2. In the m5stickc hardware flags, change:
  ```ini
  ; Segment 1: TFT (20x40 = 800 pixels), Segment 2: LED strip (60 pixels on G26)
  -D LED_TYPES=TYPE_TFT_MATRIX,TYPE_WS2812_RGB
  -D DATA_PINS=255,26
  -D PIXEL_COUNTS=800,60
  -D DEFAULT_LED_COUNT=860
  ```
  Pin 255 means "no GPIO pin" for the TFT bus (it uses SPI pins defined separately). The TFT bus constructor ignores the pin from BusConfig and uses its own SPI config.
  
  Also set 2D matrix defaults for the TFT segment:
  ```ini
  -D WLED_DEFAULT_2D
  -D DEFAULT_LED_MATRIX_WIDTH=20
  -D DEFAULT_LED_MATRIX_HEIGHT=40
  ```
  
  Must NOT change the S3 or classic ESP32 envs.
  Parallelization: Wave 3 | Blocked by: T5 | Blocks: T8
  References: cfg.cpp:9-19 (PIXEL_COUNTS/DATA_PINS/LED_TYPES defaults), cfg.cpp:264-346 (default bus creation loop), refs/tft-display-segment.md §Memory Budget (800px = 7KB total)
  Acceptance criteria: `grep 'TYPE_TFT_MATRIX,TYPE_WS2812_RGB' platformio_override.ini` returns 1+ match. `grep 'PIXEL_COUNTS=800,60' platformio_override.ini` returns 1+ match.
  Commit: N (batched)

- [ ] 7. Remove separate _tft variant envs (now redundant)
  What to do: Since TFT is integrated into the base m5stickc envs, remove the separate `[env:m5stickc_ppp_tft]` and `[env:m5stickc_pico_tft]` environment definitions from platformio_override.ini. The `[m5stickc_tft_flags]` section stays (it defines the TFT_eSPI build flags that the base envs now include).
  Must NOT remove the `[m5stickc_tft_flags]` section itself.
  Parallelization: Wave 3 | Blocked by: T3 | Blocks: T8
  References: platformio_override.ini:134-165 (env:m5stickc_ppp_tft and env:m5stickc_pico_tft)
  Acceptance criteria: `grep 'env:m5stickc_ppp_tft\|env:m5stickc_pico_tft' platformio_override.ini` returns 0 matches.
  Commit: N (batched)

### Wave 4 — Compile Gate

- [ ] 8. Compile gate: all m5stickc targets + S3 + classic targets on koero
  What to do: Push all changes, pull on koero, compile. The m5stickc targets now have TFT_eSPI dependency and TFT bus code. S3 and classic targets should be unaffected (no WLED_ENABLE_TFT_MATRIX flag). Target list after removing _tft variants: m5stickc_ppp, m5stickc_pico, atoms3_lean, atoms3u_lean, stamps3_lean, esp32s3_8m_lean, esp32dev_ppp, esp32_wrover_ppp, esp32c3_ppp (9 targets).
  Acceptance criteria: All 9 targets SUCCESS.
  Commit: Y | feat(tft): BusTFTMatrix — M5StickC TFT as 20×40 WLED pixel matrix OOTB

## Final verification wave
- [ ] F1. Plan compliance — all ADR items from refs/tft-display-segment.md addressed
- [ ] F2. Code quality — follows BusHub75Matrix pattern, no heap allocs in hot path, uint32_t buffer access
- [ ] F3. OOTB verification — m5stickc_ppp build has TYPE_TFT_MATRIX as default segment 1 without any config file
- [ ] F4. Scope fidelity — no DMA sprite, no LovyanGFX, no touch input, no _tft variant envs remaining

## Commit strategy
1 commit after compile gate: `feat(tft): BusTFTMatrix — M5StickC TFT as 20×40 WLED pixel matrix OOTB`

## Success criteria
- m5stickc_ppp compiles clean with TFT_eSPI + BusTFTMatrix
- Boot without cfg.json → TFT is segment 1 (800 px, 20×40), LED strip is segment 2 (60 px, G26)
- Effects render on TFT via dirty-pixel fillRect — no physical LED strip needed
- S3 and classic ESP32 targets compile clean without TFT code (no WLED_ENABLE_TFT_MATRIX)
- RAM budget: _ledBuffer (3.2KB) + _ledsDirty (100B) + TFT_eSPI instance (~1KB) = ~4.3KB total
