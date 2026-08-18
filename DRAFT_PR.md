## feat(ddp): per-segment targeting — dual-mode DDP routing

**Forgejo**: Fixes #26

### Problem

WLED's DDP implementation has a single boolean `useMainSegmentOnly` that
either sends all DDP data to the main segment or to the full strip. There's
no way to target a specific segment by ID, or to distribute a DDP stream
across multiple segments independently.

### Solution

Dual-mode DDP routing:

**Mode A — Destination-routed**: DDP destination byte (1–32) routes to
segment 0–31. `channelOffset` is segment-relative. Backwards compatible:
destination=0 uses legacy full-strip absolute indexing **when `ddpEligibleMask == 0`
(no eligible segments configured)**. When `ddpEligibleMask` is non-zero,
destination=0 triggers Mode B (concatenated stream across eligible segments).
Configure via `/json/cfg`: `{"if":{"live":{"ddpelig":0}}}` to restore legacy behaviour.

**Mode B — Eligibility mask**: `ddpEligibleMask` bitmask selects which
segments receive a concatenated DDP stream. Configure via `/json/cfg`:
```json
{"if":{"live":{"ddpelig":5}}}
```
(bits 0+2 set = segments 0 and 2 receive the stream)

### State

- `ddpEligibleMask` — persistent bitmask, saved to config
- `rtFrozenSegs` — runtime bitmask of segments frozen by realtime
- `ddpSlots[]` / `ddpSlotCount` — pre-computed Mode B offset table

### Undocumented changes

**`e131NewData` changed to `std::atomic<bool>`**: Previously a plain `bool`,
now an atomic to prevent data races between the lwIP tcpip_thread (DDP handler,
Core 0) and the Arduino main loop (Core 1). This is a source-level change
affecting all translation units that include `wled.h` with `WLED_USE_PPP` defined.

**`realtimeLock()` for DDP uses hardcoded 2500ms timeout**: Rather than the
user-configurable `realtimeTimeoutMs`, DDP realtime lock uses a fixed 2500ms.
Rationale: the FPS=0 lockup bug (session 16) was caused by the timeout check
running after `strip.show()` which blocks for 24ms on TFT SPI DMA. The fix
moved the timeout check before the show block, and 2500ms was chosen as a safe
value that allows multi-packet DDP frames to complete while ensuring timely
recovery when the sender stops.

### Testing

Tested on M5StickC with 2-segment layout (TFT + WS2812B strip).
Mode A: DDP to destination=1 updates only the TFT segment.
Mode B: DDP stream distributed across both segments simultaneously.
119fps sustained DDP with frozen segments.

### Related

Depends on: none (standalone feature)
Enables: pr/bus-skip-show (uses rtFrozenSegs for fast-path show())