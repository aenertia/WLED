#pragma once
#ifdef WLED_ENABLE_ARGB_PASSTHROUGH

#ifndef ARGB_RX_PIN
#define ARGB_RX_PIN 32
#endif
#ifndef ARGB_MAX_LEDS
#define ARGB_MAX_LEDS 100
#endif

typedef enum {
    ARGB_MODE_RELAY = 0,    // mirror input to LEDs, no segment mapping
    ARGB_MODE_DECODE = 1    // full decode with segment mapping + mutations
} argb_capture_mode_t;

void initARGBPassthrough();
void handleARGBPassthrough();
void startARGBPassthrough();
void stopARGBPassthrough();
bool isARGBPassthroughActive();
void setARGBCaptureMode(argb_capture_mode_t mode);
argb_capture_mode_t getARGBCaptureMode();
uint16_t getARGBCapturedLedCount();
void setARGBBootDelay(uint16_t ms);
uint16_t getARGBBootDelay();

#define ARGB_MAX_SEGMENTS 8

typedef struct {
    uint16_t start;      // first LED index in captured stream
    uint16_t count;      // number of LEDs in this segment
    uint8_t  wled_seg;   // WLED segment ID to map to (255 = unmapped)
    char     label[16];  // human-readable label
} argb_segment_t;

uint8_t getARGBSegmentCount();
const argb_segment_t* getARGBSegments();
void setARGBSegment(uint8_t idx, uint16_t start, uint16_t count, uint8_t wled_seg, const char* label);
void clearARGBSegments();

#endif
