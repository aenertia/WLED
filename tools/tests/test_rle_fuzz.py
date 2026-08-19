"""Fuzz test harness for the DDP RLE decoder (tools/ddp_codec.py).

Generates malformed / adversarial DDP-RLE byte sequences and validates
the Python reference decoder handles them safely  -- no crashes, no
unhandled exceptions, bounded output.
"""

from __future__ import annotations

import os
import random
import sys

import pytest

# Ensure tools/ is importable regardless of cwd.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ddp_codec import rle_decode, rle_encode


# Fixed seed for reproducibility across runs.
FUZZ_SEED = 0xDEAD_BEEF
FUZZ_ITERATIONS = 1000


# ── 1. Random bytes ─────────────────────────────────────────────────

class TestFuzzRandomBytes:
    """Feed random byte sequences to rle_decode; it must never crash."""

    def test_fuzz_random_bytes(self) -> None:
        rng = random.Random(FUZZ_SEED)
        for i in range(FUZZ_ITERATIONS):
            length = rng.randint(1, 256)
            data = bytes(rng.randint(0, 255) for _ in range(length))
            try:
                result = rle_decode(data)
            except Exception as exc:
                pytest.fail(
                    f"rle_decode crashed on iteration {i}, "
                    f"input len={length}: {exc}"
                )
            # Worst case: every byte is a RUN control 0x7F (128 repeats)
            # followed by a value byte  -> 128 output bytes per 2 input bytes.
            # So max decoded <= len(data) * 128.
            assert len(result) <= len(data) * 128, (
                f"decoded output unbounded: {len(result)} bytes "
                f"from {len(data)} byte input"
            )


# ── 2. Extreme run lengths ──────────────────────────────────────────

class TestFuzzExtremeRunLengths:
    """Control byte 0x7F = max RUN (128 repeats of next byte)."""

    @pytest.mark.parametrize("value", [0x00, 0x42, 0xFF])
    def test_fuzz_extreme_run_lengths(self, value: int) -> None:
        # 0x7F  -> (0x7F & 0x7F)+1 = 128 repeats
        encoded = bytes([0x7F, value])
        decoded = rle_decode(encoded)
        assert decoded == bytes([value]) * 128
        assert len(decoded) == 128


# ── 3. Extreme literal counts (truncated input) ────────────────────

class TestFuzzExtremeLiteralCounts:
    """Control byte 0xFF = 128 literals, but supply fewer bytes."""

    @pytest.mark.parametrize("available", [0, 1, 10, 64, 127])
    def test_fuzz_extreme_literal_counts(self, available: int) -> None:
        # 0xFF  -> LITERAL, expects 128 bytes following
        payload = bytes(range(available % 256)) * (available // 256 + 1)
        payload = payload[:available]
        encoded = bytes([0xFF]) + payload
        try:
            decoded = rle_decode(encoded)
        except Exception as exc:
            pytest.fail(
                f"rle_decode crashed on truncated literal "
                f"(available={available}): {exc}"
            )
        # Decoder should return whatever bytes were available (partial)
        assert len(decoded) <= 128, (
            f"decoded {len(decoded)} bytes from truncated 128-literal block"
        )


# ── 4. Zero-length input ───────────────────────────────────────────

class TestFuzzZeroLength:
    def test_fuzz_zero_length_input(self) -> None:
        decoded = rle_decode(b"")
        assert decoded == b""


# ── 5. Single control byte (no value/data follows) ─────────────────

class TestFuzzSingleControlByte:
    """A lone control byte with no payload  -- decoder must not crash."""

    @pytest.mark.parametrize("ctrl", [0x00, 0x7F, 0x80, 0xFF])
    def test_fuzz_single_control_byte(self, ctrl: int) -> None:
        try:
            decoded = rle_decode(bytes([ctrl]))
        except Exception as exc:
            pytest.fail(
                f"rle_decode crashed on single control byte 0x{ctrl:02X}: {exc}"
            )
        # RUN with no value byte  -> should produce empty or gracefully stop.
        # LITERAL with no data bytes  -> should produce empty or partial.
        assert len(decoded) <= 128, (
            f"single control byte 0x{ctrl:02X} produced {len(decoded)} bytes"
        )

    def test_run_control_no_value_produces_empty(self) -> None:
        """RUN control (bit7=0) with no following value byte  -> empty output."""
        decoded = rle_decode(bytes([0x00]))  # RUN of 1, but no value byte
        assert decoded == b""

    def test_literal_control_no_data_produces_empty(self) -> None:
        """LITERAL control (bit7=1) with no following data  -> empty output."""
        decoded = rle_decode(bytes([0x80]))  # LITERAL of 1, but no data
        assert decoded == b""


# ── 6. Amplification attack ────────────────────────────────────────

class TestFuzzAmplificationAttack:
    """128 consecutive max-RUN pairs: 256 bytes in  -> 16384 bytes out."""

    def test_fuzz_amplification_attack(self) -> None:
        # Each pair: 0x7F (128-repeat RUN) + 0xAA (value)
        num_runs = 128
        encoded = bytes([0x7F, 0xAA] * num_runs)
        assert len(encoded) == 256

        decoded = rle_decode(encoded)
        expected_len = 128 * num_runs  # 16384
        assert len(decoded) == expected_len, (
            f"expected {expected_len} bytes, got {len(decoded)}"
        )
        assert decoded == bytes([0xAA]) * expected_len

    def test_amplification_ratio(self) -> None:
        """Verify the amplification ratio is exactly 64:1."""
        encoded = bytes([0x7F, 0xBB] * 128)
        decoded = rle_decode(encoded)
        ratio = len(decoded) / len(encoded)
        assert ratio == 64.0, f"amplification ratio {ratio}, expected 64.0"


# ── 7. channelOffset overflow (protocol constraint) ─────────────────

class TestFuzzChannelOffsetOverflow:
    """Validate that channelOffset >= totalLen is logically invalid.

    This tests the protocol constraint on the Python sender side:
    make_packets() should never produce a packet whose offset exceeds
    the data length.
    """

    def test_fuzz_channelOffset_overflow(self) -> None:
        from ddp_codec import make_packets

        data = bytes([0xFF] * 300)  # 300 bytes of pixel data
        packets, _ = make_packets(data)

        import struct
        for pkt in packets:
            # DDP header: flags(1) seq(1) dataType(1) id(1) offset(4) len(2)
            offset = struct.unpack("!I", pkt[4:8])[0]
            payload_len = struct.unpack("!H", pkt[8:10])[0]
            assert offset < len(data), (
                f"channelOffset {offset} >= totalLen {len(data)}"
            )
            assert offset + payload_len <= len(data) + payload_len, (
                f"offset({offset}) + payload_len({payload_len}) overflows"
            )

    def test_offset_never_negative(self) -> None:
        """Offsets in generated packets are always non-negative (uint32)."""
        from ddp_codec import make_packets
        import struct

        data = bytes(range(256)) * 4  # 1024 bytes
        packets, _ = make_packets(data)
        for pkt in packets:
            offset = struct.unpack("!I", pkt[4:8])[0]
            assert offset >= 0


# ── 8. Roundtrip stability under fuzz ───────────────────────────────

class TestFuzzRoundtripStability:
    """Encode random data, then decode  -- result must match original."""

    def test_fuzz_roundtrip_random(self) -> None:
        rng = random.Random(FUZZ_SEED + 1)
        for i in range(FUZZ_ITERATIONS):
            length = rng.randint(0, 512)
            data = bytes(rng.randint(0, 255) for _ in range(length))
            encoded = rle_encode(data)
            decoded = rle_decode(encoded)
            assert decoded == data, (
                f"roundtrip mismatch on iteration {i}, len={length}"
            )
