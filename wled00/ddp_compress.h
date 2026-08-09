#pragma once
#ifdef WLED_ENABLE_DDP_COMPRESSION

#include <stdint.h>
#include <stddef.h>

// Byte-level RLE (PackBits-inspired)
// Control byte bit 7 = 0: RUN  — next byte repeated (ctrl & 0x7F)+1 times (1-128)
// Control byte bit 7 = 1: LITERAL — next (ctrl & 0x7F)+1 bytes copied verbatim (1-128)

// Worst-case expansion: rawLen + ceil(rawLen/128) bytes (< 0.8% overhead)
inline size_t rle_max_encoded_size(size_t rawLen) {
  return rawLen + (rawLen >> 7) + 2;
}

// Encode `src[0..srcLen-1]` into `dst`, return encoded length via `outLen`.
// Caller must ensure dst has at least rle_max_encoded_size(srcLen) bytes.
// Returns false if srcLen == 0.
inline bool rle_encode(const uint8_t *src, size_t srcLen,
                       uint8_t *dst, size_t *outLen) {
  if (!srcLen) { *outLen = 0; return false; }

  size_t si = 0, di = 0;

  while (si < srcLen) {
    uint8_t cur = src[si];

    // count run length
    size_t runLen = 1;
    while (si + runLen < srcLen && src[si + runLen] == cur && runLen < 128)
      runLen++;

    if (runLen >= 3) {
      // emit RUN: control byte (bit7=0) + value byte
      dst[di++] = (uint8_t)(runLen - 1);        // 0x00..0x7F = 1..128 repeats
      dst[di++] = cur;
      si += runLen;
    } else {
      // accumulate literal span
      size_t litStart = si;
      size_t litLen = 0;
      while (si < srcLen && litLen < 128) {
        // peek ahead: if next 3+ bytes are a run, stop literal here
        size_t ahead = 1;
        if (si + 1 < srcLen) {
          while (si + ahead < srcLen && src[si + ahead] == src[si] && ahead < 3)
            ahead++;
        }
        if (ahead >= 3) break;
        si++;
        litLen++;
      }
      if (litLen > 0) {
        dst[di++] = (uint8_t)(0x80 | (litLen - 1)); // 0x80..0xFF = 1..128 literals
        for (size_t j = 0; j < litLen; j++)
          dst[di++] = src[litStart + j];
      }
    }
  }

  *outLen = di;
  return true;
}

// Streaming RLE decoder state — decodes one byte at a time without buffering
// the full decompressed output. Used by the ESP32 receiver for zero-copy decode.
struct RLEDecoder {
  const uint8_t *src;
  size_t srcLen;
  size_t pos;       // current position in src
  size_t remaining; // bytes left in current run/literal span
  uint8_t value;    // current run value (only meaningful during a run)
  bool isRun;       // true = repeating `value`, false = reading literals

  void init(const uint8_t *data, size_t len) {
    src = data; srcLen = len; pos = 0; remaining = 0;
  }

  // Decode next byte. Returns false when input exhausted.
  bool next(uint8_t *out) {
    if (remaining > 0) {
      remaining--;
      if (isRun) {
        *out = value;
      } else {
        if (pos >= srcLen) return false;
        *out = src[pos++];
      }
      return true;
    }
    // read next control byte
    if (pos >= srcLen) return false;
    uint8_t ctrl = src[pos++];
    size_t count = (ctrl & 0x7F) + 1;
    if (ctrl & 0x80) {
      isRun = false;
      remaining = count - 1;
      if (pos >= srcLen) return false;
      *out = src[pos++];
    } else {
      isRun = true;
      if (pos >= srcLen) return false;
      value = src[pos++];
      remaining = count - 1;
      *out = value;
    }
    return true;
  }
};

#endif // WLED_ENABLE_DDP_COMPRESSION
