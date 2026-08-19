#pragma once
#ifdef WLED_ENABLE_SPI_MATRIX

#include <TFT_eSPI.h>
#include <Wire.h>

#ifndef SPI_MATRIX_W
#define SPI_MATRIX_W 40
#endif
#ifndef SPI_MATRIX_H
#define SPI_MATRIX_H 80
#endif

static TFT_eSPI *_spiDisplay = nullptr;

#endif // WLED_ENABLE_SPI_MATRIX
