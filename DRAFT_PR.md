# feat(ddp): compressed DDP decode in handleDDPPacket()

## Summary

Adds receiver-side support for compressed DDP streams using the RLE codec from `ddp_compress.h`. When the DDP compressed flag (bit 5) is set, the receiver decodes the compressed pixel stream and writes the reconstructed pixels to the LED strip. Backwards compatible — uncompressed DDP is unchanged.

## What it does

When `WLED_ENABLE_DDP_COMPRESSION` is defined:
1. **RLE decode**: decompresses byte-level RLE-encoded pixel data
2. **Delta decode**: XOR against previous frame buffer (`ddpPrevFrame`) to reconstruct full pixels
3. **Transform decode**: applies uniform color operations (blend-toward, scale-multiply) plus sparse explicit pixel writes
4. **Keyframe handling**: RLE-only packets (no delta) reset the previous frame buffer

Compression type is signalled in the upper nibble of the DDP sequence number byte:
- `0x10` — delta+RLE (most frames)
- `0x20` — RLE only (keyframes)
- `0x30` — transform mode (fade/scale + sparse writes)

## Design decisions

- **Lazy allocation**: `ddpPrevFrame` (RGB565, 2 bytes/pixel) allocated on first compressed packet, freed via `ddpFreePrevFrame()` when DDP streaming stops
- **Guard-railed**: allocation capped at 12800 pixels (~25KB) to prevent OOM on constrained devices
- **No routing changes**: compressed pixels use the same `setRealtimePixel()` path as standard DDP
- **Backwards compatible**: uncompressed DDP (flag bit 5 clear) follows the unchanged upstream code path

## Files changed

- `wled00/ddp_compress.h` (new) — RLEEncoder, RLEDecoder structs
- `wled00/e131.cpp` (modified) — compression decode in `handleDDPPacket()`
- `wled00/src/dependencies/e131/ESPAsyncE131.h` (modified) — DDP compression flag constants, enlarged packet buffer

## Testing

- Tested with `tools/ddp_bench.py --compress delta_rle` sending compressed streams
- Verified delta decode correctness: keyframe → delta frames → visual output matches uncompressed
- Verified backwards compatibility: standard DDP senders work unchanged
- 72+ hours continuous compressed streaming on M5StickC over 1.5Mbaud PPP

## Dependencies

- Requires: `pr/ddp-rle-codec` (`ddp_compress.h`)
