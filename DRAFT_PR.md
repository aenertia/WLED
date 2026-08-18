# feat(text): drop shadow for scrolling text effect

**Forgejo**: Fixes #12

**STATUS: STUB — implementation pending in fontmanager**

## Summary

Drop shadow rendering for the scrolling text effect. The current
implementation lives in `FX.cpp` (`mode_2Dscrollingtext`) as inline
rendering logic, not in `fontmanager.cpp/h`.

## Current State

The drop shadow code exists in the `pr/effects-deferred-fade` branch as
part of the scrolling text effect changes in `FX.cpp`:

- **Shadow direction**: 8 compass directions via `c3` bits 0-2
- **Shadow distance**: 4 distance multipliers via `c3` bits 3-4
- **Shadow intensity**: Mapped from `c1` (1-255 → brightness 15-180)
- **Rendering**: Draws shadow glyph at offset before main glyph

## Planned fontmanager Integration

To make drop shadow reusable across effects (not just scrolling text),
the shadow rendering should move into `fontmanager.cpp`:

- `FontManager::drawCharacterWithShadow(unicode, x, y, col1, col2, rotate, shadowDx, shadowDy, shadowAlpha)`
- Shadow color derived from foreground via `color_fade()`
- Single API call replaces the inline shadow+main double-draw pattern

## Why This Branch is a Stub

No shadow-specific code exists in `fontmanager.cpp` or `fontmanager.h` yet.
The shadow rendering is currently embedded in the effect function. This
branch is a placeholder for the planned fontmanager-level implementation.

## Files Changed

- `DRAFT_PR.md` (this file only — stub branch)

## Dependencies

- `pr/text-aa-fonts`: Anti-aliased font support (4bpp rendering in fontmanager)
- `pr/effects-deferred-fade`: Current inline shadow implementation in FX.cpp