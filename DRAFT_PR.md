# fix(segment): alloc-fill-swap-free in setName() to prevent dual-core race

## Summary

`Segment::setName()` frees the old name buffer before allocating and filling the new one. On ESP32 dual-core, the effects service loop (Core 1) reads `segment.name` while the main loop (Core 0) calls `setName()`. The free-then-alloc pattern leaves a window where `name` points to freed heap — a use-after-free that can crash the effects loop or produce garbled text in the scrolling text effect.

## What changed

Rewrote the allocation sequence in `Segment::setName()` (in `wled00/FX_fcn.cpp`):

**Before** (unsafe):
1. Free old buffer
2. Allocate new buffer
3. Copy new name into buffer

**After** (safe):
1. Allocate new buffer
2. Copy new name into new buffer
3. Start transition (copy constructor deep-copies the still-valid current name)
4. Swap pointer (`name = newBuf`) — atomic on 32-bit ESP32
5. Free old buffer

The effects loop always sees either the old valid pointer or the new valid pointer — never a dangling one.

## Impact

- Eliminates a use-after-free race condition on dual-core ESP32.
- No functional change to single-core builds (ESP8266) — the new order is safe there too.
- The scrolling text effect (`FX_MODE_2DSCROLLTEXT`) now correctly deep-copies the previous name for blend transitions before the swap.

## Testing

- Verified on M5StickC (ESP32-PICO-D4, dual-core) with rapid segment renames via JSON API while scrolling text effect was active.
- No crashes or garbled text observed over extended testing.
