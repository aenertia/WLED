# PR Description

## feat(realtime): per-segment live input eligibility (replaces useMainSegmentOnly)

Rework of #5817. Addresses all the review feedback from @softhack007 and
@netmindz.

The `useMainSegmentOnly` bool is replaced with a `uint32_t _liveSegs` bitmask
on the strip object. Each bit marks a segment as eligible for realtime data.
Eligible segments get frozen (effects suppressed) during live input; everything
else keeps running normally.

#### What changed

- `useMainSegmentOnly` global removed from `wled.h`
- `_liveSegs` bitmask added to `WS2812FX`, with `getLiveSegs()`,
  `setLiveSegs()`, `isLiveSeg()` accessors
- `useMainSegmentOnly()` kept as an inline (`_liveSegs != 0`) so existing
  usermods compile without changes
- DDP pixel routing writes into `seg.pixels[]` via `setPixelColorRaw()`, same
  as upstream's `useMainSegmentOnly` path did via `getMainSegment()`. The
  difference is that it now routes across multiple segments instead of just one
- Other protocols (WARLS, E1.31, Art-Net, Adalight, TPM2.NET) are unchanged,
  strip-absolute via `setPixelColor()`
- `arlsOffset` is skipped when DDP per-segment routing is active. I had a good
  look and think about the scenarios where this might be useful and came to the
  conclusion that this will never make sense and is safe to skip because
  the segment layout already defines the physical mapping
- Config: new `"seg"` key (uint32_t bitmask) alongside `"mso"` (bool, kept for
  firmware downgrade compat). `"seg"` takes precedence on load. Old configs with
  only `"mso"` are migrated to `1 << mainSegmentId`
- `/json/info`: `"liveseg"` still reports the lowest live segment index (or -1).
  New `"livesegs"` field has the full bitmask

#### What did NOT change (v1 feedback)

- No new rendering paths. No showFrozenSegs, no rebuildDdpSlots. The existing
  `service()` / `show()` / `blendSegment()` pipeline handles frozen segments
  correctly, I just didn't trace it far enough in v1.
- No service() gate. `strip.trigger()`, `needsUpdate()`,
  `isOffRefreshRequired()` all intact.
- No new globals.
- Usermods compile without modification (verified EleksTube_IPS, audioreactive).

#### Why DDP only gets per-segment routing

E1.31 and Art-Net have their own universe/channel addressing via `DMXAddress`
and `dataOffset`. Routing them through `_liveSegs` would conflict with that.
WARLS, Hyperion, and TPM2.NET have no segment concept in the protocol. DDP is
the only protocol where a flat pixel buffer needs the device to decide which
segment each pixel lands in. The gate in `setRealtimePixelColor()` is explicit;
other protocols can be added to it if there's a use case.

**Behaviour change worth discussing:** In upstream, `useMainSegmentOnly` routed
all protocols through `getMainSegment().setPixelColorRaw()`. This branch
narrows that to DDP only. Hyperion/WARLS with `_liveSegs` set will produce
black output where upstream would have worked, because their data goes to
`_pixels[]` which gets wiped by `show()`.

In practice `_liveSegs` is doing double duty: controlling freeze/unfreeze for
all protocols AND pixel routing for DDP. One option is to split this into two
config keys, something like keeping `"mso"` for freeze behaviour across all
protocols and adding a DDP-specific `"ddpseg"` for per-segment routing. I'd
rather get your input on the right config structure than guess at it.

#### Known limitations

- `uint32_t` limits coverage to 32 segments. PSRAM boards with
  `MAX_NUM_SEGMENTS=64` silently ignore segments 32-63. Widening to `uint64_t`
  would need `ARDUINOJSON_USE_LONG_LONG=1` project-wide; that's a separate
  discussion.
- `seg.freeze` is a 1-bit field packed with adjacent flags in the same byte.
  Setting it from the UDP callback while the service loop writes a different bit
  in the same byte is a non-atomic RMW. Pre-existing, not introduced here.
- Segment deletion or reorder during active realtime can leave `_liveSegs`
  pointing at the wrong segment. This is existing upstream behaviour:
  `useMainSegmentOnly` had the same issue with the main segment ID shifting
  on reorder. Needs its own discussion about how/if WLED wants to handle live
  segment changes during streaming.

#### Config API

```bash
# segments 0 and 2 eligible (bitmask 5 = 0b101)
curl -X POST http://<device>/json/cfg -d '{"if":{"live":{"seg":5}}}'

# clear (back to normal)
curl -X POST http://<device>/json/cfg -d '{"if":{"live":{"seg":0}}}'
```

The settings UI checkbox ("Use main segment only") still works. It sets
`_liveSegs = 1 << mainSegmentId`, same behaviour as before.

#### Build and test

esp32dev: SUCCESS, Flash 84.9%, RAM 26.5%, zero errors.

Tested on ESP32-PICO-D4 (M5StickC) with WS2812B strips and SPI TFT, DDP over
UDP. I don't have TM1814, WARLS sender, or Hub75 hardware; I verified those
code paths by reading the source.

Assisted-by: OpenCode with various LLMs (analysis and code generation). Testing
and verification by author.

---

# Follow-up comment (post after PR is open)

---

The main thing I want to flag is a behaviour change for non-DDP protocols that
I think needs discussion before this goes in.

#### _liveSegs is doing two jobs

In upstream, `useMainSegmentOnly` did two things with one bool:
1. Froze the main segment on realtime lock (effects suppressed while live)
2. Routed all protocols through `getMainSegment().setPixelColorRaw()`, writing
   pixel data into `seg.pixels[]`

This branch replaces the bool with a bitmask, which generalises (1) correctly
to multiple segments. But it narrows (2) to DDP only. Other protocols
(Hyperion, WARLS, E1.31, Art-Net, Adalight, TPM2.NET) now always use the
strip-absolute `setPixelColor()` path, writing into `_pixels[]`.

The problem: when `_liveSegs` is set, `show()` clears `_pixels[]` and
re-blends from `seg.pixels[]` every frame. DDP data survives because it's in
`seg.pixels[]`. Hyperion/WARLS data gets wiped because it's in `_pixels[]`.

Concretely: if someone had `useMainSegmentOnly` ticked and was using Hyperion,
that worked in upstream. With this branch and `_liveSegs` set, same Hyperion
setup produces black output. Nobody hits this unless they explicitly set the
bitmask then connect with a non-DDP sender (`_liveSegs = 0` is the default),
but it's still a regression from upstream behaviour.

I narrowed the routing to DDP because the per-segment arithmetic doesn't make
sense for protocols that address strip-absolute. But the freeze/unfreeze
gating is useful for all protocols. These are two different concerns sharing
one config key.

One option: keep `"mso"` (or equivalent) for freeze behaviour across all
protocols, and add a DDP-specific key like `"ddpseg"` for per-segment routing.
That would preserve upstream behaviour for Hyperion/WARLS users while adding
the new DDP capability. I'd rather get your input on the right config structure
than bake in a guess.

#### How the routing actually works

I've tried to trace these through the code and can point at specific lines if
anything looks wrong.

Every realtime protocol calls `setRealtimePixel()`, which calls
`strip.setRealtimePixelColor()`. The routing splits there:

- **DDP with `_liveSegs` set**: walks live segments in order, treats them as a
  concatenated pixel buffer. Pixel 0 goes to the first pixel of the first live
  segment. Writes into `seg.pixels[]` via `setPixelColorRaw()`. This survives
  `show()` because `blendSegment()` reads from `seg.pixels[]`.
- **Everything else** (WARLS, E1.31, Art-Net, Adalight, TPM2.NET, Hyperion,
  GENERIC): `setPixelColor(i, c)`, which writes directly to `_pixels[]` at
  the strip-absolute index. Unchanged from upstream.

The freeze/unfreeze in `realtimeLock()` and `exitRealtime()` applies to all
protocols equally: any segment with its bit set gets cleared and frozen on first
packet, unfrozen on timeout.

#### FX blending with DDP via the segment stack

Frozen segments still get composited by `blendSegment()` every frame. This
means you can overlay effects on DDP content using segment blend modes.

Example: seg0 runs a 2D effect (base layer, not live). Seg2 covers the same
physical LEDs and is live (receives DDP). Set seg2's blend mode to ADD, SCREEN,
MULTIPLY, or whatever you want. When DDP is active, seg2 is frozen and holds
DDP data; `blendSegment()` composites it on top of seg0's effect using the
blend mode you picked. The opacity slider on seg2 controls how strongly the DDP
layer blends in.

When DDP times out, seg2 unfreezes and its effect resumes. The blend mode
persists.

I think this is useful for setups where you want ambient effects running
underneath live content, but I haven't tested every blend mode combination.

#### arlsOffset

The "Realtime LED offset" setting is skipped when DDP per-segment routing is
active. With per-segment routing the sender addresses a concatenated pixel
buffer across the live segments; a global wiring offset applied on top of that
would shift the arithmetic and route pixels into the wrong segment. For all
other protocols (and DDP with `_liveSegs = 0`), `arlsOffset` works as before.

#### WebSocket DDP

WebSocket binary frames with the DDP protocol byte go through the same
`handleE131Packet()` handler as UDP DDP, so per-segment routing applies. As
far as I can tell, the existing WebSocket handler only processes single-frame
messages (multi-frame messages are silently dropped), which would limit
WebSocket DDP to roughly 476 pixels per message (1428 bytes at 3 channels per
pixel). Not an issue for UDP. I could be missing something in the WebSocket
framing though. Either way that's a whole other conversation, just something I
noticed while testing. It's existing behaviour, nothing this branch changes.
