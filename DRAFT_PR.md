# feat(realtime): per-segment eligibility mask for DDP/E1.31/Art-Net

## What

Replaces the `useMainSegmentOnly` boolean with a `ddpEligibleMask` uint32 bitmask,
allowing fine-grained control over which segments accept realtime pixel data
(DDP, E1.31, Art-Net, etc.) while other segments continue running local effects.

## Why

Multi-segment setups need some segments driven by external sources (video capture,
music visualizers, generative art tools) while others run local WLED effects
simultaneously. The old boolean was all-or-nothing: either the main segment got
realtime data, or the entire strip did. There was no way to say "segments 0 and 2
are realtime, segment 1 runs Rainbow."

## How

### Core machinery

- **`ddpEligibleMask`** (uint32 bitmask): persistent config, bits 0-31 correspond to
  segment IDs. Set via `/json/cfg` key `ddpelig`.
- **`DdpSegSlot` / `ddpSlots[]`**: pre-computed offset table mapping flat pixel stream
  positions to eligible segments. Built by `rebuildDdpSlots()` when the mask or
  segment layout changes.
- **`rtFrozenSegs`** (uint32 bitmask): runtime tracking of which segments are currently
  frozen by realtime input. Managed by `freezeSegForRealtime()` / `freezeEligibleSegs()`.
- **`showFrozenSegs()`**: rendering fast path (Cases B/C/D) that writes frozen segment
  pixel buffers directly to bus output without re-running effects. Non-frozen segments
  continue blending normally via `service()`.
- **Show gate**: `service()` skips `show()` when `rtFrozenSegs` is set; `showFrozenSegs()`
  handles display on its own cadence (PUSH from realtime packets).
- **`realtimeLock()`**: skips `fill(BLACK)` when frozen segs exist, preserving DDP pixel
  data across lock refreshes.
- **`exitRealtime()`**: clears `rtFrozenSegs`, unfreezes all segments on timeout.

### Boot order

`rebuildDdpSlots()` is called:
1. From `deserializeConfig()` after loading `ddpelig` from NVS
2. From `WLED::setup()` after `beginStrip()` (segments now have allocated pixel buffers)
3. From `deserializeState()` after segment changes via `/json/state`
4. From `handleSettingsSet()` when the HTTP settings checkbox changes

### Diagnostics

`/json/info` exposes `ddpelig`, `ddpslots`, `frozensegs` for remote diagnosis.

## Backwards compatibility

- **`mso=true`** in existing `cfg.json` migrates to `ddpEligibleMask = (1 << mainSegId)`.
- **`mso=false`** / **`ddpelig=0`** preserves legacy full-strip absolute pixel indexing.
- Zero change for existing senders (OpenRGB, xLights, LedFX, HyperHDR) -- they send
  destination=0 with flat pixel streams, which works unchanged when `ddpelig=0`.
- The HTTP settings UI checkbox `MO` (Main Output Only) maps to single-segment
  eligibility for the main segment, same behaviour as the old boolean.

## API

- `/json/cfg` key `"ddpelig"` (uint32 bitmask): set eligible segments. Example:
  `{"if":{"live":{"ddpelig":5}}}` enables segments 0 and 2.
- HTTP API: `MO=` checkbox (backwards compat, maps to main segment only).

## Use cases

The core value: run internal effects on some segments while streaming realtime DDP/E1.31/Art-Net to others, from a single ESP32 and one WLED instance.

**PC ARGB controller** -- segment 0 on a small matrix/panel receives DDP from LedFX or Hyperion (music-reactive visualisation, system status, video), while segments 1-3 (RGB fans, CPU cooler ring, desk underglow) run independent WLED effects. Today upstream forces a choice: everything realtime OR everything effects.

**Multi-zone installation** -- a retail display with a video panel (DDP from a media server) and accent lighting (WLED effects) on the same controller. The accent lighting keeps running its presets while the video panel receives live content.

**Live performance** -- a DJ booth with a hub75 matrix (DDP from Resolume/MadMapper) and LED strips (WLED effects synced to the music via audioreactive). The performer does not want the strip effects to stop when the matrix goes live.

**Home automation** -- a smart mirror with an LED matrix behind it (DDP from a Raspberry Pi showing weather/calendar) and ambient room lighting strips (WLED presets controlled via Home Assistant). The room lighting should not go dark when the mirror display updates.

In all cases: `ddpEligibleMask` marks which segments accept realtime, the rest keep running effects undisturbed. No sender changes, no protocol changes, no UI changes beyond the existing "use main segment only" checkbox (which now maps to a single-segment mask).

## Testing

Validated on M5StickC (ESP32-PICO-D4) with vanilla `esp32dev` build (no PPP, no SPI Matrix -- standard WiFi + WS2812B):

**Build**: `esp32dev_g26_test` env extending `esp32dev` with `LEDPIN=26`, `DEFAULT_LED_COUNT=256`. Flash 85.0%, RAM 26.6%. Clean build, zero warnings.

**3-segment mixed-mode test** (256px WS2812B strip on G26, split into 3 segments):
- seg0 (0-85): Rainbow effect (fx=9)
- seg1 (86-171): DDP eligible (`ddpelig=2`)
- seg2 (172-255): Ghost Rider effect (fx=120)

Results via `/json/info` during DDP streaming to seg1:
```
ddpelig: 2        -- seg1 eligible
ddpslots: 1       -- one slot in offset table
frozensegs: 2     -- bit 1 set = seg1 frozen by realtime
live: True         -- realtime mode active
liveseg: 1        -- first frozen segment is seg1
```

seg0 (Rainbow) and seg2 (Ghost Rider) continued running effects undisturbed while seg1 received DDP pixel data. Mixed frozen/non-frozen segment rendering confirmed operational.

**Additional validation from fork testing** (M5StickC with SPI Matrix TFT + WS2812B, dual-stack WiFi+PPP):
- Ghost Rider on seg0 + DDP twinkle on seg1: all codecs (Raw RGB, RLE, Delta+RLE) clean, no tearing
- 50-cycle realtime enter/exit soak: heap stable, no leak
- Full-strip legacy mode (`ddpelig=0`): unchanged behaviour confirmed

## Development process

Developed using an agentic coding harness (OpenCode/OhMyOpenCode) driving multiple
LLM providers, with human directing architecture decisions, testing on physical
hardware, and reviewing all output. Code swept for AI style artifacts per upstream
guidelines.

## Porting observations

- `useMainSegmentOnly` was referenced in `set.cpp` and `xml.cpp` for the settings UI
  checkbox -- migrated to use `ddpEligibleMask` with backwards-compatible boolean mapping.
- `FX_fcn.cpp` `show()` has a blend guard that referenced `useMainSegmentOnly` --
  changed to `rtFrozenSegs` so frozen segments skip re-blending.
- `e131.cpp` `handleNotifications()` show path was binary (trigger vs show) -- now
  dispatches to `showFrozenSegs()` when frozen segs exist.
- Audioreactive usermod suspended audio during any realtime mode unless
  `useMainSegmentOnly` was set -- updated to check `rtFrozenSegs` so audio processing
  continues when per-segment realtime leaves some segments running local effects.

## References

- #5810 (softhack007 requested separate PR for per-segment routing)
- #5547 (validation philosophy)

## Future direction

The eligibility mask machinery enables a natural follow-up: sender-driven per-packet
segment targeting via the DDP destination byte. This would allow video wall
compositors, multi-panel controllers, and tools that already use the DDP destination
field (Jinx!, Resolume, MadMapper) to route different content to different segments
within a single frame. The frozen-segment rendering and slot table from this PR are
the prerequisite infrastructure.
