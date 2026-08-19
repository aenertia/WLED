#ifdef WLED_ENABLE_ARGB_PASSTHROUGH
#include "wled.h"
#include "wled_argb_passthrough.h"
#include "wled_argb_capture.h"

#include "driver/rmt_rx.h"
#include "driver/rmt_common.h"

#define ARGB_RMT_RESOLUTION_HZ 10000000
#define ARGB_RESET_US 50
#define ARGB_SYMBOL_BUF_SIZE (ARGB_MAX_LEDS * 24 + 16)
#define ARGB_EDGE_BUF_SIZE   (ARGB_SYMBOL_BUF_SIZE * 2)
#define ARGB_PIXEL_BUF_SIZE  (ARGB_MAX_LEDS * 3)

#ifndef ARGB_BOOT_DELAY_MS
#define ARGB_BOOT_DELAY_MS 5000
#endif

static rmt_channel_handle_t argb_rx_chan = nullptr;
static rmt_symbol_word_t    argb_symbols[ARGB_SYMBOL_BUF_SIZE]; // HW target  -- must be static (ISR writes)
static rmt_symbol_word_t   *argb_pending = nullptr;     // ISR double buffer  -- heap, allocated on start
static rmt_symbol_word_t   *argb_decode_buf = nullptr;  // decode working copy  -- heap, allocated on start
static volatile bool        argb_frame_ready = false;
static volatile size_t      argb_pending_count = 0;
static bool                 argb_active = false;
static bool                 argb_initialized = false;
static argb_capture_mode_t  argb_mode = ARGB_MODE_RELAY;
static uint16_t             argb_last_led_count = 0;
static rmt_receive_config_t argb_rx_config = {};

static argb_edge_t   *argb_edges = nullptr;   // edge pairs  -- heap, allocated on start
static uint8_t       *argb_pixels = nullptr;  // decoded GRB  -- heap, allocated on start
static argb_timing_t  argb_timing;

static uint32_t       argb_boot_start_ms = 0;
static bool           argb_boot_delay_active = false;
static uint16_t       argb_boot_delay_ms = ARGB_BOOT_DELAY_MS;

static argb_segment_t argb_segments[ARGB_MAX_SEGMENTS];
static uint8_t        argb_segment_count = 0;

static bool IRAM_ATTR argb_rx_done_cb(rmt_channel_handle_t channel,
                                       const rmt_rx_done_event_data_t *edata,
                                       void *user_data)
{
    if (argb_pending && edata->num_symbols > 0 && edata->num_symbols <= ARGB_SYMBOL_BUF_SIZE) {
        memcpy((void *)argb_pending, edata->received_symbols,
               edata->num_symbols * sizeof(rmt_symbol_word_t));
        argb_pending_count = edata->num_symbols;
        argb_frame_ready = true;
    }
    rmt_receive(channel, argb_symbols, sizeof(argb_symbols), &argb_rx_config);
    return false;
}

static void feedPixelsRelay(const uint8_t *grb_data, uint16_t led_count)
{
    uint16_t maxLed = strip.getLengthTotal();
    if (led_count > maxLed) led_count = maxLed;
    for (uint16_t i = 0; i < led_count; i++) {
        uint8_t g = grb_data[i * 3];
        uint8_t r = grb_data[i * 3 + 1];
        uint8_t b = grb_data[i * 3 + 2];
        setRealtimePixel(i, r, g, b, 0);
    }
}

void initARGBPassthrough()
{
    if (argb_initialized) return;

    argb_timing = argb_timing_ws2812b();

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = (gpio_num_t)ARGB_RX_PIN;
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = ARGB_RMT_RESOLUTION_HZ;
    rx_cfg.intr_priority = 0;
    rx_cfg.flags.invert_in = 0;
    rx_cfg.flags.io_loop_back = 0;
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
    rx_cfg.flags.with_dma = 1;
    rx_cfg.mem_block_symbols = 4096;
#else
    rx_cfg.flags.with_dma = 0;
    rx_cfg.mem_block_symbols = 64;
#endif

    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &argb_rx_chan);
    if (err != ESP_OK) {
        DEBUG_PRINTF_P(PSTR("ARGB: RMT RX init failed: %d\n"), err);
        return;
    }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = argb_rx_done_cb;
    rmt_rx_register_event_callbacks(argb_rx_chan, &cbs, nullptr);

    argb_rx_config.signal_range_min_ns = 100;
    argb_rx_config.signal_range_max_ns = ARGB_RESET_US * 1000;

    rmt_enable(argb_rx_chan);
    argb_initialized = true;
    DEBUG_PRINTLN(F("ARGB: capture initialized"));
}

void startARGBPassthrough()
{
    if (!argb_initialized || argb_active) return;
    // Allocate capture buffers on demand (~39KB total)
    if (!argb_pending) {
        argb_pending   = (rmt_symbol_word_t *)malloc(ARGB_SYMBOL_BUF_SIZE * sizeof(rmt_symbol_word_t));
        argb_decode_buf = (rmt_symbol_word_t *)malloc(ARGB_SYMBOL_BUF_SIZE * sizeof(rmt_symbol_word_t));
        argb_edges     = (argb_edge_t *)malloc(ARGB_EDGE_BUF_SIZE * sizeof(argb_edge_t));
        argb_pixels    = (uint8_t *)malloc(ARGB_PIXEL_BUF_SIZE);
        if (!argb_pending || !argb_decode_buf || !argb_edges || !argb_pixels) {
            free(argb_pending);   argb_pending = nullptr;
            free(argb_decode_buf); argb_decode_buf = nullptr;
            free(argb_edges);     argb_edges = nullptr;
            free(argb_pixels);    argb_pixels = nullptr;
            DEBUG_PRINTLN(F("ARGB: buffer allocation failed"));
            return;
        }
    }

    realtimeLock(UINT32_MAX, REALTIME_MODE_ARGB_PASSTHROUGH);
    argb_frame_ready = false;
    argb_active = true;
    rmt_receive(argb_rx_chan, argb_symbols, sizeof(argb_symbols), &argb_rx_config);

    if (!argb_boot_delay_active && argb_boot_delay_ms > 0) {
        argb_boot_start_ms = millis();
        argb_boot_delay_active = true;
    }
    DEBUG_PRINTLN(F("ARGB: capture started"));
}

void stopARGBPassthrough()
{
    if (!argb_active) return;
    argb_active = false;
    exitRealtime();

    free(argb_pending);    argb_pending = nullptr;
    free(argb_decode_buf); argb_decode_buf = nullptr;
    free(argb_edges);      argb_edges = nullptr;
    free(argb_pixels);     argb_pixels = nullptr;

    DEBUG_PRINTLN(F("ARGB: capture stopped, buffers freed"));
}

bool isARGBPassthroughActive() { return argb_active; }

void setARGBCaptureMode(argb_capture_mode_t mode) { argb_mode = mode; }

argb_capture_mode_t getARGBCaptureMode() { return argb_mode; }

uint16_t getARGBCapturedLedCount() { return argb_last_led_count; }

void setARGBBootDelay(uint16_t ms) { argb_boot_delay_ms = ms; }
uint16_t getARGBBootDelay() { return argb_boot_delay_ms; }

uint8_t getARGBSegmentCount() { return argb_segment_count; }
const argb_segment_t* getARGBSegments() { return argb_segments; }

void setARGBSegment(uint8_t idx, uint16_t start, uint16_t count,
                    uint8_t wled_seg, const char* label) {
    if (idx >= ARGB_MAX_SEGMENTS) return;
    argb_segments[idx].start = start;
    argb_segments[idx].count = count;
    argb_segments[idx].wled_seg = wled_seg;
    strncpy(argb_segments[idx].label, label ? label : "", 15);
    argb_segments[idx].label[15] = '\0';
    if (idx >= argb_segment_count) argb_segment_count = idx + 1;
}

void clearARGBSegments() {
    argb_segment_count = 0;
    memset(argb_segments, 0, sizeof(argb_segments));
}

static void handleARGBBootDelay()
{
    if (!argb_boot_delay_active) return;
    if (millis() - argb_boot_start_ms >= argb_boot_delay_ms) {
        argb_boot_delay_active = false;
        stopARGBPassthrough();
        DEBUG_PRINTLN(F("ARGB: boot delay expired, releasing to WLED"));
    }
}

void handleARGBPassthrough()
{
    handleARGBBootDelay();
    if (!argb_active || !argb_frame_ready) return;
    argb_frame_ready = false;

    size_t sym_count = argb_pending_count;
    if (sym_count == 0 || sym_count > ARGB_SYMBOL_BUF_SIZE) return;

    if (!argb_pending || !argb_decode_buf || !argb_edges || !argb_pixels) return;

    memcpy(argb_decode_buf, (const void *)argb_pending,
           sym_count * sizeof(rmt_symbol_word_t));

    size_t edge_count = argb_rmt_to_edges(
        argb_decode_buf, sym_count,
        ARGB_RMT_RESOLUTION_HZ,
        argb_edges, ARGB_EDGE_BUF_SIZE);

    if (edge_count < 48) return;  // less than 1 LED (24 bits x 2 edges)

    argb_decode_result_t result = argb_decode_edges(
        &argb_timing, argb_edges, edge_count,
        argb_pixels, ARGB_PIXEL_BUF_SIZE);

    if (result.error == ARGB_DECODE_HIGH_ERROR_RATE) return;
    if (result.bytes_written < 3) return;

    uint16_t led_count = result.bytes_written / 3;
    argb_last_led_count = led_count;

    if (argb_mode == ARGB_MODE_RELAY) {
        feedPixelsRelay(argb_pixels, led_count);
    } else {
        // ARGB_MODE_DECODE: segment-aware feed
        if (argb_segment_count == 0) {
            feedPixelsRelay(argb_pixels, led_count);
        } else {
            for (uint8_t s = 0; s < argb_segment_count; s++) {
                uint16_t seg_start = argb_segments[s].start;
                uint16_t seg_count = argb_segments[s].count;
                if (seg_start + seg_count > led_count) continue;
                for (uint16_t i = 0; i < seg_count; i++) {
                    uint16_t src = (seg_start + i) * 3;
                    uint8_t g = argb_pixels[src];
                    uint8_t r = argb_pixels[src + 1];
                    uint8_t b = argb_pixels[src + 2];
                    setRealtimePixel(seg_start + i, r, g, b, 0);
                }
            }
        }
    }

    strip.show();
}

#endif
