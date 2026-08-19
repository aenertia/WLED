#pragma once
#ifdef WLED_ENABLE_SPI_MATRIX

#include <TFT_eSPI.h>
#include <Wire.h>

#ifndef SPI_MATRIX_W
  #error "SPI_MATRIX_W must be defined — set virtual panel width via build flag (e.g. -D SPI_MATRIX_W=40)"
#endif
#ifndef SPI_MATRIX_H
  #error "SPI_MATRIX_H must be defined — set virtual panel height via build flag (e.g. -D SPI_MATRIX_H=80)"
#endif

// Compile-time check: SPI_MATRIX_W must evenly divide TFT_WIDTH for full panel coverage.
// Non-integer scale is safe (no crash) but leaves dead pixels at right/bottom edges.
#if defined(TFT_WIDTH) && (TFT_WIDTH % SPI_MATRIX_W) != 0
  #warning "SPI_MATRIX_W does not evenly divide TFT_WIDTH — right edge will have dead pixels"
#endif
#if defined(TFT_HEIGHT) && (TFT_HEIGHT % SPI_MATRIX_H) != 0
  #warning "SPI_MATRIX_H does not evenly divide TFT_HEIGHT — bottom edge will have dead pixels"
#endif

// Integer scale configurations (SPI_MATRIX_W must evenly divide TFT_WIDTH,
// SPI_MATRIX_H must evenly divide TFT_HEIGHT for full panel coverage):
//
// Small embedded:
//   M5StickC ST7735S       80x160   W=40  H=80   2x2
//   M5StickC+ ST7789V2    135x240   W=45  H=80   3x3
//   SSD1351 1.5" OLED     128x128   W=32  H=32   4x4
//
// Dev boards:
//   TTGO T-Display ST7789      135x240   W=27  H=48   5x5
//   LilyGO T-Display-S3       170x320   W=34  H=64   5x5
//   ILI9341 2.8" / CYD        240x320   W=40  H=80   6x4
//   ST7789 1.3" square        240x240   W=40  H=40   6x6
//
// Pi-compatible SPI (3.5-5"):
//   ILI9486/ILI9488 3.5"      320x480   W=40  H=60   8x8
//   ILI9486/ILI9488 3.5"      320x480   W=64  H=80   5x6
//   ST7796 4"                 320x480   W=80  H=120  4x4
//   ST7796 4"                 320x480   W=40  H=60   8x8
//   SSD1963 5"                480x800   W=60  H=100  8x8
//   SSD1963 5"                480x800   W=80  H=100  6x8
//
// Non-integer scale is safe (no crash) but leaves dead pixels at
// right/bottom edges. The bus renders (W*scale) x (H*scale) physical
// pixels; any remainder is black.

static TFT_eSPI *_spiDisplay = nullptr;

#endif // WLED_ENABLE_SPI_MATRIX
