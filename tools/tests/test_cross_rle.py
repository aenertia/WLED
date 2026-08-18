"""Cross-implementation RLE verification.

Ports the C ``RLEDecoder`` (streaming, stateful, one-byte-at-a-time) from
``wled00/ddp_compress.h`` to pure Python and verifies byte-exact match with
the batch ``rle_decode()`` in ``tools/ddp_codec.py`` across 1000+ random inputs.
"""

from __future__ import annotations

import os
import random

import pytest

# ── Faithful port of the C RLEDecoder struct ────────────────────────


class CRLEDecoder:
    """Line-by-line port of ``struct RLEDecoder`` from ddp_compress.h."""

    __slots__ = ("src", "src_len", "pos", "remaining", "value", "is_run")

    def init(self, data: bytes | bytearray, length: int) -> None:
        self.src = data
        self.src_len = length
        self.pos = 0
        self.remaining = 0
        # These mirror uninitialised C fields; values don't matter until set.
        self.value = 0
        self.is_run = False

    def next(self) -> tuple[bool, int]:
        """Decode next byte.  Returns ``(ok, byte_value)``.

        Mirrors the C ``bool next(uint8_t *out)`` method exactly.
        """
        if self.remaining > 0:
            self.remaining -= 1
            if self.is_run:
                return True, self.value
            else:
                if self.pos >= self.src_len:
                    return False, 0
                out = self.src[self.pos]
                self.pos += 1
                return True, out
            # unreachable

        # read next control byte
        if self.pos >= self.src_len:
            return False, 0
        ctrl = self.src[self.pos]
        self.pos += 1
        count = (ctrl & 0x7F) + 1

        if ctrl & 0x80:
            # LITERAL
            self.is_run = False
            self.remaining = count - 1
            if self.pos >= self.src_len:
                return False, 0
            out = self.src[self.pos]
            self.pos += 1
            return True, out
        else:
            # RUN
            self.is_run = True
            if self.pos >= self.src_len:
                return False, 0
            self.value = self.src[self.pos]
            self.pos += 1
            self.remaining = count - 1
            return True, self.value


def c_rle_decode(encoded: bytes | bytearray) -> bytes:
    """Decode using the C-ported streaming decoder, byte by byte."""
    dec = CRLEDecoder()
    dec.init(encoded, len(encoded))
    result = bytearray()
    while True:
        ok, val = dec.next()
        if not ok:
            break
        result.append(val)
    return bytes(result)


# ── Import the Python batch decoder & encoder ──────────────────────

from ddp_codec import rle_decode, rle_encode  # noqa: E402


# ── Deterministic tests ────────────────────────────────────────────


class TestCrossRLE:
    """Verify byte-exact match between C-ported and Python decoders."""

    def test_cross_rle_empty(self) -> None:
        encoded = b""
        assert c_rle_decode(encoded) == b""
        assert rle_decode(encoded) == b""

    def test_cross_rle_single_run(self) -> None:
        # ctrl=0x02 → run of (2&0x7F)+1 = 3, value=0xAA
        encoded = b"\x02\xAA"
        expected = b"\xAA\xAA\xAA"
        assert c_rle_decode(encoded) == expected
        assert rle_decode(encoded) == expected

    def test_cross_rle_single_literal(self) -> None:
        # ctrl=0x82 → literal of (0x82&0x7F)+1 = 3, bytes=01 02 03
        encoded = b"\x82\x01\x02\x03"
        expected = b"\x01\x02\x03"
        assert c_rle_decode(encoded) == expected
        assert rle_decode(encoded) == expected

    def test_cross_rle_mixed(self) -> None:
        # Run of 5×0xFF, then literal [0x10, 0x20], then run of 2×0x00
        encoded = (
            b"\x04\xFF"          # run: (4&0x7F)+1=5, value=0xFF
            b"\x81\x10\x20"     # literal: (0x81&0x7F)+1=2, bytes=[0x10,0x20]
            b"\x01\x00"          # run: (1&0x7F)+1=2, value=0x00
        )
        expected = b"\xFF" * 5 + b"\x10\x20" + b"\x00\x00"
        c_result = c_rle_decode(encoded)
        py_result = rle_decode(encoded)
        assert c_result == expected
        assert py_result == expected
        assert c_result == py_result

    def test_cross_rle_boundary_128(self) -> None:
        # Max run length: 128 (ctrl=0x7F → (0x7F&0x7F)+1=128)
        run_128 = b"\x7F\x42"  # 128×0x42
        expected_run = b"\x42" * 128
        assert c_rle_decode(run_128) == expected_run
        assert rle_decode(run_128) == expected_run

        # Max literal length: 128 (ctrl=0xFF → (0xFF&0x7F)+1=128)
        lit_data = bytes(range(128))
        lit_128 = b"\xFF" + lit_data
        assert c_rle_decode(lit_128) == lit_data
        assert rle_decode(lit_128) == lit_data

    def test_cross_rle_all_control_bytes(self) -> None:
        """Exercise every possible control byte value 0x00–0xFF."""
        for ctrl in range(256):
            count = (ctrl & 0x7F) + 1
            if ctrl & 0x80:
                # LITERAL: need `count` data bytes after ctrl
                data_bytes = bytes([i & 0xFF for i in range(count)])
                encoded = bytes([ctrl]) + data_bytes
            else:
                # RUN: need 1 value byte after ctrl
                encoded = bytes([ctrl, 0xBB])

            c_result = c_rle_decode(encoded)
            py_result = rle_decode(encoded)
            assert c_result == py_result, (
                f"ctrl=0x{ctrl:02X}: C={c_result.hex()} != Py={py_result.hex()}"
            )

    def test_cross_rle_random_1000(self) -> None:
        """Encode 1000+ random payloads, decode with both, assert byte-exact."""
        seed = int(os.environ.get("RLE_TEST_SEED", "42"))
        rng = random.Random(seed)
        n_cases = 1200

        for i in range(n_cases):
            length = rng.randint(1, 4096)
            raw = bytes(rng.getrandbits(8) for _ in range(length))
            encoded = rle_encode(raw)

            c_result = c_rle_decode(encoded)
            py_result = rle_decode(encoded)

            assert c_result == py_result, (
                f"case {i}: len={length} seed={seed} "
                f"C({len(c_result)}) != Py({len(py_result)})"
            )
            # Also verify round-trip correctness
            assert c_result == raw, (
                f"case {i}: round-trip failed, len={length} seed={seed}"
            )

    def test_cross_rle_roundtrip_pathological(self) -> None:
        """Pathological patterns: all same byte, alternating, ascending."""
        patterns = [
            b"\x00" * 500,
            b"\xFF" * 500,
            bytes([i % 2 for i in range(500)]),
            bytes([i & 0xFF for i in range(500)]),
            bytes([0xAA, 0xAA, 0xAA, 0xBB] * 100),
            b"\x00",
            b"\xFF",
        ]
        for raw in patterns:
            encoded = rle_encode(raw)
            c_result = c_rle_decode(encoded)
            py_result = rle_decode(encoded)
            assert c_result == py_result == raw
