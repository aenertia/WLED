"""DDP codec — RLE encode/decode, delta-XOR, adaptive compression, packet framing.

Byte-level RLE (PackBits-inspired):
  Control byte bit 7 = 0: RUN     — next byte repeated (ctrl & 0x7F)+1 times (1–128)
  Control byte bit 7 = 1: LITERAL — next (ctrl & 0x7F)+1 bytes copied verbatim (1–128)
"""

from __future__ import annotations

import struct
import subprocess
import re

# ── DDP protocol constants ──────────────────────────────────────────
DDP_DEFAULT_PORT = 4048
DDP_VER1 = 0x40
DDP_PUSH = 0x01
DDP_COMPRESSED = 0x20
DDP_RGB = 0x0B

COMP_NONE = 0x00
COMP_DELTA_RLE = 0x10
COMP_RLE = 0x20

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
            # LITERAL — copy *count* bytes verbatim
            out.extend(src[pos : pos + count])
            pos += count
        else:
            # RUN — repeat next byte *count* times
            if pos >= n:
                break
            out.extend(bytes([src[pos]]) * count)
            pos += 1

    return bytes(out)


def xor_delta(cur: bytes | bytearray, prev: bytes | bytearray) -> bytes:
    """Byte-wise XOR between two equal-length buffers."""
    return bytes(a ^ b for a, b in zip(cur, prev))


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
    """Split *data* into DDP-framed UDP packets (≤ max_payload each).

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

        if comp != COMP_NONE:
            compressed_chunk = rle_encode(raw_chunk)
            if len(compressed_chunk) < len(raw_chunk):
                flags |= DDP_COMPRESSED
                payload = compressed_chunk
            else:
                payload = raw_chunk
        else:
            payload = raw_chunk

        hdr = struct.pack(
            "!BBBBIH", flags, (seq & 0x0F) | (comp & 0xF0), DDP_RGB, 0xFF, off, len(payload),
        )
        pkts.append(hdr + payload)
        off += chunk_size
        seq = (seq % 15) + 1

    return pkts, seq
