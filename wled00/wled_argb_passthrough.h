#pragma once
#ifdef WLED_ENABLE_ARGB_PASSTHROUGH

#ifndef ARGB_RX_PIN
#define ARGB_RX_PIN 32  // Grove yellow — M5StickC
#endif
#ifndef ARGB_MAX_LEDS
#define ARGB_MAX_LEDS 100
#endif

void initARGBPassthrough();
void handleARGBPassthrough();   // call from loop()
void startARGBPassthrough();    // engage passthrough (realtimeLock)
void stopARGBPassthrough();     // release to WLED (exitRealtime)
bool isARGBPassthroughActive();

#endif // WLED_ENABLE_ARGB_PASSTHROUGH
