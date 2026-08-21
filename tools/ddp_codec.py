"""DDP codec  -- RLE encode/decode, delta-XOR, adaptive compression, packet framing.

Byte-level RLE (PackBits-inspired):
  Control byte bit 7 = 0: RUN      -- next byte repeated (ctrl & 0x7F)+1 times (1-128)
  Control byte bit 7 = 1: LITERAL  -- next (ctrl & 0x7F)+1 bytes copied verbatim (1-128)
"""

from __future__ import annotations

import struct
import subprocess
import re

# ── DDP protocol constants ──────────────────────────────────────────
DDP_DEFAULT_PORT = 4048
DDP_VER1 = 0x40
DDP_PUSH = 0x01
DDP_TYPE_COMPRESSED = 0x80
DDP_RGB = 0x0B

COMP_NONE = 0x00
COMP_DELTA_RLE = 0x10
COMP_RLE = 0x20
COMP_TUPLE_RLE = 0x30  # Python-only for benchmarking, no firmware type code
COMP_DELTA_ONLY = 0x40  # raw XOR delta without RLE -- benchmark baseline

MAX_PAYLOAD = 4086
MAX_COMPRESSED_PAYLOAD = 2048

DDP_HEADER_LEN = 10


def detect_mtu(interface: str = "ppp0") -> int | None:
    """Read MTU from a network interface via ip link show. Returns None on failure."""
    try:
        out = subprocess.check_output(["ip", "link", "show", interface],
                                      text=True, timeout=2, stderr=subprocess.DEVNULL)
        m = re.search(r"mtu\s+(\d+)", out)
        return int(m.group(1)) if m else None
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        return None


def max_payload_for_mtu(mtu: int) -> int:
    """DDP payload limit for a given IP MTU (subtract UDP+DDP headers)."""
    return ((mtu - 28 - DDP_HEADER_LEN) // 3) * 3  # 28 = IP(20) + UDP(8), round to channel


# ── RLE codec ───────────────────────────────────────────────────────

def rle_encode(src: bytes | bytearray) -> bytes:
    """Encode *src* with PackBits-style byte-level RLE.

    Mirrors the C ``rle_encode()`` in ``wled00/ddp_compress.h``.
    """
    out = bytearray()
    i, n = 0, len(src)

    while i < n:
        cur = src[i]
        run = 1
        while i + run < n and src[i + run] == cur and run < 128:
            run += 1

        if run >= 3:
            out.append(run - 1)
            out.append(cur)
            i += run
        else:
            lit_start = i
            lit_len = 0
            while i < n and lit_len < 128:
                ahead = 1
                while i + ahead < n and src[i + ahead] == src[i] and ahead < 3:
                    ahead += 1
                if ahead >= 3:
                    break
                i += 1
                lit_len += 1
            if lit_len:
                out.append(0x80 | (lit_len - 1))
                out.extend(src[lit_start : lit_start + lit_len])

    return bytes(out)


def rle_decode(src: bytes | bytearray) -> bytes:
    """Decode PackBits-style byte-level RLE.

    Ported from the streaming ``RLEDecoder`` in ``wled00/ddp_compress.h``.
    """
    out = bytearray()
    pos, n = 0, len(src)

    while pos < n:
        ctrl = src[pos]
        pos += 1
        count = (ctrl & 0x7F) + 1

        if ctrl & 0x80:
            # LITERAL  -- copy *count* bytes verbatim
            out.extend(src[pos : pos + count])
            pos += count
        else:
            # RUN  -- repeat next byte *count* times
            if pos >= n:
                break
            out.extend(bytes([src[pos]]) * count)
            pos += 1

    return bytes(out)


def rle_planar_encode(src: bytes | bytearray, channels: int = 3) -> bytes:
    """RLE-encode after splitting interleaved pixel data into per-channel planes.

    Wire format: for each channel, a 2-byte little-endian length prefix
    followed by the RLE-encoded plane data.
    """
    n = len(src)
    if n == 0:
        return b"\x00\x00" * channels

    planes = [bytes(src[ch::channels]) for ch in range(channels)]
    out = bytearray()
    for plane in planes:
        enc = rle_encode(plane)
        out.extend(struct.pack("<H", len(enc)))
        out.extend(enc)
    return bytes(out)


def rle_planar_decode(src: bytes | bytearray, channels: int = 3) -> bytes:
    """Decode planar-RLE back to interleaved pixel data."""
    pos = 0
    planes: list[bytes] = []
    for _ in range(channels):
        if pos + 2 > len(src):
            break
        plen = struct.unpack_from("<H", src, pos)[0]
        pos += 2
        planes.append(rle_decode(src[pos:pos + plen]))
        pos += plen

    if not planes or all(len(p) == 0 for p in planes):
        return b""

    pixel_count = max(len(p) for p in planes)
    out = bytearray(pixel_count * channels)
    for ch, plane in enumerate(planes):
        for i, val in enumerate(plane):
            out[i * channels + ch] = val
    return bytes(out)


def rle_tuple_encode(src: bytes | bytearray, channels: int = 3) -> bytes:
    """Encode *src* with tuple-level RLE (channels bytes per run unit).

    Same control-byte format as byte-level RLE but operates on
    fixed-width tuples (e.g. 3 for RGB, 4 for RGBW).
    """
    out = bytearray()
    n = len(src)
    if n == 0:
        return b""
    if n % channels:
        raise ValueError(f"length {n} not a multiple of {channels}")

    nt = n // channels
    i = 0

    while i < nt:
        off = i * channels
        cur = src[off : off + channels]
        run = 1
        while i + run < nt and src[(i + run) * channels : (i + run + 1) * channels] == cur and run < 128:
            run += 1

        if run >= 3:
            out.append(run - 1)
            out.extend(cur)
            i += run
        else:
            lit_start = i
            lit_len = 0
            while i < nt and lit_len < 128:
                toff = i * channels
                tup = src[toff : toff + channels]
                ahead = 1
                while i + ahead < nt and src[(i + ahead) * channels : (i + ahead + 1) * channels] == tup and ahead < 3:
                    ahead += 1
                if ahead >= 3:
                    break
                i += 1
                lit_len += 1
            if lit_len:
                out.append(0x80 | (lit_len - 1))
                out.extend(src[lit_start * channels : (lit_start + lit_len) * channels])

    return bytes(out)


def rle_tuple_decode(src: bytes | bytearray, channels: int = 3) -> bytes:
    """Decode tuple-level RLE back to interleaved pixel data."""
    out = bytearray()
    pos, n = 0, len(src)

    while pos < n:
        ctrl = src[pos]
        pos += 1
        count = (ctrl & 0x7F) + 1

        if ctrl & 0x80:
            # LITERAL -- copy *count* tuples verbatim
            nbytes = count * channels
            out.extend(src[pos : pos + nbytes])
            pos += nbytes
        else:
            # RUN -- repeat next tuple *count* times
            if pos + channels > n:
                break
            tup = src[pos : pos + channels]
            pos += channels
            out.extend(tup * count)

    return bytes(out)


def xor_delta(cur: bytes | bytearray, prev: bytes | bytearray) -> bytes:
    return bytes(a ^ b for a, b in zip(cur, prev))


def compress_delta_only(cur: bytes, prev: bytes) -> tuple[bytes, int]:
    """XOR delta without RLE -- benchmark baseline for delta+RLE comparison."""
    if not prev or len(prev) != len(cur):
        return cur, COMP_NONE
    delta = xor_delta(cur, prev)
    return delta, COMP_DELTA_ONLY


# ── Adaptive compression ───────────────────────────────────────────

def compress_adaptive(
    cur: bytes | bytearray,
    prev: bytes | bytearray | None = None,
    max_payload: int = MAX_PAYLOAD,
) -> tuple[bytes, int]:
    """Try raw RLE and delta+RLE, return (payload, comp_type) for the smallest."""
    best, btype = cur, COMP_NONE

    rle = rle_encode(cur)
    if len(rle) < len(best):
        best, btype = rle, COMP_RLE

    if prev is not None and len(prev) == len(cur):
        delta = xor_delta(cur, prev)
        drle = rle_encode(delta)
        if len(drle) < len(best):
            best, btype = drle, COMP_DELTA_RLE

    return best, btype


# ── DDP packet framing ─────────────────────────────────────────────

def make_packets(
    data: bytes | bytearray,
    seq: int = 1,
    push: bool = True,
    comp: int = COMP_NONE,
    max_payload: int = MAX_PAYLOAD,
) -> tuple[list[bytes], int]:
    """Split *data* into DDP-framed UDP packets (<= max_payload each).

    When comp != COMP_NONE, *data* must be raw (uncompressed) pixel bytes.
    Each packet chunk is compressed independently so the receiver can decode
    each packet without state from previous packets.
    """
    pkts: list[bytes] = []
    off = 0

    while off < len(data):
        chunk_size = min(max_payload, len(data) - off)
        raw_chunk = data[off : off + chunk_size]
        last = off + chunk_size >= len(data)
        flags = DDP_VER1
        if last and push:
            flags |= DDP_PUSH

        data_type = DDP_RGB
        if comp != COMP_NONE:
            compressed_chunk = rle_encode(raw_chunk)
            if len(compressed_chunk) < len(raw_chunk):
                data_type |= DDP_TYPE_COMPRESSED
                payload = compressed_chunk
            else:
                payload = raw_chunk
        else:
            payload = raw_chunk

        hdr = struct.pack(
            "!BBBBIH", flags, (seq & 0x0F) | (comp & 0xF0), data_type, 0xFF, off, len(payload),
        )
        pkts.append(hdr + payload)
        off += chunk_size
        seq = (seq % 15) + 1

    return pkts, seq
