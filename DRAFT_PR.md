# perf(effects): snap fade_out/fadeToBlackBy to target on final step

## Problem

`fade_out()` and `fadeToBlackBy()` use integer multiply-divide arithmetic
which asymptotically approaches but never reaches the target value. Pixels
linger at 1-2 brightness counts indefinitely, causing:

- Visible "ghost" pixels on dark backgrounds
- Wasted CPU cycles processing effects that should have completed
- Dirty frames for DDP compression (non-zero deltas on every frame)

## Solution

**fade_out()**: When the calculated delta is zero but the channel hasn't
reached its target, snap directly to the target (`c1 = c2`) instead of
bumping by ±1. The old `±1` bump created an oscillation around the target
for multi-channel colors.

**fadeToBlackBy()**: After scaling, if the result equals the input (scale
had no effect due to integer truncation) and the pixel isn't already black,
snap to zero. This eliminates the infinite tail where `fast_color_scale(1, 254) == 1`.

## Files Changed

- `wled00/FX_fcn.cpp`: `fade_out()` and `fadeToBlackBy()` only

## Testing

- Visual: fade effects reach true black/target within expected frame count
- DDP compression: delta frames go to zero after fade completes
- No effect regressions observed with Rainbow, Fire, Meteor effects

## Upstream Reference

Related to #4976 (fade transitions)
