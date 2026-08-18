## feat(ddp): DDP flood survival — heap guard, rate limiter, starvation detector

### Problem

An uncapped DDP sender (or network burst) can crash a constrained ESP32
device in three ways:
1. **OOM**: DDP packet buffers exhaust the heap → crash
2. **CPU starvation**: DDP RX task starves the main loop → TFT SPI DMA
   never completes → watchdog reset
3. **Rate mismatch**: accepting DDP faster than the display can render
   wastes CPU on frames that will never be shown

Observed on M5StickC (ESP32-PICO-D4, 520KB SRAM, no PSRAM) under
uncapped DDP at 670+ fps: device crashes or hangs within 60 seconds.

### Solution

Three independent protection layers in `handleDDPPacket()`:

**Layer 1 — Heap guard**: Drop all DDP packets when `ESP.getFreeHeap()`
falls below 20KB. Prevents OOM crashes. Counter: `ddpHeapGuardDrops`
(atomic, visible in `/diag`).

**Layer 2 — Rate limiter**: `ddpMaxFps` cap (default: 40fps when TFT bus
is active, 60fps otherwise). Micros-based gate using `ddpLastFrameUs`.
Excess frames dropped silently. Counter: `ddpRateLimitDrops`. Rationale:
TFT SPI DMA takes ~24ms per frame — accepting DDP faster than the display
can render wastes CPU and starves the main loop.

**Layer 3 — Starvation detector**: When the main loop hasn't run for
>200ms (detected via `lastLoopMs` atomic updated each loop iteration),
the tcpip_thread boosts the loop task priority to 19 via
`vTaskPrioritySet(loopTaskHandle, 19)`. The main loop restores priority
to 1 on its next iteration. Prevents TFT SPI DMA from being permanently
starved under sustained flood.

**`/diag` additions**: `ddpRateLimitDrops`, `ddpHeapGuardDrops` counters,
realtime lock state (`timeout`/`now`/`diff`), RTC crash
snapshot (preserved across resets via RTC memory for post-crash diagnosis).

### Results (M5StickC, uncapped DDP flood at 670+ fps)

| Scenario | Without | With |
|----------|---------|------|
| 60s soak | crash/hang | stable |
| 10min soak | — | 0 crashes, heap ≥ 40KB |
| WiFi AP during flood | unavailable | accessible |

### Files changed

- `wled00/e131.cpp` — heap guard, rate limiter, starvation detector in `handleDDPPacket()`
- `wled00/wled.h` — `ddpMaxFps`, `ddpRateLimitDrops`, `ddpHeapGuardDrops`, `loopPriorityBoosted`, `lastLoopMs` declarations
- `wled00/wled.cpp` — `lastLoopMs.store()` in main loop, priority restore, RTC crash snapshot
- `wled00/wled_server.cpp` — `/diag` endpoint: DDP counters, realtime state, RTC crash snapshot

### Notes

- `ddpMaxFps` is configurable (0 = unlimited). Default 40fps matches TFT
  SPI DMA throughput ceiling; 60fps for strip-only configurations.
- The starvation detector uses FreeRTOS `vTaskPrioritySet()` — ESP32-specific.
  On non-FreeRTOS platforms, Layer 3 is a no-op.
- RTC crash snapshot requires `CONFIG_ESP32_RTC_CLK_SRC_INT_8MD256` or
  similar RTC memory support.

### Testing

10-minute DDP soak at uncapped rate on M5StickC over 1.5Mbaud PPP.
Heap stable at ≥40KB, 0 crashes, WiFi AP accessible throughout.
