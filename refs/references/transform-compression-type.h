#pragma once
// Reference implementation: DDP Transform Compression Type (Level 3)
// ADR: refs/wled-compression-friendly-internals.md
// This file is a design reference, not production code.
//
// DDP_COMP_TYPE_TRANSFORM = 0x30 in sequenceNum upper nibble
//
// Wire format:
//   [DDP header 10B, flags=0x61, seq upper nibble=0x3]
//   [1B transform_op]
//   [1B parameter]
//   [3-4B target_color]     (RGB or RGBW, matching dataType)
//   [2B num_explicit]       (little-endian count of explicit pixel writes)
//   [per write: 2B pixel_index (LE) + 3-4B color] × num_explicit
//
// Transform operations:
//   0x01 = SCALE_TOWARD: scale each pixel toward target_color by parameter/256
//          Equivalent to: for each pixel: c = color_blend(c, target, parameter)
//          This captures fade_out() semantics.
//
//   0x02 = SCALE_MULTIPLY: multiply each pixel by parameter/256
//          Equivalent to: for each pixel: c = fast_color_scale(c, parameter)
//          This captures fadeToBlackBy() semantics.
//
//   0x03 = NOP: no transform, only explicit writes
//          Equivalent to delta with mostly zeros but avoids RLE overhead
//          for very sparse updates (1-3 pixels changed).

#include <stdint.h>
#include <stddef.h>

#define DDP_COMP_TYPE_TRANSFORM 0x30

#define DDP_TRANSFORM_SCALE_TOWARD  0x01
#define DDP_TRANSFORM_SCALE_MULT    0x02
#define DDP_TRANSFORM_NOP           0x03

struct DDP_TransformHeader {
    uint8_t  op;             // transform operation
    uint8_t  param;          // operation parameter (scale factor)
    uint8_t  target[4];      // RGBW target color (3 or 4 bytes used)
    uint16_t numExplicit;    // count of explicit pixel writes following
} __attribute__((packed));

struct DDP_ExplicitPixel {
    uint16_t index;          // pixel index (little-endian)
    uint8_t  color[4];       // RGB(W) color (3 or 4 bytes used)
} __attribute__((packed));

// Example wire sizes for 300 RGB LEDs, chase + fade_out(224):
//
//   DDP header:     10 bytes
//   Transform hdr:   6 bytes (op=0x01, param=240, target=0,0,0, num=2)
//   2 explicit px:  10 bytes (2 × (2B index + 3B RGB))
//   Total:          26 bytes
//
//   vs raw DDP:    910 bytes (10 header + 900 data)
//   vs delta+RLE: ~610 bytes (fade makes all pixels dirty)
//   Compression:    35:1
