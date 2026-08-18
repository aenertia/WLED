# feat(text): DejaVu Bold 18px + 40px anti-aliased fonts for scrolling text

**Forgejo**: Fixes #11

## Summary

Adds high-quality anti-aliased bitmap fonts for the scrolling text effect,
plus 4bpp rendering support in the font manager.

## New Fonts

| Font | Size | BPP | Use Case |
|------|------|-----|----------|
| DejaVu Bold 18px | 18px height | 4bpp (16-level AA) | Standard LED matrix density |
| DejaVu Bold 40px | 40px height | 4bpp (16-level AA) | Large displays / low density |
| Montserrat 11px | 11px height | 1bpp | Compact alternative |

## Font Manager Changes

- **MAX_FONTS**: 5 → 7 (font numbers 0-6)
- **`bpp()` accessor**: Reads bits-per-pixel from WBF header flags byte
  (bits 2-4: 0=1bpp, 2=4bpp, 3=8bpp)
- **Glyph size calculation**: Updated throughout to handle 4bpp nibble
  packing (`(pixels + 1) / 2` instead of `(bits + 7) / 8`)
- **`drawCharacter()`**: 4bpp rendering path with alpha blending —
  `color_blend(bg, fg, alpha)` for anti-aliased edges

## Anti-Aliasing Technique

4bpp fonts store 16 alpha levels per pixel (nibble-packed, high nibble first).
Rendering scales nibble value 0-15 to alpha 0-255 (`alpha = nibble * 17`),
then blends foreground over background. Fully opaque pixels (alpha=255)
skip the blend for performance.

## Files Changed

- `wled00/src/font/font_dejavu_18px.h` (new)
- `wled00/src/font/font_dejavu_40px.h` (new)
- `wled00/src/font/font_montserrat_11px.h` (new)
- `wled00/fontmanager.cpp` — includes, font switch, bpp calculations, AA rendering
- `wled00/fontmanager.h` — MAX_FONTS, bpp() accessor, flags documentation

## Note

The FX.cpp scrolling text effect font selector range (`map(..., 0, 4)` → `0, 6`)
is in a separate branch (`pr/effects-deferred-fade`) and must be merged
alongside this PR for the new fonts to be selectable via the UI.
## Related upstream issues

| Issue/PR | Repo | Title | Relevance |
|----------|------|-------|-----------|
| [#4938](https://github.com/wled/WLED/issues/4938) | Aircoookie/WLED | H, M, W indistinguishable in scrolling text at small sizes | Anti-aliased fonts at 18px directly address this |
| [#5101](https://github.com/wled/WLED/issues/5101) | Aircoookie/WLED | Cyrillic font support for scrolling text | AA font infrastructure enables non-Latin character sets |
| [PR #5372](https://github.com/wled/WLED/pull/5372) | Aircoookie/WLED | Custom font infrastructure (merged) | **This PR builds on #5372** — the font loading mechanism is already in place |
| [PR #4982](https://github.com/wled/WLED/pull/4982) | Aircoookie/WLED | PixelForge font improvements (merged) | Related font work |
