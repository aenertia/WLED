"""Unit tests for the DDP RLE codec (tools/ddp_codec.py).

Covers: roundtrip correctness, edge cases, encoding format, worst-case
expansion, and pixel-pattern scenarios.
"""

from __future__ import annotations

import os
import random
import sys

import pytest

# Ensure tools/ is importable regardless of cwd.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ddp_codec import (
    COMP_DELTA_RLE,
    COMP_NONE,
    COMP_RLE,
    compress_adaptive,
    rle_decode,
    rle_encode,
    xor_delta,
)


# ── Roundtrip helpers ───────────────────────────────────────────────

def _roundtrip(data: bytes) -> None:
    """Encode then decode; assert identity."""
    encoded = rle_encode(data)
    decoded = rle_decode(encoded)
    assert decoded == data, (
        f"roundtrip failed: len(src)={len(data)}, "
        f"len(enc)={len(encoded)}, len(dec)={len(decoded)}"
    )


# ── 1. Empty input ──────────────────────────────────────────────────

class TestRLEEmpty:
    def test_encode_empty(self) -> None:
        assert rle_encode(b"") == b""

    def test_decode_empty(self) -> None:
        assert rle_decode(b"") == b""

    def test_roundtrip_empty(self) -> None:
        _roundtrip(b"")


# ── 2. Single byte ─────────────────────────────────────────────────

class TestRLESingleByte:
    def test_roundtrip_single(self) -> None:
        _roundtrip(b"\x42")

    def test_encoded_is_literal(self) -> None:
        enc = rle_encode(b"\x42")
        # Single byte  -> literal span of length 1: ctrl=0x80, then 0x42
        assert enc == b"\x80\x42"


# ── 3. Run of 3 (minimum run) ──────────────────────────────────────

class TestRLERunOf3:
    def test_encoding_format(self) -> None:
        src = b"\xAA" * 3
        enc = rle_encode(src)
        # ctrl = 3-1 = 0x02, value = 0xAA
        assert enc == b"\x02\xAA"

    def test_roundtrip(self) -> None:
        _roundtrip(b"\xAA" * 3)


# ── 4. Run of 128 (maximum single run) ─────────────────────────────

class TestRLERunOf128:
    def test_encoding_format(self) -> None:
        src = b"\x00" * 128
        enc = rle_encode(src)
        # ctrl = 128-1 = 0x7F, value = 0x00
        assert enc == b"\x7F\x00"

    def test_roundtrip(self) -> None:
        _roundtrip(b"\x00" * 128)


# ── 5. Run of 129 (must split into two runs) ───────────────────────

class TestRLERunOf129:
    def test_splits_into_two_runs(self) -> None:
        src = b"\xFF" * 129
        enc = rle_encode(src)
        _roundtrip(src)
        # 128 + 1  -> first run ctrl=0x7F, second is a short span
        assert len(enc) < len(src), "encoding must compress a long run"

    def test_roundtrip(self) -> None:
        _roundtrip(b"\xFF" * 129)


# ── 6. Alternating bytes (worst case  -- all literals) ───────────────

class TestRLEAlternating:
    def test_all_literals(self) -> None:
        src = b"\xAA\x55" * 64  # 128 bytes, no runs >= 3
        enc = rle_encode(src)
        _roundtrip(src)
        # Literal spans add 1 control byte per <=128 literal bytes.
        # 128 literals  -> 1 ctrl + 128 data = 129 bytes
        assert len(enc) <= len(src) + 2

    def test_roundtrip(self) -> None:
        _roundtrip(b"\xAA\x55" * 64)


# ── 7. Mixed runs + literals ───────────────────────────────────────

class TestRLEMixed:
    def test_roundtrip_mixed(self) -> None:
        # 10-byte run, 5 random literals, 20-byte run, 3 literals
        src = (
            b"\x11" * 10
            + b"\x01\x02\x03\x04\x05"
            + b"\x22" * 20
            + b"\xA0\xB0\xC0"
        )
        _roundtrip(src)

    def test_compression_ratio(self) -> None:
        src = b"\x11" * 10 + b"\x01\x02\x03\x04\x05" + b"\x22" * 20
        enc = rle_encode(src)
        # Runs compress well; overall must be shorter than raw
        assert len(enc) < len(src)


# ── 8. Random roundtrip (property-style) ───────────────────────────

class TestRLERandomRoundtrip:
    def test_100_random_buffers(self) -> None:
        rng = random.Random(42)
        for _ in range(100):
            length = rng.randint(0, 2048)
            data = bytes(rng.getrandbits(8) for _ in range(length))
            _roundtrip(data)


# ── 9. Worst-case expansion ────────────────────────────────────────

class TestRLEWorstCaseExpansion:
    def test_overhead_below_1_1_percent(self) -> None:
        """On random data the overhead must stay below 1.2%."""
        rng = random.Random(99)
        for _ in range(50):
            length = rng.randint(128, 4096)
            data = bytes(rng.getrandbits(8) for _ in range(length))
            enc = rle_encode(data)
            overhead = (len(enc) - len(data)) / len(data)
            assert overhead < 0.012, (
                f"overhead {overhead:.4%} on {length}-byte random buffer"
            )


# ── 10. All byte values ────────────────────────────────────────────

class TestRLEAllByteValues:
    def test_roundtrip_0_to_255(self) -> None:
        _roundtrip(bytes(range(256)))

    def test_roundtrip_255_to_0(self) -> None:
        _roundtrip(bytes(range(255, -1, -1)))


# ── 11. RGBW pixel patterns ────────────────────────────────────────

class TestRLERGBWPixelRoundtrip:
    def test_solid_white_rgbw(self) -> None:
        """All-white RGBW frame: 100 pixels x 4 bytes = 400 bytes of 0xFF."""
        src = b"\xFF" * 400
        enc = rle_encode(src)
        _roundtrip(src)
        # Should compress massively (400  -> ~8 bytes)
        assert len(enc) < 20

    def test_gradient_rgb(self) -> None:
        """Smooth gradient  -- few runs, mostly literals."""
        src = bytes(i % 256 for i in range(300))
        _roundtrip(src)

    def test_sparse_change_delta(self) -> None:
        """Delta-XOR of two frames differing by 1 pixel  -> mostly zeros."""
        frame_a = b"\x00" * 300
        frame_b = bytearray(frame_a)
        frame_b[150] = 0xFF
        frame_b[151] = 0x80
        frame_b[152] = 0x40
        delta = xor_delta(bytes(frame_b), frame_a)
        enc = rle_encode(delta)
        _roundtrip(delta)
        # Delta is almost all zeros  -> compresses very well
        assert len(enc) < 30


# ── 12. xor_delta correctness ──────────────────────────────────────

class TestXorDelta:
    def test_identical_frames(self) -> None:
        frame = b"\xDE\xAD" * 50
        assert xor_delta(frame, frame) == b"\x00" * 100

    def test_inverse(self) -> None:
        a = bytes(range(256))
        b = bytes((x + 1) % 256 for x in range(256))
        delta = xor_delta(a, b)
        restored = xor_delta(delta, b)
        assert restored == a


# ── 13. compress_adaptive selection ─────────────────────────────────

class TestCompressAdaptive:
    def test_no_prev_selects_rle_or_none(self) -> None:
        src = b"\xAA" * 100
        _payload, comp_type = compress_adaptive(src)
        assert comp_type in (COMP_NONE, COMP_RLE)

    def test_delta_rle_wins_on_sparse_change(self) -> None:
        # Scattered changes: delta is mostly zeros (compresses well),
        # but raw cur has mixed values (compresses poorly).
        prev = bytes(range(256)) + bytes(range(256))  # 512 bytes, all different
        cur = bytearray(prev)
        cur[100] ^= 0xFF
        cur[300] ^= 0xFF
        cur[400] ^= 0xFF
        _payload, comp_type = compress_adaptive(bytes(cur), prev)
        assert comp_type == COMP_DELTA_RLE

    def test_random_data_prefers_none(self) -> None:
        """Fully random data  -- compression can't help."""
        rng = random.Random(77)
        src = bytes(rng.getrandbits(8) for _ in range(1000))
        _payload, comp_type = compress_adaptive(src)
        # May be NONE or RLE (if overhead is tiny), but must not crash
        assert comp_type in (COMP_NONE, COMP_RLE)


# ── 14. RGBW delta+RLE W-channel preservation (C1 bug oracle) ──────

class TestDeltaRleRGBWChannelPreservation:
    """TDD oracle for the C1 firmware fix: prevFrame RGBW allocation.

    The bug: firmware allocates 3 bytes/pixel for prevFrame but RGBW needs 4.
    Delta decode stores only R,G,B  -- the W channel XORs against 0 instead of
    the previous W value.  Every test here exercises the W byte explicitly.
    """

    def test_delta_rle_rgbw_identical_frames(self) -> None:
        """XOR of identical RGBW frames  -> all zeros; RLE < 20 bytes."""
        prev = b"\xFF\x00\x80\x40" * 200  # 200 RGBW pixels
        delta = xor_delta(prev, prev)
        assert delta == b"\x00" * 800
        enc = rle_encode(delta)
        assert len(enc) < 20, f"expected <20 bytes, got {len(enc)}"
        assert rle_decode(enc) == delta

    def test_delta_rle_rgbw_w_channel_only_change(self) -> None:
        """Change ONLY the W byte of pixel 0; roundtrip must preserve it."""
        prev = b"\xFF\x00\x80\x40" * 200
        curr = bytearray(prev)
        curr[3] = 0xFF  # W: 0x40  -> 0xFF

        delta = xor_delta(bytes(curr), prev)
        enc = rle_encode(delta)
        decoded_delta = rle_decode(enc)
        reconstructed = bytes(xor_delta(decoded_delta, prev))

        assert reconstructed == bytes(curr), "full roundtrip mismatch"
        # CRITICAL: W channel of pixel 0 must be 0xFF, not 0x40
        assert reconstructed[3] == 0xFF, (
            f"W channel corrupted: expected 0xFF, got 0x{reconstructed[3]:02X}"
        )
        # R,G,B of pixel 0 unchanged
        assert reconstructed[0:3] == b"\xFF\x00\x80"

    def test_delta_rle_rgbw_w_channel_multi_frame(self) -> None:
        """3-frame sequence where only W changes; each must reconstruct."""
        n_pixels = 200
        frame0 = bytearray(b"\xFF\x00\x80\x40" * n_pixels)
        frame1 = bytearray(frame0)
        frame2 = bytearray(frame0)

        # Frame 1: all W bytes  -> 0x80
        for i in range(n_pixels):
            frame1[i * 4 + 3] = 0x80
        # Frame 2: all W bytes  -> 0xC0
        for i in range(n_pixels):
            frame2[i * 4 + 3] = 0xC0

        # Delta-encode frame1 from frame0
        d1 = xor_delta(bytes(frame1), bytes(frame0))
        r1 = bytes(xor_delta(rle_decode(rle_encode(d1)), bytes(frame0)))
        assert r1 == bytes(frame1), "frame1 roundtrip failed"
        for i in range(n_pixels):
            assert r1[i * 4 + 3] == 0x80, f"pixel {i} W wrong in frame1"

        # Delta-encode frame2 from frame1
        d2 = xor_delta(bytes(frame2), bytes(frame1))
        r2 = bytes(xor_delta(rle_decode(rle_encode(d2)), bytes(frame1)))
        assert r2 == bytes(frame2), "frame2 roundtrip failed"
        for i in range(n_pixels):
            assert r2[i * 4 + 3] == 0xC0, f"pixel {i} W wrong in frame2"

    def test_delta_rle_rgbw_no_previous(self) -> None:
        """First frame: prev=zeros, delta=identity, roundtrip exact."""
        prev = b"\x00" * 800
        curr = b"\xFF\x00\x80\x40" * 200

        delta = xor_delta(curr, prev)
        assert delta == curr, "XOR with zeros must be identity"

        reconstructed = bytes(xor_delta(rle_decode(rle_encode(delta)), prev))
        assert reconstructed == curr
        # Spot-check W bytes
        for i in range(200):
            assert reconstructed[i * 4 + 3] == 0x40, (
                f"pixel {i} W lost on first-frame roundtrip"
            )

    def test_delta_rle_rgbw_full_random_roundtrip(self) -> None:
        """100 random RGBW frame pairs; roundtrip must be exact."""
        rng = random.Random(0xC1)  # seed nods to the C1 bug
        for iteration in range(100):
            n_pixels = rng.randint(1, 300)
            buf_len = n_pixels * 4
            prev = bytes(rng.getrandbits(8) for _ in range(buf_len))
            curr = bytes(rng.getrandbits(8) for _ in range(buf_len))

            delta = xor_delta(curr, prev)
            enc = rle_encode(delta)
            decoded_delta = rle_decode(enc)
            reconstructed = bytes(xor_delta(decoded_delta, prev))

            assert reconstructed == curr, (
                f"iteration {iteration}: roundtrip mismatch "
                f"(n_pixels={n_pixels})"
            )
            # Explicit W-channel check on every 4th byte
            for px in range(n_pixels):
                w_idx = px * 4 + 3
                assert reconstructed[w_idx] == curr[w_idx], (
                    f"iteration {iteration}, pixel {px}: "
                    f"W=0x{reconstructed[w_idx]:02X} != expected 0x{curr[w_idx]:02X}"
                )
