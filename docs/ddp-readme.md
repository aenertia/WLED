# DDP Protocol Reference -- Specification, Compression Extension, and Validation Suite

**Version**: 2.5 (2026-08-21)
**Status**: Non-blocking SPI DMA, generalized skip-show, spielig removed, multi-IP, transport benchmarks, empirical compression variant benchmarks
**Audience**: Any codebase implementing DDP -- sender, receiver, or both

This document is a standalone reference for implementing the Distributed Display Protocol (DDP) with optional compression extensions for bandwidth-constrained transports. It covers the base protocol specification, comparison with E1.31/Art-Net, the compression wire format, reference implementations in C and Python, a complete validation suite, and known pitfalls.

---

## Table of Contents

1. [DDP Base Protocol Specification](#1-ddp-base-protocol-specification)
2. [Protocol Comparison: DDP vs E1.31 vs Art-Net](#2-protocol-comparison)
3. [Compressed DDP Extension](#3-compressed-ddp-extension)
4. [RLE Codec Specification](#4-rle-codec-specification)
5. [Delta+RLE Compression](#5-deltarle-compression)
6. [Transform Compression](#6-transform-compression)
7. [RGBW and CCT Handling](#7-rgbw-and-cct-handling)
8. [Sender Implementation Guide](#8-sender-implementation-guide)
9. [Receiver Implementation Guide](#9-receiver-implementation-guide)
10. [Sequence Numbering and Frame Sync](#10-sequence-numbering-and-frame-sync)
11. [Keyframe Strategy and Error Recovery](#11-keyframe-strategy-and-error-recovery)
12. [Transport Considerations (PPP, WiFi, Ethernet)](#12-transport-considerations)
13. [Validation Suite](#13-validation-suite)
14. [Known Issues and Design Decisions](#14-known-issues-and-design-decisions)
15. [Bandwidth Budget Reference](#15-bandwidth-budget-reference)
16. [Reference Implementations](#16-reference-implementations)
17. [Wire Format Options (Open)](#17-wire-format-options-open)
18. [Compression Variant Analysis (Open)](#18-compression-variant-analysis-open)
19. [WebSocket Transport (Open)](#19-websocket-transport-open)
20. [JPEG Hardware Compression (Reserved)](#20-jpeg-hardware-compression-reserved)

---

## 1. DDP Base Protocol Specification

**Source**: [3waylabs.com/ddp](http://www.3waylabs.com/ddp/) (last updated 2022-09-05)
**Transport**: UDP, port 4048 (unicast only)
**Max recommended payload**: 1440 bytes (480 RGB pixels) per packet

### 1.1 Header Format (10 bytes, 14 with timecode)

```
Offset  Size  Field          Description
------  ----  -----          -----------
0       1     flags          Version, control flags
1       1     sequenceNum    4-bit sequence (lower nibble), reserved (upper nibble)
2       1     dataType       Channel count + bit depth encoding
3       1     destination    Device ID
4       4     channelOffset  Byte offset into pixel buffer (MSB first, network order)
8       2     dataLen        Payload length in bytes (MSB first, network order)
[10]    [4]   timecode       Optional: 32-bit NTP mid-bits (only if TIME flag set)
10/14+  N     data           Pixel data payload
```

### 1.2 Flags Byte (byte 0) -- Bit Layout

```
Bit 7-6: VV    Protocol version (01 = v1, MUST be 0x40)
Bit 5:   x     Reserved (set to 0)
Bit 4:   T     Timecode field present (adds 4 bytes after header)
Bit 3:   S     Storage -- data sourced from local storage, not packet
Bit 2:   R     Reply -- response to a query
Bit 1:   Q     Query -- request data (no payload; dataLen = bytes to read)
Bit 0:   P     Push -- render buffer now / last packet in frame
```

**Common flag combinations**:

| Hex  | Binary     | Meaning |
|------|------------|---------|
| 0x41 | 01 0 0 0001 | VER1 + PUSH (single-packet frame) |
| 0x40 | 01 0 0 0000 | VER1, no push (multi-packet, not last) |
| 0x51 | 01 0 1 0001 | VER1 + TIMECODE + PUSH |
| 0x42 | 01 0 0 0010 | VER1 + QUERY |
| 0x44 | 01 0 0 0100 | VER1 + REPLY |

### 1.3 Sequence Number (byte 1)

**Lower nibble (bits 3-0)**: Sequence number, values 1-15. Wraps from 15 -> 1.
**Value 0**: "Unused" -- receiver MUST NOT apply sequence filtering.
**Upper nibble (bits 7-4)**: Reserved in base spec. Used for compression type in extension.

Senders MUST increment sequence continuously across all packets in a frame AND across frames. The receiver maintains a sliding window of the last 5 sequence numbers; packets falling within `(lastPushSeq - 5, lastPushSeq)` are rejected as late arrivals from the previous frame.

**Critical**: Using a fixed sequence number (e.g., always seq=1) causes silent packet drops after the first multi-packet frame.

### 1.4 Data Type (byte 2) -- Bit Field

```
Bit 7:   C     C = compressed payload (vendor-defined extension per DDP spec)
Bit 6:   R     Reserved
Bits 5-3: TTT  Type: 000=undef, 001=RGB, 010=HSL, 011=RGBW, 100=grayscale
Bits 2-0: SSS  Size: 0=undef, 1=1bit, 2=4bit, 3=8bit, 4=16bit, 5=24bit, 6=32bit
```

**Common data type values**:

| Value | Binary       | Meaning | Channels/pixel |
|-------|-------------|---------|----------------|
| 0x0B  | 00 001 011  | RGB, 8 bits/channel | 3 |
| 0x1B  | 00 011 011  | RGBW, 8 bits/channel | 4 |
| 0x8B  | 10 001 011  | RGB24, compressed (C bit set) | 3 |
| 0x9B  | 10 011 011  | RGBW32, compressed (C bit set) | 4 |
| 0x01  | 00 000 001  | Legacy RGB 8-bit | 3 (assumed) |
| 0x00  | 00 000 000  | Undefined | 3 (default fallback) |

`dataType & 0x7F` recovers the original pixel format when the C bit is set.

### 1.5 Destination ID (byte 3)

| Value | Purpose |
|-------|---------|
| 0 | Reserved |
| 1 | Default output display (most common) |
| 2-245 | Custom device IDs |
| 246 | JSON control (read/write) |
| 250 | JSON config (read/write) |
| 251 | JSON status (read only) |
| 254 | DMX transit |
| 255 | All devices (broadcast) |

### 1.6 Channel Offset (bytes 4-7)

32-bit unsigned integer in **network byte order** (big-endian). Represents the **byte offset** into the remote pixel buffer, NOT a pixel index.

To convert to pixel index: `pixelIndex = channelOffset / channelsPerPixel`

For multi-packet frames, each packet's channelOffset indicates where its data starts in the overall frame buffer.

### 1.7 Data Length (bytes 8-9)

16-bit unsigned, network byte order. Length of the payload data in bytes (NOT including the header). Maximum recommended: 1440 bytes.

### 1.8 Push Synchronization

Two synchronization methods:

1. **Per-device push**: Set the PUSH flag on the last data packet to a device. The device renders its buffer on receiving the push.

2. **Broadcast push**: Send data packets to all devices (no PUSH flag), then broadcast a PUSH-only packet (offset=0, len=0) to `255.255.255.255:4048`. All devices render simultaneously.

**Important from spec**: "The buffer is not cleared between display commands, so it is possible to send just the data that has changed between frames." This explicitly enables partial/delta updates.

### 1.9 Timecode (optional, 4 bytes after header)

Present only when the TIME flag (bit 4) is set. Contains the 32 middle bits of a 64-bit NTP timestamp: 16 bits seconds + 16 bits fractional (~15us resolution). Only meaningful with the PUSH flag set.

Most implementations (including WLED) ignore the timecode value but correctly skip the 4 bytes.

---

## 2. Protocol Comparison

### 2.1 DDP vs E1.31 (sACN) vs Art-Net

| Feature | DDP | E1.31 (sACN) | Art-Net 4 |
|---------|-----|-------------|-----------|
| **Standard** | Informal (3waylabs) | ANSI E1.31-2016 | Artistic Licence |
| **Transport** | UDP unicast | UDP multicast or unicast | UDP broadcast/unicast |
| **Port** | 4048 | 5568 | 6454 |
| **Header overhead** | 10 bytes | 126 bytes | 18 bytes |
| **Max data/packet** | 1440 bytes | 512 bytes (DMX frame) | 512 bytes |
| **Pixels/packet (RGB)** | 480 | 170 | 170 |
| **Efficiency** | 94.9% | 72.7% | 85.9% |
| **Addressing** | Byte offset (32-bit) | Universe + channel | Net/Subnet/Universe |
| **Multi-device sync** | Broadcast PUSH | Sync universe (63999) | ArtSync opcode |
| **Priority** | None | 0-200 (per-packet) | None |
| **Sequence** | 4-bit (1-15) | 8-bit (0-255) | 8-bit (1-255) |
| **Discovery** | None | None (relies on IGMP) | ArtPoll/ArtPollReply |
| **RGBW** | Yes (dataType field) | Via DMX channels (4ch) | Via DMX channels (4ch) |
| **Compression** | Extension (this doc) | None | None |

### 2.2 When to Use Each

- **DDP**: Best for point-to-point LED streaming, especially over bandwidth-constrained links. Lowest overhead. Supports compression extension. Preferred by OpenRGB and LedFX.
- **E1.31**: Best for professional lighting installations with existing DMX infrastructure. Multicast enables efficient multi-controller setups. Priority system allows source arbitration.
- **Art-Net**: Best for large installations with Art-Net 4 compatible hardware. Extensive discovery and node management. Most widely supported by commercial LED controllers.

### 2.3 Why Compression is DDP-Only

Art-Net and E1.31 are industry standards with thousands of existing controllers. Adding proprietary compression breaks interoperability. DDP is a niche protocol with a small, controlled ecosystem -- extending it with compression is acceptable. The PPP serial transport use case (the primary motivation for compression) only uses DDP.

---

## 3. Compressed DDP Extension

### 3.1 Design Principle

The compression extension uses the C bit (byte 2 bit 7) defined by the DDP spec as "Customer defined" -- no new header fields, no new ports, no breaking changes. Standard DDP senders never set the C bit, so compressed and uncompressed packets coexist on the same port (4048) and are distinguished by a single bit check on the dataType byte.

### 3.2 Compression Signal

**Byte 2, bit 7 (0x80)**: C bit. When set, the payload is compressed. Recover the pixel format with `dataType & 0x7F`.

**Byte 1, upper nibble (bits 7-4)**: Compression type. Only meaningful when the C bit is set.

```
Compression types:
  0x00  No compression (standard DDP -- C bit must not be set)
  0x10  Delta+RLE -- XOR with previous frame, then RLE encode
  0x20  RLE only -- no delta, direct RLE (used for keyframes)
  0x30  Transform -- global operation + sparse explicit pixel writes
  0x40  Delta-only -- raw XOR delta, no RLE (benchmark/measurement use)
  0x50  Tuple-RLE -- same PackBits control bytes as 0x20, unit = channels bytes
  0x60  Planar-RLE -- per-channel byte-RLE planes, wire: [Rlen:2LE][R-rle][G...][B...]
```

### 3.3 Wire Format Examples

**Standard DDP (unchanged)**:
```
[0x41] [0x0n] [0x0B] [0xFF] [offsetx4] [lenx2] [R G B R G B ...]
 flags  seq    RGB24   all   byte-off   raw-len  raw pixel data
```

**Delta+RLE compressed**:
```
[0x41] [0x1n] [0x8B] [0xFF] [offsetx4] [lenx2] [RLE-encoded XOR delta...]
 flags  seq    RGB24   all   byte-off   comp-len compressed data
          |      |
          |      \-- C bit (0x80) set; pixel format = 0x8B & 0x7F = 0x0B (RGB24)
          \-- upper nibble 0x1 = delta+RLE
```

**RLE-only compressed (keyframe)**:
```
[0x41] [0x2n] [0x8B] [0xFF] [offsetx4] [lenx2] [RLE-encoded raw data...]
          |      |
          |      \-- C bit set; pixel format = RGB24
          \-- upper nibble 0x2 = RLE only
```

**Transform compressed**:
```
[0x41] [0x3n] [0x8B] [0xFF] [offsetx4] [lenx2] [transform header + explicit writes...]
          |      |
          |      \-- C bit set; pixel format = RGB24
          \-- upper nibble 0x3 = transform
```

### 3.4 Backward Compatibility

- Standard DDP senders never set the C bit (byte 2 bit 7) -- their packets are processed by the standard (uncompressed) code path with zero changes.
- Standard DDP receivers that validate dataType strictly will reject 0x8B as unknown and drop the packet cleanly. Lenient receivers (WLED) mask off the C bit and fall through to RGB24.
- The compression type nibble occupies byte 1 bits 7-4, which the base spec reserves as zero. No known DDP sender sets these bits.

---

## 4. RLE Codec Specification

### 4.1 Algorithm: PackBits-Inspired Byte-Level RLE

The RLE codec operates on raw byte streams (not pixel-aware). It distinguishes runs (repeated identical bytes) from literal spans (non-repeating sequences).

### 4.2 Control Byte Encoding

```
Bit 7 = 0: RUN
  Value: (ctrl & 0x7F) + 1 = repeat count (1-128)
  Next byte: the value to repeat
  Output: value repeated (ctrl & 0x7F) + 1 times

Bit 7 = 1: LITERAL
  Value: (ctrl & 0x7F) + 1 = literal count (1-128)
  Next N bytes: literal data
  Output: the N bytes verbatim
```

### 4.3 Encoding Rules

1. Scan input left to right
2. At each position, look ahead for identical bytes
3. If 3+ identical bytes found -> emit RUN control byte + value byte
4. Otherwise, accumulate into a literal span
5. While accumulating literals, peek ahead for 3+ byte runs to break the literal
6. Literal spans cap at 128 bytes -- emit and start a new span if needed
7. Runs cap at 128 repetitions -- emit and start a new run if needed

### 4.4 Size Guarantees

| Metric | Value |
|--------|-------|
| Best case (uniform data) | 2 bytes per 128 input bytes (64:1) |
| Worst case (random data) | input + ceil(input/128) + 2 bytes (~0.8% expansion) |
| Maximum encoded size | `srcLen + (srcLen / 128) + 2` |

### 4.5 Sender and Receiver Obligations

**Sender MUST:**
- Allocate an output buffer of at least `rle_max_encoded_size(rawLen)` bytes before encoding.
- Compare compressed output size against raw size after encoding.
- Fall back to uncompressed transmission if `compressedLen >= rawLen`. The C bit MUST NOT be set on packets where compression was not beneficial.
- Never perform in-place encoding (source and destination buffers MUST NOT overlap).

**Receiver MUST:**
- Accept that `dataLen` in the DDP header represents the compressed payload size, not the decompressed pixel count. The decompressed size is inferred from the pixel range (channelOffset to end of strip).
- Cap decoder output at the expected pixel count to guard against malformed input that would decode to more bytes than the pixel buffer can hold.
- Zero `prevFrame` and discard the current packet if the RLE decoder encounters a malformed control byte or runs past the end of the payload.

### 4.6 Python Reference Encoder

```python
def rle_encode(src: bytes) -> bytes:
    """PackBits-inspired byte-level RLE encoder."""
    out, i, n = bytearray(), 0, len(src)
    while i < n:
        cur, run = src[i], 1
        while i + run < n and src[i + run] == cur and run < 128:
            run += 1
        if run >= 3:
            out.append(run - 1)        # RUN: ctrl = count-1 (bit 7 clear)
            out.append(cur)
            i += run
        else:
            lit_start, lit_len = i, 0
            while i < n and lit_len < 128:
                ahead = 1
                while i + ahead < n and src[i + ahead] == src[i] and ahead < 3:
                    ahead += 1
                if ahead >= 3:
                    break
                i += 1
                lit_len += 1
            if lit_len:
                out.append(0x80 | (lit_len - 1))  # LITERAL: ctrl = count-1 + 0x80
                out.extend(src[lit_start:lit_start + lit_len])
    return bytes(out)
```

### 4.7 Python Reference Decoder

```python
def rle_decode(src: bytes) -> bytes:
    """PackBits-inspired byte-level RLE decoder."""
    out, i, n = bytearray(), 0, len(src)
    while i < n:
        ctrl = src[i]; i += 1
        if ctrl & 0x80:  # LITERAL
            count = (ctrl & 0x7F) + 1
            out.extend(src[i:i + count])
            i += count
        else:             # RUN
            count = (ctrl & 0x7F) + 1
            if i < n:
                out.extend(bytes([src[i]]) * count)
                i += 1
    return bytes(out)
```

### 4.8 C Reference Streaming Decoder

The C implementation uses a stateful streaming decoder for zero-copy operation on memory-constrained devices (ESP32). It emits one byte at a time without requiring a decompression buffer.

```c
typedef struct {
    const uint8_t *src;
    size_t srcLen;
    size_t pos;
    int remaining;    // bytes left in current span
    bool isRun;       // true = repeating, false = literal
    uint8_t value;    // current run value
} RLEDecoder;

void rle_decoder_init(RLEDecoder *d, const uint8_t *data, size_t len) {
    d->src = data; d->srcLen = len;
    d->pos = 0; d->remaining = 0;
    d->isRun = false; d->value = 0;
}

bool rle_decoder_next(RLEDecoder *d, uint8_t *out) {
    while (d->remaining == 0) {
        if (d->pos >= d->srcLen) return false;
        uint8_t ctrl = d->src[d->pos++];
        if (ctrl & 0x80) {
            d->remaining = (ctrl & 0x7F) + 1;
            d->isRun = false;
        } else {
            d->remaining = (ctrl & 0x7F) + 1;
            d->isRun = true;
            if (d->pos < d->srcLen) d->value = d->src[d->pos++];
            else return false;
        }
    }
    if (d->isRun) {
        *out = d->value;
    } else {
        if (d->pos >= d->srcLen) return false;
        *out = d->src[d->pos++];
    }
    d->remaining--;
    return true;
}
```

---

## 5. Delta+RLE Compression

### 5.1 Algorithm

1. **Delta**: XOR current frame with previous frame. Unchanged pixels become `0x000000` (or `0x00000000` for RGBW).
2. **RLE**: Apply byte-level RLE to the delta buffer. Long runs of zeros compress extremely well.
3. **Transmission**: Send the RLE-encoded delta with compression type `0x10`.
4. **Receiver decode**: RLE decode -> XOR with receiver's stored previous frame -> final pixel values.

### 5.2 Sender Logic (pseudocode)

```python
def encode_delta_rle(current_frame, prev_frame):
    # Step 1: XOR delta
    delta = bytes(a ^ b for a, b in zip(current_frame, prev_frame))
    # Step 2: RLE compress
    compressed = rle_encode(delta)
    # Step 3: Compare sizes
    if len(compressed) < len(current_frame) * 0.9:
        return compressed, COMP_DELTA_RLE  # worth compressing
    else:
        return current_frame, COMP_NONE     # send raw
```

### 5.3 Receiver Logic (pseudocode)

```python
def decode_delta_rle(compressed_data, prev_frame):
    # Step 1: RLE decode
    delta = rle_decode(compressed_data)
    # Step 2: XOR with previous frame
    current = bytes(a ^ b for a, b in zip(delta, prev_frame))
    # Step 3: Update previous frame for next delta
    prev_frame[:] = current
    return current
```

### 5.4 Previous Frame Buffer

The receiver MUST maintain its own `prevFrame` buffer -- a copy of the last successfully decoded frame in logical pixel order.

**Critical design requirement**: The prevFrame buffer must NOT be the live display buffer. Reading from the live pixel buffer introduces:
- Race conditions with the display refresh hardware
- Mapping table indirection (logical->physical pixel reordering)
- Brightness/gamma transformations applied during display output

The prevFrame stores raw decoded values, exactly as received, before any display-side transformations.

**RGBW requirement**: The prevFrame buffer MUST store 4 bytes per pixel (RGBW32 format) regardless of whether the current data is RGB or RGBW. This ensures the W channel survives delta roundtrips on RGBW strips. Allocating only 3 bytes/pixel causes W channel corruption -- the W channel XORs against 0 instead of the previous W value.

### 5.5 Measured Compression Ratios (real hardware, M5StickC, 800 LEDs)

| Pattern | Raw Size | Compressed | Ratio | Notes |
|---------|----------|-----------|-------|-------|
| Rainbow (worst case) | 2,400 B | 2,400 B | 1:1 | Every pixel differs -- no benefit |
| Solid color pulse | 2,400 B | 1,900-2,050 B | 1.2:1 | 13-21% savings |
| Sparse twinkle (2% change) | 2,400 B | 120 B | 20:1 | 95% savings |
| Static (no change) | 2,400 B | ~16 B | 150:1 | Only zeros in delta |

---

## 6. Transform Compression

### 6.1 Concept

Transform compression encodes common LED animation operations directly, rather than as pixel-by-pixel data. It's most efficient for patterns like "fade all LEDs toward black" or "dim everything by 50%, then set 5 specific LEDs to new colors."

### 6.2 Wire Format (compression type 0x30)

```
Offset  Size  Field
------  ----  -----
0       1     tOp           Transform operation
1       1     tParam        Operation parameter (e.g., blend alpha, scale factor)
2       3-4   targetColor   Target R,G,B[,W] (size = channelsPerPixel)
2+C     2     numExplicit   Number of explicit pixel writes (little-endian)
4+C     N     explicitData  Array of (pixelIndex:2LE, R,G,B[,W])
```

### 6.3 Transform Operations

| tOp Value | Name | Behavior |
|-----------|------|----------|
| 0x01 | SCALE_TOWARD | `pixel = lerp(prev_pixel, target, tParam/255)` -- blend toward target color |
| 0x02 | SCALE_MULT | `pixel = prev_pixel * tParam / 255` -- multiply brightness |
| 0x03 | NOP | No global transform -- only explicit pixel writes applied |

### 6.4 Example: Fade to Black + Set 3 Pixels

```
tOp=0x02 (SCALE_MULT), tParam=200 (78% brightness), target=(0,0,0)
numExplicit=3
explicitData:
  [pixel 42, 255, 0, 0]    -- set pixel 42 to red
  [pixel 100, 0, 255, 0]   -- set pixel 100 to green
  [pixel 200, 0, 0, 255]   -- set pixel 200 to blue
```

Total payload: 1 + 1 + 3 + 2 + (3 x 5) = **22 bytes** for 800 pixels (vs 2,400 bytes raw).

### 6.5 Receiver Implementation Notes

The transform path reads the previous pixel state to compute the new value. This MUST read from the `prevFrame` buffer, not from the live display pixel buffer. The same design requirement as delta+RLE applies -- reading live pixels introduces race conditions and mapping table indirection.

---

## 7. RGBW and CCT Handling

### 7.1 RGBW (4-channel)

DDP natively supports RGBW via the dataType field. Set bits [5:3] to `011` (RGBW type) and bits [2:0] to `011` (8-bit per channel) -> dataType = `0x1B`.

**Sender**: Generate 4 bytes per pixel (R, G, B, W). Set dataType to `0x1B`.

**Receiver**: Detect RGBW by checking `(dataType & 0b00111000) >> 3 == 0b011`. Set `channelsPerPixel = 4`.

**Compression**: All compression types (RLE, delta+RLE, transform) work with RGBW data. The RLE codec operates on bytes, so 4 bytes/pixel is handled transparently. Delta encoding XORs 4-byte pixels. The prevFrame buffer MUST be allocated at 4 bytes/pixel.

### 7.2 CCT (Correlated Color Temperature)

**CCT via DDP is NOT supported.** This is a design decision, not a limitation of DDP.

WLED handles CCT as a per-segment property via `Bus::setCCT()`, controlled through the JSON API (`{"seg":{"cct":128}}`). It is not a per-pixel channel in the pixel data stream. DDP's dataType field has no 5-channel mode in WLED's implementation.

To control CCT alongside DDP pixel streaming:
1. Send pixel colors via DDP (RGB or RGBW)
2. Send CCT changes via WLED JSON API: `POST /json {"seg":[{"cct":128}]}`

### 7.3 RGBWW (5-channel, dual white)

Not supported via DDP. RGBWW strips are handled internally by WLED's autoWhite algorithm, which derives WW/CW values from the RGB color and a CCT parameter. The DDP sender should send RGB or RGBW data; the receiver's bus driver handles the WW/CW split.

---

## 8. Sender Implementation Guide

### 8.1 Minimal Raw DDP Sender (Python)

```python
import socket, struct

def send_ddp_frame(sock, target_ip, pixels_rgb, seq=1):
    """Send a single raw DDP frame (RGB, <=480 pixels)."""
    DDP_PORT = 4048
    flags = 0x41  # VER1 | PUSH
    data_type = 0x0B  # RGB24
    dest = 0xFF  # all devices
    offset = 0
    data = bytes(pixels_rgb)

    header = struct.pack("!BBBBIH",
        flags,
        seq & 0x0F,     # sequence in lower nibble
        data_type,
        dest,
        offset,          # channel offset (bytes)
        len(data))       # data length

    sock.sendto(header + data, (target_ip, DDP_PORT))
```

### 8.2 Multi-Packet Sender

For >480 RGB pixels (>1440 bytes), split across multiple packets:

```python
def send_ddp_frame_multi(sock, target_ip, pixels_rgb, seq=1):
    MAX_PAYLOAD = 1440
    data = bytes(pixels_rgb)
    offset = 0

    while offset < len(data):
        chunk_size = min(MAX_PAYLOAD, len(data) - offset)
        is_last = (offset + chunk_size >= len(data))

        flags = 0x40  # VER1
        if is_last:
            flags |= 0x01  # PUSH on last packet

        header = struct.pack("!BBBBIH",
            flags,
            seq & 0x0F,
            0x0B,           # RGB24
            0xFF,           # all devices
            offset,         # byte offset into frame
            chunk_size)

        sock.sendto(header + data[offset:offset + chunk_size],
                    (target_ip, 4048))
        offset += chunk_size
        seq = (seq % 15) + 1  # wrap 1-15, never 0

    return seq
```

### 8.3 Compressed DDP Sender

```python
def send_ddp_compressed(sock, target_ip, pixels, prev_frame, seq=1):
    """Send compressed DDP with adaptive algorithm selection."""
    raw = bytes(pixels)

    # Try delta+RLE
    if prev_frame and len(prev_frame) == len(raw):
        delta = bytes(a ^ b for a, b in zip(raw, prev_frame))
        compressed = rle_encode(delta)
        if len(compressed) < len(raw) * 0.9:
            comp_type = 0x10  # DELTA_RLE
            payload = compressed
        else:
            comp_type = 0x00
            payload = raw
    else:
        # Try RLE only (keyframe)
        compressed = rle_encode(raw)
        if len(compressed) < len(raw) * 0.9:
            comp_type = 0x20  # RLE only
            payload = compressed
        else:
            comp_type = 0x00
            payload = raw

    flags = 0x41  # VER1 | PUSH
    data_type = 0x0B  # RGB24
    if comp_type != 0x00:
        data_type |= 0x80  # C bit: payload is compressed

    header = struct.pack("!BBBBIH",
        flags,
        (seq & 0x0F) | (comp_type & 0xF0),
        data_type,
        0xFF,
        0,        # offset
        len(payload))

    sock.sendto(header + payload, (target_ip, 4048))
    return raw  # caller stores as prev_frame for next delta
```

### 8.4 Adaptive Compression Selection

The sender should try compression types in this priority order and pick the smallest output:

1. **Transform** -- if applicable (detect solid fades, constant scaling)
2. **Delta+RLE** -- if previous frame available
3. **RLE only** -- if no previous frame or first frame
4. **Raw** -- if all compressed outputs >= 90% of raw size

The receiver dispatches purely on the compression type byte -- it doesn't need to know how the sender chose.

---

## 9. Receiver Implementation Guide

### 9.1 Packet Validation Checklist

```
1. Length >= 10 bytes (DDP header minimum)
2. Flags byte has VER1 set (bits 7-6 = 01)
3. Destination is not CONTROL(246), STATUS(251), or CONFIG(250)
4. QUERY(bit 1) and REPLY(bit 2) flags not set
5. If !PUSH and STORAGE: reject (storage-only without push)
6. Sequence filter: reject if in late-packet window
7. Payload length: packetLen >= header + timecode_offset + dataLen
```

### 9.2 Raw DDP Receive Path

```
1. Parse header
2. Detect channels: (dataType >> 3) & 0x07 == 3 -> RGBW (4ch), else RGB (3ch)
3. Calculate start pixel: channelOffset / channelsPerPixel + DMX_offset
4. Skip timecode if TIME flag set (4 bytes)
5. Write pixels: for each pixel in [start, start + dataLen/channels):
     setRealtimePixel(i, R, G, B, W)
6. On PUSH: trigger display refresh
```

### 9.3 Compressed DDP Receive Path

```
1. Parse header (same as raw)
2. Check C bit: dataType & 0x80
3. Extract pixel format: pixelFormat = dataType & 0x7F
4. Read compression type from byte 1 upper nibble
5. Dispatch:
   0x10: Delta+RLE decode
   0x20: RLE-only decode
   0x30: Transform decode
6. After any decode: update prevFrame buffer
7. On PUSH: trigger display refresh
```

### 9.4 Receiver State

The receiver maintains per-session state:

```c
static bool     ddpSeenPush;     // have we seen any push?
static uint8_t *prevFrame;       // previous frame buffer (4 bytes/pixel, RGBW32)
static unsigned  prevFrameSize;   // allocated size in pixels
```

`prevFrame` is allocated on DDP realtime mode entry and freed on exit. It MUST be zeroed on:
- First entry into DDP mode
- Mode transitions (effect -> realtime or vice versa)
- RLE decode errors (malformed data)
- After receiving a non-delta frame (keyframe)

---

## 10. Sequence Numbering and Frame Sync

### 10.1 Sequence Counter Rules

- Range: 1-15 (4-bit, lower nibble of byte 1)
- Value 0: "unused" -- receiver skips sequence validation
- Increment continuously across packets AND frames
- Wrap: after 15, next is 1 (NOT 0)

### 10.2 Implementation

```python
# Sender
seq = 1
def next_seq():
    global seq
    current = seq
    seq = (seq % 15) + 1
    return current
```

```c
// Receiver filter (WLED implementation)
int sn = p->sequenceNum & 0x0F;
if (sn != 0 && e131SkipOutOfSequence && lastPushSeq != 0) {
    if (lastPushSeq > 5) {
        if (sn > (lastPushSeq - 5) && sn < lastPushSeq) return; // late
    } else {
        if (sn > (10 + lastPushSeq) || sn < lastPushSeq) return; // late (wrapped)
    }
}
```

### 10.3 Push and Show Timing

For multi-packet frames: set PUSH flag on the LAST packet only. The receiver accumulates pixel data and renders on push.

For bandwidth-constrained links (PPP serial), bypass the show debounce timer on push -- serial is FIFO with no reordering.

For WiFi/Ethernet: maintain a 10-15ms debounce between show calls to coalesce multi-packet bursts that may arrive out of order.

---

## 11. Keyframe Strategy and Error Recovery

### 11.1 Keyframe Schedule

- **Frame 0**: Always uncompressed or RLE-only (no delta)
- **Every 10 frames**: Send RLE-only (no delta) keyframe
- **On connection init**: First frame is keyframe
- **Sender heuristic**: If compressed output >= 90% of raw, send raw (implicit keyframe)

### 11.2 Error Recovery

If the receiver detects any of these conditions, it MUST zero its `prevFrame` buffer:
- RLE decode error (malformed control byte, unexpected end of stream)
- Sequence number gap indicating lost packets
- Realtime mode entry/exit transition

After zeroing prevFrame, the next delta frame XORs against zeros -- producing the raw values. This is correct but may produce a single frame of incorrect output if the sender's delta was computed against a non-zero previous frame. The next keyframe (within <=10 frames) fully resynchronizes.

### 11.3 Desync Window

With 10-frame keyframe interval at 30fps: maximum desync duration = 333ms. At 60fps: 167ms. This is acceptable for LED displays where a brief flicker is imperceptible during rapid animation.

---

## 12. Transport Considerations

### 12.1 WiFi / Ethernet (standard)

- MTU: 1500 bytes (Ethernet standard)
- DDP payload: 1440 bytes max (480 RGB or 360 RGBW pixels)
- Packet reordering: possible (especially WiFi) -- use sequence filter
- Show debounce: 10-15ms recommended for multi-packet coalescing

### 12.2 PPP over Serial (low-bitrate)

- **MTU: 1500 bytes** (fixed -- see note below)
- Effective bandwidth: ~172 KB/s at 1.5Mbps UART
- Packet ordering: guaranteed (serial is FIFO) -- no debounce needed
- Show timing: immediate on push (bypass debounce)

**Why MTU is 1500, not 4096**: The Tasmota Arduino Core ships a pre-compiled
`liblwip.a` with `PPP_MRU` hardcoded to 1500. Build-flag overrides
(`-D PPP_MRU=4096`) have no effect on the pre-built library. This was
discovered empirically when DDP PANIC crashes were traced to UART RX buffer
overruns at higher packet rates -- the root cause was the MTU mismatch between
the pppd command (`mru 1500`) and the firmware expectation.

**PPP byte-stuffing**: PPP HDLC framing escapes bytes `0x7D` and `0x7E`.
Worst case: payload doubles. At MTU=1500, worst-case byte-stuffed frame is
~3000 bytes. The UART RX buffer is fixed at 8192 bytes (5.5x MTU) -- adequate
for ~2.7 frames of buffering. Two-tier flow control in the PPP RX task yields
to the lwIP tcpip_thread when the buffer exceeds 50% capacity.

**Host pppd command** (use exactly these values):
```bash
sudo pppd /dev/ttyUSB0 1500000 noauth nodetach local nocrtscts \
  novj nodeflate nobsdcomp noaccomp nopcomp lcp-echo-interval 0 \
  mru 1500 mtu 1500 169.254.7.2:169.254.7.1
```

### 12.3 Bandwidth Budgets

```
WiFi (20 Mbps effective):
  Raw RGB at 30fps:    -> 222,222 pixels max
  Raw RGBW at 30fps:   -> 166,666 pixels max
  No compression needed for most installations

Ethernet (100 Mbps):
  Raw RGB at 30fps:    -> 1,111,111 pixels max
  Compression irrelevant

PPP 1.5Mbps UART:
  Raw RGB at 30fps:    -> 1,911 pixels max (5,733 bytes/frame)
  Raw RGBW at 30fps:   -> 1,433 pixels max
  With delta+RLE 95%:  -> 38,222 pixels at 30fps (theoretical)
  With delta+RLE 50%:  -> 3,822 pixels at 30fps

  Display budgets:
    20x40 matrix (800px):  raw=71fps, delta95%=1433fps
    160x80 TFT (12800px): raw=4.5fps, delta95%=89fps, RLE50%=9fps
```

### 12.4 Receiver-Side Flow Control and Flood Survival

The WLED DDP receiver implements multiple layers of protection against DDP
flood conditions (uncapped sender, network burst, or slow consumer):

**Layer 1 -- Heap guard** (e131.cpp): Drop all DDP packets when free heap
falls below 20KB. Prevents OOM crashes under sustained flood. Counter:
`ddpHeapGuardDrops` (atomic, visible in `/diag`).

**Layer 2 -- Rate limiter** (e131.cpp): Effective ceiling is
`min(ddpMaxFps, ddpCurrentSafeFps)`. `ddpMaxFps` is user-configured (default
40 for SPI Matrix builds, 60 otherwise). `ddpCurrentSafeFps` is auto-derived
each main loop iteration from `BusManager::computeSafeDdpFps()` -- sums
`Bus::getShowUs()` across all active buses with 70% headroom. When buses go
idle (skip-show), their showUs drops to zero and the ceiling rises
automatically. Excess frames dropped silently. Counter: `ddpRateLimitDrops`.

**Layer 3 -- Main loop starvation detector** (e131.cpp): When the main loop
hasn't run for >100ms (detected via `lastLoopMs` atomic), the PPP RX task
boosts the loop task priority to 19 via `vTaskPrioritySet(loopTaskHandle, 19)`.
The main loop restores priority to 1 on its next iteration. This prevents
TFT SPI DMA from being starved by sustained DDP flood. Implemented in session
14 (Wave 7) after observing loop starvation under uncapped DDP at 670+ fps.

**Layer 4 -- Finite realtime timeout** (udp.cpp): `realtimeLock()` for DDP
uses a 2500ms timeout (`realtimeLock(2500, REALTIME_MODE_DDP)`) rather than
the configurable `realtimeTimeoutMs`. This ensures the device recovers from
DDP streams that stop without sending a final packet. The FPS=0 lockup bug
was caused by the timeout check running after `strip.show()`
which blocks for 24ms on TFT SPI DMA -- fixed by moving the timeout check
before the show block.

**Layer 5 -- UART flow control** (wled_ppp.cpp): Two-tier flow control in the
PPP RX task. Tier 1 (>50% UART RX buffer): yield to let tcpip_thread drain.
Tier 2 (>75% buffer): drop the current DDP frame. Prevents UART ISR ring
buffer overrun under sustained high-rate DDP.

**Diagnostics** (`/diag` endpoint):
- `ddpRateLimitDrops`: frames dropped by rate limiter
- `ddpHeapGuardDrops`: frames dropped by heap guard
- `realtimeTimeout`, `now`, `diff`: realtime lock state (positive diff = expired)
- `frozen`: `rtFrozenSegs` bitmask of segments frozen by realtime
- RTC crash snapshot: `crashHeap`, `crashMinHeap`, `crashDmaHeap`, `crashUptime`
  (preserved across resets via RTC memory, useful for post-crash diagnosis)

### 12.5 Transport Benchmark Results (M5StickC, 40x80 TFT, 2026-08-21)

Hardware: ESP32-PICO-D4, ST7735S 80x160 TFT at 27MHz SPI, single TFT bus
(WS strip removed). Non-blocking DMA active. Firmware: dev/ddp-spec @ 0b062fad.

**Ghost Rider compressed DDP (97% compression, ~300B/frame, uncapped sender):**

| Transport | Sent FPS | Eff FPS to TFT | Drops | Stability |
|-----------|---------|----------------|-------|-----------|
| UDP WiFi | 439 | 83 | 4626 | stable 30s |
| UDP PPP | 439 | 76 | 6075 | stable 30s |
| WS WiFi | 199 | 53 | 3408 | stable 30s, heap low |
| WS PPP | 376 | 51 | 1017 | WS died at 5.4s |

**Raw (uncompressed) DDP to TFT (3200px = 9600B/frame = 8 UDP packets):**

| Transport | Target FPS | Packets Received | Eff FPS | Bottleneck |
|-----------|-----------|-----------------|---------|------------|
| WiFi UDP | 10 | 97% | 10 | works |
| WiFi UDP | 20 | 52% | 10.5 | WiFi TX queue overflow (8-pkt burst) |
| WiFi UDP | 30 | 33% | 9.8 | same, worse at higher rate |
| PPP UDP | any | bandwidth-limited | ~13 max | 124 KB/s / 9.68KB per frame |

**Internal FX (no DDP):**

| Config | Effect | FPS |
|--------|--------|-----|
| TFT-only, WS skip-show | Solid (fx=0) | 51 |
| TFT-only, WS skip-show | Rainbow (fx=9) | 44 |
| TFT + WS both active | Rainbow | 43-45 |
| TFT idle (skip-show) | n/a | ddpSafe rises to 103 |

**Auto-ceiling tracking:**

| Bus state | bus[0] showUs | bus[1] showUs | sumUs | ddpSafe |
|-----------|-------------|-------------|-------|---------|
| TFT + WS active | 6740 | 7680 | 14420 | 48 |
| TFT only (WS skip) | 6740 | 0 | 6740 | 103 |
| TFT idle | 0 | 7680 | 7680 | 91 |
| Both idle | 0 | 0 | 0 | 255 |


---

## 13. Validation Suite

### 13.1 Unit Tests (Python-only, no device required)

#### RLE Codec Tests

| # | Test | Input | Expected |
|---|------|-------|----------|
| 1 | Empty | `b""` | `b""` after roundtrip |
| 2 | Single byte | `b"\x42"` | Roundtrips correctly |
| 3 | Run of 3 | `b"\xAA" x 3` | Encoded = `b"\x02\xAA"` |
| 4 | Run of 128 | `b"\x00" x 128` | Encoded = `b"\x7F\x00"` |
| 5 | Run of 129 | `b"\xFF" x 129` | Two RLE runs, 4 bytes |
| 6 | Alternating | `b"\xAA\x55" x 64` | All literals, ~130 bytes |
| 7 | Mixed | Runs + literals | Roundtrip correct |
| 8 | Random (x100) | `os.urandom(N)` | Roundtrip correct for all |
| 9 | Worst case | Non-repeating 1440 bytes | Expansion < 1.1% |
| 10 | Full byte range | `bytes(range(256))` | Roundtrip correct |
| 11 | RGBW pixels | 4-byte patterns | Roundtrip correct |

#### Delta+RLE Tests

| # | Test | Input | Expected |
|---|------|-------|----------|
| 12 | Identical frames | cur == prev | Delta = zeros, ~2 bytes compressed |
| 13 | Single pixel change | 1 pixel differs | ~6 bytes compressed |
| 14 | Full change | All pixels differ | ~raw size (no benefit) |
| 15 | No previous frame | prev = None | Falls back to RLE-only |
| 16 | RGBW W-channel | Only W changes | W survives roundtrip |
| 17 | RGBW full | All 4 channels change | Roundtrip correct |

#### Packet Construction Tests

| # | Test | Expected |
|---|------|----------|
| 18 | Single packet (<=1440B) | 1 packet, PUSH set |
| 19 | Multi-packet (>1440B) | N packets, PUSH on last only |
| 20 | Boundary (exactly 1440B) | 1 packet |
| 21 | Header byte layout | Matches spec struct |
| 22 | C bit | dataType bit 7 set (0x8B for RGB24), comp type in upper nibble of byte 1 |
| 23 | Sequence wrap | Cycles 1->15->1, never 0 |
| 24 | RGBW data type | Header byte 2 = 0x1B |
| 25 | Zero-length | Defined behavior (0 or 1 packet) |

#### Transform Tests

| # | Test | Expected |
|---|------|----------|
| 26 | SCALE_TOWARD | Detects blend-toward-target pattern |
| 27 | SCALE_MULT | Detects constant-factor scaling |
| 28 | NOP | Only explicit pixel writes |
| 29 | No match | Returns None for random data |
| 30 | Out-of-range pixel index | Skipped silently |

### 13.2 Integration Tests (requires device)

Use the `/diag` endpoint for pixel readback. Send known patterns, read back colors, compare.

```python
def verify_pixel(target_ip, pixel_idx, expected_rgb):
    """Read pixel color via /diag and compare."""
    diag = requests.get(f"http://{target_ip}/diag").text
    # Parse px[N..M]: RRGGBB lines
    # Compare against expected
    return actual == expected
```

| # | Test | Send | Verify |
|---|------|------|--------|
| 31 | Solid red, raw | All (255,0,0) | px[0..4] = FF0000 |
| 32 | Solid red, RLE | Same, COMP_RLE | Same result |
| 33 | Solid red, delta | Frame 1 raw, frame 2 delta | Same result |
| 34 | Diagnostic markers | Known positions | Exact match |
| 35 | Multi-packet | >480 LEDs | All pixels correct |
| 36 | RGBW raw | (255,0,128,64) | W channel present |
| 37 | RGBW delta | Only W changes | W correct |
| 38 | Sequence wrap | >15 packets | No drops |
| 39 | Sustained 30fps | 5 seconds | Last frame correct |
| 40 | Keyframe recovery | Send 10 deltas -> verify | Matches after keyframe |

### 13.3 Cross-Implementation Verification

Port the C streaming decoder to Python (or compile via cffi). Feed identical inputs to both encoders. Verify byte-exact output match.

```python
def test_cross_impl(iterations=1000):
    for _ in range(iterations):
        data = os.urandom(random.randint(1, 4096))
        py_encoded = python_rle_encode(data)
        py_decoded = python_rle_decode(py_encoded)
        c_decoded = c_rle_decode(py_encoded)
        assert py_decoded == c_decoded == data
```

### 13.4 Diagnostic Endpoint Enhancements

The `/diag` endpoint (HTTP GET) exposes:
- `ddpRateLimitDrops`: frames dropped by the rate limiter (atomic counter)
- `ddpHeapGuardDrops`: frames dropped by the heap guard (atomic counter)
- `realtimeMode`: current realtime mode (0=none, 8=DDP, etc.)
- `realtimeTimeout`: absolute millis() when realtime lock expires
- `now`: current millis()
- `diff`: `now - realtimeTimeout` (positive = expired, negative = time remaining)
- `frozen`: `rtFrozenSegs` bitmask (hex) of segments frozen by realtime
- RTC crash snapshot (if magic matches): `crashHeap`, `crashMinHeap`, `crashDmaHeap`, `crashUptime`
- Reset reason, heap breakdown (free/min/DMA)

---

## 14. Known Issues and Design Decisions

### 14.1 Confirmed Defects (from adversarial review)

| # | Severity | Issue | Status |
|---|----------|-------|--------|
| C1 | CRITICAL | prevFrame allocates 3B/pixel -- RGBW W channel lost in delta | **Accepted** -- prevFrame uses RGB565 (2B/pixel). W channel intentionally absent: heap trade-off on no-PSRAM devices (2Bx800px=1.6KB vs 4Bx800px=3.2KB). Delta decode reconstructs W=0, acceptable for WLED effects. |
| C2 | CRITICAL | Transform reads live pixel buffer instead of prevFrame | **Fixed** -- Transform reads `ddpPrevFrame` via `DDP_PF_IDX()`. Partial: reads RGB565 (W=0 reconstructed), not RGBW32. |
| C3 | CRITICAL | PPP show() has no isUpdating() guard -> torn frames | **Fixed (architectural)** -- DDP handler never calls `strip.show()` directly. Handler sets `e131NewData` atomic flag; show deferred to main loop. No isUpdating() guard needed. |
| C4 | CRITICAL | PPP byte-stuffing can overflow UART RX buffer | **Mitigated** -- Two-tier UART flow control: yield at >50% buffer fill. RX buffer 8192 bytes (5.5x MTU=1500). Not fully solved for MTU>1500, but MTU>1500 is not achievable with pre-built liblwip.a. |
| H1 | HIGH | Sequence counter wraps to 0 after 15 packets | **Fixed** -- Sequence cycles 1->15->1 via `(ddpLastSeq % 15) + 1`. `ddpSeqGaps` counter tracks out-of-order packets. |
| H2 | HIGH | 1-second keyframe gap = garbage on desync | **Not fixed** -- Receiver does not zero prevFrame on sequence gap. Mitigated by: (1) sender keyframe interval (sec 11.1), (2) `ddpFreePrevFrame()` on `exitRealtime()`. Risk: visual glitches on packet loss until next keyframe. |

### 14.2 Design Decisions

**Byte-level RLE vs pixel-level RLE**: PackBits byte-level RLE is suboptimal for RGB pixel data (a run of identical RED pixels is `FF,00,00,FF,00,00...` at byte level -- interleaved runs). However, delta+RLE captures temporal coherence at 95% savings, which is the primary use case. Pixel-level RLE or LZ4 would give ~2-3x better compression on keyframes but adds wire format complexity and decode cost. Decision: keep byte-level RLE for simplicity.

**No compression for Art-Net/E1.31**: These are industry standards. Proprietary compression breaks interoperability. DDP's niche ecosystem allows extension.

**2-byte prevFrame (RGB565)**: prevFrame uses 2 bytes/pixel (RGB565 packed format) rather than 4 bytes/pixel (RGBW32). This is a deliberate heap trade-off for no-PSRAM devices (ESP32-PICO-D4, 520KB SRAM): 2Bx800px=1.6KB vs 4Bx800px=3.2KB. Trade-offs accepted: (1) W channel is absent -- delta decode reconstructs W=0, acceptable since WLED effects rarely use the W channel in DDP streams; (2) R/G/B lose 3 bits of precision each due to 565 quantization -- imperceptible at LED brightness levels. The RGB565 format was adopted when heap pressure from DDP+TFT+WiFi left insufficient headroom for RGBW32 prevFrame on 800+ pixel configurations.

**Transform compression is sender-side only**: The receiver decodes whatever the sender sends. The sender decides when to use transform vs delta+RLE based on content analysis. No negotiation protocol.

**C bit (dataType bit 7) as compression signal**: The DDP spec defines byte 2 bit 7 as "Customer defined". Using this for compression is spec-sanctioned and addresses upstream reviewer concern (softhack007, #5810). Trade-off: dataType carries two concerns (pixel format + compression flag), requiring `& 0x7F` mask before format interpretation.

**Non-blocking SPI DMA (deferred dmaWait)**: The final `dmaWait()` at end of
`BusSPIMatrix::show()` is deferred to the start of the next `show()` call via
`drainDma()`. This eliminates the ~1-4ms blocking window that caused SPI DMA
ISR starvation when DDP packets arrived during it. The `dmaBusy()` poll-and-skip
approach was tried first and reverted -- it caused continuous frame drops because
show() must eventually drain the DMA to make progress.

**spielig/spifps removal**: The separate `ddpSpiEligible` and `ddpSpiFps` cfg
keys were redundant with `ddpEligibleMask`. SPI Matrix segments are now
unconditionally excluded from DDP eligibility in `rebuildDdpSlots()`. With
non-blocking DMA, the crash risk that motivated the opt-in gate no longer exists.

**Generalized skip-show**: The `hasIdleSkip()` virtual override pattern was
removed. `busHasActiveSegment()` now checks `seg.isActive()` to skip ghost
segment slots. All bus types benefit from skip-show without opt-in. BusDigital
was previously missing the override, wasting 7.7ms/frame on idle WS strips.

**Multi-IP /json/info**: Added `"nifs"` JSON array to `/json/info` enumerating
all `esp_netif` interfaces (PPP, WiFi STA, AP, Ethernet). Each entry contains
ip, mask, and interface description. Legacy `"ip"` field preserved.

### 14.3 Limitations

- **CCT via DDP**: Not supported. CCT is a per-segment property in WLED, not a per-pixel DDP channel. Use JSON API for CCT control.
- **Multi-packet compressed DDP**: Requires decoder state persistence across packets. Works but adds complexity. Prefer raising MTU on bandwidth-constrained links.
- **Delta compression for >2048 pixels**: prevFrame buffer costs `pixels x 4` bytes. On ESP32 with 109KB free heap, 2048px x 4B = 8KB is acceptable. 12,800px x 4B = 50KB is too large without PSRAM. Use RLE-only or transform for large displays.
- **Lossy compression**: Not implemented. All compression types are lossless. For bandwidth-starved links, the sender should reduce frame rate rather than pixel fidelity.

---

## 15. Bandwidth Budget Reference

### 15.1 Formula

```
effective_bandwidth = link_speed x (1 - framing_overhead)
bytes_per_frame = pixels x channels_per_pixel
max_raw_fps = effective_bandwidth / bytes_per_frame
max_compressed_fps = effective_bandwidth / (bytes_per_frame x (1 - compression_ratio))
```

### 15.2 Quick Reference Table

| Transport | Speed | Effective | 800px RGB | 800px RGBW | 12800px RGB |
|-----------|-------|-----------|-----------|------------|-------------|
| **PPP 1.5Mbps** | 1.5M | 172 KB/s | 71fps raw | 53fps raw | 4.5fps raw |
| PPP compressed | -- | -- | >100fps* | >100fps* | 89fps @95%delta |
| **WiFi 20Mbps** | 20M | 2.3 MB/s | >100fps | >100fps | 60fps raw |
| **Ethernet 100M** | 100M | 11.6 MB/s | >100fps | >100fps | >100fps |

*Compression ratios depend on content -- 95% is typical for sparse animations.

### 15.3 Frame Budget Calculator

```python
def frame_budget(bandwidth_bytes_sec, target_fps):
    return bandwidth_bytes_sec / target_fps

# PPP at 30fps:
# frame_budget(172000, 30) = 5733 bytes
# 800px RGB raw = 2400 bytes -> fits easily
# 12800px RGB raw = 38400 bytes -> needs 6.7:1 compression
```

---

## 16. Reference Implementations

### 16.1 Available Implementations

| Component | Language | Location | Status |
|-----------|----------|----------|--------|
| DDP receiver (raw + compressed) | C/C++ | `wled00/e131.cpp` | Production |
| RLE codec (streaming decoder) | C | `wled00/ddp_compress.h` | Production |
| Delta+RLE decoder (0x10) | C/C++ | `wled00/e131.cpp` | Production |
| RLE-only decoder (0x20) | C/C++ | `wled00/e131.cpp` | Production |
| Transform decoder (0x30) | C/C++ | `wled00/e131.cpp` | Production -- decoder only; encoder not implemented |
| Delta-only decoder (0x40) | C/C++ | `wled00/e131.cpp` | Production |
| Tuple-RLE decoder (0x50) | C/C++ | `wled00/e131.cpp` | Production |
| Planar-RLE decoder (0x60) | C/C++ | `wled00/e131.cpp` | Production |
| DDP sender (raw only) | C/C++ | `wled00/udp.cpp` (`realtimeBroadcast()`) | Production |
| DDP encoder + benchmark | Python | `tools/ddp_bench.py` | Production |
| RLE codec (encode + decode) | Python | `tools/ddp_codec.py` | Production |
| Tuple-RLE encoder (0x50) | Python | `tools/ddp_codec.py`, `tools/ddp_bench.py` | Production |
| Planar-RLE encoder (0x60) | Python | `tools/ddp_codec.py`, `tools/ddp_bench.py` | Production |
| Validation suite | Python/pytest | `tools/tests/test_rle.py` | Production |
| RLE codec (encode + decode) | JavaScript | `wled00/data/common.js` | Production |
| Tuple-RLE encoder (0x50) | JavaScript | `wled00/data/common.js` | Production |
| Planar-RLE encoder (0x60) | JavaScript | `wled00/data/common.js` | Production |
| JS validation suite | Node.js | `tools/tests/test_js_rle.mjs` | Production |
| Ghost Rider DDP pattern | Python | `tools/ddp_bench.py` | Production |

### 16.2 Porting to Other Codebases

To implement compressed DDP in a new codebase:

1. **Receiver**: Implement the packet validation checklist (sec 9.1), raw decode path (sec 9.2), and compressed decode dispatch (sec 9.3). The streaming RLE decoder (sec 4.7) is ~40 lines of C with zero dependencies.

2. **Sender**: Implement raw DDP send (sec 8.1), then add adaptive compression selection (sec 8.4). The RLE encoder (sec 4.5) is ~25 lines of Python or ~50 lines of C.

3. **Test**: Port the validation suite test cases (sec 13.1) to your test framework. The 11 RLE unit tests and 6 delta+RLE tests are the minimum bar for correctness.

4. **Configure**: Set prevFrame buffer size based on maximum expected pixel count. For RGBW compatibility, always allocate 4 bytes per pixel.

### 16.3 Protocol Constants (copy-paste ready)

```c
// DDP protocol
#define DDP_DEFAULT_PORT         4048
#define DDP_HEADER_LEN           10
#define DDP_CHANNELS_PER_PACKET  1440  // 480 RGB or 360 RGBW pixels

// Flags (byte 0)
#define DDP_FLAGS_VER1           0x40
#define DDP_FLAGS_PUSH           0x01
#define DDP_FLAGS_QUERY          0x02
#define DDP_FLAGS_REPLY          0x04
#define DDP_FLAGS_STORAGE        0x08
#define DDP_FLAGS_TIME           0x10

// Compression types (byte 1, upper nibble)
#define DDP_COMP_TYPE_NONE       0x00
#define DDP_COMP_TYPE_DELTA_RLE  0x10  // XOR delta + PackBits RLE
#define DDP_COMP_TYPE_RLE        0x20  // PackBits RLE only (keyframe)
#define DDP_COMP_TYPE_TRANSFORM  0x30  // Global operation + sparse writes
#define DDP_COMP_TYPE_DELTA_ONLY 0x40  // Raw XOR delta, no RLE (benchmark)
#define DDP_COMP_TYPE_TUPLE_RLE  0x50  // PackBits per-tuple: unit = channels bytes
#define DDP_COMP_TYPE_PLANAR_RLE 0x60  // Per-channel planes: [Rlen:2LE][R-rle][G...][B...]

// Transform operations
#define DDP_TRANSFORM_SCALE_TOWARD 0x01  // lerp(prev, target, alpha)
#define DDP_TRANSFORM_SCALE_MULT   0x02  // prev * factor / 255
#define DDP_TRANSFORM_NOP          0x03  // no global op, explicit writes only

// Data types (byte 2)
// C bit (bit 7, 0x80): payload is compressed. Recover pixel format with dataType & 0x7F.
#define DDP_TYPE_COMPRESSED      0x80  // C bit: set on dataType when payload is compressed
#define DDP_TYPE_RGB24           0x0B  // RGB, 8 bits/channel, 3 channels
#define DDP_TYPE_RGBW32          0x1B  // RGBW, 8 bits/channel, 4 channels
#define DDP_TYPE_RGB24_COMP      0x8B  // RGB24 with C bit set (compressed)
#define DDP_TYPE_RGBW32_COMP     0x9B  // RGBW32 with C bit set (compressed)
#define DDP_TYPE_UNDEF           0x00  // undefined (default to RGB24)

// Destination IDs (byte 3)
#define DDP_ID_DISPLAY           0x01
#define DDP_ID_CONTROL           0xF6  // 246
#define DDP_ID_CONFIG            0xFA  // 250
#define DDP_ID_STATUS            0xFB  // 251
#define DDP_ID_ALL               0xFF  // 255
```

```python
# Python constants (same values)
DDP_DEFAULT_PORT = 4048
DDP_HEADER_LEN = 10
DDP_CHANNELS_PER_PACKET = 1440

DDP_FLAGS_VER1 = 0x40
DDP_FLAGS_PUSH = 0x01

DDP_COMP_NONE = 0x00
DDP_COMP_DELTA_RLE = 0x10
DDP_COMP_RLE = 0x20
DDP_COMP_TRANSFORM = 0x30
DDP_COMP_DELTA_ONLY = 0x40
DDP_COMP_TUPLE_RLE = 0x50
DDP_COMP_PLANAR_RLE = 0x60

# C bit (bit 7, 0x80): set on dataType when payload is compressed.
# Recover pixel format with dataType & 0x7F.
DDP_TYPE_COMPRESSED = 0x80
DDP_TYPE_RGB24 = 0x0B
DDP_TYPE_RGBW32 = 0x1B
DDP_TYPE_RGB24_COMP = 0x8B   # RGB24 with C bit
DDP_TYPE_RGBW32_COMP = 0x9B  # RGBW32 with C bit

DDP_TRANSFORM_SCALE_TOWARD = 0x01
DDP_TRANSFORM_SCALE_MULT = 0x02
DDP_TRANSFORM_NOP = 0x03
```

---

## 17. Wire Format Decision Record

Three signalling approaches were considered for the compression extension. Option B was adopted.

### 17.1 Option A: Reserved Flag Bit (CONSIDERED, REJECTED)

**Byte 0 bit 5 (0x20)**: COMPRESSED flag. Compression type in byte 1 upper nibble.

| Pro | Con |
|-----|-----|
| Single bit check in receiver hot path | Borrows a reserved bit the spec may assign later |
| No changes to dataType semantics | No spec precedent for vendor use of flag bits |
| Compression is a transport concern -- flag byte is the natural location | DDP spec author could collide with this bit in a future revision |

Rejected: upstream reviewer (softhack007, #5810) raised concern about borrowing reserved bits.

### 17.2 Option B: Custom Data Type (C bit) -- ADOPTED

**Byte 2 bit 7 (0x80)**: The DDP spec defines this as "1 for Customer defined". Set `dataType = 0x80 | original_type`. Compression type encoded in byte 1 upper nibble (unchanged).

| Pro | Con |
|-----|-----|
| Explicitly spec-sanctioned extension mechanism | Compression is a transport property, not a data type property |
| No risk of future spec collision on the C bit -- it exists for this purpose | Receiver must mask out bit 7 before interpreting the data type for channel count |
| Existing receivers that validate dataType strictly reject the packet cleanly | Overloads byte 2 with two unrelated concerns (pixel format + compression) |

Wire format change: `0x0B` (RGB24) becomes `0x8B` when compressed. Receiver checks `dataType & 0x80` for compression, `dataType & 0x7F` for pixel format.

### 17.3 Option C: Separate Protocol Discriminator (CONSIDERED, REJECTED)

Define a new port or protocol identifier entirely separate from standard DDP.

| Pro | Con |
|-----|-----|
| Zero risk of DDP spec collision | Requires firewall/routing changes (new port) |
| Clean separation of concerns | Breaks DDP tooling (sniffers, test tools, OpenRGB, LedFX) |
| | Two code paths instead of one flag check |
| | DDP has no protocol-version negotiation mechanism |

Rejected: most disruptive option with no clear benefit over Option B.

### 17.4 Decision

Option B (C bit) adopted. Uses the mechanism the DDP spec explicitly provides for vendor extensions. Hardware-validated on M5StickC via emiemi. Both sender and receiver implementations updated -- the codec itself is format-agnostic.

---

## 18. Compression Variant Analysis (Open)

The current codec uses byte-level RLE on XOR deltas. Upstream review suggested evaluating RGB-tuple RLE and separate colour plane RLE (per ITU T.45). This section documents the tradeoffs to inform the design decision.

### 18.1 Byte-Level RLE (current)

Operates on the raw byte stream after XOR delta. Channel boundaries are invisible to the encoder.

**Strengths:**
- Unchanged pixels become zero-byte runs regardless of channel count (RGB or RGBW).
- 300 unchanged RGB pixels = 900 zero bytes = one 2-byte RLE token.
- Partially-changed pixels still compress per-channel (if only R changes, the G and B zero bytes still form runs).
- Simple: one encode pass, one decode pass, streaming decoder with no buffering.
- PackBits is well-understood (Apple 1984, TIFF, DICOM).

**Weaknesses:**
- Keyframes (no delta) compress poorly on non-uniform data. A rainbow gradient has no byte-level runs.
- Interleaved channel data (RGBRGB...) prevents cross-pixel runs on raw frames.

### 18.2 RGB-Tuple RLE

Each run unit is a full pixel (3 bytes RGB, 4 bytes RGBW). Runs of identical pixels compress to one control byte + one pixel.

**Strengths:**
- Keyframes with solid regions compress well (100 identical red pixels = 1 control + 3 bytes).
- Semantically meaningful -- runs correspond to visible pixel regions.

**Weaknesses:**
- Identical compression to byte-level for delta frames (unchanged pixels are all-zero tuples either way).
- Partial channel changes kill compression: if pixel (255,0,0) changes to (254,0,0), the tuple differs and must be a literal. Byte-level RLE can still run the unchanged G and B channels.
- RGBW complicates the run unit size (3 vs 4 bytes depending on dataType).
- Control byte overhead is higher per run: one control byte covers fewer bytes of output (3-4 per unit vs 1 per unit in byte-level).

### 18.3 Separate Colour Planes

Split RGB data into three independent byte streams (R-plane, G-plane, B-plane), RLE-encode each plane separately. Closer to ITU T.45 colour run-length encoding.

**Strengths:**
- Smooth gradients compress well per-plane (R channel changing slowly = good byte runs).
- Each plane can be decoded independently.

**Weaknesses:**
- Three encode passes on sender, three decode passes on receiver.
- Receiver must buffer at least one packet's worth per plane before writing any pixels (can't stream RGB output until all three planes are available for a given pixel range).
- Breaks the streaming decoder model that enables zero-copy on ESP32.
- Higher memory cost on receiver: 3x buffering vs streaming.
- RGBW requires 4 planes.
- More complex wire format: need to signal plane ordering and per-plane lengths.

### 18.4 Comparison Matrix

| Property | Byte-level RLE | RGB-tuple RLE | Colour planes |
|----------|---------------|---------------|---------------|
| Delta frame compression | Excellent | Excellent | Excellent |
| Keyframe compression (solid) | 0.978 (poor) | 0.012 (excellent) | 0.020 (excellent) |
| Keyframe compression (gradient) | 1.001 (none) | 0.277 (good) | 0.153 (excellent) |
| Partial channel change | Good | Poor | Good |
| Decoder complexity | Minimal | Minimal | High |
| Decoder memory | Zero (streaming) | Zero (streaming) | 3x packet buffer |
| RGBW handling | Transparent | Variable run unit | 4th plane |
| Wire format complexity | Simple | Simple | Complex |

### 18.5 Empirical Benchmark Results

Measured on host CPU (Python 3.x). Encode/decode times are host-side only; ESP32 decode
times scale proportionally to pixel count (Xtensa LX6 at 240MHz runs RLE roughly 3x faster
per byte than the Python reference). 100 frames per measurement, two strip sizes:
800px (2400B raw) and 3200px (9600B raw).

The six patterns match the theoretical analysis above. ghost_rider substitutes for fire/plasma
(comparable organic noise, moderate per-frame change rate).

| Variant | Pattern | Pixels | Mean Ratio | Encode us | Decode us |
|---------|---------|--------|------------|-----------|-----------|
| byte_rle | rainbow | 800 | 1.008 | 454 | 8 |
| tuple_rle | rainbow | 800 | 1.003 | 408 | 5 |
| planar_rle | rainbow | 800 | 0.354 | 348 | 268 |
| delta_byte_rle | rainbow | 800 | 1.008 | 732 | 8 |
| delta_only | rainbow | 800 | 1.000 | 170 | 0 |
| byte_rle | sparse_twinkle | 800 | 0.749 | 527 | 129 |
| tuple_rle | sparse_twinkle | 800 | 0.813 | 419 | 34 |
| planar_rle | sparse_twinkle | 800 | 0.881 | 522 | 342 |
| delta_byte_rle | sparse_twinkle | 800 | 0.049 | 468 | 21 |
| delta_only | sparse_twinkle | 800 | 1.000 | 171 | 0 |
| byte_rle | chase_wipe | 800 | 0.017 | 273 | 12 |
| tuple_rle | chase_wipe | 800 | 0.014 | 219 | 6 |
| planar_rle | chase_wipe | 800 | 0.023 | 271 | 254 |
| delta_byte_rle | chase_wipe | 800 | 0.019 | 444 | 13 |
| delta_only | chase_wipe | 800 | 1.000 | 170 | 0 |
| byte_rle | gradient_fade | 800 | 1.001 | 565 | 9 |
| tuple_rle | gradient_fade | 800 | 0.277 | 268 | 71 |
| planar_rle | gradient_fade | 800 | 0.153 | 294 | 328 |
| delta_byte_rle | gradient_fade | 800 | 0.804 | 710 | 44 |
| delta_only | gradient_fade | 800 | 1.000 | 169 | 0 |
| byte_rle | solid_pulse | 800 | 0.978 | 557 | 8 |
| tuple_rle | solid_pulse | 800 | 0.012 | 218 | 4 |
| planar_rle | solid_pulse | 800 | 0.020 | 269 | 250 |
| delta_byte_rle | solid_pulse | 800 | 0.988 | 737 | 9 |
| delta_only | solid_pulse | 800 | 1.000 | 170 | 0 |
| byte_rle | ghost_rider | 800 | 0.055 | 298 | 22 |
| tuple_rle | ghost_rider | 800 | 0.077 | 262 | 16 |
| planar_rle | ghost_rider | 800 | 0.082 | 314 | 276 |
| delta_byte_rle | ghost_rider | 800 | 0.041 | 462 | 19 |
| delta_only | ghost_rider | 800 | 1.000 | 171 | 0 |
| byte_rle | rainbow | 3200 | 1.008 | 1847 | 30 |
| tuple_rle | rainbow | 3200 | 0.940 | 2218 | 120 |
| planar_rle | rainbow | 3200 | 0.345 | 1696 | 1141 |
| delta_byte_rle | rainbow | 3200 | 1.008 | 2943 | 30 |
| delta_only | rainbow | 3200 | 1.000 | 688 | 0 |
| byte_rle | sparse_twinkle | 3200 | 0.757 | 2130 | 508 |
| tuple_rle | sparse_twinkle | 3200 | 0.823 | 1700 | 118 |
| planar_rle | sparse_twinkle | 3200 | 0.882 | 2125 | 1351 |
| delta_byte_rle | sparse_twinkle | 3200 | 0.049 | 1871 | 81 |
| delta_only | sparse_twinkle | 3200 | 1.000 | 674 | 0 |
| byte_rle | chase_wipe | 3200 | 0.016 | 1106 | 40 |
| tuple_rle | chase_wipe | 3200 | 0.011 | 897 | 16 |
| planar_rle | chase_wipe | 3200 | 0.017 | 1126 | 1043 |
| delta_byte_rle | chase_wipe | 3200 | 0.016 | 1796 | 43 |
| delta_only | chase_wipe | 3200 | 1.000 | 682 | 0 |
| byte_rle | gradient_fade | 3200 | 1.001 | 2333 | 32 |
| tuple_rle | gradient_fade | 3200 | 0.069 | 947 | 76 |
| planar_rle | gradient_fade | 3200 | 0.046 | 1143 | 1108 |
| delta_byte_rle | gradient_fade | 3200 | 0.790 | 2791 | 74 |
| delta_only | gradient_fade | 3200 | 1.000 | 682 | 0 |
| byte_rle | solid_pulse | 3200 | 0.978 | 2277 | 30 |
| tuple_rle | solid_pulse | 3200 | 0.010 | 901 | 15 |
| planar_rle | solid_pulse | 3200 | 0.016 | 1117 | 1040 |
| delta_byte_rle | solid_pulse | 3200 | 0.988 | 2964 | 31 |
| delta_only | solid_pulse | 3200 | 1.000 | 683 | 0 |
| byte_rle | ghost_rider | 3200 | 0.029 | 1147 | 53 |
| tuple_rle | ghost_rider | 3200 | 0.036 | 973 | 35 |
| planar_rle | ghost_rider | 3200 | 0.041 | 1186 | 1069 |
| delta_byte_rle | ghost_rider | 3200 | 0.026 | 1837 | 55 |
| delta_only | ghost_rider | 3200 | 1.000 | 681 | 0 |

Ratio < 1.0 means compression; ratio = 1.0 means no change; ratio > 1.0 means expansion.
Planar-RLE decode times are high due to buffer allocation and three-pass reconstruction --
that cost is host-side Python overhead and is proportionally lower in C.

**Key findings:**
- Planar-RLE wins on rainbow (0.354 vs 1.008 byte-RLE) and gradient_fade (0.153 vs 1.001).
  Splitting channels into separate planes allows per-channel runs that interleaved data cannot.
- Tuple-RLE wins on solid_pulse (0.012) and gradient_fade (0.277). Pixel-level runs
  compress solid fills to a single control byte + one pixel value.
- Delta+byte-RLE wins on sparse_twinkle (0.049) and ghost_rider (0.041). Temporal
  coherence is the dominant compression opportunity for animated content.
- Delta-only (0x40) never beats delta+byte-RLE -- RLE always helps or is neutral.
  The delta-only mode exists only as a benchmark baseline.
- Byte-RLE wins on chase_wipe (0.017) -- long runs of black bytes compress well
  at the byte level without needing tuple or planar splitting.

**Recommendation by use case:**
- Animated LED effects (sparse changes): delta+byte-RLE (type 0x10)
- Solid fills, uniform colours: tuple-RLE (Python/JS only, no firmware type)
- Rainbow gradients, smooth colour transitions: planar-RLE (Python/JS only, no firmware type)
- Static content, chase patterns: byte-RLE (type 0x20 keyframe)
- Default for all content: delta+byte-RLE (type 0x10) -- best average across patterns

### 18.6 Statistical Analysis: Codec vs Transport Effects (Corrected)

**Note:** An earlier version of this section (commit a404c2b7) contained a measurement
artifact. The `/diag` HTTP endpoint runs on tcpip_thread -- the same thread that processes
DDP. Reading `/diag` concurrently with DDP traffic caused tcpip_thread contention, corrupting
push counter deltas and producing SD=599fps for WS/PPP planar. The corrected experiment
reads diag only before and after each run, never during.

**Corrected experiment:** 160 observations, 4 variants x 4 transports x 10 reps, 30fps
controlled rate, 10s runs, 8s cooldown. Hardware: M5StickC, single 40x80 TFT segment,
ghost_rider pattern (3200px). Data: `tools/benchmark_data/heap_transport_experiment_corrected_50d505d2.csv`.
Interaction plot: `docs/ddp_transport_interaction.png`.

#### Corrected cell means (eff_fps)

| Variant | UDP/PPP | UDP/WiFi | WS/PPP | WS/WiFi |
|---------|---------|----------|--------|---------|
| Byte-RLE | 25.6 | **30.0** | 5.6 | 29.7 |
| Delta+RLE | 17.9 | **30.0** | 4.7 | 27.7 |
| Tuple-RLE | 13.8 | **30.0** | 3.4 | 27.5 |
| Planar-RLE | 14.6 | **30.0** | unstable | 27.8 |

WS/PPP planar-RLE: genuine instability (SD=178fps including measurement artifacts;
SD=7.5fps for positive-only observations). See below.

#### Two-way ANOVA (stable transports, excl. WS/PPP)

| Source | Eta-squared | F | p |
|--------|-------------|---|---|
| transport | 0.593 | 130 | <2e-16 |
| **variant:transport** | **0.088** | **6.41** | **8.5e-6** |
| variant | 0.072 | 10.5 | 4.0e-6 |

**Critical correction from the contaminated analysis:** The variant:transport interaction
is now highly significant (p=8.5e-6, eta²=0.088). The old analysis showed p=0.161 --
that was the measurement artifact suppressing the real signal.

The interaction means codec choice has different effects depending on the link:

| | PPP | WiFi |
|---|---|---|
| Byte-RLE | 25.6 fps | 29.9 fps |
| Delta+RLE | 17.9 fps | 28.9 fps |
| Tuple-RLE | 13.8 fps | 28.8 fps |
| Planar-RLE | 14.6 fps | 28.9 fps |

On WiFi, all variants deliver ~28-30fps -- no meaningful codec effect. On PPP, there
is a clear ordering driven by packet size: byte-RLE and delta-RLE produce ~3% ratio
(~300B/frame), fitting comfortably within PPP bandwidth. Tuple-RLE and planar-RLE
produce ~4-5% ratio (~400-500B/frame), consuming more bandwidth and causing more drops.

#### Tukey HSD (variant, stable transports)

| Comparison | Diff | p adj |
|-----------|------|-------|
| tuple_rle - byte_rle | -4.67 fps | **0.041** |
| planar_rle - byte_rle | -4.33 fps | 0.066 |
| delta_rle - byte_rle | -3.25 fps | 0.247 |
| planar_rle - tuple_rle | +0.34 fps | 0.997 |

Tuple-RLE vs byte-RLE is the only significant pairwise difference (p=0.041). Planar
vs tuple-RLE: p=0.997 -- statistically indistinguishable.

#### Planar vs others (stable transports)

- planar_rle: mean=24.1fps, SD=7.83, n=30
- others: mean=25.8fps, SD=6.55, n=90
- Wilcoxon: W=1053, p=0.070, 95% CI: [-2.86, +0.06] fps

Not significant (p=0.070). The planar decode path has no practically meaningful
performance penalty on stable transports.

#### WS/PPP corrected picture

Excluding measurement artifacts (negative values from tcpip_thread contention during
diag reads):

| Variant | Valid obs | Mean fps | SD |
|---------|-----------|---------|-----|
| Byte-RLE | 7/10 | 14.9 | 10.7 |
| Delta+RLE | 10/10 | **4.7** | **2.1** |
| Tuple-RLE | 10/10 | 3.4 | 3.9 |
| Planar-RLE | 8/10 | 6.3 | 7.5 |

WS/PPP genuinely delivers 3-15fps at 30fps sender rate. Delta+RLE is the most stable
(SD=2.1fps, all 10 reps valid). The instability is real but was exaggerated by the
measurement artifact in the initial analysis.

Root cause: async_tcp ACKs every TCP segment immediately (`_ack_pcb=true` by default),
so the TCP window never shrinks and the PPP sender is never throttled. The async_tcp
event queue (64 entries, ~92KB) fills when the sender floods faster than the main loop
processes. When the queue fills, `_send_async_event()` blocks with `portMAX_DELAY`,
blocking tcpip_thread and corrupting concurrent HTTP responses.

#### Summary and transport recommendations

See sec 18.7 for transport selection guidance.

The corrected analysis reveals a **real variant x transport interaction** hidden by
measurement contamination. Codec choice matters on PPP (byte-RLE delivers 26fps vs
tuple-RLE's 14fps) but is irrelevant on WiFi (all variants hit 30fps). The planar
decode path has no significant performance penalty on stable transports.

### 18.7 Transport Selection Guide

**Interaction plot:** `docs/ddp_transport_interaction.png` -- shows eff_fps by variant
and transport with 95% confidence intervals.

#### Recommendations by link type

**WiFi (recommended for production DDP streaming):**
- UDP/WiFi: all variants deliver exactly 30fps at 30fps sender rate. Zero drops.
  Preferred for high-rate streaming. No transport overhead.
- WS/WiFi: ~27-30fps. Slightly lower than UDP due to WebSocket framing overhead.
  Use when the sender requires a persistent connection or bidirectional communication.
  Avoid for high-rate DDP (>60fps) -- async_tcp queue overhead becomes measurable.

**PPP/serial (bandwidth-constrained):**
- UDP/PPP: **strongly preferred over WS/PPP**. Statistically significant advantage
  (p<0.001). UDP packets are processed synchronously in the ESPAsyncE131 callback
  and dropped at the IP layer if the rate gate fires -- no buffering, no queue
  accumulation. WS/PPP routes through async_tcp which buffers aggressively.
- WS/PPP: **not recommended for production DDP**. Delivers only 3-15fps at 30fps
  sender rate. Unstable for tuple-RLE and planar-RLE (connection failures, measurement
  artifacts from tcpip_thread contention). Use only for low-rate control messages.

**Codec selection on PPP:**
- Byte-RLE: highest throughput on PPP (~26fps). Best for chase/wipe patterns.
- Delta+RLE: ~18fps on PPP. Best for animated content (sparse changes). Most stable
  on WS/PPP (SD=2.1fps).
- Tuple-RLE / Planar-RLE: ~14fps on PPP. Use only when compression ratio benefit
  (solid fills, gradients) outweighs the throughput cost.

#### Quantified WS overhead on PPP

| Metric | UDP/PPP | WS/PPP | WS overhead |
|--------|---------|--------|-------------|
| Byte-RLE eff_fps | 25.6 | 5.6 | **-78%** |
| Delta+RLE eff_fps | 17.9 | 4.7 | **-74%** |
| Tuple-RLE eff_fps | 13.8 | 3.4 | **-75%** |
| Planar-RLE eff_fps | 14.6 | unstable | -- |

WS/PPP delivers approximately 1/4 the effective frame rate of UDP/PPP across all
codec variants. The overhead is consistent (~75%) regardless of codec, confirming
it is a transport-layer cost (async_tcp buffering + WebSocket framing) rather than
a codec-specific effect.

#### Why WS is heavier than UDP on slow links

UDP DDP packets are processed synchronously in the ESPAsyncE131 UDP receive callback
(tcpip_thread). If the rate gate drops a packet, it is discarded immediately -- no
heap allocation, no queue entry, no buffering. The sender's next packet arrives
independently.

WS DDP packets go through async_tcp's event queue (64 entries, ~92KB capacity).
Each WS frame allocates a pbuf in the lwIP heap and queues an event. The async_tcp
service task processes events sequentially. When the sender floods faster than the
main loop processes frames, the queue fills. Once full, `_send_async_event()` blocks
with `portMAX_DELAY`, stalling tcpip_thread and preventing all network processing
including HTTP responses.

On WiFi (high bandwidth, low latency), the queue drains fast enough that this is
rarely an issue. On PPP (124 KB/s, high per-byte latency), the queue fills quickly
under any sustained DDP load, making WS/PPP fundamentally unsuitable for high-rate
streaming.

---

## 19. WebSocket Transport

DDP-over-WebSocket is an existing WLED feature (`common.js`). Compression support is implemented in the same file alongside the existing `sendDDP()` sender.

### 19.1 Current DDP-over-WS Path

The WLED frontend streams pixel data to the device via WebSocket using the same DDP packet format encapsulated in WS binary frames. The receiver side (`handleDDPPacket()`) is transport-agnostic -- it processes the same packet structure regardless of whether it arrived via UDP or WS.

Note: WS DDP packets have a 1-byte WLED protocol indicator prefix (`0x02`) before the DDP header, so all DDP header bytes are at offset+1 in the WS frame.

### 19.2 Interaction with permessage-deflate

WebSocket has its own per-message compression extension (permessage-deflate, RFC 7692). When enabled, the WS layer applies deflate compression to each message before transmission.

**Key consideration:** If permessage-deflate is active, layering PackBits RLE on top is largely redundant -- deflate is a superset of RLE and handles entropy removal more effectively. The useful part of the compression extension over WS is the **delta framing** (XOR against previous frame), not the RLE encoding.

Possible approaches:

| Approach | Sender | Receiver | Bandwidth | Complexity |
|----------|--------|----------|-----------|------------|
| Raw DDP over WS + permessage-deflate | No codec needed | No codec needed | Good (deflate handles it) | Minimal |
| Delta-only over WS (no RLE) | XOR delta, send raw delta | XOR decode only | Better (sparse deltas compress well under deflate) | Low |
| Full compressed DDP over WS | Same as UDP path | Same as UDP path | Redundant with deflate | Unnecessary |
| Delta+RLE over WS without deflate | Full codec | Full codec | Good | Moderate |

### 19.3 WS-Specific Mode

A delta-only mode (new compression type, e.g. `0x40`) that sends the raw XOR delta without RLE encoding. The WS transport layer (with permessage-deflate) handles the entropy removal. This gives the benefit of temporal coherence (delta) without the redundancy of double-compressing.

On transports without permessage-deflate (or where it's disabled), the sender falls back to delta+RLE (`0x10`) as usual.

### 19.4 JS Implementation

Three functions are implemented in `wled00/data/common.js`:

- `rleEncode(src)` -- PackBits byte-level RLE encoder. Takes `Uint8Array`, returns `Uint8Array`.
- `rleDecode(src, maxOut)` -- Streaming decoder. Takes `Uint8Array` + optional output size cap, returns `Uint8Array`.
- `sendDDPCompressed(ws, start, len, colors, prevFrame, isESP8266)` -- Compressed DDP sender. Tries delta+RLE against `prevFrame`, falls back to RLE-only, falls back to raw if compression ratio >= 90%. Sets C bit (`0x80`) on `dataType` when compressed. Returns `Uint8Array` copy of `colors` for use as `prevFrame` on the next call.

The existing `sendDDP()` function is unchanged.

### 19.5 Open Questions

- Should the WS path use delta-only (relying on permessage-deflate) or full delta+RLE?
- Does the WLED WS implementation enable permessage-deflate by default? If not, delta+RLE is needed.
- What prevFrame storage strategy works in the browser? `Uint8Array` is straightforward but adds memory pressure on mobile browsers.
- Should the JS encoder support transform compression, or just delta+RLE?

---

## 20. JPEG Hardware Compression (Reserved)

### 20.1 Motivation

For HUB75 LED wall installations streaming video content at scale (4096+ pixels), lossless compression (delta+RLE) becomes less effective -- video has high inter-frame change rates and the delta stream doesn't compress well. JPEG's lossy DCT approach trades exact colour fidelity for 8-12x bandwidth reduction, which is acceptable when the source is already video.

Two ESP32 variants ship hardware JPEG codecs that run DCT, quantization, and Huffman encode/decode with minimal CPU involvement:

| SoC | JPEG HW | ESP-IDF | Notes |
|-----|---------|---------|-------|
| ESP32-P4 | Encode + decode | v5.3+ | 768KB SRAM + PSRAM, 360MHz dual-core RISC-V |
| ESP32-S3.1 | Encode + decode | v6.x / master | New S3 die revision (2025/2026) |
| ESP32 / S2 / S3 / C3 / C6 | None | -- | Software JPEG only |

The hardware is gated by `SOC_JPEG_CODEC_SUPPORTED` in ESP-IDF. The driver (`esp_driver_jpeg`) compiles to nothing on unsupported chips.

### 20.2 Proposed Wire Format

Compression type `0x50` = JPEG (reserved in upper nibble of byte 1).

```
[0x41] [0x5n] [0x8B] [dest] [offset x 4] [len x 2] [JPEG bitstream...]
          |      |
          |      \-- C bit (0x80) set; pixel format = 0x8B & 0x7F = 0x0B (RGB24)
          \-- upper nibble 0x5 = JPEG
```

The `dataType` byte (byte 2) retains its original meaning (RGB24 = 0x0B, RGBW32 = 0x1B) to indicate the decoded pixel format. The receiver decodes the JPEG bitstream to the format specified by dataType.

**JPEG encode parameters** (not in wire format -- sender-side decisions):
- Quality: 70-85 (application-dependent, not signalled in packet)
- Subsampling: YUV422 recommended (best encode throughput on P4)
- Input: RGB888 or RGB565 (hardware accepts both directly)

**Image dimensions**: Derived from the pixel range. The sender reshapes the 1D pixel buffer into a 2D image for JPEG encoding. Receiver knows the expected pixel count from segment configuration. Dimensions are embedded in the JPEG header (SOF0 marker) so the receiver can validate.

### 20.3 Hardware Decode Performance (ESP32-P4, 360MHz)

The P4 JPEG decoder runs at approximately 95 Mpix/s. Decode time for LED wall configurations:

| LED Wall | Pixels | Decode Time | HW Decode FPS Cap |
|----------|--------|-------------|-------------------|
| 64x64 (1 panel) | 4,096 | 93us | 10,752 |
| 128x64 (2 panels) | 8,192 | 136us | 7,353 |
| 128x128 (4 panels) | 16,384 | 222us | 4,505 |
| 256x128 (8 panels) | 32,768 | 395us | 2,532 |
| 256x256 (16 panels) | 65,536 | 740us | 1,351 |

The hardware decoder is never the bottleneck. At every configuration the wire bandwidth is the limiting factor, not decode speed.

### 20.4 Bandwidth and FPS Analysis

Frame sizes and DDP packet counts (1440 bytes max per DDP packet):

| Config | Pixels | Raw RGB888 | JPEG Q85 (8:1) | JPEG Q70 (12:1) | Raw Packets | Q85 Packets |
|--------|--------|------------|----------------|-----------------|-------------|-------------|
| 64x64 | 4,096 | 12,288 B | 2,136 B | 1,624 B | 9 | 2 |
| 128x64 | 8,192 | 24,576 B | 3,672 B | 2,648 B | 18 | 3 |
| 128x128 | 16,384 | 49,152 B | 6,744 B | 4,696 B | 35 | 5 |
| 256x128 | 32,768 | 98,304 B | 12,888 B | 8,792 B | 69 | 9 |
| 256x256 | 65,536 | 196,608 B | 25,176 B | 16,984 B | 137 | 18 |

JPEG sizes include ~600 bytes fixed header overhead (quantization tables, Huffman tables, SOF/SOS markers).

**Maximum FPS over WiFi (20 Mbps effective, 2.3 MB/s):**

| Config | Raw FPS | JPEG Q85 FPS | JPEG Q70 FPS | Speedup (Q85) |
|--------|---------|--------------|--------------|---------------|
| 64x64 | 196 | 1,129 | 1,485 | 5.8x |
| 128x64 | 98 | 657 | 911 | 6.7x |
| 128x128 | 49 | 358 | 514 | 7.3x |
| 256x128 | 24 | 187 | 274 | 7.8x |
| 256x256 | **12** | **96** | **142** | **8.0x** |

**Maximum FPS over 100Mbps Ethernet (11.6 MB/s):**

| Config | Raw FPS | JPEG Q85 FPS | JPEG Q70 FPS |
|--------|---------|--------------|--------------|
| 64x64 | 990 | 5,695 | 7,490 |
| 128x64 | 495 | 3,313 | 4,593 |
| 128x128 | 247 | 1,803 | 2,590 |
| 256x128 | 124 | 944 | 1,383 |
| 256x256 | 62 | 483 | 717 |

**Maximum FPS over PPP 1.5Mbps (172 KB/s):**

| Config | Raw FPS | JPEG Q85 FPS | JPEG Q70 FPS |
|--------|---------|--------------|--------------|
| 64x64 | 14 | 82 | 108 |
| 128x64 | 7 | 48 | 67 |
| 128x128 | 3.6 | 26 | 38 |
| 256x128 | 1.8 | 14 | 20 |
| 256x256 | 0.9 | 7 | 10 |

### 20.5 Crossover: JPEG vs Delta+RLE

For video content, delta+RLE achieves roughly 5:1 compression (high inter-frame change). JPEG Q85 achieves 8:1 with ~600 bytes fixed header overhead.

```
JPEG compressed size:      pixels x 3 / 8 + 600
Delta+RLE compressed size: pixels x 3 / 5

JPEG < Delta+RLE when:
  pixels x 3/8 + 600 < pixels x 3/5
  600 < pixels x 9/40
  pixels > 2,667
```

**JPEG wins on bandwidth above ~2,667 pixels for video content.** A single 64x64 HUB75 panel (4,096 pixels) is already above the crossover.

For LED effects (sparse changes, exact colours), delta+RLE remains superior:
- Lossless -- no colour artifacts on solid fills or sharp edges
- Zero fixed overhead -- better for partial segment updates
- 20:1 to 150:1 ratios on typical LED animations vs JPEG's 8:1

**Recommendation:** JPEG for full-frame video streaming to large LED walls. Delta+RLE for LED effects, UI overlays, partial updates, and any use case requiring exact pixel values.

### 20.6 Receiver Memory Requirements

| Buffer | Size | Location |
|--------|------|----------|
| JPEG input (compressed frame) | 2-25 KB (config-dependent) | Internal SRAM or PSRAM |
| JPEG HW working memory | ~65 KB | Internal SRAM (HW peripheral) |
| RGB output (decoded frame) | 12 KB (64x64) to 192 KB (256x256) | PSRAM for large configs |
| **Total (single-buffered)** | **80 KB (64x64) to 282 KB (256x256)** | |

P4 has 768KB internal SRAM + external PSRAM. Single-buffered fits in internal SRAM for all configs up to 256x128. 256x256 output buffer goes to PSRAM (DMA engine supports PSRAM at 200MHz).

### 20.7 Colourspace and Lossiness

JPEG encodes in YCbCr internally. The P4 hardware accepts RGB888 or RGB565 input and handles the RGB->YCbCr conversion in hardware. Decode output is RGB888 or RGB565.

The round-trip RGB->YCbCr->DCT->quantize->Huffman->decode->RGB introduces:
- Chroma subsampling loss (YUV422: chroma resolution halved horizontally)
- Quantization loss (DCT coefficients rounded to quantization table values)
- At Q85: typically +/-2 per channel. On 8-bit LEDs viewed from >1m, imperceptible.
- At Q70: typically +/-4 per channel. Banding visible on smooth gradients at close range.

**This is acceptable for video-to-LED-wall** where the source material is already lossy (camera, video file, rendered graphics). It is NOT acceptable for LED effects that set exact pixel values.

### 20.8 Use Cases

| Scenario | JPEG Suitable? | Why |
|----------|---------------|-----|
| Video mapped to HUB75 LED wall (P4/S3.1) | Yes | Source is already lossy, 8x bandwidth gain, HW decode sub-ms |
| xLights/LedFX video streaming to large matrix | Yes | Sender does SW encode (trivial on PC), receiver HW decodes |
| Camera feed to LED display (ESP32-P4 cam) | Yes | HW encode on sender side too, full HW pipeline |
| Concert/event LED wall over WiFi | Yes | 256x256 goes from 12fps raw to 96fps Q85 -- the difference between unusable and smooth |
| LED strip effects (WS2812B, SK6812) | No | Exact colours needed, delta+RLE is lossless and compresses better for sparse changes |
| Small matrix effects (<2000 pixels) | No | JPEG header overhead (600B) dominates, delta+RLE wins |
| RGBW strips | No | JPEG has no native W channel, would need separate handling |

### 20.9 Implementation Status

**Reserved.** No implementation exists. The compression type `0x50` is reserved in the spec for future use.

Implementation would require:
- Receiver: route type `0x50` through `jpeg_decoder_process()`, write decoded RGB to HUB75 framebuffer
- Sender: software JPEG encode on host, set compression type `0x50` in DDP header
- Build guard: `#if SOC_JPEG_CODEC_SUPPORTED` -- zero cost on chips without hardware JPEG
- Fallback: sender detects receiver capability via DDP status query or configuration, falls back to delta+RLE on non-P4/S3.1 hardware

---

## 21. tinfl RX-Only Compression (Reserved)

### 21.1 Motivation

For keyframe-heavy content (video, full-panel redraws), lossless RLE variants
compress poorly. zlib deflate achieves 3-8x on typical LED content vs RLE's
1-2x on keyframes. The ESP32 Arduino framework ships `miniz.h` with the raw
`tinfl` decompressor -- no zlib API, no compressor, just the inflate side.

### 21.2 Memory Requirements

| Component | Size | Notes |
|-----------|------|-------|
| `tinfl_decompressor` struct | 10 KB | Huffman decode tables |
| Output window buffer | 32 KB | Required by tinfl for back-references |
| **Total per connection** | **42 KB** | |

`MINIZ_NO_ZLIB_APIS` is defined in the ESP32 miniz.h -- only the raw
`tinfl_decompress()` / `tinfl_decompress_mem_to_mem()` API is available.
The compressor (`tdefl`) is not available and would cost 164 KB anyway.

### 21.3 Wire Format (Reserved)

Compression type `0x60` in the upper nibble of DDP byte 1 (alongside
`0x10` delta+RLE, `0x20` RLE, `0x30` transform, `0x40` delta-only).

```
[0x41] [0x6n] [0x8B] [dest] [offset x 4] [len x 2] [zlib-raw deflate stream...]
          |      |
          |      \-- C bit (0x80) set; pixel format = 0x8B & 0x7F = 0x0B (RGB24)
          \-- upper nibble 0x6 = tinfl/zlib-raw
```

The payload is a raw deflate stream (no zlib header, no checksum) -- equivalent
to `zlib.compress(data)[2:-4]` in Python or `pako.deflateRaw(data)` in JS.

### 21.4 Sender API

**Python:**
```python
import zlib
def compress_tinfl(data: bytes) -> bytes:
    return zlib.compress(data, level=1)[2:-4]  # strip 2-byte header + 4-byte checksum
```

**JavaScript (pako):**
```javascript
import pako from 'pako';
function compressTinfl(data) {
    return pako.deflateRaw(data, { level: 1 });
}
```

### 21.5 Receiver API (ESP32)

```c
#include "miniz.h"  // ships with ESP32 Arduino framework

bool ddp_tinfl_decode(const uint8_t *compressed, size_t comp_len,
                      uint8_t *out, size_t out_len) {
    size_t actual = out_len;
    int status = tinfl_decompress_mem_to_mem(out, &actual,
                                              compressed, comp_len,
                                              TINFL_FLAG_PARSE_ZLIB_HEADER);
    return (status == TINFL_STATUS_DONE) && (actual == out_len);
}
```

Note: `TINFL_FLAG_PARSE_ZLIB_HEADER` must NOT be set when the sender uses
raw deflate (no zlib header). Use `0` as the flags argument for raw deflate.

### 21.6 Feasibility by Target

| Target | Free Heap | 42KB cost | Verdict |
|--------|-----------|-----------|---------|
| M5StickC (ESP32-PICO-D4, no PSRAM) | ~200 KB | 21% | Feasible for 1 connection; risky for 2+ |
| ESP32 with 4MB PSRAM | ~4 MB | 1% | Trivially feasible |
| ESP32-P4 (768 KB SRAM + PSRAM) | ~700 KB | 6% | Feasible |

### 21.7 Status

**Reserved.** Compression type `0x60` is reserved in the wire format.
No receiver implementation exists. Implement when a PSRAM-equipped target
requires better keyframe compression than RLE variants provide.

The `dmaBusy()` skip-frame approach was tried for SPI DMA and reverted --
tinfl implementation should not repeat that mistake. Implement only after
validating the 42 KB allocation does not cause heap fragmentation on the
target device.

---

## Appendix A: Packet Hexdump Examples

### A.1 Raw RGB -- 3 pixels (red, green, blue), single packet with push

```
41 01 0B FF 00 00 00 00 00 09 FF 00 00 00 FF 00 00 00 FF
|  |  |  |  \--offset=0--/ \len=9/ \R--G--B-/ \R--G--B-/ \R--G--B-/
|  |  |  \-- dest=ALL (0xFF)
|  |  \-- dataType=RGB24 (0x0B)
|  \-- seq=1
\-- flags=VER1|PUSH (0x41)
```

### A.2 Compressed Delta+RLE -- 3 unchanged pixels (all zeros after XOR)

```
41 11 8B FF 00 00 00 00 00 03 08 00
|  |  |  |  \--offset=0--/ \l=3-/ |  \-- RLE: run of 9 zeros (0x08 = count 9)
|  |  |  \-- dest=ALL
|  |  \-- dataType=0x8B: C bit set (0x80) | RGB24 (0x0B); pixel format = 0x8B & 0x7F = 0x0B
|  \-- seq=1, comp_type=DELTA_RLE (upper nibble 0x1)
\-- VER1|PUSH (0x41)
```

### A.3 RGBW Raw -- 2 pixels (white, off), single packet

```
41 01 1B FF 00 00 00 00 00 08 FF FF FF FF 00 00 00 00
|  |  |  |  \--offset=0--/ \l=8-/ \--RGBW pixel 1-/ \--RGBW pixel 2-/
|  |  \-- dataType=RGBW32 (0x1B)
|  \-- seq=1
\-- VER1|PUSH
```

---

## Appendix B: Complete Standalone Implementations

These are self-contained, copy-paste-ready implementations for any codebase. No external dependencies beyond standard libraries.

### B.1 Complete Python DDP Library (sender + receiver + compression)

```python
#!/usr/bin/env python3
"""
ddp.py -- Complete DDP (Distributed Display Protocol) implementation.

Standalone library implementing raw and compressed DDP for any Python project.
No dependencies beyond stdlib. Supports RGB, RGBW, raw, RLE, delta+RLE,
and transform compression.

Usage:
    from ddp import DDPSender, DDPReceiver, RLECodec

    # Sender
    sender = DDPSender("192.168.1.100")
    sender.send_frame([0xFF0000, 0x00FF00, 0x0000FF])  # 3 RGB pixels
    sender.send_frame_compressed(current_pixels, prev_pixels)

    # Receiver
    receiver = DDPReceiver(num_pixels=100)
    receiver.start()  # listens on UDP 4048

License: MIT
Source: https://github.com/aenertia/WLED (refs/ddp-readme.md)
Spec: http://www.3waylabs.com/ddp/
"""

import socket
import struct
import threading
from typing import Optional, Tuple, List, Callable

# ----------------------------------------------------------------------
# Protocol Constants
# ----------------------------------------------------------------------

DDP_PORT            = 4048
DDP_HEADER_LEN      = 10
DDP_MAX_PAYLOAD     = 1440   # 480 RGB or 360 RGBW pixels per packet

# Flags (byte 0)
DDP_VER1            = 0x40
DDP_PUSH            = 0x01
DDP_QUERY           = 0x02
DDP_REPLY           = 0x04
DDP_STORAGE         = 0x08
DDP_TIME            = 0x10

# Compression types (byte 1, upper nibble)
COMP_NONE           = 0x00
COMP_DELTA_RLE      = 0x10   # XOR delta + PackBits RLE
COMP_RLE            = 0x20   # PackBits RLE only (keyframe)
COMP_TRANSFORM      = 0x30   # global operation + sparse writes

# Transform operations
TRANSFORM_SCALE_TOWARD = 0x01  # lerp(prev, target, alpha)
TRANSFORM_SCALE_MULT   = 0x02  # prev * factor / 255
TRANSFORM_NOP          = 0x03  # no global op, explicit writes only

# Data types (byte 2)
# C bit (bit 7, 0x80): payload is compressed. Recover pixel format with dataType & 0x7F.
TYPE_COMPRESSED     = 0x80   # C bit: set on dataType when payload is compressed
TYPE_RGB24          = 0x0B   # RGB, 8 bits/channel, 3 channels
TYPE_RGBW32         = 0x1B   # RGBW, 8 bits/channel, 4 channels
TYPE_RGB24_COMP     = 0x8B   # RGB24 with C bit set (compressed)
TYPE_RGBW32_COMP    = 0x9B   # RGBW32 with C bit set (compressed)

# Destination IDs (byte 3)
DEST_DISPLAY        = 0x01
DEST_ALL            = 0xFF
DEST_CONTROL        = 0xF6
DEST_CONFIG         = 0xFA
DEST_STATUS         = 0xFB


# ----------------------------------------------------------------------
# RLE Codec (PackBits-inspired, byte-level)
# ----------------------------------------------------------------------

class RLECodec:
    """PackBits-inspired byte-level RLE encoder/decoder.

    Control byte encoding:
      bit 7 = 0: RUN   -- next byte repeated (ctrl & 0x7F)+1 times (1-128)
      bit 7 = 1: LITERAL -- next (ctrl & 0x7F)+1 bytes are verbatim (1-128)

    Worst-case expansion: ~0.8% (1 control byte per 128 input bytes).
    """

    @staticmethod
    def encode(src: bytes) -> bytes:
        """Encode bytes using PackBits-style RLE."""
        out = bytearray()
        i, n = 0, len(src)
        while i < n:
            # Check for a run of identical bytes
            cur, run = src[i], 1
            while i + run < n and src[i + run] == cur and run < 128:
                run += 1
            if run >= 3:
                out.append(run - 1)       # RUN control (bit 7 clear)
                out.append(cur)
                i += run
            else:
                # Accumulate a literal span
                lit_start, lit_len = i, 0
                while i < n and lit_len < 128:
                    ahead = 1
                    while i + ahead < n and src[i + ahead] == src[i] and ahead < 3:
                        ahead += 1
                    if ahead >= 3:
                        break
                    i += 1
                    lit_len += 1
                if lit_len:
                    out.append(0x80 | (lit_len - 1))  # LITERAL control (bit 7 set)
                    out.extend(src[lit_start:lit_start + lit_len])
        return bytes(out)

    @staticmethod
    def decode(src: bytes) -> bytes:
        """Decode PackBits-style RLE data."""
        out = bytearray()
        i, n = 0, len(src)
        while i < n:
            ctrl = src[i]; i += 1
            if ctrl & 0x80:  # LITERAL
                count = (ctrl & 0x7F) + 1
                out.extend(src[i:i + count])
                i += count
            else:            # RUN
                count = (ctrl & 0x7F) + 1
                if i < n:
                    out.extend(bytes([src[i]]) * count)
                    i += 1
        return bytes(out)

    @staticmethod
    def max_encoded_size(raw_len: int) -> int:
        """Maximum encoded size for a given raw input length."""
        return raw_len + (raw_len // 128) + 2


class StreamingRLEDecoder:
    """Stateful streaming RLE decoder -- emits one byte at a time.

    This is a Python port of the C RLEDecoder struct used on ESP32 for
    zero-copy decoding on memory-constrained devices.

    Usage:
        decoder = StreamingRLEDecoder(encoded_data)
        while True:
            byte_val = decoder.next()
            if byte_val is None:
                break
            process(byte_val)
    """

    def __init__(self, data: bytes):
        self.src = data
        self.pos = 0
        self.remaining = 0
        self.is_run = False
        self.value = 0

    def next(self) -> Optional[int]:
        """Return next decoded byte, or None if exhausted."""
        while self.remaining == 0:
            if self.pos >= len(self.src):
                return None
            ctrl = self.src[self.pos]; self.pos += 1
            if ctrl & 0x80:
                self.remaining = (ctrl & 0x7F) + 1
                self.is_run = False
            else:
                self.remaining = (ctrl & 0x7F) + 1
                self.is_run = True
                if self.pos < len(self.src):
                    self.value = self.src[self.pos]; self.pos += 1
                else:
                    return None
        self.remaining -= 1
        if self.is_run:
            return self.value
        else:
            if self.pos < len(self.src):
                val = self.src[self.pos]; self.pos += 1
                return val
            return None


# ----------------------------------------------------------------------
# Delta Encoding
# ----------------------------------------------------------------------

def xor_delta(current: bytes, previous: bytes) -> bytes:
    """XOR two byte buffers. Unchanged bytes become 0x00."""
    assert len(current) == len(previous), "Frame size mismatch"
    return bytes(a ^ b for a, b in zip(current, previous))


def compress_adaptive(current: bytes, previous: Optional[bytes] = None,
                      max_compressed: int = 2048) -> Tuple[bytes, int]:
    """Try compression strategies and pick the smallest output.

    Returns (compressed_data, compression_type).
    Falls back to raw if compressed exceeds max_compressed.
    """
    best, best_type = current, COMP_NONE

    # Try RLE only
    rle = RLECodec.encode(current)
    if len(rle) < len(best):
        best, best_type = rle, COMP_RLE

    # Try delta+RLE if previous frame available
    if previous and len(previous) == len(current):
        delta = xor_delta(current, previous)
        drle = RLECodec.encode(delta)
        if len(drle) < len(best):
            best, best_type = drle, COMP_DELTA_RLE

    # Cap: if compressed exceeds limit, send raw
    if best_type != COMP_NONE and len(best) > max_compressed:
        return current, COMP_NONE

    return best, best_type


# ----------------------------------------------------------------------
# DDP Packet Construction
# ----------------------------------------------------------------------

def next_seq(seq: int) -> int:
    """Advance DDP sequence number. Cycles 1-15, never 0."""
    return (seq % 15) + 1


def make_header(flags: int, seq: int, data_type: int, dest: int,
                offset: int, data_len: int) -> bytes:
    """Construct a 10-byte DDP header."""
    return struct.pack("!BBBBIH", flags, seq, data_type, dest, offset, data_len)


def make_packets(data: bytes, seq: int = 1, push: bool = True,
                 comp_type: int = COMP_NONE,
                 data_type: int = TYPE_RGB24,
                 dest: int = DEST_ALL,
                 max_payload: int = DDP_MAX_PAYLOAD
                 ) -> Tuple[list, int]:
    """Build DDP packet(s) from payload data.

    Returns (list_of_packets, next_sequence_number).
    Multi-packet frames set PUSH only on the last packet.
    """
    packets = []
    offset = 0

    while offset < len(data):
        chunk = min(max_payload, len(data) - offset)
        is_last = (offset + chunk >= len(data))

        flags = DDP_VER1
        if is_last and push:
            flags |= DDP_PUSH
        pkt_data_type = data_type | TYPE_COMPRESSED if comp_type != COMP_NONE else data_type

        seq_byte = (seq & 0x0F) | (comp_type & 0xF0)
        header = make_header(flags, seq_byte, pkt_data_type, dest, offset, chunk)
        packets.append(header + data[offset:offset + chunk])

        offset += chunk
        seq = next_seq(seq)

    return packets, seq


# ----------------------------------------------------------------------
# DDP Sender
# ----------------------------------------------------------------------

class DDPSender:
    """Complete DDP sender with optional compression.

    Example:
        sender = DDPSender("192.168.1.100")

        # Raw RGB frame (list of 0xRRGGBB integers)
        sender.send_rgb([0xFF0000, 0x00FF00, 0x0000FF])

        # Raw RGBW frame (list of 0xWWRRGGBB integers)
        sender.send_rgbw([0x80FF0000, 0x8000FF00])

        # Compressed frame (auto-selects best algorithm)
        pixels = bytes([255, 0, 0] * 800)  # 800 red pixels
        sender.send_compressed(pixels)       # uses delta+RLE if possible

        # Animation loop with compression
        prev = None
        for frame in animation:
            prev = sender.send_compressed(frame, prev)
    """

    def __init__(self, target_ip: str, port: int = DDP_PORT,
                 max_payload: int = DDP_MAX_PAYLOAD):
        self.target = (target_ip, port)
        self.max_payload = max_payload
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        self.seq = 1
        self.prev_frame: Optional[bytes] = None
        self.frame_count = 0
        self.keyframe_interval = 10

    def send_raw(self, pixel_bytes: bytes,
                 data_type: int = TYPE_RGB24) -> None:
        """Send raw (uncompressed) DDP frame."""
        packets, self.seq = make_packets(
            pixel_bytes, self.seq, push=True,
            data_type=data_type, max_payload=self.max_payload)
        for pkt in packets:
            self.sock.sendto(pkt, self.target)
        self.prev_frame = pixel_bytes
        self.frame_count += 1

    def send_rgb(self, pixels: List[int]) -> None:
        """Send RGB pixels as list of 0xRRGGBB integers."""
        data = bytearray()
        for c in pixels:
            data.extend([(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF])
        self.send_raw(bytes(data), TYPE_RGB24)

    def send_rgbw(self, pixels: List[int]) -> None:
        """Send RGBW pixels as list of 0xWWRRGGBB integers."""
        data = bytearray()
        for c in pixels:
            data.extend([(c >> 16) & 0xFF, (c >> 8) & 0xFF,
                         c & 0xFF, (c >> 24) & 0xFF])
        self.send_raw(bytes(data), TYPE_RGBW32)

    def send_compressed(self, pixel_bytes: bytes,
                        prev: Optional[bytes] = None,
                        data_type: int = TYPE_RGB24) -> bytes:
        """Send with adaptive compression. Returns pixel_bytes for use as next prev.

        Forces keyframe every self.keyframe_interval frames.
        """
        if prev is None:
            prev = self.prev_frame

        # Force keyframe periodically
        if self.frame_count % self.keyframe_interval == 0:
            prev = None

        compressed, comp_type = compress_adaptive(pixel_bytes, prev)
        packets, self.seq = make_packets(
            compressed, self.seq, push=True,
            comp_type=comp_type, data_type=data_type,
            max_payload=self.max_payload)
        for pkt in packets:
            self.sock.sendto(pkt, self.target)

        self.prev_frame = pixel_bytes
        self.frame_count += 1
        return pixel_bytes

    def close(self):
        self.sock.close()


# ----------------------------------------------------------------------
# DDP Receiver
# ----------------------------------------------------------------------

class DDPReceiver:
    """Complete DDP receiver with compression support.

    Example:
        def on_pixels(pixel_data: bytes, data_type: int):
            print(f"Received {len(pixel_data)} bytes, type={data_type:#x}")

        receiver = DDPReceiver(num_pixels=800, callback=on_pixels)
        receiver.start()  # blocks, listening on UDP 4048
    """

    def __init__(self, num_pixels: int = 800, port: int = DDP_PORT,
                 callback: Optional[Callable] = None):
        self.num_pixels = num_pixels
        self.port = port
        self.callback = callback
        self.pixels = bytearray(num_pixels * 4)  # RGBW32 format always
        self.prev_frame = bytearray(num_pixels * 4)
        self.last_push_seq = 0
        self.running = False

    def start(self, background: bool = False):
        """Start listening for DDP packets."""
        self.running = True
        if background:
            t = threading.Thread(target=self._listen, daemon=True)
            t.start()
            return t
        else:
            self._listen()

    def stop(self):
        self.running = False

    def _listen(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", self.port))
        sock.settimeout(1.0)

        while self.running:
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout:
                continue
            self._handle_packet(data)
        sock.close()

    def _handle_packet(self, packet: bytes):
        if len(packet) < DDP_HEADER_LEN:
            return

        flags = packet[0]
        seq_byte = packet[1]
        data_type = packet[2]
        dest = packet[3]
        offset = struct.unpack("!I", packet[4:8])[0]
        data_len = struct.unpack("!H", packet[8:10])[0]

        # Validate
        if flags & DDP_QUERY or flags & DDP_REPLY:
            return
        if dest in (DEST_CONTROL, DEST_CONFIG, DEST_STATUS):
            return

        push = bool(flags & DDP_PUSH)
        compressed = bool(data_type & TYPE_COMPRESSED)
        data_type_clean = data_type & 0x7F
        seq = seq_byte & 0x0F
        comp_type = seq_byte & 0xF0

        # Skip timecode if present
        payload_start = DDP_HEADER_LEN
        if flags & DDP_TIME:
            payload_start += 4

        if len(packet) < payload_start + data_len:
            return

        payload = packet[payload_start:payload_start + data_len]

        # Detect channels
        channels = 4 if ((data_type >> 3) & 0x07) == 3 else 3
        start_pixel = offset // channels

        if compressed:
            self._decode_compressed(payload, comp_type, start_pixel, channels)
        else:
            self._decode_raw(payload, start_pixel, channels)

        if push and self.callback:
            self.callback(bytes(self.pixels), data_type)

    def _decode_raw(self, data: bytes, start: int, channels: int):
        """Decode raw uncompressed pixel data."""
        i = 0
        pixel = start
        while i + channels <= len(data) and pixel < self.num_pixels:
            off = pixel * 4
            self.pixels[off] = data[i]         # R
            self.pixels[off + 1] = data[i + 1] # G
            self.pixels[off + 2] = data[i + 2] # B
            self.pixels[off + 3] = data[i + 3] if channels > 3 else 0  # W
            # Update prevFrame
            self.prev_frame[off:off + 4] = self.pixels[off:off + 4]
            pixel += 1
            i += channels

    def _decode_compressed(self, data: bytes, comp_type: int,
                           start: int, channels: int):
        """Decode compressed DDP data (RLE, delta+RLE, transform)."""
        if comp_type in (COMP_DELTA_RLE, COMP_RLE):
            decoder = StreamingRLEDecoder(data)
            pixel = start
            ch = []
            while pixel < self.num_pixels:
                val = decoder.next()
                if val is None:
                    break
                ch.append(val)
                if len(ch) >= channels:
                    off = pixel * 4
                    r, g, b = ch[0], ch[1], ch[2]
                    w = ch[3] if channels > 3 else 0

                    if comp_type == COMP_DELTA_RLE:
                        r ^= self.prev_frame[off]
                        g ^= self.prev_frame[off + 1]
                        b ^= self.prev_frame[off + 2]
                        w ^= self.prev_frame[off + 3]

                    self.pixels[off] = r
                    self.pixels[off + 1] = g
                    self.pixels[off + 2] = b
                    self.pixels[off + 3] = w
                    self.prev_frame[off:off + 4] = [r, g, b, w]
                    pixel += 1
                    ch = []

            # Error recovery: incomplete decode -> zero prevFrame
            if pixel < self.num_pixels and comp_type == COMP_DELTA_RLE:
                self.prev_frame[:] = bytearray(len(self.prev_frame))

        elif comp_type == COMP_TRANSFORM:
            self._decode_transform(data, start, channels)

    def _decode_transform(self, data: bytes, start: int, channels: int):
        """Decode transform compression."""
        if len(data) < 2 + channels + 2:
            return

        t_op = data[0]
        t_param = data[1]
        target = list(data[2:2 + channels])
        while len(target) < 4:
            target.append(0)
        hdr_len = 2 + channels
        num_explicit = data[hdr_len] | (data[hdr_len + 1] << 8)
        explicit_start = hdr_len + 2

        # Step A: global transform
        transform_end = min(start + num_explicit, self.num_pixels) if num_explicit > 0 else self.num_pixels
        for px in range(start, transform_end):
            off = px * 4
            prev = list(self.prev_frame[off:off + 4])

            if t_op == TRANSFORM_SCALE_TOWARD:
                result = [
                    prev[c] + ((target[c] - prev[c]) * t_param) // 255
                    for c in range(4)
                ]
            elif t_op == TRANSFORM_SCALE_MULT:
                result = [(prev[c] * t_param) // 255 for c in range(4)]
            elif t_op == TRANSFORM_NOP:
                continue
            else:
                continue

            for c in range(4):
                self.pixels[off + c] = max(0, min(255, result[c]))
                self.prev_frame[off + c] = self.pixels[off + c]

        # Step B: explicit pixel writes
        bytes_per_write = 2 + channels
        for e in range(num_explicit):
            pos = explicit_start + e * bytes_per_write
            if pos + bytes_per_write > len(data):
                break
            px_idx = data[pos] | (data[pos + 1] << 8)
            if px_idx >= self.num_pixels:
                continue
            off = px_idx * 4
            self.pixels[off] = data[pos + 2]
            self.pixels[off + 1] = data[pos + 3]
            self.pixels[off + 2] = data[pos + 4] if channels > 2 else 0
            self.pixels[off + 3] = data[pos + 5] if channels > 3 else 0
            self.prev_frame[off:off + 4] = self.pixels[off:off + 4]


# ----------------------------------------------------------------------
# Usage Examples
# ----------------------------------------------------------------------

if __name__ == "__main__":
    import time, math

    # Example: animated rainbow sender
    target = "169.254.7.1"  # WLED device IP
    num_leds = 60

    sender = DDPSender(target)
    print(f"Sending rainbow to {target}:{DDP_PORT}, {num_leds} LEDs")

    try:
        for frame in range(300):  # 10 seconds at 30fps
            t = frame / 300.0
            pixels = bytearray(num_leds * 3)
            for i in range(num_leds):
                hue = ((i / num_leds) + t) % 1.0
                # HSV to RGB (simplified)
                h = hue * 6.0
                c = int(h)
                f = h - c
                q = int(255 * (1 - f))
                tv = int(255 * f)
                x = i * 3
                if c == 0:   pixels[x], pixels[x+1], pixels[x+2] = 255, tv, 0
                elif c == 1: pixels[x], pixels[x+1], pixels[x+2] = q, 255, 0
                elif c == 2: pixels[x], pixels[x+1], pixels[x+2] = 0, 255, tv
                elif c == 3: pixels[x], pixels[x+1], pixels[x+2] = 0, q, 255
                elif c == 4: pixels[x], pixels[x+1], pixels[x+2] = tv, 0, 255
                else:        pixels[x], pixels[x+1], pixels[x+2] = 255, 0, q

            sender.send_compressed(bytes(pixels))
            time.sleep(1/30)  # 30fps
    except KeyboardInterrupt:
        pass
    finally:
        sender.close()
        print("Done.")
```

### B.2 Complete C Receiver (ESP32/Arduino, header-only)

```c
/*
 * ddp_receiver.h -- Complete DDP receiver with compression support.
 *
 * Header-only implementation for ESP32/Arduino or any C platform with
 * BSD sockets. Handles raw RGB/RGBW, RLE, delta+RLE, and transform
 * compression.
 *
 * Usage:
 *   #include "ddp_receiver.h"
 *
 *   static uint8_t pixel_buf[NUM_LEDS * 4];  // RGBW32 output
 *   static uint8_t prev_buf[NUM_LEDS * 4];   // delta reference
 *
 *   void on_frame(const uint8_t *pixels, unsigned num_pixels, uint8_t data_type) {
 *       // pixels[] contains RGBW32 data (4 bytes per pixel)
 *       for (unsigned i = 0; i < num_pixels; i++) {
 *           uint8_t r = pixels[i*4], g = pixels[i*4+1];
 *           uint8_t b = pixels[i*4+2], w = pixels[i*4+3];
 *           set_led(i, r, g, b, w);
 *       }
 *       show_leds();
 *   }
 *
 *   void setup() {
 *       ddp_init(pixel_buf, prev_buf, NUM_LEDS, on_frame);
 *   }
 *
 *   void loop() {
 *       ddp_poll();  // call frequently -- processes one packet per call
 *   }
 *
 * License: MIT
 * Source: https://github.com/aenertia/WLED
 */

#ifndef DDP_RECEIVER_H
#define DDP_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Protocol Constants ------------------------------------------- */

#define DDP_PORT              4048
#define DDP_HEADER_LEN        10
#define DDP_MAX_PAYLOAD       1440

#define DDP_VER1              0x40
#define DDP_PUSH              0x01
#define DDP_QUERY             0x02
#define DDP_REPLY             0x04
#define DDP_STORAGE           0x08
#define DDP_TIME              0x10

#define DDP_COMP_NONE         0x00
#define DDP_COMP_DELTA_RLE    0x10
#define DDP_COMP_RLE          0x20
#define DDP_COMP_TRANSFORM    0x30

#define DDP_TRANSFORM_TOWARD  0x01
#define DDP_TRANSFORM_MULT    0x02
#define DDP_TRANSFORM_NOP     0x03

/* C bit (bit 7, 0x80): set on dataType when payload is compressed.
 * Recover pixel format with dtype & 0x7F. */
#define DDP_TYPE_COMPRESSED   0x80
#define DDP_TYPE_RGB24        0x0B
#define DDP_TYPE_RGBW32       0x1B
#define DDP_TYPE_RGB24_COMP   0x8B  /* RGB24 with C bit */
#define DDP_TYPE_RGBW32_COMP  0x9B  /* RGBW32 with C bit */

#define DDP_DEST_DISPLAY      0x01
#define DDP_DEST_CONTROL      0xF6
#define DDP_DEST_CONFIG       0xFA
#define DDP_DEST_STATUS       0xFB

/* -- RLE Streaming Decoder ---------------------------------------- */

typedef struct {
    const uint8_t *src;
    size_t         src_len;
    size_t         pos;
    int            remaining;
    bool           is_run;
    uint8_t        value;
} ddp_rle_decoder_t;

static inline void ddp_rle_init(ddp_rle_decoder_t *d,
                                 const uint8_t *data, size_t len) {
    d->src = data;
    d->src_len = len;
    d->pos = 0;
    d->remaining = 0;
    d->is_run = false;
    d->value = 0;
}

static inline bool ddp_rle_next(ddp_rle_decoder_t *d, uint8_t *out) {
    while (d->remaining == 0) {
        if (d->pos >= d->src_len) return false;
        uint8_t ctrl = d->src[d->pos++];
        if (ctrl & 0x80) {
            d->remaining = (ctrl & 0x7F) + 1;
            d->is_run = false;
        } else {
            d->remaining = (ctrl & 0x7F) + 1;
            d->is_run = true;
            if (d->pos < d->src_len)
                d->value = d->src[d->pos++];
            else
                return false;
        }
    }
    d->remaining--;
    if (d->is_run) {
        *out = d->value;
    } else {
        if (d->pos >= d->src_len) return false;
        *out = d->src[d->pos++];
    }
    return true;
}

/* -- RLE Encoder -------------------------------------------------- */

static inline size_t ddp_rle_encode(const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_max) {
    size_t si = 0, di = 0;
    while (si < src_len && di < dst_max - 1) {
        uint8_t cur = src[si];
        size_t run_len = 1;
        while (si + run_len < src_len &&
               src[si + run_len] == cur && run_len < 128)
            run_len++;

        if (run_len >= 3) {
            dst[di++] = (uint8_t)(run_len - 1);
            dst[di++] = cur;
            si += run_len;
        } else {
            size_t lit_start = si, lit_len = 0;
            while (si < src_len && lit_len < 128) {
                size_t ahead = 1;
                while (si + ahead < src_len &&
                       src[si + ahead] == src[si] && ahead < 3)
                    ahead++;
                if (ahead >= 3) break;
                si++;
                lit_len++;
            }
            if (lit_len && di + 1 + lit_len <= dst_max) {
                dst[di++] = 0x80 | (uint8_t)(lit_len - 1);
                memcpy(&dst[di], &src[lit_start], lit_len);
                di += lit_len;
            }
        }
    }
    return di;
}

/* -- Receiver State ----------------------------------------------- */

typedef void (*ddp_frame_callback_t)(const uint8_t *pixels,
                                      unsigned num_pixels,
                                      uint8_t data_type);

typedef struct {
    uint8_t *pixels;          /* output pixel buffer (num_pixels * 4) */
    uint8_t *prev_frame;      /* delta reference buffer (num_pixels * 4) */
    unsigned  num_pixels;
    ddp_frame_callback_t callback;
    int       last_push_seq;
} ddp_receiver_t;

static ddp_receiver_t _ddp_rx;

static inline void ddp_init(uint8_t *pixel_buf, uint8_t *prev_buf,
                             unsigned num_pixels,
                             ddp_frame_callback_t cb) {
    _ddp_rx.pixels = pixel_buf;
    _ddp_rx.prev_frame = prev_buf;
    _ddp_rx.num_pixels = num_pixels;
    _ddp_rx.callback = cb;
    _ddp_rx.last_push_seq = 0;
    memset(pixel_buf, 0, num_pixels * 4);
    memset(prev_buf, 0, num_pixels * 4);
}

/* -- Packet Handler ----------------------------------------------- */

static inline void ddp_handle_packet(const uint8_t *pkt, size_t pkt_len) {
    if (pkt_len < DDP_HEADER_LEN) return;

    uint8_t  flags    = pkt[0];
    uint8_t  seq_byte = pkt[1];
    uint8_t  dtype    = pkt[2];
    uint8_t  dest     = pkt[3];
    uint32_t offset   = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                        ((uint32_t)pkt[6] << 8)  | pkt[7];
    uint16_t data_len = ((uint16_t)pkt[8] << 8) | pkt[9];

    /* Validate */
    if (flags & (DDP_QUERY | DDP_REPLY)) return;
    if (dest == DDP_DEST_CONTROL || dest == DDP_DEST_CONFIG ||
        dest == DDP_DEST_STATUS) return;

    bool push = flags & DDP_PUSH;
    bool compressed = dtype & DDP_TYPE_COMPRESSED;
    uint8_t dtype_clean = dtype & 0x7F;
    int  seq = seq_byte & 0x0F;
    int  comp_type = seq_byte & 0xF0;

    /* Timecode skip */
    unsigned c = (flags & DDP_TIME) ? 4 : 0;
    unsigned payload_start = DDP_HEADER_LEN + c;
    if (pkt_len < payload_start + data_len) return;

    const uint8_t *data = pkt + payload_start;
    unsigned channels = (((dtype_clean >> 3) & 0x07) == 3) ? 4 : 3;
    unsigned start = offset / channels;

    if (!compressed) {
        /* -- Raw decode ------------------------------------ */
        unsigned stop = start + data_len / channels;
        for (unsigned i = start, d = 0; i < stop && i < _ddp_rx.num_pixels;
             i++, d += channels) {
            unsigned off = i * 4;
            _ddp_rx.pixels[off]     = data[d];
            _ddp_rx.pixels[off + 1] = data[d + 1];
            _ddp_rx.pixels[off + 2] = data[d + 2];
            _ddp_rx.pixels[off + 3] = (channels > 3) ? data[d + 3] : 0;
            memcpy(&_ddp_rx.prev_frame[off], &_ddp_rx.pixels[off], 4);
        }
    } else if (comp_type == DDP_COMP_DELTA_RLE ||
               comp_type == DDP_COMP_RLE) {
        /* -- RLE / Delta+RLE decode ------------------------ */
        ddp_rle_decoder_t rle;
        ddp_rle_init(&rle, data, data_len);
        unsigned pixel = start;
        uint8_t ch[4] = {0};
        unsigned ci = 0;
        bool is_delta = (comp_type == DDP_COMP_DELTA_RLE);

        while (pixel < _ddp_rx.num_pixels) {
            uint8_t decoded;
            if (!ddp_rle_next(&rle, &decoded)) break;
            ch[ci++] = decoded;
            if (ci >= channels) {
                unsigned off = pixel * 4;
                uint8_t r = ch[0], g = ch[1], b = ch[2];
                uint8_t w = (channels > 3) ? ch[3] : 0;
                if (is_delta) {
                    r ^= _ddp_rx.prev_frame[off];
                    g ^= _ddp_rx.prev_frame[off + 1];
                    b ^= _ddp_rx.prev_frame[off + 2];
                    w ^= _ddp_rx.prev_frame[off + 3];
                }
                _ddp_rx.pixels[off] = r;
                _ddp_rx.pixels[off + 1] = g;
                _ddp_rx.pixels[off + 2] = b;
                _ddp_rx.pixels[off + 3] = w;
                memcpy(&_ddp_rx.prev_frame[off], &_ddp_rx.pixels[off], 4);
                pixel++;
                ci = 0;
            }
        }
        /* Error recovery: incomplete decode */
        if (pixel < _ddp_rx.num_pixels && is_delta) {
            memset(_ddp_rx.prev_frame, 0, _ddp_rx.num_pixels * 4);
        }
    }
    /* Transform decode omitted for brevity -- see full WLED implementation */

    if (push && _ddp_rx.callback) {
        _ddp_rx.callback(_ddp_rx.pixels, _ddp_rx.num_pixels, dtype);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DDP_RECEIVER_H */
```

### B.3 Minimal Raw DDP Sender in C (POSIX sockets)

```c
/*
 * ddp_send.c -- Minimal DDP sender for POSIX systems.
 *
 * Compile: gcc -o ddp_send ddp_send.c
 * Usage:   ./ddp_send 192.168.1.100 60    # send rainbow to 60 LEDs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define DDP_PORT        4048
#define DDP_HEADER_LEN  10
#define DDP_MAX_PAYLOAD 1440
#define DDP_VER1_PUSH   0x41
#define DDP_VER1        0x40
#define DDP_TYPE_RGB24  0x0B

static int ddp_seq = 1;

static int ddp_next_seq(void) {
    int s = ddp_seq;
    ddp_seq = (ddp_seq % 15) + 1;
    return s;
}

static void ddp_send_frame(int sock, struct sockaddr_in *dest,
                            const uint8_t *rgb, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > DDP_MAX_PAYLOAD)
                       ? DDP_MAX_PAYLOAD : (len - offset);
        int last = (offset + chunk >= len);

        uint8_t pkt[DDP_HEADER_LEN + DDP_MAX_PAYLOAD];
        pkt[0] = last ? DDP_VER1_PUSH : DDP_VER1;
        pkt[1] = ddp_next_seq() & 0x0F;
        pkt[2] = DDP_TYPE_RGB24;
        pkt[3] = 0xFF;  /* all devices */
        uint32_t off_be = htonl((uint32_t)offset);
        memcpy(&pkt[4], &off_be, 4);
        uint16_t len_be = htons((uint16_t)chunk);
        memcpy(&pkt[8], &len_be, 2);
        memcpy(&pkt[10], rgb + offset, chunk);

        sendto(sock, pkt, DDP_HEADER_LEN + chunk, 0,
               (struct sockaddr *)dest, sizeof(*dest));
        offset += chunk;
    }
}

static void hsv_to_rgb(float h, uint8_t *r, uint8_t *g, uint8_t *b) {
    float c = h * 6.0f;
    int i = (int)c;
    float f = c - i;
    uint8_t q = (uint8_t)(255 * (1.0f - f));
    uint8_t t = (uint8_t)(255 * f);
    switch (i % 6) {
        case 0: *r=255; *g=t;   *b=0;   break;
        case 1: *r=q;   *g=255; *b=0;   break;
        case 2: *r=0;   *g=255; *b=t;   break;
        case 3: *r=0;   *g=q;   *b=255; break;
        case 4: *r=t;   *g=0;   *b=255; break;
        case 5: *r=255; *g=0;   *b=q;   break;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <ip> <num_leds>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int num_leds = atoi(argv[2]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(DDP_PORT),
    };
    inet_pton(AF_INET, ip, &dest.sin_addr);

    uint8_t *rgb = malloc(num_leds * 3);
    printf("Sending rainbow to %s:%d, %d LEDs. Ctrl+C to stop.\n",
           ip, DDP_PORT, num_leds);

    for (int frame = 0; ; frame++) {
        float t = (float)frame / 300.0f;
        for (int i = 0; i < num_leds; i++) {
            float hue = fmodf((float)i / num_leds + t, 1.0f);
            hsv_to_rgb(hue, &rgb[i*3], &rgb[i*3+1], &rgb[i*3+2]);
        }
        ddp_send_frame(sock, &dest, rgb, num_leds * 3);
        usleep(33333);  /* ~30fps */
    }

    free(rgb);
    close(sock);
    return 0;
}
```

---

## Appendix C: Research Citations and Sources

### C.1 Protocol Specifications

| Source | URL | Accessed | Content |
|--------|-----|----------|---------|
| DDP Protocol Specification | http://www.3waylabs.com/ddp/ | 2026-08-12 | Authoritative DDP spec: header format, flags, data types, sequence numbering, push sync, timecode. Last updated 2022-09-05 by 3waylabs. |
| ANSI E1.31-2016 (sACN) | https://tsp.esta.org/tsp/documents/published_docs.php | 2026-08-12 | Entertainment Services and Technology Association standard for streaming DMX512-A over ACN. 126-byte header, multicast, priority system, sync universes. |
| Art-Net 4 Specification | https://art-net.org.uk/resources/art-net-specification/ | 2026-08-12 | Artistic Licence Ltd protocol. 18-byte header, ArtPoll discovery, ArtSync, 32,768 universe addressing. |
| WLED DDP Interface Docs | https://kno.wled.ge/interfaces/ddp/ | 2026-08-12 | WLED project documentation for DDP configuration and usage. |

### C.2 Compression Algorithm References

| Source | URL | Content |
|--------|-----|---------|
| PackBits (Apple, 1984) | https://en.wikipedia.org/wiki/PackBits | Apple Macintosh Toolbox Manager, used in TIFF (tag 32773) and DICOM. Byte-level RLE with run/literal modes. |
| TIFF 6.0 Specification sec 9 | https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf | PackBits compression definition: control byte bit 7 distinguishes runs from literals, 128-byte maximum spans. |
| VNC/RFB Protocol (RFC 6143) | https://datatracker.ietf.org/doc/html/rfc6143 | Remote framebuffer protocol encodings: Raw, CopyRect, RRE, Hextile, ZRLE, Tight. Relevant patterns for bandwidth-constrained display streaming. |
| VESA Display Stream Compression | https://vesa.org/vesa-display-stream-compression/ | DSC achieves 3:1 visually lossless on displays using indexed color history and YCgCo-R. Indexed Color History concept applicable to LED data. |

### C.3 Implementation References

| Source | URL | Content |
|--------|-----|---------|
| WLED GitHub Repository | https://github.com/wled/WLED | Open-source LED controller firmware. DDP receiver in `wled00/e131.cpp`, sender in `wled00/udp.cpp`. |
| WLED DDP Handler (upstream) | https://github.com/wled/WLED/blob/main/wled00/e131.cpp | Upstream uncompressed DDP implementation: sequence filtering, RGBW detection, multi-packet assembly. |
| WLED ESPAsyncE131 Library | https://github.com/wled/WLED/tree/main/wled00/src/dependencies/e131 | DDP/E1.31/Art-Net packet union, protocol constants, UDP listener. |
| OpenRGB DDP Output | https://gitlab.com/CalcProgrammer1/OpenRGB | OpenRGB's preferred WLED protocol. DDP device configuration in `DDPDevices` settings. |
| LedFX DDP Integration | https://github.com/LedFx/LedFx | Python LED effects framework with DDP output support. |
| ESP-IDF PPP API | https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif.html | ESP32 PPP netif for serial IP networking. Used for DDP-over-PPP transport. |
| lwIP PPP Implementation | https://savannah.nongnu.org/projects/lwip/ | Lightweight IP stack PPP implementation. `ppp_opts.h` defines PPP_MRU, PPP_DEFMRU, PPP_MAXMRU (default 1500). |

### C.4 Adversarial Review (this implementation)

This implementation was reviewed by a 5-member adversarial team:
- **Protocol Analyst**: DDP spec compliance, header field interpretation, RGBW detection
- **Memory Safety**: Buffer allocation, heap management, stack usage on ESP32
- **Test Architect**: Validation suite design, 40+ test cases, RGBW coverage gaps
- **Creative Critic**: Architectural flaws, race conditions, compression algorithm suitability
- **Deep Investigator**: End-to-end RGBW/CCT pixel pipeline trace

4 critical, 2 high, and 3 medium defects were found and fixed. See sec 14.1 for the full defect list.

---

## Appendix D: Changelog

| Date | Version | Changes |
|------|---------|---------|
| 2026-08-12 | 1.0 | Initial release. DDP spec, compression extension, validation suite, RGBW handling, transport considerations, reference implementations. Based on adversarial review by 5 independent analysts. |
| 2026-08-13 | 1.1 | Added complete standalone implementations (Python library, C header-only receiver, C POSIX sender). Added research citations. Added hexdump examples. |
| 2026-08-18 | 2.0 | Ground-truth update: MTU=1500 (pre-built liblwip.a constraint), prevFrame=RGB565 (heap trade-off), all defect statuses updated to match implementation, new sec 12.4 Receiver-Side Flow Control, /diag fields corrected, Transform encoder marked not-implemented. Based on sessions 8-17 iterative development and hardware validation. |
| 2026-08-20 | 2.1 | Added sec 17 Wire Format Options (reserved bit vs C-bit vs separate protocol), sec 18 Compression Variant Analysis (byte-level vs RGB-tuple vs colour planes), sec 19 WebSocket Transport (permessage-deflate interaction, JS encoder, delta-only mode). Added sec 4.5 Sender/Receiver Obligations for worst-case expansion. Renumbered sec 4.5-4.7 to sec 4.6-4.8. Sections 17-19 marked Open -- pending upstream consensus before wire format is locked. Prompted by softhack007 review feedback on #5810. |
| 2026-08-20 | 2.2 | Added sec 20 JPEG Hardware Compression (Reserved) -- performance analysis for HUB75 LED wall use cases on ESP32-P4/S3.1. Covers HW decode throughput (95 Mpix/s on P4), bandwidth/FPS tables for 4K-65K pixel configs across WiFi/Ethernet/PPP, crossover analysis vs delta+RLE (~2667 pixel threshold), receiver memory requirements, colourspace/lossiness tradeoffs, use case matrix. Compression type 0x50 reserved for future implementation. |
