"""Tests for DDP packet construction and RGBW support."""
import struct
import sys
import os

# Add tools/ to path so we can import ddp_bench
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ddp_bench import (
    DDP_RGB, DDP_TYPE_RGBW32, DDP_VER1, DDP_PUSH,
    COMP_NONE, COMP_DELTA_RLE, COMP_RLE,
    make_packets, compress_adaptive, rle_encode,
    rainbow, rainbow_rgbw, solid_rgbw,
    diagnostic_pattern, diagnostic_pattern_rgbw,
    solid_pulse, solid_pulse_rgbw,
    sparse_twinkle, sparse_twinkle_rgbw,
)


# --- RGB baseline tests (ensure no regression) ---

class TestRGBBaseline:
    def test_rgb_packet_data_type(self):
        """Header byte 2 == 0x0B for RGB packets."""
        data = rainbow(10, 0.0)
        pkts = make_packets(data, seq=1, data_type=DDP_RGB)
        assert len(pkts) >= 1
        hdr = pkts[0][:10]
        assert hdr[2] == DDP_RGB  # data type byte

    def test_rgb_pattern_length(self):
        """rainbow(100) produces exactly 300 bytes (3 bytes/pixel)."""
        data = rainbow(100, 0.5)
        assert len(data) == 300

    def test_rgb_default_data_type(self):
        """make_packets() defaults to DDP_RGB when data_type not specified."""
        data = bytes([255, 0, 0] * 5)
        pkts = make_packets(data, seq=1)
        hdr = pkts[0][:10]
        assert hdr[2] == DDP_RGB


# --- RGBW packet construction ---

class TestRGBWPackets:
    def test_rgbw_packet_data_type(self):
        """Header byte 2 == 0x1B for RGBW packets."""
        data = rainbow_rgbw(10, 0.0)
        pkts = make_packets(data, seq=1, data_type=DDP_TYPE_RGBW32)
        assert len(pkts) >= 1
        hdr = pkts[0][:10]
        assert hdr[2] == DDP_TYPE_RGBW32

    def test_rgbw_packet_data_type_value(self):
        """DDP_TYPE_RGBW32 encodes 4 channels: bits[5:3] == 0b011."""
        # Receiver checks: (dataType & 0b00111000) >> 3 == 0b011
        channels_field = (DDP_TYPE_RGBW32 & 0b00111000) >> 3
        assert channels_field == 0b011  # means 4 channels

    def test_rgbw_packet_preserves_flags(self):
        """RGBW packets still set VER1 and PUSH flags correctly."""
        data = solid_rgbw(5, 255, 0, 0, 128)
        pkts = make_packets(data, seq=1, push=True, data_type=DDP_TYPE_RGBW32)
        flags = pkts[-1][0]
        assert flags & DDP_VER1
        assert flags & DDP_PUSH

    def test_rgbw_multi_packet_offset(self):
        """Large RGBW data splits across packets with correct offsets."""
        n = 500  # 2000 bytes, needs 2 packets at 1440 max
        data = solid_rgbw(n, 100, 150, 200, 50)
        assert len(data) == 2000
        pkts = make_packets(data, seq=1, data_type=DDP_TYPE_RGBW32)
        assert len(pkts) == 2
        # First packet: offset 0
        _, _, _, _, off1, chunk1 = struct.unpack("!BBBBIH", pkts[0][:10])
        assert off1 == 0
        assert chunk1 == 1440
        # Second packet: offset 1440
        _, _, _, _, off2, chunk2 = struct.unpack("!BBBBIH", pkts[1][:10])
        assert off2 == 1440
        assert chunk2 == 560

    def test_rgbw_compressed_packet_data_type(self):
        """Compressed RGBW packets still carry 0x1B data type."""
        data = solid_rgbw(50, 128, 128, 128, 64)
        pkts = make_packets(data, seq=1, comp=COMP_RLE, data_type=DDP_TYPE_RGBW32)
        hdr = pkts[0][:10]
        assert hdr[2] == DDP_TYPE_RGBW32


# --- RGBW pattern generators ---

class TestRGBWPatterns:
    def test_rgbw_pattern_length(self):
        """rainbow_rgbw(100) produces exactly 400 bytes (4 bytes/pixel)."""
        data = rainbow_rgbw(100, 0.5)
        assert len(data) == 400

    def test_solid_rgbw_length(self):
        """solid_rgbw(50) produces exactly 200 bytes."""
        data = solid_rgbw(50, 255, 128, 0, 64)
        assert len(data) == 200

    def test_solid_rgbw_values(self):
        """solid_rgbw fills every pixel with the same RGBW values."""
        data = solid_rgbw(3, 10, 20, 30, 40)
        assert data == bytes([10, 20, 30, 40] * 3)

    def test_diagnostic_pattern_rgbw_length(self):
        """diagnostic_pattern_rgbw(100, 10) produces 400 bytes."""
        data = diagnostic_pattern_rgbw(100, 10)
        assert len(data) == 400

    def test_diagnostic_pattern_rgbw_markers(self):
        """First pixel is red+W, last pixel is white+fullW."""
        data = diagnostic_pattern_rgbw(100, 10)
        # Pixel 0: R=255, G=0, B=0, W=64
        assert data[0] == 255
        assert data[1] == 0
        assert data[2] == 0
        assert data[3] == 64
        # Last pixel: R=255, G=255, B=255, W=255
        assert data[-4] == 255
        assert data[-3] == 255
        assert data[-2] == 255
        assert data[-1] == 255

    def test_rainbow_rgbw_has_nonzero_w(self):
        """rainbow_rgbw produces non-zero W channel values."""
        data = rainbow_rgbw(100, 0.5)
        w_values = [data[i*4+3] for i in range(100)]
        assert any(w > 0 for w in w_values), "W channel should have non-zero values"

    def test_solid_pulse_rgbw_length(self):
        """solid_pulse_rgbw produces 4 bytes per pixel."""
        data = solid_pulse_rgbw(50, 0.25)
        assert len(data) == 200

    def test_sparse_twinkle_rgbw_length(self):
        """sparse_twinkle_rgbw produces 4 bytes per pixel."""
        data = sparse_twinkle_rgbw(50, 0.1)
        assert len(data) == 200


# --- Delta/RLE compression with RGBW ---

class TestRGBWCompression:
    def test_rgbw_delta_preserves_w(self):
        """Delta encode/decode roundtrip preserves the W channel."""
        frame1 = solid_rgbw(10, 100, 150, 200, 80)
        frame2 = bytearray(solid_rgbw(10, 100, 150, 200, 80))
        # Change W on pixel 3 only
        frame2[3*4+3] = 160
        frame2 = bytes(frame2)

        # Delta XOR
        delta = bytes(a ^ b for a, b in zip(frame2, frame1))
        # Only byte at index 15 (pixel 3, W channel) should be non-zero
        assert delta[3*4+3] == (160 ^ 80)
        # All other bytes should be zero
        for i in range(len(delta)):
            if i != 3*4+3:
                assert delta[i] == 0, f"byte {i} should be 0, got {delta[i]}"

        # Reconstruct frame2 from frame1 + delta
        reconstructed = bytes(a ^ b for a, b in zip(delta, frame1))
        assert reconstructed == frame2

    def test_rgbw_delta_rle_compression(self):
        """compress_adaptive with RGBW prev frame picks delta+RLE for similar frames."""
        frame1 = solid_rgbw(100, 128, 128, 128, 64)
        frame2 = bytearray(solid_rgbw(100, 128, 128, 128, 64))
        frame2[0] = 129  # tiny change
        frame2 = bytes(frame2)

        compressed, comp_type = compress_adaptive(frame2, prev=frame1)
        # Delta of nearly-identical frames should compress very well
        assert len(compressed) < len(frame2)
        # Should pick delta+RLE since frames are almost identical
        assert comp_type == COMP_DELTA_RLE

    def test_rgbw_rle_uniform_channel(self):
        """Solid RGBW with uniform channels compresses with RLE."""
        # All 4 channels same value = long byte runs = good RLE
        data = solid_rgbw(100, 64, 64, 64, 64)
        compressed, comp_type = compress_adaptive(data)
        assert len(compressed) < len(data)






    def test_rgbw_roundtrip_delta_full(self):
        """Full delta roundtrip: two different RGBW rainbow frames."""
        frame1 = rainbow_rgbw(50, 0.0)
        frame2 = rainbow_rgbw(50, 0.1)

        delta = bytes(a ^ b for a, b in zip(frame2, frame1))
        reconstructed = bytes(a ^ b for a, b in zip(delta, frame1))
        assert reconstructed == frame2
        # Verify W channel specifically
        for i in range(50):
            assert reconstructed[i*4+3] == frame2[i*4+3]


# --- Constant validation ---

class TestConstants:
    def test_ddp_rgb_value(self):
        assert DDP_RGB == 0x0B

    def test_ddp_rgbw32_value(self):
        assert DDP_TYPE_RGBW32 == 0x1B

    def test_rgb_vs_rgbw_differ(self):
        """RGB and RGBW data type constants are different."""
        assert DDP_RGB != DDP_TYPE_RGBW32

    def test_rgbw_channel_encoding(self):
        """RGBW type encodes 4 channels (bits[5:3] == 3) vs RGB 2 channels field (bits[5:3] == 1)."""
        rgb_channels = (DDP_RGB & 0b00111000) >> 3
        rgbw_channels = (DDP_TYPE_RGBW32 & 0b00111000) >> 3
        assert rgb_channels == 1   # RGB: 3 channels encoded as 1
        assert rgbw_channels == 3  # RGBW: 4 channels encoded as 3
