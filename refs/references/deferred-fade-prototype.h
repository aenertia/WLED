#pragma once
// Reference implementation: Deferred Fade for WLED Segments
// ADR: refs/wled-compression-friendly-internals.md
// This file is a design reference, not production code.

#include <stdint.h>

// --- Level 1: Snap-to-target (5-line patch to FX_fcn.cpp:1075-1077) ---
//
// Current:
//   if (delta == 0) delta += (c2 == c1) ? 0 : (c2 > c1) ? 1 : -1;
//
// Proposed:
//   if (delta == 0) c1 = c2;  // snap to target when proportional fade exhausted
//
// Visual impact: pixels within ~17/255 of target snap instantly.
// At 30fps: 560ms of sub-7% brightness change → 33ms. Below human JND.


// --- Level 2: Deferred Fade (segment-level state, ~2 bytes per segment) ---

// State to add to Segment class (FX.h, private section):
//   uint8_t _fadeAccum = 0;      // accumulated fade blend factor
//   uint8_t _scaleAccum = 255;   // accumulated fadeToBlackBy scale (255 = no fade)

// Modified fade_out() — O(1), no pixel writes:
// void Segment::fade_out(uint8_t rate) const {
//     rate = (256-rate) >> 1;
//     const int mappedRate = 256 / (rate + 1);
//     // Accumulate fade factor instead of applying per-pixel
//     // blend8(a, b) ≈ a + (b - a) * mappedRate / 256
//     _fadeAccum = _fadeAccum + ((255 - _fadeAccum) * mappedRate) / 256;
//     if (_fadeAccum > 253) _fadeAccum = 255;  // snap to fully faded
// }

// Modified fadeToBlackBy() — O(1), no pixel writes:
// void Segment::fadeToBlackBy(uint8_t fadeBy) const {
//     if (fadeBy == 0) return;
//     uint16_t newScale = ((uint16_t)_scaleAccum * (255 - fadeBy)) >> 8;
//     _scaleAccum = (uint8_t)newScale;
// }

// Modified getPixelColorRaw() — apply deferred fade on read:
// inline uint32_t getPixelColorRaw(unsigned i) const {
//     uint32_t c = pixels[i];
//     if (_scaleAccum < 255)
//         c = fast_color_scale(c, _scaleAccum);
//     if (_fadeAccum > 0)
//         c = color_blend(c, colors[1], _fadeAccum);
//     return c;
// }

// Modified setPixelColorRaw() — written pixels get current state baked in:
// inline void setPixelColorRaw(unsigned i, uint32_t c) const {
//     // When an effect explicitly writes a pixel, it expects to see that
//     // exact value on the next getPixelColorRaw() call. So we must store
//     // the value relative to the current deferred fade state.
//     // Option A (simple): just store c directly. getPixelColorRaw will
//     //   apply fade on read, so the effect sees a faded version of c.
//     //   This is usually correct — effects write "bright" values expecting
//     //   them to fade over subsequent frames.
//     // Option B (compensate): store the inverse-faded value so that
//     //   getPixelColorRaw(i) returns exactly c. More complex, rarely needed.
//     pixels[i] = c;
// }

// Flush point — materialize deferred state into buffer:
// Called before blur() (which needs actual neighbor values) or periodically
// to prevent fade factor overflow.
// void Segment::flushDeferredFade() const {
//     if (_fadeAccum == 0 && _scaleAccum == 255) return;
//     for (unsigned i = 0; i < rawLength(); i++) {
//         uint32_t c = pixels[i];
//         if (_scaleAccum < 255) c = fast_color_scale(c, _scaleAccum);
//         if (_fadeAccum > 0) c = color_blend(c, colors[1], _fadeAccum);
//         pixels[i] = c;
//     }
//     _fadeAccum = 0;
//     _scaleAccum = 255;
// }

// Integration points:
// 1. blur() must call flushDeferredFade() before executing (neighbor reads
//    need materialized values for correct convolution)
// 2. blendSegment() in WS2812FX::show() reads via getPixelColorRaw() which
//    already applies the deferred fade — no change needed there
// 3. beginDraw() or handleTransition() could call flushDeferredFade() if
//    the effect mode changes (new effect starts with clean state)
