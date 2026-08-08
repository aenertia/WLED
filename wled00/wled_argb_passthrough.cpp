#ifdef WLED_ENABLE_ARGB_PASSTHROUGH
#include "wled.h"
#include "wled_argb_passthrough.h"

#include "driver/rmt_rx.h"
#include "driver/rmt_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// RMT RX resolution: 10 MHz → 1 tick = 100 ns
#define ARGB_RMT_RESOLUTION_HZ 10000000
// WS2812B bit threshold: >500 ns high = 1 (5 ticks at 10 MHz)
#define ARGB_BIT_THRESHOLD_TICKS 5
// WS2812B reset: >50 µs idle — use 50 µs as max signal range
#define ARGB_RESET_US 50

// RMT symbol buffer: 24 bits/LED × 1 symbol/bit + margin
#define ARGB_SYMBOL_BUF_SIZE (ARGB_MAX_LEDS * 24 + 16)

static rmt_channel_handle_t argb_rx_chan = nullptr;
static rmt_symbol_word_t    argb_symbols[ARGB_SYMBOL_BUF_SIZE];
static volatile bool        argb_frame_ready = false;
static volatile size_t      argb_frame_symbols = 0;
static bool                 argb_active = false;
static bool                 argb_initialized = false;

// Double buffer: ISR writes to pending, loop swaps to process
static rmt_symbol_word_t    argb_pending[ARGB_SYMBOL_BUF_SIZE];
static volatile size_t      argb_pending_count = 0;

static rmt_receive_config_t argb_rx_config = {};

// ISR callback — copy symbols to pending buffer, signal main loop
static bool IRAM_ATTR argb_rx_done_cb(rmt_channel_handle_t channel,
                                       const rmt_rx_done_event_data_t *edata,
                                       void *user_data)
{
  if (edata->num_symbols > 0 && edata->num_symbols <= ARGB_SYMBOL_BUF_SIZE) {
    memcpy((void *)argb_pending, edata->received_symbols,
           edata->num_symbols * sizeof(rmt_symbol_word_t));
    argb_pending_count = edata->num_symbols;
    argb_frame_ready = true;
  }
  // Re-arm receive for next frame
  rmt_receive(channel, argb_symbols, sizeof(argb_symbols), &argb_rx_config);
  return false;
}

// Decode RMT symbols into RGB pixels and feed to WLED realtime
static void decodeAndFeedPixels(const rmt_symbol_word_t *symbols, size_t count)
{
  uint8_t grb[3] = {0, 0, 0};
  size_t bit = 0;
  uint16_t led = 0;
  uint16_t maxLed = strip.getLengthTotal();
  if (maxLed > ARGB_MAX_LEDS) maxLed = ARGB_MAX_LEDS;

  for (size_t i = 0; i < count && led < maxLed; i++) {
    // Each RMT symbol encodes one WS2812B bit
    // The high-level pulse duration determines 0 vs 1
    uint16_t highDur;
    if (symbols[i].level0 == 1) {
      highDur = symbols[i].duration0;
    } else {
      highDur = symbols[i].duration1;
    }

    bool bitVal = (highDur > ARGB_BIT_THRESHOLD_TICKS);
    grb[bit / 8] = (grb[bit / 8] << 1) | (uint8_t)bitVal;
    bit++;

    if (bit == 24) {
      // GRB → RGB reorder
      setRealtimePixel(led, grb[1], grb[0], grb[2], 0);
      led++;
      bit = 0;
      grb[0] = grb[1] = grb[2] = 0;
    }
  }
}

void initARGBPassthrough()
{
  if (argb_initialized) return;

  rmt_rx_channel_config_t rx_cfg = {};
  rx_cfg.gpio_num = (gpio_num_t)ARGB_RX_PIN;
  rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  rx_cfg.resolution_hz = ARGB_RMT_RESOLUTION_HZ;
  rx_cfg.mem_block_symbols = 64;  // hardware memory blocks
  rx_cfg.intr_priority = 0;
  rx_cfg.flags.invert_in = 0;
  rx_cfg.flags.with_dma = 0;     // ESP32 original has no RMT DMA
  rx_cfg.flags.io_loop_back = 0;

  esp_err_t err = rmt_new_rx_channel(&rx_cfg, &argb_rx_chan);
  if (err != ESP_OK) {
    DEBUG_PRINTF_P(PSTR("ARGB: RMT RX init failed: %d\n"), err);
    return;
  }

  rmt_rx_event_callbacks_t cbs = {};
  cbs.on_recv_done = argb_rx_done_cb;
  rmt_rx_register_event_callbacks(argb_rx_chan, &cbs, nullptr);

  // Configure receive parameters
  argb_rx_config.signal_range_min_ns = 100;                // glitch filter: ignore <100 ns
  argb_rx_config.signal_range_max_ns = ARGB_RESET_US * 1000;  // reset detect: 50 µs

  rmt_enable(argb_rx_chan);
  argb_initialized = true;
  DEBUG_PRINTLN(F("ARGB: passthrough initialized"));
}

void startARGBPassthrough()
{
  if (!argb_initialized || argb_active) return;

  realtimeLock(UINT32_MAX, REALTIME_MODE_ARGB_PASSTHROUGH);
  argb_frame_ready = false;
  argb_active = true;

  // Start receiving — callback re-arms after each frame
  rmt_receive(argb_rx_chan, argb_symbols, sizeof(argb_symbols), &argb_rx_config);
  DEBUG_PRINTLN(F("ARGB: passthrough started"));
}

void stopARGBPassthrough()
{
  if (!argb_active) return;

  argb_active = false;
  exitRealtime();
  DEBUG_PRINTLN(F("ARGB: passthrough stopped"));
}

bool isARGBPassthroughActive()
{
  return argb_active;
}

void handleARGBPassthrough()
{
  if (!argb_active || !argb_frame_ready) return;

  argb_frame_ready = false;
  size_t count = argb_pending_count;
  if (count == 0 || count > ARGB_SYMBOL_BUF_SIZE) return;

  // Copy from volatile pending buffer to local for decode
  rmt_symbol_word_t localBuf[ARGB_SYMBOL_BUF_SIZE];
  memcpy(localBuf, (const void *)argb_pending, count * sizeof(rmt_symbol_word_t));

  decodeAndFeedPixels(localBuf, count);
  strip.show();
}

#endif // WLED_ENABLE_ARGB_PASSTHROUGH
