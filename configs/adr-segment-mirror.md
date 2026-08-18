# ADR: TFT-to-Matrix Segment Mirroring via Copy Segment Effect

**Status**: Accepted
**Date**: 2026-08-16
**Context**: M5StickC + WS2812B 8×32 matrix panel on G26

## Decision

Use WLED's built-in **Copy Segment** effect (`FX_MODE_COPY = 77`, `mode_copy_segment()` in `FX.cpp:144`) to mirror a TFT preview region onto the physical WS2812B matrix panel. No custom effect code needed.

## Background

The original plan was to implement a new "Follow Segment X" effect. Research revealed that WLED already ships `mode_copy_segment()` which does exactly this:

- Reads pixels from a source segment's render buffer (or global buffer for last-frame reads)
- Writes them to the current segment at matching (x,y) coordinates
- Supports 2D→2D, 1D→2D, 2D→1D, and 1D→1D copies
- Source segment ID selected via `custom3` slider (0-31)
- Color adjustment via intensity (hue shift), custom1 (lighten), custom2 (brighten)
- Axis flip via `check1` checkbox
- Global buffer read (last rendered frame) via `check2` checkbox

### Cross-Segment Pixel Read Architecture

`getPixelColorXY()` uses static class members `_vWidth`/`_vHeight` for bounds checking. When reading from another segment, the effect must:

```cpp
sourcesegment.setDrawDimensions();  // set static dims to source
color = sourcesegment.getPixelColorXY(x, y);
SEGMENT.setDrawDimensions();        // restore to current segment
```

This is already handled correctly in `mode_copy_segment()`.

## Matrix Coordinate System

Current 2-panel matrix configuration:

| Panel | Type | Offset | Size | Physical LEDs |
|-------|------|--------|------|---------------|
| 0 | TFT_MATRIX (bus 0) | (0, 0) | 40×80 | 3200 |
| 1 | WS2812B (bus 1) | (0, 80) | 8×32 | 256 |

`Segment::maxWidth = 40`, `Segment::maxHeight = 112`

Grid is 40×112 = 4480 logical pixels. The WS2812B panel occupies only columns 0-7 of rows 80-111; columns 8-39 of those rows are phantom (mapped to 0xFFFF in `customMappingTable`).

## 3-Segment Layout

```
Matrix Grid (40 wide × 112 tall)
    x=0  x=4          x=36  x=40
┌────┬────────────────────┬────┐
│    │ seg0: 32×8 preview │    │ y=0
│    │ (x:4-36, y:0-8)    │    │
│    │ centred, 4px margin │    │
├────┴────────────────────┴────┤ y=8
│                              │
│   seg1: TFT Main 40×72      │
│   (x:0-40, y:8-80)          │
│                              │
│                              │
└──────────────────────────────┘ y=80
┌────────┐
│ seg2   │  (phantom cols 8-39)
│ 8×32   │
│ WS2812B│
│ Copy   │  o1=true (axis flip)
│ src=0  │  rev=true (mirror fix)
└────────┘ y=112
```

### Segment Definitions

| Seg | Start | Stop | StartY | StopY | Size | Bus | Effect | Flags | Notes |
|-----|-------|------|--------|-------|------|-----|--------|-------|-------|
| 0 | 4 | 36 | 0 | 8 | 32×8 | TFT | Any 2D | o1=true | Preview (centred, 4px margins) |
| 1 | 0 | 40 | 8 | 80 | 40×72 | TFT | Any 2D | | Main TFT area |
| 2 | 0 | 8 | 80 | 112 | 8×32 | WS2812B | Copy Segment (77), c3=0 | o1=true, rev=true | Mirrors seg0 |

**Orientation handling**: The WS2812B panel is physically landscape (32 wide × 8 tall) but wired column-first (w=8, h=32 in panel config). The Copy Segment axis flip (`o1=true`) transposes the 32×8 source to the 8×32 destination grid. The `rev=true` corrects the resulting horizontal mirror so text reads left-to-right.

**Tested layout** (non-overlapping on TFT, centred preview):

| Seg | Start | Stop | StartY | StopY | Size | Effect | Flags | Notes |
|-----|-------|------|--------|-------|------|--------|-------|-------|
| 0 | 4 | 36 | 0 | 8 | 32×8 | Any 2D | o1=true | TFT preview (top strip, centred with 4px margins) |
| 1 | 0 | 40 | 8 | 80 | 40×72 | Any 2D | | TFT remainder (below preview) |
| 2 | 0 | 8 | 80 | 112 | 8×32 | Copy Segment (77), c3=0 | o1=true, rev=true | WS2812B mirrors seg0 |

**Key flags on seg2 (Copy Segment)**:
- `o1=true` (Axis flip): swaps X↔Y when reading from source, mapping the 32×8 landscape source to the 8×32 column-first WS2812B panel
- `rev=true`: reverses X direction to correct text/content mirroring caused by the axis swap

**Preview centring**: seg0 starts at x=4 instead of x=0, giving 4 black pixels on each side of the 32-pixel-wide preview within the 40-pixel-wide TFT. The Copy Segment reads from seg0's virtual coordinates (0-31, 0-7) regardless of the physical offset.

## Copy Segment Effect Parameters

Metadata: `"Copy Segment@,Color shift,Lighten,Brighten,ID,Axis(2D),FullStack(last frame);;;12;ix=0,c1=0,c2=0,c3=0"`

| Parameter | Slider/Check | Range | Default | Purpose |
|-----------|-------------|-------|---------|---------|
| Speed | unused | - | - | - |
| Intensity | Color shift | 0-255 | 0 | Hue rotation of copied pixels |
| Custom1 | Lighten | 0-255 | 0 | Additive brightness |
| Custom2 | Brighten | 0-255 | 0 | Multiplicative brightness |
| Custom3 | ID | 0-31 | 0 | Source segment index |
| Check1 | Axis(2D) | bool | false | Swap X/Y when reading source |
| Check2 | FullStack | bool | false | Read from global buffer (last frame) instead of segment buffer |

For our use case: `c3=0` (source = seg0), all others at defaults (no color adjustment, no axis flip, segment buffer read).

## Risks & Mitigations

1. **Render order**: Segments render in index order. seg0 renders before seg2, so seg2 always reads seg0's current-frame data (not stale). If using `check2` (global buffer), there's a 1-frame lag.

2. **Dimension mismatch**: Copy Segment iterates over the *destination's* virtual dimensions. If source is smaller, `getPixelColorXY` returns black for out-of-bounds reads. Both seg0 and seg2 are 8×32, so this is a 1:1 copy.

3. **`custom3` range**: Only 5 bits (0-31). With MAX_NUM_SEGMENTS typically 32, this covers all possible segments.

4. **Performance**: One extra `getPixelColorXY` per pixel per frame. For 256 pixels at 43 FPS, this is ~11K reads/frame — negligible.

## Alternatives Considered

1. **New "Follow Segment" effect**: Rejected — `mode_copy_segment()` already exists and does everything needed.
2. **DDP per-segment targeting**: Separate feature (DDP receiver writes to specific segment offset). Not needed for TFT-to-matrix mirroring.
3. **Ledmap aliasing**: Map WS2812B physical LEDs to the same logical positions as TFT preview pixels. Would work but couples the hardware layout to the effect — less flexible than Copy Segment.

## Implementation

No code changes required. Configuration only via `/json/cfg` API or WLED web UI.
