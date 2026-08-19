## feat(bus): skip show() for idle slow buses + showFrozenSegs() DDP fast path

**Forgejo**: Fixes #22

### Problem

On constrained ESP32 devices (no PSRAM), the TFT SPI DMA `show()` call
takes ~24ms per frame. When DDP is streaming to a WS2812B strip only, the
TFT bus still runs its full DMA cycle every frame — wasting CPU, blocking
the main loop, and capping DDP throughput at ~45fps.

Additionally, when the TFT segment is off (no active effects), the DMA
ping-pong buffers (~16KB) and snapshot buffer (~12.8KB) remain allocated
even though they're not needed.

### Solution

**Bus skip-show gate**: `BusManager::show()` calls `busHasActiveSegment()`
before invoking `show()` on each bus. When no active segment covers a slow
bus (TFT, Hub75, Network), `show()` is skipped. For TFT: DMA buffers are
freed (`deallocateBuffers()`) when the bus goes idle, saving ~29KB heap.
Re-allocated lazily on first active `show()`.

**`showFrozenSegs()` fast path**: When DDP has frozen specific segments via
`rtFrozenSegs` (see `pr/ddp-per-segment`), `showFrozenSegs()` bypasses the
full `_pixels[]` pipeline and calls `show()` only on buses covering frozen
segments. Eliminates the TFT SPI DMA bottleneck for DDP streams targeting
only the WS2812B strip.

**Dirty-row partial render**: `recalcActiveRowRange()` computes the active
row range from segment geometry each frame (~5µs). DMA strips outside the
active range are skipped, reducing loopLag for sub-panel TFT segments.

### Results (M5StickC, ESP32-PICO-D4, 40×80 TFT + 8×32 WS2812B)

| Scenario | Before | After |
|----------|--------|-------|
| DDP to strip only (fps) | 45 | 119 (+2.7×) |
| TFT off (free heap) | 59 KB | 88.7 KB (+29 KB) |
| Sub-panel TFT loopLag | 17 ms | 0–3 ms |

### Files changed

- `wled00/bus_manager.cpp` — `busHasActiveSegment()`, skip-show gate in `BusManager::show()`, `BusSPIMatrix` lazy alloc/dealloc, `recalcActiveRowRange()`
- `wled00/bus_manager.h` — `_skipShow`, `_buffersAllocated`, `_activeRowMin/Max` fields, new method declarations
- `wled00/bus_spi_matrix.h` — SPI matrix bus implementation
- `wled00/FX_fcn.cpp` — `showFrozenSegs()` implementation
- `wled00/FX.h` — `showFrozenSegs()` declaration
- `wled00/wled.h` — `rtFrozenSegs` (shared with `pr/ddp-per-segment`)
- `wled00/wled.cpp` — `allSegsFrozenByDDP` guard in main loop
- `wled00/udp.cpp` — `showFrozenSegs()` call sites, timeout-before-show fix

### Dependencies

Requires: `pr/ddp-per-segment` (provides `rtFrozenSegs` bitmask)

### Testing

Tested on M5StickC over 1.5Mbaud PPP link. 10-minute DDP soak: 0 crashes,
heap stable, 119fps sustained to WS2812B strip with TFT idle.