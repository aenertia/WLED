## feat(ddp): per-segment targeting — dual-mode DDP routing

### Problem

WLED's DDP implementation has a single boolean `useMainSegmentOnly` that
either sends all DDP data to the main segment or to the full strip. There's
no way to target a specific segment by ID, or to distribute a DDP stream
across multiple segments independently.

### Solution

Dual-mode DDP routing:

**Mode A — Destination-routed**: DDP destination byte (1–32) routes to
segment 0–31. `channelOffset` is segment-relative. Backwards compatible:
destination=0 uses legacy full-strip absolute indexing.

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

### Testing

Tested on M5StickC with 2-segment layout (TFT + WS2812B strip).
Mode A: DDP to destination=1 updates only the TFT segment.
Mode B: DDP stream distributed across both segments simultaneously.
119fps sustained DDP with frozen segments.

### Related

Depends on: none (standalone feature)
Enables: pr/bus-skip-show (uses rtFrozenSegs for fast-path show())
