# ADR: Compression-Friendly Internal Pixel Operations for WLED

## Status
Proposed — research phase

## Context

WLED's DDP compression extension (delta+RLE, see `refs/compressed-ddp.md`) achieves 3–200× bandwidth reduction for sparse pixel changes. However, analysis of the 217 registered effects in `FX.cpp` reveals that ~60% of animated effects use `fade_out()`, `fadeToBlackBy()`, or `blur()` — functions that touch EVERY pixel per frame, destroying delta sparsity and reducing compression to ~1:1.

This ADR analyzes whether these functions can be reimplemented in a compression-friendly way while preserving identical visual output, constrained by:

- **(a)** Data structure integrity — pixel buffer invariants, segment compositing pipeline
- **(b)** ESP32 memory constraints — 50–80 KB free heap typical, 320 KB DRAM total
- **(c)** ESP-IDF RTOS architecture — FreeRTOS task model, RMT/I2S DMA paths, cache behavior
- **(d)** Wire size — delta+RLE and raw RLE compression ratios

## 1. Pixel Buffer Architecture

### 1.1 Three-Buffer Pipeline

WLED uses a three-level buffer pipeline, NOT a single framebuffer:

```
Effects Engine          Compositor           LED Driver
     │                     │                     │
     ▼                     ▼                     ▼
Segment::pixels[]  →  WS2812FX::_pixels[]  →  NeoPixelBus buffer
(per-segment)          (global frame)          (DMA-ready, internal DRAM)
     │                     │                     │
 Written by:           Built by:             Written by:
 effect functions      blendSegment()        BusDigital::setPixelColor()
 fade_out()            memset(0) first       Read by: RMT ISR or I2S DMA
 blur()
```

**Source references:**
- `Segment::pixels` — `FX.h:474`, allocated at `FX.h:598` with `BFRALLOC_PREFER_PSRAM | BFRALLOC_NOBYTEACCESS`
- `WS2812FX::_pixels` — `FX.h:1015`, allocated at `FX_fcn.cpp:1302` with `BFRALLOC_ENFORCE_PSRAM | BFRALLOC_NOBYTEACCESS`
- `setPixelColorRaw()` — `FX.h:535`: `pixels[i] = c;` (direct uint32_t write, no function call overhead)
- `getPixelColorRaw()` — `FX.h:536`: `return pixels[i];` (direct read)

### 1.2 Critical: `_pixels[]` Is Rebuilt Every Frame

In `WS2812FX::show()` (`FX_fcn.cpp:1726`):

```cpp
// line 1746: CLEAR entire global buffer every frame
memset(_pixels, 0, sizeof(uint32_t) * totalLen);
// line 1748-1750: rebuild from all active segments
for (Segment &seg : _segments)
    if (seg.isActive()) blendSegment(seg);
```

The global frame buffer is **zeroed and recomposited** every frame. This means:
- Delta between consecutive `_pixels[]` frames reflects actual visual change
- The segment buffer (`Segment::pixels[]`) is where persistent effect state lives
- Effects write to segment buffer; the compositor reads it

### 1.3 Paint Loop

After compositing, `WS2812FX::show()` paints to bus hardware (`FX_fcn.cpp:1764`):

```cpp
for (size_t i = 0; i < totalLen; i++) {
    uint32_t c = _pixels[i];  // read from global buffer
    if (c > 0 && useGammaCorrection) c = gamma32(c);
    BusManager::setPixelColor(getMappedPixelIndex(i), c);  // write to NeoPixelBus buffer
}
BusManager::show();  // trigger DMA/RMT transmission
```

## 2. Pixel Mutation Surface — Quantitative Analysis

### 2.1 Effect Function Counts (FX.cpp)

| Mutation Type | Call Count | What It Does | Touches All Pixels? |
|---|---|---|---|
| `SEGMENT.setPixelColor()` | 212 | Set individual pixel | No — sparse |
| `SEGMENT.fill()` | 45 | Fill segment with single color | Yes — but uniform value |
| `SEGMENT.fade_out()` | 27 | Fade toward background color | **Yes — per-pixel different delta** |
| `SEGMENT.fadeToBlackBy()` | 24 | Scale toward black | **Yes — per-pixel different delta** |
| `SEGMENT.blur()` | 27 | Neighbor-averaging convolution | **Yes — read-modify-write all** |
| `SEGMENT.blurRows/blurCols` | 1 | 2D blur variant | Yes |
| `for (i < SEGLEN)` loops | 90 | Full-strip iteration | Yes — per-pixel computation |
| Total registered effects | 217 | via `addEffect()` | |

### 2.2 Mutation Pattern Classification (219 effects)

Exhaustive categorization of all registered effects by their per-frame pixel mutation pattern:

| Category | Count | % | Pattern | Compression | Examples |
|---|---|---|---|---|---|
| **Per-pixel loop** | 52 | 24% | For-loop writing every pixel with unique computed color | Hostile (~1:1) | Rainbow cycle, Fire 2012, Noise, Plasma |
| **Delegates** | 48 | 22% | Calls another mode function | Inherits target | Chase variants delegate to `chase()` |
| **Fade/blur + sparse** | 38 | 17% | `fade_out`/`fadeToBlackBy`/`blur` ALL pixels, then sparse overwrites | **Hostile (~1.5:1)** — PRIMARY TARGET | Twinkle, Meteor, Fireworks, Gravimeter |
| **Fill + sparse** | 36 | 16% | `fill()` uniform color, then sparse overwrites | Mixed (raw RLE good, delta poor) | Chase, Wipe, Scanner |
| **Particle system** | 31 | 14% | Renders sparse particles on faded background | Friendly with deferred fade | Particle effects, Starburst |
| **Fill only** | 7 | 3% | Only `fill()` — single color | Maximally compressible (225:1) | Solid, Static |
| **Sparse writes** | 7 | 3% | Only writes to specific positions | Friendly (60:1+) | Simple sparkle |

### 2.3 Impact Summary

**38 effects (17%) in "fade/blur + sparse" are the primary optimization target.** These effects WANT sparse pixel changes (twinkle = fade background + light a few pixels) but `fade_out()` makes the entire buffer dirty. The visual intent IS sparse; the implementation is not.

**31 particle effects (14%) also benefit** — they use fade for trails, then render sparse particles.

**52 per-pixel loop effects (24%) are inherently incompressible** regardless of fade changes — they recompute every pixel from mathematical functions. No optimization helps these.

### 2.4 Existing Precedent: HUB75 Dirty Tracking

WLED already has pixel-level dirty tracking for ONE bus type — `BusHub75Matrix` (`bus_manager.cpp:1059-1197`):

```cpp
void BusHub75Matrix::setPixelColor(unsigned pix, uint32_t c) {
    if (_ledBuffer[pix] != fastled_col) {
        _ledBuffer[pix] = fastled_col;
        setBitInArray(_ledsDirty, pix, true);  // flag as dirty
    }
}

void BusHub75Matrix::show(void) {
    for (...) {
        if (getBitFromArray(_ledsDirty, pix)) {  // ONLY repaint dirty pixels
            drawPixelRGB888(...);
        }
    }
    setBitArray(_ledsDirty, _len, false);  // reset
}
```

This proves the concept works within WLED's architecture. Extending dirty tracking to the segment buffer or the global `_pixels[]` frame buffer would benefit compression and bus output performance alike.

## 3. ESP32 Hardware Constraints

### 3.1 Memory Budget

| Parameter | Value | Source |
|---|---|---|
| Total internal SRAM | 520 KB (320 DRAM + 200 IRAM) | ESP-IDF Memory Types |
| DRAM heap at boot | ~311 KB | ESP-IDF heap_init |
| Free heap in WLED (typical) | 50–80 KB with WiFi | Community reports |
| Pixel buffer (1000 LEDs) | 4 KB (1000 × uint32_t) | `FX.h:598` |
| Internal SRAM access | ~4.2 ns (1 cycle @ 240 MHz) | Direct bus, no cache |
| PSRAM access (cache miss) | ~120–200 ns | SPI @ 80 MHz |
| PSRAM cache line | 32 bytes | ESP32 TRM |
| DMA can access PSRAM? | **No** | ESP-IDF docs |

### 3.2 RMT and I2S DMA Behavior

**RMT (ESP32 original): No DMA.** Uses interrupt-driven ping-pong encoding from user buffer. The user buffer (pixel data) is **read by ISR during transmission** (~31 ms for 1024 LEDs). Buffer must not be modified during this window.

**I2S/NeoPixelBus: Copy-then-transmit.** NeoPixelBus allocates its own DMA buffer in internal DRAM, encodes pixel data into it, then DMA transmits. The pixel buffer is only read during the CPU-bound encoding phase. After encoding, the pixel buffer is free.

**Implication:** Any deferred pixel transform must be resolved BEFORE the paint loop reads `_pixels[]`. The RMT ISR reads the NeoPixelBus buffer (not `_pixels[]`), so the constraint is on the compositor/paint path, not DMA.

### 3.3 RTOS Task Model

| Task | Priority | Core | Relevance |
|---|---|---|---|
| WiFi driver | 23 | Core 0 | DDP/E1.31 packets arrive here |
| lwIP TCP/IP | 18 | Unpinned | UDP callback for DDP |
| WLED main loop (`loopTask`) | 1 | Core 1 | Effects + show() + paint |

WLED is **single-threaded cooperative** on Core 1. Effects, compositing, painting, and bus output all run sequentially in the same task. No mutex needed on the pixel buffer. Network-received pixels (DDP, E1.31) arrive via UDP callback on Core 0/lwIP task and write to `_pixels[]` via `setRealtimePixel()` — this is the one cross-core access point, protected by `realtimeLock()` (logical flag, not mutex).

### 3.4 `BFRALLOC_NOBYTEACCESS` Constraint

On classic ESP32, pixel buffers use the 32-bit-only IRAM region (`MALLOC_CAP_32BIT`) which **cannot be byte-accessed** — only 32-bit aligned word reads/writes are valid. This is why `_pixels[]` is `uint32_t*` and why `WS2812FX::show()` copies to a local variable before extracting bytes:

```cpp
uint32_t c = _pixels[i]; // word read — OK
// NOT: uint8_t r = ((uint8_t*)_pixels)[i*4]; // byte read — CRASH on 32-bit-only region
```

Any deferred fade implementation must respect this: transforms on `pixels[]` must operate on `uint32_t` values, not individual bytes.

## 4. The `fade_out()` Problem in Detail

### 4.1 Current Implementation (`FX_fcn.cpp:1063`)

```cpp
void Segment::fade_out(uint8_t rate) const {
    rate = (256-rate) >> 1;
    const int mappedRate = 256 / (rate + 1);
    for (unsigned j = 0; j < rlength; j++) {
        uint32_t color = getPixelColorRaw(j);
        if (color == colors[1]) continue;           // skip if already at target
        for (int i = 0; i < 32; i += 8) {
            uint8_t c2 = (colors[1]>>i);            // target channel
            uint8_t c1 = (color>>i);                // current channel
            int delta = (c2 - c1) * mappedRate / 256;
            if (delta == 0)
                delta += (c2 == c1) ? 0 : (c2 > c1) ? 1 : -1;  // FORCE ±1
            color = (color & ~(0xFF<<i)) | ((c1 + delta) & 0xFF) << i;
        }
        setPixelColorRaw(j, color);
    }
}
```

**Line 1077 is the compression killer:** `delta += ... ? 1 : -1` forces every non-target pixel to change by at least 1 LSB per frame, regardless of the proportional fade rate. This ensures fade always progresses but makes EVERY pixel dirty for delta compression.

### 4.2 `fadeToBlackBy()` (`FX_fcn.cpp:1094`)

```cpp
void Segment::fadeToBlackBy(uint8_t fadeBy) const {
    for (unsigned i = 0; i < rlength; i++)
        setPixelColorRaw(i, fast_color_scale(getPixelColorRaw(i), 255-fadeBy));
}
```

Same pattern: touches every pixel. `fast_color_scale()` is a uniform operation (multiply by scale factor) but produces per-pixel-different results because each pixel has a different starting color.

### 4.3 `blur()` (`FX_fcn.cpp:1100+`)

Reads each pixel and its neighbors, computes a weighted average, writes back. Every pixel is read and written. Additionally, blur is a spatial convolution — the output of pixel N depends on pixel N-1 and N+1, making it inherently sequential and non-deferrable without buffering.

## 5. Proposed Solutions

### 5.1 Level 1: Snap-to-Target (5 lines, zero visual change)

**Change:** When proportional fade delta rounds to zero, snap to target instead of forcing ±1.

```cpp
// Current:
if (delta == 0) delta += (c2 == c1) ? 0 : (c2 > c1) ? 1 : -1;

// Proposed:
if (delta == 0) c1 = c2;  // snap to target
```

**Visual impact:** Pixels within ~17/255 (6.7%) of target snap instantly instead of counting down one-by-one over 17 frames. At 30fps, this is 560ms of sub-7% brightness change compressed to 33ms — below human JND (just-noticeable difference) under typical LED viewing conditions.

**Compression impact:** After a few frames, most trail pixels reach exact target → `if (color == colors[1]) continue` skips them → delta becomes sparse. Chase with fade: ~1.5:1 → ~30:1.

**Memory cost:** 0 bytes. **CPU cost:** Slightly less (fewer iterations per pixel).

**Risk:** Effects that rely on exact fade timing for visual synchronization could behave subtly differently. The tail of the fade is 17 frames shorter for the dimmest pixels. Mitigation: configurable threshold or compile-time option.

### 5.2 Level 2: Deferred Fade (moderate refactor, zero visual change)

**Concept:** Instead of `fade_out()` eagerly modifying every pixel, store fade parameters on the segment and apply the fade lazily when pixels are read.

**Architecture:**

```
Current:
  fade_out() → reads+modifies ALL pixels → buffer dirty everywhere
  effect()  → sets a few pixels
  getPixelColorRaw() → returns stored value

Proposed:
  fade_out() → stores _fadeScale, increments _fadeGen (O(1), no pixel writes)
  effect()   → sets a few pixels, marks their _pixelGen = _fadeGen (sparse writes)
  getPixelColorRaw() → applies deferred fade: blend(stored, target, accumulated_fade)
```

**Segment additions (2 bytes per segment, not per pixel):**
```cpp
// In Segment private:
uint8_t _fadeAccum;     // accumulated fade factor (0 = no pending fade)
uint8_t _fadeGenCount;  // generation counter, incremented each fade_out() call
```

**Modified `fade_out()`:**
```cpp
void Segment::fade_out(uint8_t rate) const {
    uint8_t scaledRate = /* existing rate computation */;
    _fadeAccum = blend8(_fadeAccum, scaledRate);  // accumulate
    // NO pixel writes — buffer untouched
}
```

**Modified `getPixelColorRaw()`:**
```cpp
inline uint32_t getPixelColorRaw(unsigned i) const {
    uint32_t c = pixels[i];
    if (_fadeAccum == 0) return c;
    return color_blend(c, colors[1], _fadeAccum);  // apply deferred fade on read
}
```

**Flush point:** Before `blendSegment()` reads the segment buffer, or at the start of each frame, the deferred fade must be resolved. Two options:

- **Option A (lazy):** `getPixelColorRaw()` applies fade on every read. Cost: one `color_blend()` per pixel read (~50ns on ESP32 at 240MHz). For 300 LEDs read once: 15µs. Current eager `fade_out()` costs ~45µs (read+compute+write). Net win.

- **Option B (epoch):** At the start of each frame, if `_fadeAccum > threshold`, materialize the fade into the buffer and reset `_fadeAccum`. This periodically "flushes" the deferred state, keeping `getPixelColorRaw()` fast for effects that read many pixels.

**Compression impact:** Delta between frames sees ONLY the effect's explicit `setPixelColor()` writes. For twinkle (1-5 pixels written per frame): delta has 1-5 non-zero pixels → ~100:1 compression.

**Memory cost:** 2 bytes per segment (not per pixel). For 12 max segments: 24 bytes total.

**Limitations:**
- `blur()` reads neighbors and writes blended results — deferred fade must be resolved before blur, or blur must account for deferred state. This adds complexity.
- Effects that read pixel colors to make decisions (e.g., `if (getPixelColor(i) < threshold)`) would see faded values, which IS the correct behavior.
- The `BFRALLOC_NOBYTEACCESS` constraint is respected — `color_blend()` operates on `uint32_t`.

### 5.3 Level 3: Transform Compression Type (protocol extension)

**Concept:** New DDP compression type that describes a uniform pixel transform + sparse explicit writes, instead of per-pixel delta.

```
DDP_COMP_TYPE_TRANSFORM = 0x30  // byte 1 upper nibble

Payload format:
  [1B: transform_op]     0x01 = scale toward target
  [1B: scale_factor]     e.g., 240 = multiply by 240/256
  [3-4B: target_color]   RGB(W) target for the scale
  [2B: num_explicit]     count of explicit pixel writes
  [per write: 2B index + 3-4B color]  × num_explicit
```

**Example:** Chase effect with `fade_out(224)` on 300 LEDs:
- Transform: scale=240, target=BLACK → 5 bytes
- 2 explicit chase pixels: 2 × 5 bytes = 10 bytes
- **Total: 17 bytes** vs 900 raw = **53:1**

**vs Delta+RLE on same data:** ~1.5:1 (fade makes all pixels dirty)

**This is the only approach that correctly captures the semantic intent of "fade everything, then draw a few pixels."**

**Implementation cost:** Requires sender (Pico or ESP32 BusNetwork) to recognize uniform-transform-then-sparse-write patterns. Requires receiver to apply transform before explicit writes. Protocol extension to `DDP_COMP_TYPE_*`.

## 6. `fadeToBlackBy()` Analysis

`fadeToBlackBy()` is simpler than `fade_out()` — it always fades toward black, and uses `fast_color_scale()` which is a pure multiplication. This makes it even more amenable to deferred execution:

```cpp
// Deferred fadeToBlackBy:
void Segment::fadeToBlackBy(uint8_t fadeBy) const {
    _scaleAccum = scale8(_scaleAccum, 255 - fadeBy);  // accumulate multiplier
    // no pixel writes
}

// Resolved in getPixelColorRaw:
inline uint32_t getPixelColorRaw(unsigned i) const {
    uint32_t c = pixels[i];
    if (_scaleAccum < 255) c = fast_color_scale(c, _scaleAccum);
    return c;
}
```

**Memory cost:** 1 byte per segment for `_scaleAccum`.

## 7. `blur()` — Cannot Be Deferred

Unlike fade, `blur()` is a **spatial convolution**: each pixel's new value depends on its neighbors' current values. This creates a read-write dependency chain that cannot be deferred without buffering the entire result.

**Compression strategy for blur:** Accept that blur dirties all pixels. Use raw RLE (not delta) for frames after blur — blurred images often have spatial runs of similar values that RLE can exploit. The sender should adaptively choose delta+RLE vs raw RLE.

## 8. Decision Matrix

| Approach | Visual Change | Memory Cost | CPU Impact | Compression Gain | Complexity |
|---|---|---|---|---|---|
| **Level 1: Snap** | Imperceptible | 0 | Slight speedup | 1.5:1 → 30:1 for fade effects | 5 lines |
| **Level 2: Deferred fade** | None | 2-3B/segment | Neutral to faster | 1.5:1 → 100:1 for fade effects | ~80 lines |
| **Level 3: Transform type** | None | 0 on device | N/A (protocol) | 1.5:1 → 53:1 on wire | Protocol + sender + receiver |
| Adaptive raw/delta RLE | None | prev-frame buffer | Extra encode pass | Handles blur/fire at 2-3:1 | ~30 lines sender |

## 9. Recommendation

**Implement in order:**

1. **Level 1 now** — 5-line snap-to-target in `fade_out()`. Zero risk, immediate benefit.
2. **Level 2 next** — Deferred fade for `fade_out()` and `fadeToBlackBy()`. Architecturally clean, measurably faster (fewer memory writes per frame), and makes delta compression viable for ~50 effects.
3. **Level 3 later** — Transform compression type, when the Pico compressor is implemented. The Pico can detect uniform-transform patterns in the DDP stream.
4. **Adaptive RLE always** — Sender tries both delta+RLE and raw RLE, picks the smaller. Handles blur/fire/noise cases where neither deferred fade nor snap helps.

## 10. ESP-IDF/RTOS Implications

### 10.1 No Cross-Task Concerns

All pixel operations (effects, fade, compositing, painting) run in the same FreeRTOS task (`loopTask`, priority 1, Core 1). No mutex or atomic operations needed for deferred fade state. The `_fadeAccum` field is read and written by the same task.

### 10.2 Cache-Friendly Access

The deferred fade adds one `color_blend()` per pixel read in `getPixelColorRaw()`. This is a multiply-and-shift operation on a `uint32_t` — no memory access beyond the pixel itself. The computation is ALU-bound, not memory-bound. On ESP32's Xtensa LX6 (no hardware multiply for 32-bit?), `color_blend` uses shifts and adds — still ~50ns per pixel.

If the pixel buffer is in PSRAM, the deferred approach is BETTER than eager: eager fade does read+write (two PSRAM accesses per pixel), deferred does read+compute (one PSRAM access + ALU). Saves ~120ns per pixel for PSRAM-resident buffers.

### 10.3 32-Bit Access Requirement

The deferred fade respects `BFRALLOC_NOBYTEACCESS`: all operations on `pixels[]` use `uint32_t` reads/writes. The `color_blend()` function operates on `uint32_t` values using bit shifts and masks — no byte-level access.

### 10.4 Stack Budget

The deferred approach adds no stack usage (no local arrays). Level 1 snap adds no stack usage. The only stack concern is if `color_blend()` is inlined (it should be — it's already used extensively in WLED's hot path).

## Appendix A: Effect Categories by Fade Pattern

### Effects using `fade_out()` (27 calls, ~25 distinct effects):
- Twinkle, Meteor, Fireworks, Gravimeter, Juggles, Ripple, Popcorn, Blobs, Starburst, Drip, various audio-reactive effects

### Effects using `fadeToBlackBy()` (24 calls, ~20 distinct effects):
- Sparkle, Running Lights, Candle, Heartbeat, Pacifica, Flow, Perlin Move, various particle effects

### Effects using `blur()` (27 calls, ~20 distinct effects):
- Pride, Colorwaves, Flow Stripe, Blurz, DJ Light, Funkypanda, various 2D effects

### Effects using `fill()` (45 calls, ~35 distinct effects):
- Solid, Rainbow, Breathing, Color Wipe, various static/cycling effects

## Appendix B: Reference — `color_blend()` Performance

`color_blend()` from `colors.cpp` blends two RGBW32 colors by a factor. On ESP32 at 240MHz, measured at ~50ns per call (12 clock cycles). For 1000 LEDs: 50µs total — negligible compared to the ~31ms LED transmission time.

## Appendix C: Wire Size Comparison (300 RGB LEDs, chase + fade_out)

| Method | Frame Size | Compression Ratio |
|---|---|---|
| Raw DDP | 900 bytes | 1:1 |
| Delta+RLE (current, fade makes all dirty) | ~600 bytes | 1.5:1 |
| Delta+RLE with Level 1 snap (after convergence) | ~30 bytes | 30:1 |
| Delta+RLE with Level 2 deferred fade | ~15 bytes | 60:1 |
| Transform compression (Level 3) | ~17 bytes | 53:1 |
| Raw RLE (solid fill frame) | ~4 bytes | 225:1 |
