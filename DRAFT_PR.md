# ⚠️ INCOMPLETE — feat(effects): deferred fade accumulator

**Forgejo**: Fixes #10

**STATUS: INCOMPLETE — DO NOT MERGE**

V2 broke scrolling text gradient interaction. V3 redesign needed.

## Problem

Per-pixel fade operations (`fade_out`, `fadeToBlackBy`) dirty every pixel
every frame, defeating delta+RLE compression for DDP streaming. A 40×80
matrix generates ~9.6KB of deltas per frame even when the visual change
is minimal.

## Approach (V2 — broken)

Compositor-level deferred fade: batch fade operations and apply at frame
boundary. Static accumulators (`_deferredFadeBlend[]`, `_deferredFadeScale[]`)
track pending fade per segment. The show() pipeline applies accumulated
fades in one pass.

Scrolling text effect updated to use crisp fill + shadow instead of
continuous trail fade when `c1 < 128`.

## What Broke

V2 scrolling text with gradient palettes: the deferred fade interacts
badly with `color_from_palette()` cycling — palette index shifts happen
per-frame but the deferred fade batches across frames, causing visible
color banding and stutter.

## Files Changed

- `wled00/FX_fcn.cpp`: deferred fade static accumulators + removal comment
- `wled00/FX.cpp`: scrolling text trail→fill/shadow, drop shadow rendering,
  rotation disabled (c3 repurposed for shadow angle), speed range extended

## V3 Plan

- Move fade accumulation into Segment struct (not global statics)
- Apply deferred fade before palette lookup, not after
- Separate shadow rendering from fade logic