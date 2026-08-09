# deferred-fade-v2 - Work Plan

## TL;DR (For humans)

**What you'll get:** A working deferred fade that doesn't crash. The fade accumulator lives in a global array (not inside the Segment class), and the fade is applied once per frame in the compositor (`WS2812FX::show()`) after segment blending — not per-pixel in `getPixelColorRaw()`. Zero Segment class modifications, zero changes to any inline function.

**Why this approach:** The V1 crash was caused by inserting members into the middle of the Segment class (shifting struct layout) and calling `color_blend()` from an inline in FX.h that reads from 32-bit-only IRAM pixel buffers. V2 avoids both: external state array + compositor-level application.

**What it will NOT do:** No Segment class modifications. No changes to getPixelColorRaw/setPixelColorRaw. No per-pixel overhead in the effects hot path.

**Effort:** Short
**Risk:** Low — the compositor already does a full pass over `_pixels[]`; adding fade there is trivial and architecturally clean.
**Decisions to sanity-check:** Global array indexed by segment ID vs per-segment pointer. Global array is simpler and has no lifetime concerns.

Your next move: approve to begin. `$start-work deferred-fade-v2`

---

> TL;DR (machine): Short effort, low risk. Global fade accumulator array + compositor-level fade application. No Segment class changes. Replaces crashed V1.

## Scope
### Must have
- Remove `_fadeAccum`/`_scaleAccum` from Segment class entirely (revert V1 struct changes)
- Remove modified `getPixelColorRaw()` (revert to original `return pixels[i]`)
- Remove `flushDeferredFade()` and blur() flush calls (revert)
- Remove `WLED_DISABLE_DEFERRED_FADE` flag (no longer needed)
- New: global `segFadeAccum[]` and `segScaleAccum[]` arrays in FX_fcn.cpp
- New: `fade_out()` under `WLED_ENABLE_DDP_COMPRESSION` writes to global array using segment index
- New: `fadeToBlackBy()` under same guard writes to global array
- New: in `WS2812FX::show()`, after `blendSegment()` loop, apply accumulated fade to `_pixels[]` per-segment-range
- Compile gate on koero
- Flash and test on M5StickC — effect #17 (Twinkle) must not crash

### Must NOT have (guardrails, anti-slop, scope boundaries)
- No modifications to Segment class definition in FX.h (zero struct layout changes)
- No modifications to getPixelColorRaw or setPixelColorRaw
- No per-pixel overhead in effects (fade applied once in compositor, not per-read)
- No new heap allocations (global arrays are static)
- No changes to FX.cpp effects code

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: none — compile gate + flash test on M5StickC via emiemi
- Compile: koero SSH, `pio run -e m5stickc_ppp`
- Flash test: esptool via emiemi, cycle to effect #17 (Twinkle), verify no crash
- Evidence: .omo/evidence/

## Execution strategy
### Parallel execution waves

**Wave 1:** T1 — revert V1 changes (clean slate)
**Wave 2:** T2-T3 — new implementation (global array + compositor fade)
**Wave 3:** T4 — compile + flash + test

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| T1 (revert V1) | — | T2, T3 | — |
| T2 (global array + fade_out/fadeToBlackBy) | T1 | T3 | — |
| T3 (compositor fade application) | T2 | T4 | — |
| T4 (compile + flash + test) | T3 | — | — |

## Todos
> Implementation + Test = ONE todo. Never separate.

- [ ] 1. Revert V1 deferred fade — clean slate
  What to do: Remove ALL V1 deferred fade artifacts:
  (A) FX.h: remove `_fadeAccum`, `_scaleAccum` members from Segment class. Revert `getPixelColorRaw()` to original `return pixels[i]`. Remove `flushDeferredFade()` declaration. Remove `getPixelColorXYRaw()` deferred fade routing. Revert constructor init. Change all `#if defined(WLED_ENABLE_DDP_COMPRESSION) && !defined(WLED_DISABLE_DEFERRED_FADE)` back to just `#ifdef WLED_ENABLE_DDP_COMPRESSION` in FX.h.
  (B) FX_fcn.cpp: revert `fade_out()` — remove the `#ifdef` deferred path, keep ONLY the original path with snap-to-target fix. Same for `fadeToSecondaryBy()` and `fadeToBlackBy()`. Remove `flushDeferredFade()` definition. Remove blur() flush call. Change all `#if defined(WLED_ENABLE_DDP_COMPRESSION) && !defined(WLED_DISABLE_DEFERRED_FADE)` back to `#ifdef WLED_ENABLE_DDP_COMPRESSION` where they guard DDP receiver code (e131.cpp) — but NOT the fade functions (those lose the ifdef entirely since we're reverting).
  (C) FX_2Dfcn.cpp: remove blur2D() flush call.
  (D) platformio_override.ini: remove `-D WLED_DISABLE_DEFERRED_FADE` from m5stickc_ppp_ftdi_flags.
  Must NOT touch: e131.cpp (DDP receiver), ddp_compress.h (RLE library), bus_manager.cpp (TFT bus), const.h (type IDs). Those are unrelated to the deferred fade.
  Parallelization: Wave 1 | Blocked by: — | Blocks: T2, T3
  References: FX.h:477-480 (_fadeAccum/_scaleAccum), FX.h:540-549 (getPixelColorRaw deferred), FX.h:550-552 (flushDeferredFade decl), FX.h:555-559 (getPixelColorXYRaw routing), FX.h:613-616 (constructor init), FX_fcn.cpp:1064-1070 (fade_out deferred), FX_fcn.cpp:1096-1100 (fadeToSecondaryBy deferred), FX_fcn.cpp:1109-1112 (fadeToBlackBy deferred), FX_fcn.cpp:1125-1138 (flushDeferredFade), FX_fcn.cpp:1145-1147 (blur flush), FX_2Dfcn.cpp:247-249 (blur2D flush)
  Acceptance criteria: `grep _fadeAccum wled00/FX.h` returns 0 matches. `grep flushDeferredFade wled00/FX_fcn.cpp` returns 0 matches. `grep WLED_DISABLE_DEFERRED_FADE wled00/` returns 0 matches across all files. `getPixelColorRaw` in FX.h is back to single-line `return pixels[i]`.
  Commit: N (batched)

- [ ] 2. Global fade accumulator + deferred fade_out/fadeToBlackBy/fadeToSecondaryBy
  What to do: In FX_fcn.cpp, add a global fade state array (NOT inside Segment class):
  ```cpp
  #ifdef WLED_ENABLE_DDP_COMPRESSION
  static uint8_t _segFadeAccum[MAX_NUM_SEGMENTS] = {};
  static uint8_t _segScaleAccum[MAX_NUM_SEGMENTS];
  // init all scale accums to 255 (no fade)
  static bool _segFadeInit = []{ memset(_segScaleAccum, 255, sizeof(_segScaleAccum)); return true; }();
  #endif
  ```
  Then modify `fade_out()` — add a NEW `#ifdef WLED_ENABLE_DDP_COMPRESSION` block that uses the global array:
  ```cpp
  void Segment::fade_out(uint8_t rate) const {
  #ifdef WLED_ENABLE_DDP_COMPRESSION
    if (!isActive()) return;
    unsigned idx = cycleCount(); // need segment index — use strip._segment_index
    // Actually, use a different approach: store in SEGENV-like fashion
    // The _segment_index is a static in WS2812FX, accessible via strip
    unsigned segIdx = cycleCount(); // WRONG — need actual segment index
  ```
  
  WAIT — the problem is getting the segment index from inside a `const` Segment method. The Segment doesn't know its own index in the vector. BUT `WS2812FX` tracks `_segment_index` during `service()`. We can use `strip._segment_index` but it's private.
  
  BETTER APPROACH: use the segment's `data` pointer address as a hash key. No — too complex.
  
  SIMPLEST APPROACH: store fade state in `SEGENV.step` / `SEGENV.aux0` / `SEGENV.aux1` — these are per-segment runtime variables that effects already use. BUT effects overwrite them, so we can't hijack them.
  
  ACTUAL SIMPLEST: use `Segment::data` — the effect data buffer. Add 2 bytes at the START of every segment's data allocation for fade state. But this changes the data layout for every effect.
  
  OK, REAL SIMPLEST: Don't defer in `fade_out()` at all. Instead, override the behavior at the COMPOSITOR level. In `WS2812FX::show()`, after `blendSegment()`:
  1. For each segment that called `fade_out()` this frame, apply the fade to `_pixels[]` in the segment's pixel range
  2. Track "did this segment call fade_out this frame?" via a small bitfield
  
  But how does the compositor know which segments called fade_out? We need a signal from fade_out to the compositor.
  
  THE ACTUAL CLEANEST APPROACH: Add a static array in FX_fcn.cpp that `fade_out()` writes to, and `WS2812FX::show()` reads from. Use `strip._segment_index` (which IS set during `service()` before effects run) passed through a global:
  
  ```cpp
  // In FX_fcn.cpp:
  #ifdef WLED_ENABLE_DDP_COMPRESSION
  static uint8_t _deferredFadeAmount[MAX_NUM_SEGMENTS] = {};
  static uint8_t _deferredScaleAmount[MAX_NUM_SEGMENTS];
  static bool _initScale = []{ memset(_deferredScaleAmount, 255, sizeof(_deferredScaleAmount)); return true; }();
  #endif
  
  void Segment::fade_out(uint8_t rate) const {
  #ifdef WLED_ENABLE_DDP_COMPRESSION
    if (!isActive()) return;
    rate = (256-rate) >> 1;
    const int mappedRate = 256 / (rate + 1);
    uint8_t idx = cycleCount(); // STILL need segment index
  ```
  
  The segment index problem: `fade_out()` is a `const` method on Segment. It doesn't have direct access to its index in `strip._segments`. But `strip._segment_index` is set to the current segment's index during `service()`. Let me check if it's accessible.
  
  Looking at FX.h: `_segment_index` is private in WS2812FX. But there's `getCurrSegmentId()` — let me check.
  
  Actually, SEGENV is a macro: `#define SEGENV strip._segments[strip._segment_index]`. And `strip._segment_index` is set during `service()`. But is it accessible from `fade_out()`? `strip` is a global. `strip._segment_index` is private but `strip.getCurrSegmentId()` might exist... let me check.
  
  Parallelization: Wave 2 | Blocked by: T1 | Blocks: T3
  References: FX_fcn.cpp:1063+ (fade_out), FX.h:812+ (WS2812FX class), FX.h:1306+ (WS2812FX::service sets _segment_index)
  Acceptance criteria: `grep _deferredFadeAmount wled00/FX_fcn.cpp` returns 2+ matches. fade_out under WLED_ENABLE_DDP_COMPRESSION has no pixel iteration loop.
  Commit: N (batched)

- [ ] 3. Apply deferred fade in WS2812FX::show() compositor
  What to do: In `WS2812FX::show()` (FX_fcn.cpp ~1769), after the `blendSegment()` loop (line ~1793) and before the paint loop (line ~1807), add:
  ```cpp
  #ifdef WLED_ENABLE_DDP_COMPRESSION
  // Apply deferred fades to composited _pixels[]
  for (size_t s = 0; s < _segments.size(); s++) {
    Segment &seg = _segments[s];
    if (!seg.isActive()) continue;
    uint8_t fadeAmt = _deferredFadeAmount[s];
    uint8_t scaleAmt = _deferredScaleAmount[s];
    if (fadeAmt == 0 && scaleAmt == 255) continue; // no deferred fade for this segment
    unsigned start = seg.start;
    unsigned stop = seg.stop;
    for (unsigned i = start; i < stop && i < totalLen; i++) {
      uint32_t c = _pixels[i];
      if (scaleAmt < 255) c = fast_color_scale(c, scaleAmt);
      if (fadeAmt > 0) c = color_blend(c, seg.colors[1], fadeAmt);
      _pixels[i] = c;
    }
    // Reset deferred state for next frame
    _deferredFadeAmount[s] = 0;
    _deferredScaleAmount[s] = 255;
  }
  #endif
  ```
  This runs ONCE per frame, AFTER all segments are composited into _pixels[], BEFORE the paint loop writes to buses. It operates on _pixels[] which is a regular uint32_t buffer (NOT 32-bit-only IRAM — _pixels uses BFRALLOC_ENFORCE_PSRAM). No alignment issues.
  Must NOT modify the paint loop or the blendSegment calls.
  Parallelization: Wave 2 | Blocked by: T2 | Blocks: T4
  References: FX_fcn.cpp:1769-1808 (WS2812FX::show), FX_fcn.cpp:1787-1793 (blendSegment loop), FX_fcn.cpp:1807+ (paint loop)
  Acceptance criteria: `grep _deferredFadeAmount wled00/FX_fcn.cpp` includes a match inside `WS2812FX::show()`. The fade loop iterates `_segments`, not `_pixels` as the outer loop.
  Commit: N (batched)

- [ ] 4. Compile gate + flash test on M5StickC
  What to do: Build `m5stickc_ppp` on koero, flash to M5StickC via emiemi, cycle to effect #17 (Twinkle). Must not crash. Then cycle through 50+ effects — must be stable.
  Acceptance criteria: Effect #17 runs without POWERON_RESET. 50+ effect cycles without lockup.
  Commit: Y | feat(effects): deferred fade V2 — compositor-level, no Segment class changes

## Final verification wave
- [ ] F1. Plan compliance — no Segment class members added, no getPixelColorRaw changes
- [ ] F2. Code quality — global arrays properly sized to MAX_NUM_SEGMENTS, reset each frame
- [ ] F3. Real manual QA — M5StickC effect #17 (Twinkle) runs, all fade effects work
- [ ] F4. Scope fidelity — no FX.cpp changes, no DDP receiver changes

## Commit strategy
1 commit after all tests pass: `feat(effects): deferred fade V2 — compositor-level, no Segment class changes`

## Success criteria
- Effect #17 (Twinkle) and all fade_out effects work without crash
- Zero Segment class modifications (sizeof(Segment) unchanged from upstream)
- Deferred fade applied once per frame in compositor (not per-pixel-read)
- Compile clean on all targets
- `WLED_DISABLE_DEFERRED_FADE` flag removed (no longer needed)
