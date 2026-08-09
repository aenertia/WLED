#pragma once
#ifdef WLED_ENABLE_TFT_MATRIX

#include <TFT_eSPI.h>
#include <Wire.h>

#ifndef TFT_VIRTUAL_W
#define TFT_VIRTUAL_W 20
#endif
#ifndef TFT_VIRTUAL_H
#define TFT_VIRTUAL_H 40
#endif

static TFT_eSPI *_tft_instance = nullptr;

#endif // WLED_ENABLE_TFT_MATRIX
