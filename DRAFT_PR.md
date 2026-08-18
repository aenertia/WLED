# perf(effects): snap fade_out/fadeToBlackBy to target on final step

**Forgejo**: Fixes #9

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
## Related upstream issues

| Issue/PR | Repo | Title | Relevance |
|----------|------|-------|-----------|
| [#4976](https://github.com/wled/WLED/issues/4976) | Aircoookie/WLED | Continuous transitions between effects | **Direct match** — this PR implements the snap-to-target that makes transitions crisp |
| [#5620](https://github.com/wled/WLED/issues/5620) | Aircoookie/WLED | Fade transition leaves residual glow | Caused by the same integer rounding this PR fixes |
| [#5731](https://github.com/wled/WLED/issues/5731) | Aircoookie/WLED | Effects don't fully fade to black | Same root cause |
| [#5520](https://github.com/wled/WLED/issues/5520) | Aircoookie/WLED | Transition artifacts between presets | Related: snap-to-target eliminates the artifact window |
| [PR #5601](https://github.com/wled/WLED/pull/5601) | Aircoookie/WLED | Fade improvements (merged, acknowledged incomplete by devs) | This PR completes what #5601 started |
| [PR #4658](https://github.com/wled/WLED/pull/4658) | Aircoookie/WLED | Transition smoothing (merged) | Prior art |
| [PR #5524](https://github.com/wled/WLED/pull/5524) | Aircoookie/WLED | Effect transition improvements (open draft) | Coordinate — overlapping scope |
