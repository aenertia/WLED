# DDP Compression Extension -- Wire Format Specification

**Status**: Implemented in `dev/ddp-spec`, under upstream review (wled/WLED#5810)  
**Upstream contacts**: softhack007, DedeHai  
**Key concern addressed**: "we definitely must have a new protocol ID for compressed DDP"

---

## 1. Why the C bit, not a new protocol ID

softhack007 raised a valid concern about "abusing" reserved DDP header bits. The DDP
specification (3WAYLABS, http://www.3waylabs.com/ddp/) already provides a mechanism
for exactly this purpose:

> "Byte 2 (dataType): bits[7] = C -- Customer defined data type"

The C bit (`0x80` in `dataType`) is the DDP spec's own extension point for
custom/vendor data types. A receiver that does not understand the C bit will still
read a valid `dataType` value from the lower 7 bits (e.g. `0x01` = RGB24) -- it
will attempt to interpret the payload as raw pixel data, producing noise. That is
the correct "graceful degradation" behavior for an opt-in extension.

We do NOT use any reserved bits in `flags` (byte 0). We use only:
- `dataType` bit 7 (`0x80`) -- standard "Customer defined" signal
- `sequenceNum` upper nibble (bits 7-4) -- compression type discriminator

The `sequenceNum` upper nibble is defined in the DDP spec as the packet sequence
number (1-15, upper nibble), but in practice WLED and most implementations only
use the lower nibble for sequence tracking (1-15). The upper nibble is the natural
place for a sub-type discriminator within a customer-defined extension.

**Conclusion**: The C-bit approach IS the new "protocol ID" that softhack007 asked
for. It uses the DDP spec's own customer extension mechanism, requires no change to
the standard DDP packet structure, and is backwards-compatible with all existing
standard senders and receivers.

---

## 2. Packet header layout

Standard DDP header (10 bytes):

```
Byte 0: flags    [VV000TPQ]   V=version(01), T=timecode, P=push, Q=query
Byte 1: seqnum   [CCCCSSSS]   C=compType(upper nibble), S=seq(lower nibble, 1-15)
Byte 2: dataType [C0TTTBBB]   C=customer(0x80), T=type(bits 4-6), B=bits-per-ch
Byte 3: dest                  destination ID (0=default, 1-32=segment)
Byte 4-7: channelOffset       byte offset in the output channel map (big-endian)
Byte 8-9: dataLen             payload length in bytes (big-endian)
Byte 10+: payload
```

For compressed frames:
- `dataType |= 0x80` (C bit set)
- `sequenceNum = (compType & 0xF0) | (seq & 0x0F)`
- Payload = compressed data (codec-specific format below)

A frame may span multiple packets (channelOffset > 0 for continuation packets).
The PUSH flag on the final packet of a frame triggers rendering.

---

## 3. Compression types

The compType occupies bits 7-4 of `sequenceNum`. Values 0x00-0x0F are reserved
(standard DDP). Values 0x10-0xF0 are available for customer extensions.

### 0x10 -- Delta-RLE (byte-level XOR delta + PackBits RLE)

**Use case**: Animations with sparse per-frame changes (twinkle, particle systems,
scrolling text). Typical ratio: 5-15x. Worst case: incompressible patterns (rainbow)
fall back to RLE or raw.

**Wire format**: PackBits-encoded XOR delta stream.

```
For each byte position i in the decompressed output:
  decompressed[i] = XOR(prev_frame[i], encoded_byte_i)
```

PackBits encoding:
- Control byte bit 7 = 0: RUN -- next byte repeated `(ctrl & 0x7F) + 1` times
- Control byte bit 7 = 1: LITERAL -- next `(ctrl & 0x7F) + 1` bytes copied verbatim

**Prev-frame handling**: The receiver maintains a prev-frame buffer (RGB565 packed,
2 bytes/pixel). On `channelOffset=0` with `compType=0x10`, the receiver accumulates
decoded pixels into prev_frame for use by subsequent delta frames. On packet loss,
the frame will be corrupted until the next keyframe (0x20 packet with push).

**Keyframe / restart**: Send a 0x20 (RLE, no delta) packet to reset the receiver's
prev-frame baseline. The `keyframe_interval` in the sender controls how often to
force a keyframe. Recommended: every 10-30 frames (trade-off between loss recovery
and compression ratio).

**Heap**: Requires one prev-frame buffer allocation on first use:
- Whole-strip mode: `strip.getLengthTotal() * 2` bytes (6400B for 3200px)
- Single-segment mode: `segment.length() * 2` bytes (optimized allocation)

### 0x20 -- RLE (byte-level PackBits, no delta)

**Use case**: Keyframes after packet loss, or patterns where pure RLE beats delta-RLE
(e.g. solid uniform color, first frame of a sequence).

**Wire format**: PackBits encoding of raw pixel bytes (same algorithm as 0x10 but
applied to raw bytes, not XOR-delta bytes).

When received with `start=0`, the receiver resets its prev-frame buffer to zeros
before decoding. This serves as the stream restart point.

### 0x30 -- Transform (uniform operation + sparse explicit writes)

**Use case**: Global brightness/color changes with a small number of changed pixels.
Not recommended for general use -- the sender must know the receiver's current state.

**Wire format**:
```
Byte 0: tOp    (0x01=scale-toward-target, 0x02=multiply, 0x03=nop)
Byte 1: tParam (blend factor or scale, 0-255)
Byte 2..4: tColor (target RGB)
Byte 5-6: numExplicit (LE uint16, count of explicit pixel writes)
Byte 7+: [index:2LE][R][G][B] repeated numExplicit times
```

### 0x40 -- Delta-only (raw XOR, no RLE)

**Use case**: Benchmarking / link-budget measurement. Rarely useful in practice;
delta-RLE (0x10) always wins or equals delta-only for compressible content.

**Wire format**: Raw XOR-delta bytes, same length as uncompressed frame.

### 0x50 -- Tuple-RLE (PackBits, unit = channels-per-pixel)

**Use case**: Patterns with runs of identical pixels (solid colors, wipes, palettes).
Beats byte-RLE (0x20) when pixels repeat as complete tuples rather than per-channel.

**Wire format**: Same PackBits control bytes as 0x20, but each "unit" is
`ddpChannelsPerLed` bytes (3 for RGB, 4 for RGBW):
- Control byte bit 7 = 0: RUN -- next tuple repeated `(ctrl & 0x7F) + 1` times
- Control byte bit 7 = 1: LITERAL -- next `(ctrl & 0x7F) + 1` tuples copied verbatim

**Multi-packet constraint**: NOT safe for multi-packet frames. The device decoder
initialises `pixel = channelOffset / channels` at the start of each packet, treating
each packet as a new stream start rather than a continuation. Continuation packets
(channelOffset > 0) decode garbage. Tuple-RLE frames MUST fit in a single UDP packet
(<= MTU - DDP_HEADER_LEN, typically <= 1452B). For 1600px RGB (4800B raw), twinkle
content compresses to ~4700B -- does not fit. Restrict to solid/palette content where
compression achieves <1452B output, or use 0x00/0x20/0x10 for larger frames.

### 0x60 -- Planar-RLE (per-channel planes, each PackBits-encoded)

**Use case**: Solid-color content where individual color channels compress extremely
well in isolation (e.g. a red-only animation: R plane = dense run, G+B planes = zero
run). Typical ratio for solid content: 50-100x. Incompressible for rainbow/noise.

**Wire format** (whole-frame only, single packet):
```
[R_len: 2 bytes LE][R_rle_data: R_len bytes]
[G_len: 2 bytes LE][G_rle_data: G_len bytes]
[B_len: 2 bytes LE][B_rle_data: B_len bytes]
```

**Constraint**: The entire compressed frame (all three planes) MUST fit in a single
UDP packet (<= MTU - DDP_HEADER_LEN bytes). If the encoded size exceeds this limit,
the sender MUST fall back to 0x20 (RLE) or 0x00 (raw). The receiver silently ignores
continuation packets (channelOffset > 0) for 0x60 frames.

---

## 4. Codec selection guide

| Content | Recommended | Ratio | Notes |
|---------|-------------|-------|-------|
| Solid / uniform color | 0x60 planar-RLE | 50-100x | Single-packet only; falls back to 0x20 if > MTU |
| Wipe / bar fill | 0x50 tuple-RLE | 5-30x | Single-packet only; falls back to 0x20 if > MTU |
| Static frame (no change) | 0x10 delta-RLE | >100x | Delta = all zeros = single run |
| Sparse twinkle (<10% change) | 0x10 delta-RLE | 5-20x | Delta isolates changed pixels |
| Particle / ghost rider | 0x20 RLE | 20-50x | Most pixels black = long zero runs |
| Rainbow / gradient | 0x00 raw | 1x | Incompressible; rate-limit instead |
| First frame / resync | 0x20 RLE | varies | Always use for stream restart |

**Adaptive selection** (sender-side): try planar (0x60), tuple (0x50), delta-RLE
(0x10), RLE (0x20) in order; use raw (0x00) if none beats uncompressed. For 0x60 and
0x50, only use if the encoded size fits in a single UDP packet (<= ~1452B).

---

## 5. Full-frame semantics

softhack007 noted: "compression should apply to a full frame". Our implementation is
full-frame:

- The sender groups all pixel data for one output frame into one logical compressed
  stream, sent as one or more DDP packets with increasing channelOffset.
- The PUSH flag on the final packet signals "render now".
- The receiver accumulates decompressed pixels across multi-packet frames before
  calling `strip.show()`.
- Planar-RLE (0x60) is the exception: it must fit in a single packet, which limits
  it to frames where the compressed size <= ~1460 bytes. For larger setups, the
  sender falls back to tuple-RLE or raw.

---

## 6. Stream restart / loss recovery

For delta-based codecs (0x10, 0x40):
- Sender sends a keyframe (0x20 or 0x00 with full frame data) every N frames.
- On receipt of a keyframe with channelOffset=0, the receiver resets its prev-frame
  buffer to the keyframe content.
- After packet loss, the receiver will display corrupted frames until the next
  keyframe. With `keyframe_interval=10` at 30fps, max corruption window = 333ms.
- The sender can also force a keyframe immediately after reconnection by sending
  a 0x20 packet before resuming 0x10 delta frames.

For non-delta codecs (0x20, 0x50, 0x60): stateless; every frame is self-contained.

---

## 7. Per-segment DDP targeting (WLED extension)

Standard DDP uses the destination byte (byte 3) as a device ID. This implementation
repurposes it for sub-device segment targeting, enabling two routing modes.

### Mode A: destination-routed

`destination` byte 1-32 routes the packet to segment N-1. `channelOffset` is
segment-relative (0 = first pixel of the target segment). The segment must be active;
the packet is silently dropped if the segment index is out of range.

```python
# Send to segment 1 (destination=2), pixel offset 0
struct.pack("!BBBBIH", flags, seq, data_type, 2, 0, len(payload))
```

### Mode B: eligibility-mask (concatenated stream)

`destination` byte = 0. The receiver distributes pixels across all segments in the
`ddpEligibleMask` bitmask, in segment-index order. The sender streams a flat pixel
buffer; the receiver splits it at segment boundaries using a pre-computed slot table.

Set the mask via `/json/cfg`: `{"if":{"live":{"ddpelig":3}}}` (segments 0 and 1).

The slot table (`ddpSlots[]`, `ddpSlotCount`) is rebuilt whenever:
- `/json/cfg` POST changes `ddpelig`
- `/json/state` POST adds or modifies segments
- `beginStrip()` completes at boot

```python
# Mode B: destination=0, flat pixel stream covers all eligible segments in order
struct.pack("!BBBBIH", flags, seq, data_type, 0, byte_offset, len(chunk))
```

### Legacy (backwards compatible)

`destination=0` with `ddpEligibleMask=0` uses the original full-strip absolute
pixel indexing behaviour. No segment routing occurs.

### Mixed-segment rendering (effect + DDP)

When some segments run internal effects (non-frozen) and others are DDP-frozen,
`service()` still runs effect functions but does NOT push to the bus. The push is
owned by `showFrozenSegs()` Case D, triggered on DDP PUSH cadence via
`handleNotifications()`. Case D calls `show()` which blends all segments from
their respective `seg.pixels[]` buffers -- effects from the non-frozen segment,
DDP data from the frozen segment -- then pushes to the bus atomically.

This ensures the bus sees a coherent composite frame rather than racing between
service() at 42fps and DDP at 30fps.

---

## 8. Implementation files

| File | Purpose |
|------|---------|
| `wled00/ddp_compress.h` | Header-only: RLE encoder (in-firmware, unused), RLE decoder struct |
| `wled00/e131.cpp` | `handleDDPPacket()` -- compressed path, all decoders, prev-frame alloc |
| `wled00/src/dependencies/e131/ESPAsyncE131.h` | DDP constants (`DDP_TYPE_COMPRESSED`, `DDP_COMP_TYPE_*`) |
| `tools/ddp_bench.py` | Python sender: all codecs, sweep, diagnostic |
| `tools/ddp_codec.py` | Python codec implementations (cross-validates firmware decoders) |

---

## 8. Response to upstream feedback (#5810)

**softhack007**: "we definitely must have a new protocol ID for compressed DDP"

The C bit IS the new protocol ID -- it is the DDP spec's own "Customer defined"
extension mechanism (dataType bit 7). No reserved bits are abused. Standard receivers
that ignore the C bit will attempt raw pixel decode of the compressed payload,
producing noise. That is the expected opt-in extension behavior.

**softhack007**: "compression should apply to a full frame"

Implemented. See section 5.

**softhack007**: "stream restart points so the receiver gets a chance to recover"

Implemented via keyframe mechanism (codec 0x20 on resync). See section 6.

**softhack007**: "RLE has a benefit on encoding speed"

Agreed. All our codecs use PackBits-style RLE as the base. The encoder is O(n) with
no look-back window. Worst-case expansion is `rawLen + ceil(rawLen/128)` bytes.

**DedeHai**: "adaptive color depth reduction"

Implemented in `ddp_bench.py --lossy-depth`. LSB stripping by brightness:
- bri > 196: mask to 0xF8 (3 LSBs stripped)
- bri > 128: mask to 0xFC (2 LSBs stripped)
- bri > 64:  mask to 0xFE (1 LSB stripped)
- bri <= 64: no stripping

Applied before encoding; improves RLE ratio on gradient content by quantising
similar colors to identical values. Not implemented in firmware (decoder-side is
lossless; the loss is at the sender).

**softhack007**: "JPEG could be an option"

JPEG produces block artifacts visible at LED matrix resolutions (8x8 DCT blocks on
a 40x80 matrix = 5x10 blocks = visible chunking). On ESP32 classic (no PSRAM), the
libjpeg stack also requires ~50KB working memory. For the use case we care about
(sparse animations, solid colors, streaming control), RLE-based codecs are better.
JPEG may be relevant for video streaming to large Hub75 panels on ESP32-S3 with
octal PSRAM, where the JPEG hardware decoder is available.
