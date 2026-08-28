# feat(realtime): per-segment live input eligibility (replaces useMainSegmentOnly)

Rework of #5817 and #5818. Addresses all review feedback from @softhack007 and
@netmindz.

The `useMainSegmentOnly` bool is replaced with a `uint32_t _liveSegs` bitmask
on the strip object. Each bit marks a segment as eligible for realtime data.
Eligible segments get frozen (effects suppressed) during live input; everything
else keeps running normally.

## What changed

- `useMainSegmentOnly` global removed from `wled.h`
- `_liveSegs` (std::atomic<uint32_t>) added to `WS2812FX`
- Accessors: `getLiveSegs()`, `setLiveSegs(mask)`, `isLiveSeg(idx)`
- `useMainSegmentOnly()` kept as an inline (`_liveSegs != 0`) for usermod compat
- `seg.freeze` converted from bitfield to separate `bool` (atomic w.r.t. preemption)
- `setRealtimePixelColor()` routes all protocols through live segments, not DDP-only
- `rebuildDdpSlots()` reads `strip.getLiveSegs()` instead of the old global
- `ddpSlotEpoch` seqlock (std::atomic<uint32_t>) prevents slot table races
- Config key: `if.live.seg` (uint32_t bitmask); old `ddpelig` key migrated on read
- `/diag` shows `_liveSegs=0x...` for remote diagnosis

Files: `FX.h`, `FX_fcn.cpp`, `wled.h`, `wled.cpp`, `udp.cpp`, `cfg.cpp`,
`json.cpp`, `set.cpp`, `xml.cpp`, `e131.cpp`, `wled_server.cpp` +
`usermods/EleksTube_IPS/TFTs.h`, `usermods/audioreactive/audio_reactive.cpp`

## Why this approach

Previous versions (v1 #5817, v2 #5818) used a separate `showFrozenSegs()` fast
path that bypassed `blendSegment()`. Review correctly identified that this broke
the `service() -> show() -> blendSegment()` pipeline contract: TM1814 off-refresh,
`needsUpdate()`, `strip.trigger()`, and WARLS routing all depend on that path.

This version eliminates `showFrozenSegs()` entirely. The standard pipeline handles
all cases: frozen segments have DDP data in `seg.pixels[]`; `blendSegment()` reads
that directly and composites into `_pixels[]`. No separate rendering path, no
pipeline bypass.

## Backwards compatibility

- `mso=true` in cfg.json migrates to `_liveSegs = (1 << mainSegId)` on read
- `ddpelig` key still accepted on read; writes use `seg`
- `getLiveSegs()==0` (default) preserves legacy full-strip absolute pixel indexing
- `useMainSegmentOnly()` inline returns `_liveSegs != 0` for existing usermods
- No change for existing senders (OpenRGB, xLights, LedFX, HyperHDR)

## Overhead

- **RAM**: +4B (`_liveSegs` atomic on WS2812FX). `seg.freeze` bool replaces
  bitfield bit -- sizeof(Segment) unchanged on tested targets.
- **Flash**: ~200B net (slot table + epoch counter; showFrozenSegs removed)
- **CPU when _liveSegs==0**: zero -- all paths gated on getLiveSegs()

## Testing

Tested on ESP32-PICO-D4 (M5StickC), `m5stickc_ppp_wifi` and `esp32dev` builds:

```
Build:         70.8% flash, 26.0% RAM (m5stickc_ppp_wifi)
DDP flood:     112fps actual (PPP link-limited), 60s, no WDT
Heap soak:     50 enter/exit cycles, delta=0-4B (<1KB threshold)
TFT+DDP:       60fps concurrent, 30s, no SPI DMA crash
Codecs:        raw (0x00), planar-RLE (0x60), delta-RLE (0x01) all pass
PPP+WiFi:      dual-stack confirmed (sta + ap + ppp in /json/info nifs)
Config:        seg key round-trips across reboot
Uptime:        770s continuous, resetReason=3 (no WDT)
```

Mixed-segment: Ghost Rider on seg0 + DDP twinkle on seg1 -- effects undisturbed
on non-eligible segments throughout.

`grep -rn 'ddpEligibleMask\|rtFrozenSegs\|showFrozenSegs' wled00/` -- zero hits.
