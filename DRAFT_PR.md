# Per-segment realtime eligibility mask

Replaces `useMainSegmentOnly` with a `ddpEligibleMask` bitmask. Segments marked
eligible accept DDP/E1.31/Art-Net realtime data; the rest keep running effects.

## Motivation

Multi-segment setups (PC ARGB with fans + matrix, retail video panel + accent
lighting, DJ booth with hub75 + LED strips) need some segments on realtime while
others run local effects. The old boolean was main-segment-only or the entire strip.

Some additional comments from separate DDP Compression conversation #5810.

## Changes

- `ddpEligibleMask` (uint32): per-segment eligibility, cfg key `ddpelig`
- `ddpSlots[]`: pre-computed offset table for flat-stream-to-segment routing
- `rtFrozenSegs` (uint32): tracks which segments are frozen by realtime
- `showFrozenSegs()`: renders frozen segment pixels to bus without re-running effects
- `service()` show gate: skips `show()` when frozen segs exist, prevents race
- `realtimeLock()`/`exitRealtime()`: freeze/unfreeze lifecycle
- `rebuildDdpSlots()` called from config load, segment API changes, and strip init

Files: wled.h, udp.cpp, FX.h, FX_fcn.cpp, e131.cpp, cfg.cpp, json.cpp, wled.cpp,
set.cpp, xml.cpp + audioreactive and EleksTube_IPS usermods (replaced references).

## Backwards compatibility

- `mso=true` in cfg.json migrates to `ddpEligibleMask = (1 << mainSegId)`
- `ddpelig=0` (default) preserves legacy full-strip pixel indexing
- `MO` settings checkbox maps to single-segment mask, same UI behaviour
- cfg.json writes both `mso` (bool) and `ddpelig` (bitmask)
- No change for existing senders (OpenRGB, xLights, LedFX, HyperHDR)

## Overhead

- **RAM**: +164B static (ddpSlots[32] = 160B, masks + counters = 4B). Not heap-allocated
  because rebuildDdpSlots() runs from packet handlers.
- **Flash**: ~750B (showFrozenSegs ~70 lines, slot functions ~30 lines)
- **CPU when ddpelig=0**: zero -- all paths gated on ddpSlotCount/rtFrozenSegs

## Testing

Tested on ESP32-PICO-D4 (M5StickC), vanilla esp32dev build, WS2812B 8x32 panel:
- 3 segments: seg0 Rainbow, seg1 DDP-eligible, seg2 Ghost Rider
- During DDP to seg1: `frozensegs=2`, seg0+seg2 effects undisturbed
- Realtime enter/exit cycling: heap stable
- Legacy mode (ddpelig=0): unchanged behaviour
- Audioreactive: audio continues for non-frozen segments

Build: esp32dev SUCCESS, Flash 85.0%, RAM 26.6%.

## Future direction

The slot table and frozen-segment rendering enable a follow-up: per-packet segment
targeting via the DDP destination byte, for video wall compositors that need to route
different content to different segments within a single frame.

Assisted-by: OpenCode/OhMyOpenCode agentic harness with multiple LLM providers
