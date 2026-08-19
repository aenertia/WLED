/**
 * Standalone New API I2S PDM Microphone Test  -- M5StickC SPM1423
 *
 * Tests the SPM1423 PDM mic using the IDF 5.x native driver/i2s_pdm.h API.
 * This bypasses all legacy I2S code paths that have known PDM bugs.
 *
 * Uses hardware PDM-to-PCM converter on ESP32 I2S0 for clean 16-bit PCM output.
 * Prints sample statistics every 500ms. Blinks LED on G10 when signal detected.
 *
 * Serial commands:
 *   'd'  -- toggle downsample mode (DSR_8S  <-> DSR_16S)
 *   'l'  -- switch slot mask to LEFT
 *   'r'  -- switch slot mask to RIGHT
 *   'b'  -- switch slot mask to BOTH
 *   'i'  -- reinitialize with current settings
 *   's'  -- print current config summary
 *   'w'  -- swap CLK/DATA pins (test pin assignment)
 *   '1'  -- set sample rate 16000
 *   '2'  -- set sample rate 22050
 *   '3'  -- set sample rate 44100
 *   '4'  -- set sample rate 48000
 *   'v'  -- set mic voltage 2.8V (M5Stack default)
 *   'V'  -- set mic voltage 3.3V
 *   'p'  -- print raw samples (one burst of 64 values)
 *
 * Build: pio run -e newapi_pdm -t upload && pio device monitor -b 115200
 */

#include <Arduino.h>
#include <Wire.h>

// IDF 5.x new I2S PDM API
#include "driver/i2s_pdm.h"
#include "driver/i2s_types.h"

// --- Hardware pins (M5StickC) ---
#define LED_PIN       10           // Red LED, active low
#define I2C_SDA       21
#define I2C_SCL       22
#define AXP192_ADDR   0x34

// --- Test config ---
#define READ_BUF_SAMPLES  1024
#define NOISE_THRESHOLD   100
#define PRINT_INTERVAL    500

// --- State ---
static i2s_chan_handle_t rx_handle = nullptr;
static i2s_pdm_dsr_t current_dsr = I2S_PDM_DSR_8S;
static i2s_pdm_slot_mask_t current_slot = I2S_PDM_SLOT_RIGHT;
static bool channel_enabled = false;
static gpio_num_t clk_pin = GPIO_NUM_0;    // SPM1423 CLK
static gpio_num_t data_pin = GPIO_NUM_34;  // SPM1423 DATA
static bool pins_swapped = false;
static uint32_t sample_rate = 44100;
static bool print_raw = false;
static bool clk_inverted = false;

// ============================================================
// AXP192 Power Management
// ============================================================

static void axp192_write(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(AXP192_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static uint8_t axp192_read(uint8_t reg) {
    Wire1.beginTransmission(AXP192_ADDR);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)AXP192_ADDR, (uint8_t)1);
    return Wire1.read();
}

static void axp192_init() {
    // M5StickC uses Wire1 for internal I2C bus
    Wire1.begin(I2C_SDA, I2C_SCL, 400000);

    // Enable power output register  -- LDO2 + LDO3 + DCDC1 + DCDC3
    uint8_t reg12 = axp192_read(0x12);
    reg12 = (reg12 & 0xEF) | 0x4D;  // Match M5Stack: enable LDO2, LDO3, DCDC1, DCDC3
    axp192_write(0x12, reg12);

    // Set LDO2 & LDO3 voltage: 3.0V (for TFT)
    axp192_write(0x28, 0xCC);

    // Set MIC voltage to 2.8V (M5Stack default) via LDOio0
    axp192_write(0x91, 0xA0);  // 0xA0 = 2.8V (M5Stack uses this, NOT 0xF0/3.3V)

    // Set GPIO0 to LDO output mode
    axp192_write(0x90, 0x02);

    // Verify
    uint8_t reg90 = axp192_read(0x90);
    uint8_t reg91 = axp192_read(0x91);
    uint8_t reg12v = axp192_read(0x12);

    Serial.printf("[AXP192] Using Wire1 (M5StickC internal I2C bus)\n");
    Serial.printf("[AXP192] LDOio0: reg90=0x%02X (expect 0x02), reg91=0x%02X (expect 0xA0=2.8V)\n", reg90, reg91);
    Serial.printf("[AXP192] Output enable reg12=0x%02X\n", reg12v);
    Serial.printf("[AXP192] Mic power rail: %s\n",
        (reg90 == 0x02) ? "OK (LDO mode)" : "FAILED  -- not in LDO mode");

    delay(100);  // Let power rail stabilize
}

static void axp192_set_mic_voltage(uint8_t reg91_val, const char* label) {
    axp192_write(0x91, reg91_val);
    uint8_t readback = axp192_read(0x91);
    Serial.printf("[AXP192] Mic voltage set to %s (reg91=0x%02X, readback=0x%02X)\n",
        label, reg91_val, readback);
}

// ============================================================
// I2S PDM Driver  -- New IDF 5.x API
// ============================================================

static const char* dsr_name(i2s_pdm_dsr_t dsr) {
    return dsr == I2S_PDM_DSR_8S ? "DSR_8S (÷8)" : "DSR_16S (÷16)";
}

static const char* slot_name(i2s_pdm_slot_mask_t slot) {
    switch (slot) {
        case I2S_PDM_SLOT_RIGHT: return "RIGHT";
        case I2S_PDM_SLOT_LEFT:  return "LEFT";
        case I2S_PDM_SLOT_BOTH:  return "BOTH";
        default: return "UNKNOWN";
    }
}

static void i2s_deinit() {
    if (rx_handle) {
        if (channel_enabled) {
            i2s_channel_disable(rx_handle);
            channel_enabled = false;
        }
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
    }
}

static void i2s_init() {
    i2s_deinit();
    delay(100);

    Serial.printf("\n[I2S-NEW] Initializing: %luHz, 16-bit, slot=%s, dsr=%s\n",
        sample_rate, slot_name(current_slot), dsr_name(current_dsr));
    Serial.printf("[I2S-NEW] Pins: CLK=GPIO%d, DATA=GPIO%d %s, clk_inv=%s\n",
        clk_pin, data_pin, pins_swapped ? "(SWAPPED)" : "(normal)",
        clk_inverted ? "YES" : "NO");

    // Step 1: Allocate RX channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 128;

    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (err != ESP_OK) {
        Serial.printf("[I2S-NEW] ERROR: i2s_new_channel failed: %s (0x%x)\n", esp_err_to_name(err), err);
        return;
    }

    // Step 2: Configure PDM RX mode
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = clk_pin,
            .din = data_pin,
            .invert_flags = {
                .clk_inv = clk_inverted,
            },
        },
    };

    pdm_rx_cfg.clk_cfg.dn_sample_mode = current_dsr;
    pdm_rx_cfg.slot_cfg.slot_mask = current_slot;

    err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
    if (err != ESP_OK) {
        Serial.printf("[I2S-NEW] ERROR: i2s_channel_init_pdm_rx_mode failed: %s (0x%x)\n", esp_err_to_name(err), err);
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
        return;
    }

    // Step 3: Enable channel
    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        Serial.printf("[I2S-NEW] ERROR: i2s_channel_enable failed: %s (0x%x)\n", esp_err_to_name(err), err);
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
        return;
    }

    channel_enabled = true;
    uint32_t pdm_clk = sample_rate * (current_dsr == I2S_PDM_DSR_8S ? 64 : 128);
    Serial.printf("[I2S-NEW] Channel enabled. PDM CLK freq: %lu Hz\n", pdm_clk);
}

// ============================================================
// Sample Reading + Statistics
// ============================================================

static int16_t read_buf[READ_BUF_SAMPLES];

struct SampleStats {
    int16_t min_val;
    int16_t max_val;
    int32_t sum;
    int32_t abs_sum;
    size_t  count;
    size_t  bytes_read;
    bool    has_signal;
};

static SampleStats read_samples() {
    SampleStats stats = { INT16_MAX, INT16_MIN, 0, 0, 0, 0, false };

    if (!rx_handle || !channel_enabled) return stats;

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle, read_buf, sizeof(read_buf), &bytes_read, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        Serial.printf("[READ] ERROR: i2s_channel_read failed: %s\n", esp_err_to_name(err));
        return stats;
    }

    stats.bytes_read = bytes_read;
    stats.count = bytes_read / sizeof(int16_t);

    for (size_t i = 0; i < stats.count; i++) {
        int16_t s = read_buf[i];
        if (s < stats.min_val) stats.min_val = s;
        if (s > stats.max_val) stats.max_val = s;
        stats.sum += s;
        stats.abs_sum += abs(s);
    }

    int32_t peak = max(abs((int32_t)stats.min_val), abs((int32_t)stats.max_val));
    stats.has_signal = (peak > NOISE_THRESHOLD);

    // Print raw samples if requested
    if (print_raw && stats.count > 0) {
        Serial.println("\n[RAW SAMPLES] First 64 values:");
        for (size_t i = 0; i < min((size_t)64, stats.count); i++) {
            Serial.printf("%6d ", read_buf[i]);
            if ((i + 1) % 16 == 0) Serial.println();
        }
        Serial.println();
        print_raw = false;
    }

    return stats;
}

// ============================================================
// Serial Command Handler
// ============================================================

static void handle_serial() {
    if (!Serial.available()) return;

    char c = Serial.read();
    switch (c) {
        case 'd':
        case 'D':
            current_dsr = (current_dsr == I2S_PDM_DSR_8S) ? I2S_PDM_DSR_16S : I2S_PDM_DSR_8S;
            Serial.printf("\n>>> Downsample: %s  -- send 'i' to reinitialize\n", dsr_name(current_dsr));
            break;
        case 'l':
        case 'L':
            current_slot = I2S_PDM_SLOT_LEFT;
            Serial.printf("\n>>> Slot: LEFT  -- send 'i' to reinitialize\n");
            break;
        case 'r':
            current_slot = I2S_PDM_SLOT_RIGHT;
            Serial.printf("\n>>> Slot: RIGHT  -- send 'i' to reinitialize\n");
            break;
        case 'b':
        case 'B':
            current_slot = I2S_PDM_SLOT_BOTH;
            Serial.printf("\n>>> Slot: BOTH  -- send 'i' to reinitialize\n");
            break;
        case 'i':
        case 'I':
            Serial.println("\n>>> Reinitializing I2S...");
            i2s_init();
            break;
        case 's':
        case 'S':
            Serial.printf("\n>>> Config: slot=%s, dsr=%s, rate=%lu, clk=GPIO%d, data=GPIO%d, swapped=%s, chip_rev=%d\n",
                slot_name(current_slot), dsr_name(current_dsr), sample_rate,
                clk_pin, data_pin, pins_swapped ? "YES" : "NO", ESP.getChipRevision());
            break;
        case 'w':
        case 'W':
            pins_swapped = !pins_swapped;
            if (pins_swapped) {
                clk_pin = GPIO_NUM_34;
                data_pin = GPIO_NUM_0;
            } else {
                clk_pin = GPIO_NUM_0;
                data_pin = GPIO_NUM_34;
            }
            Serial.printf("\n>>> Pins %s: CLK=GPIO%d, DATA=GPIO%d  -- send 'i' to reinitialize\n",
                pins_swapped ? "SWAPPED" : "NORMAL", clk_pin, data_pin);
            break;
        case '1':
            sample_rate = 16000;
            Serial.printf("\n>>> Sample rate: %lu  -- send 'i' to reinitialize\n", sample_rate);
            break;
        case '2':
            sample_rate = 22050;
            Serial.printf("\n>>> Sample rate: %lu  -- send 'i' to reinitialize\n", sample_rate);
            break;
        case '3':
            sample_rate = 44100;
            Serial.printf("\n>>> Sample rate: %lu  -- send 'i' to reinitialize\n", sample_rate);
            break;
        case '4':
            sample_rate = 48000;
            Serial.printf("\n>>> Sample rate: %lu  -- send 'i' to reinitialize\n", sample_rate);
            break;
        case 'v':
            axp192_set_mic_voltage(0xA0, "2.8V (M5Stack default)");
            break;
        case 'V':
            axp192_set_mic_voltage(0xF0, "3.3V");
            break;
        case 'p':
        case 'P':
            print_raw = true;
            Serial.println("\n>>> Will print next 64 raw samples...");
            break;
        case 'c':
            clk_inverted = !clk_inverted;
            Serial.printf("\n>>> Clock inversion: %s  -- send 'i' to reinitialize\n",
                clk_inverted ? "INVERTED" : "NORMAL");
            break;
        case 'R':
            // Full reset: power cycle mic
            Serial.println("\n>>> Power cycling mic...");
            axp192_write(0x90, 0x07);  // GPIO0 floating (power off)
            delay(500);
            axp192_write(0x91, 0xA0);  // 2.8V
            axp192_write(0x90, 0x02);  // LDO mode (power on)
            delay(200);
            Serial.println(">>> Mic power cycled. Send 'i' to reinitialize I2S.");
            break;
        default:
            break;
    }
}

// ============================================================
// Main
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  M5StickC SPM1423 PDM Mic Test v2");
    Serial.println("  New IDF 5.x API (driver/i2s_pdm.h)");
    Serial.println("========================================");
    Serial.printf("  Chip: ESP32 rev %d\n", ESP.getChipRevision());
    Serial.printf("  IDF:  %s\n", esp_get_idf_version());
    Serial.printf("  Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.println("  Commands:");
    Serial.println("    d=toggle_dsr  l/r/b=slot  i=reinit  s=status");
    Serial.println("    w=swap_pins   1/2/3/4=rate(16k/22k/44k/48k)");
    Serial.println("    v=2.8V  V=3.3V  p=print_raw  R=power_cycle");
    Serial.println("========================================\n");

    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    // Power up mic (using Wire1 like M5Stack, 2.8V like M5Stack)
    axp192_init();

    // Initialize I2S PDM (new API)
    i2s_init();

    Serial.println("\n[READY] Listening for PDM audio...\n");
    Serial.println("  Timestamp  | bytes | samples |   min   |   max   |  mean  | abs_mean | signal");
    Serial.println("  -----------|-------|---------|---------|---------|--------|----------|-------");
}

void loop() {
    static uint32_t last_print = 0;
    static SampleStats accum = { INT16_MAX, INT16_MIN, 0, 0, 0, 0, false };

    handle_serial();

    if (!rx_handle || !channel_enabled) {
        delay(100);
        return;
    }

    SampleStats stats = read_samples();

    // Accumulate
    if (stats.min_val < accum.min_val) accum.min_val = stats.min_val;
    if (stats.max_val > accum.max_val) accum.max_val = stats.max_val;
    accum.sum += stats.sum;
    accum.abs_sum += stats.abs_sum;
    accum.count += stats.count;
    accum.bytes_read += stats.bytes_read;
    if (stats.has_signal) accum.has_signal = true;

    // Blink LED
    digitalWrite(LED_PIN, stats.has_signal ? LOW : HIGH);

    uint32_t now = millis();
    if (now - last_print >= PRINT_INTERVAL) {
        if (accum.count > 0) {
            int32_t mean = accum.sum / (int32_t)accum.count;
            int32_t abs_mean = accum.abs_sum / (int32_t)accum.count;
            Serial.printf("  %10lu | %5u | %7u | %7d | %7d | %6ld | %8ld | %s\n",
                now, (unsigned)accum.bytes_read, (unsigned)accum.count,
                accum.min_val, accum.max_val, (long)mean, (long)abs_mean,
                accum.has_signal ? "YES ***" : "no");
        } else {
            Serial.printf("  %10lu | NO DATA  -- i2s_channel_read returned 0 bytes\n", now);
        }

        accum = { INT16_MAX, INT16_MIN, 0, 0, 0, 0, false };
        last_print = now;
    }
}
