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
    COMP_DELTA_ONLY,
    COMP_DELTA_RLE,
    COMP_NONE,
    COMP_RLE,
    compress_adaptive,
    compress_delta_only,
    rle_decode,
    rle_encode,
    rle_planar_decode,
    rle_planar_encode,
    rle_tuple_decode,
    rle_tuple_encode,
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


def _tuple_roundtrip(data: bytes, channels: int = 3) -> None:
    encoded = rle_tuple_encode(data, channels)
    decoded = rle_tuple_decode(encoded, channels)
    assert decoded == data, (
        f"tuple roundtrip failed: len(src)={len(data)}, "
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


# ── 15. Delta-only (benchmark baseline) ────────────────────────────

class TestDeltaOnly:
    def test_identical_frames_all_zeros(self) -> None:
        """Identical frames: delta = all zeros, returned raw (no compression)."""
        frame = b"\xFF\x00\x80" * 100
        payload, comp = compress_delta_only(frame, frame)
        assert comp == COMP_DELTA_ONLY
        assert payload == b"\x00" * 300

    def test_single_pixel_change_800px(self) -> None:
        """Single pixel change in 800-pixel RGB frame: output = 2400 raw bytes."""
        prev = b"\x00" * 2400
        cur = bytearray(prev)
        cur[600] = 0xFF
        cur[601] = 0x80
        cur[602] = 0x40
        payload, comp = compress_delta_only(bytes(cur), prev)
        assert comp == COMP_DELTA_ONLY
        assert len(payload) == 2400

    def test_no_prev_falls_back_to_none(self) -> None:
        """No previous frame: returns raw with COMP_NONE."""
        cur = b"\xAA" * 300
        payload, comp = compress_delta_only(cur, b"")
        assert comp == COMP_NONE
        assert payload == cur

    def test_delta_only_ge_delta_rle(self) -> None:
        """For 10 random frame pairs: delta-only >= delta+RLE in size."""
        rng = random.Random(0xD0)
        for i in range(10):
            n = rng.randint(100, 2000) * 3
            prev = bytes(rng.getrandbits(8) for _ in range(n))
            cur = bytearray(prev)
            for _ in range(rng.randint(1, 20)):
                cur[rng.randint(0, n - 1)] ^= 0xFF
            cur = bytes(cur)

            do_payload, _ = compress_delta_only(cur, prev)
            dr_payload, _ = compress_adaptive(cur, prev)
            assert len(do_payload) >= len(dr_payload), (
                f"iter {i}: delta-only ({len(do_payload)}) < "
                f"delta+RLE ({len(dr_payload)})"
            )

    def test_roundtrip_xor_decode(self) -> None:
        """XOR decode of delta-only output recovers original frame."""
        prev = bytes(range(256)) * 3
        cur = bytearray(prev)
        cur[0] = 0xFF
        cur[100] = 0x42
        cur[500] = 0x00
        cur = bytes(cur)

        payload, comp = compress_delta_only(cur, prev)
        assert comp == COMP_DELTA_ONLY
        recovered = xor_delta(payload, prev)
        assert recovered == cur


# ── 15. Planar RLE ─────────────────────────────────────────────────

def _rainbow_800() -> bytes:
    """800-pixel rainbow gradient as interleaved RGB."""
    data = bytearray(800 * 3)
    for i in range(800):
        h = (i / 800) * 6
        c = int(h)
        f = h - c
        q = int(255 * (1 - f))
        t = int(255 * f)
        r, g, b = [
            (255, t, 0), (q, 255, 0), (0, 255, t),
            (0, q, 255), (t, 0, 255), (255, 0, q),
        ][c % 6]
        data[i * 3 : i * 3 + 3] = bytes([r, g, b])
    return bytes(data)


def _planar_roundtrip(data: bytes, channels: int = 3) -> None:
    enc = rle_planar_encode(data, channels)
    dec = rle_planar_decode(enc, channels)
    assert dec == data, (
        f"planar roundtrip failed: len(src)={len(data)}, "
        f"len(enc)={len(enc)}, len(dec)={len(dec)}"
    )


class TestRLEPlanar:
    def test_empty_roundtrip(self) -> None:
        _planar_roundtrip(b"")

    def test_single_pixel_roundtrip(self) -> None:
        _planar_roundtrip(b"\xFF\x80\x40")

    def test_solid_red_800px(self) -> None:
        """Solid red: R=255 run, G=0 run, B=0 run -- each plane ~2 bytes."""
        src = b"\xFF\x00\x00" * 800
        enc = rle_planar_encode(src)
        dec = rle_planar_decode(enc)
        assert dec == src
        # 800 bytes of one value -> ~14 bytes RLE (128-byte runs).
        # 3 planes * ~14 + 6-byte header = ~48 bytes max.
        assert len(enc) < 60, f"solid red too large: {len(enc)}"

    def test_rainbow_planar_beats_byte_rle(self) -> None:
        """Planar RLE must beat byte-level RLE on a rainbow gradient."""
        raw = _rainbow_800()
        byte_enc = rle_encode(raw)
        planar_enc = rle_planar_encode(raw)
        _planar_roundtrip(raw)
        assert len(planar_enc) < len(byte_enc), (
            f"planar ({len(planar_enc)}) must beat byte-level ({len(byte_enc)})"
        )

    def test_smooth_gradient_plane_sizes(self) -> None:
        """R ramps, G=128 constant, B=64 constant.

        G and B planes compress to small runs; R plane is large (no runs).
        """
        pixels = 800
        data = bytearray(pixels * 3)
        for i in range(pixels):
            data[i * 3] = i % 256
            data[i * 3 + 1] = 128
            data[i * 3 + 2] = 64
        src = bytes(data)
        enc = rle_planar_encode(src)
        dec = rle_planar_decode(enc)
        assert dec == src

        import struct
        r_len = struct.unpack_from("<H", enc, 0)[0]
        g_len = struct.unpack_from("<H", enc, 2 + r_len)[0]
        b_len = struct.unpack_from("<H", enc, 2 + r_len + 2 + g_len)[0]
        # Constant planes: 800 identical bytes -> ~14 bytes RLE
        assert g_len < 20, f"G plane too large: {g_len}"
        assert b_len < 20, f"B plane too large: {b_len}"
        # R ramp has no runs, mostly literals
        assert r_len > 700, f"R plane suspiciously small: {r_len}"

    def test_random_roundtrip_100(self) -> None:
        """100 random 3-byte-aligned buffers must roundtrip."""
        rng = random.Random(0xA1)
        for _ in range(100):
            n_pixels = rng.randint(0, 500)
            data = bytes(rng.getrandbits(8) for _ in range(n_pixels * 3))
            _planar_roundtrip(data)

    def test_rgbw_roundtrip(self) -> None:
        """4-channel (RGBW) planar roundtrip."""
        src = b"\xFF\x00\x80\x40" * 200
        _planar_roundtrip(src, channels=4)
        enc = rle_planar_encode(src, channels=4)
        # 4 planes * 2-byte header = 8 bytes header
        assert enc[:2] != b"\x00\x00", "R plane must have data"

    def test_worst_case_expansion(self) -> None:
        """Random 800px: encoded size < raw * 1.02 + header."""
        rng = random.Random(0xBC)
        for _ in range(20):
            n_pixels = 800
            raw = bytes(rng.getrandbits(8) for _ in range(n_pixels * 3))
            enc = rle_planar_encode(raw)
            _planar_roundtrip(raw)
            limit = len(raw) * 1.02 + 6
            assert len(enc) < limit, (
                f"encoded {len(enc)} >= limit {limit:.0f}"
            )


# ── Tuple-level RLE ────────────────────────────────────────────────

class TestRLETuple:

    def test_empty_roundtrip(self) -> None:
        assert rle_tuple_encode(b"") == b""
        assert rle_tuple_decode(b"") == b""
        _tuple_roundtrip(b"")

    def test_single_pixel(self) -> None:
        _tuple_roundtrip(b"\xFF\x00\x00")

    def test_run_of_3(self) -> None:
        src = b"\xFF\x00\x00" * 3
        enc = rle_tuple_encode(src)
        # ctrl = 3-1 = 0x02, then one RGB tuple
        assert enc == b"\x02\xFF\x00\x00"
        assert len(enc) == 4
        _tuple_roundtrip(src)

    def test_run_of_128(self) -> None:
        src = b"\xFF\x00\x00" * 128
        enc = rle_tuple_encode(src)
        assert enc == b"\x7F\xFF\x00\x00"
        assert len(enc) == 4
        _tuple_roundtrip(src)

    def test_run_of_129_splits(self) -> None:
        src = b"\xFF\x00\x00" * 129
        enc = rle_tuple_encode(src)
        _tuple_roundtrip(src)
        # 128 + 1 = two tokens (RUN of 128 + LITERAL of 1), each 4 bytes
        assert len(enc) == 8

    def test_alternating_worst_case(self) -> None:
        src = (b"\xFF\x00\x00" + b"\x00\xFF\x00") * 400  # 800 pixels
        enc = rle_tuple_encode(src)
        _tuple_roundtrip(src)
        overhead = (len(enc) - len(src)) / len(src)
        assert overhead < 0.015

    def test_mixed_roundtrip(self) -> None:
        src = (
            b"\x11\x22\x33" * 10
            + b"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
            + b"\xAA\xBB\xCC" * 20
        )
        _tuple_roundtrip(src)

    def test_100_random_roundtrips(self) -> None:
        rng = random.Random(0xBEEF)
        for _ in range(100):
            n_pixels = rng.randint(1, 800)
            data = bytes(rng.getrandbits(8) for _ in range(n_pixels * 3))
            _tuple_roundtrip(data)

    def test_rgbw_roundtrip(self) -> None:
        rng = random.Random(0xABCD)
        for _ in range(20):
            n_pixels = rng.randint(1, 200)
            data = bytes(rng.getrandbits(8) for _ in range(n_pixels * 4))
            _tuple_roundtrip(data, channels=4)

    def test_solid_red_800px(self) -> None:
        src = b"\xFF\x00\x00" * 800  # 2400 bytes
        enc = rle_tuple_encode(src)
        _tuple_roundtrip(src)
        # 800 / 128 = 7 RUN tokens (6x128 + 1x32), each 1 ctrl + 3 data = 4 bytes
        assert len(enc) == 28

    def test_worst_case_expansion(self) -> None:
        rng = random.Random(0xDEAD)
        src = bytes(rng.getrandbits(8) for _ in range(800 * 3))
        enc = rle_tuple_encode(src)
        _tuple_roundtrip(src)
        assert len(enc) < len(src) * 1.015
