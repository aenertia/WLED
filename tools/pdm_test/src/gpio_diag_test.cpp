/**
 * GPIO0 Clock Diagnostic  -- M5StickC
 *
 * Tests whether GPIO0 can output a clock signal at PDM frequencies.
 * GPIO0 is a boot strapping pin with external pull-up on M5StickC.
 * This test verifies the pin is actually toggling when used by I2S.
 *
 * Test sequence:
 * 1. Manual GPIO toggle test  -- verify GPIO0 can be driven high/low
 * 2. LEDC PWM test  -- output a measurable frequency on GPIO0
 * 3. I2S PDM init  -- then sample GPIO34 (data pin) with ADC to see if
 *    the mic is responding to the clock
 *
 * Build: pio run -e gpio_diag -t upload && pio device monitor -b 115200
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <driver/i2s_pdm.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>

#define PDM_CLK_PIN   GPIO_NUM_0
#define PDM_DATA_PIN  GPIO_NUM_34
#define LED_PIN       10
#define I2C_SDA       21
#define I2C_SCL       22
#define AXP192_ADDR   0x34

// AXP192 helpers
static void axp_write(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(AXP192_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static uint8_t axp_read(uint8_t reg) {
    Wire1.beginTransmission(AXP192_ADDR);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)AXP192_ADDR, (uint8_t)1);
    return Wire1.read();
}

static void axp192_init() {
    Wire1.begin(I2C_SDA, I2C_SCL, 400000);
    // Enable power outputs
    uint8_t reg12 = axp_read(0x12);
    axp_write(0x12, (reg12 & 0xEF) | 0x4D);
    // Mic power: 2.8V LDOio0
    axp_write(0x91, 0xA0);
    axp_write(0x90, 0x02);
    delay(100);
    Serial.printf("[AXP192] Mic power: reg90=0x%02X, reg91=0x%02X\n", axp_read(0x90), axp_read(0x91));
}

// ============================================================
// Test 1: Manual GPIO Toggle
// ============================================================
static void test_gpio_toggle() {
    Serial.println("\n=== TEST 1: Manual GPIO0 Toggle ===");

    // Deconfigure any I2S usage first
    gpio_reset_pin(PDM_CLK_PIN);
    gpio_set_direction(PDM_CLK_PIN, GPIO_MODE_OUTPUT);

    // Read GPIO_OUT register to see if GPIO0 is in the output set
    uint32_t gpio_out = REG_READ(GPIO_OUT_REG);
    Serial.printf("  GPIO_OUT before: 0x%08lX (GPIO0 = %d)\n", gpio_out, (gpio_out >> 0) & 1);

    // Toggle high
    gpio_set_level(PDM_CLK_PIN, 1);
    delayMicroseconds(10);
    gpio_out = REG_READ(GPIO_OUT_REG);
    uint32_t gpio_in = REG_READ(GPIO_IN_REG);
    Serial.printf("  Set HIGH: GPIO_OUT=0x%08lX, GPIO_IN=0x%08lX (GPIO0 out=%d, in=%d)\n",
        gpio_out, gpio_in, (gpio_out >> 0) & 1, (gpio_in >> 0) & 1);

    // Toggle low
    gpio_set_level(PDM_CLK_PIN, 0);
    delayMicroseconds(10);
    gpio_out = REG_READ(GPIO_OUT_REG);
    gpio_in = REG_READ(GPIO_IN_REG);
    Serial.printf("  Set LOW:  GPIO_OUT=0x%08lX, GPIO_IN=0x%08lX (GPIO0 out=%d, in=%d)\n",
        gpio_out, gpio_in, (gpio_out >> 0) & 1, (gpio_in >> 0) & 1);

    // Toggle test: count how fast we can toggle
    uint32_t start = micros();
    for (int i = 0; i < 10000; i++) {
        gpio_set_level(PDM_CLK_PIN, 1);
        gpio_set_level(PDM_CLK_PIN, 0);
    }
    uint32_t elapsed = micros() - start;
    Serial.printf("  10000 toggles in %lu us (~%lu kHz toggle rate)\n", elapsed, 10000000UL / elapsed);

    // Check IO_MUX configuration for GPIO0
    uint32_t io_mux_0 = REG_READ(IO_MUX_GPIO0_REG);
    Serial.printf("  IO_MUX_GPIO0_REG: 0x%08lX\n", io_mux_0);
    Serial.printf("    Function select: %lu\n", (io_mux_0 >> 12) & 0x7);
    Serial.printf("    Pull-up: %s\n", (io_mux_0 & (1 << 8)) ? "YES" : "NO");
    Serial.printf("    Pull-down: %s\n", (io_mux_0 & (1 << 7)) ? "YES" : "NO");
    Serial.printf("    Drive strength: %lu\n", (io_mux_0 >> 10) & 0x3);

    gpio_reset_pin(PDM_CLK_PIN);
    Serial.println("  PASS: GPIO0 can be toggled manually.");
}

// ============================================================
// Test 2: LEDC PWM on GPIO0
// ============================================================
static void test_ledc_pwm() {
    Serial.println("\n=== TEST 2: LEDC PWM on GPIO0 ===");

    // Use LEDC to generate a 1MHz clock on GPIO0
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,  // 50% duty
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000000,  // 1 MHz
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    Serial.printf("  LEDC timer config (1MHz): %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = PDM_CLK_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,  // 50% for 1-bit resolution
        .hpoint = 0,
        .flags = { .output_invert = 0 },
    };
    err = ledc_channel_config(&ch_cfg);
    Serial.printf("  LEDC channel config on GPIO0: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));

    if (err == ESP_OK) {
        // Let it run for a bit, then read GPIO34 to see if mic responds
        delay(100);

        // Sample GPIO34 (PDM DATA) rapidly to see if it's toggling
        int high_count = 0;
        int low_count = 0;
        for (int i = 0; i < 10000; i++) {
            if (gpio_get_level(PDM_DATA_PIN)) high_count++;
            else low_count++;
        }
        Serial.printf("  GPIO34 (DATA) while 1MHz clock on GPIO0: HIGH=%d, LOW=%d (ratio=%.2f%%)\n",
            high_count, low_count, 100.0f * high_count / (high_count + low_count));
        Serial.println("  If ratio is ~50%, data line has signal. If ~0% or ~100%, mic not responding.");

        // Stop LEDC
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    }

    gpio_reset_pin(PDM_CLK_PIN);
    Serial.println("  LEDC test complete.");
}

// ============================================================
// Test 3: I2S PDM with GPIO register dump
// ============================================================
static void test_i2s_pdm_with_diag() {
    Serial.println("\n=== TEST 3: I2S PDM with GPIO Diagnostics ===");

    i2s_chan_handle_t rx_handle = nullptr;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 128;

    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (err != ESP_OK) {
        Serial.printf("  i2s_new_channel FAILED: %s\n", esp_err_to_name(err));
        return;
    }

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_PIN,
            .din = PDM_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;

    err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
    if (err != ESP_OK) {
        Serial.printf("  i2s_channel_init_pdm_rx_mode FAILED: %s\n", esp_err_to_name(err));
        i2s_del_channel(rx_handle);
        return;
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        Serial.printf("  i2s_channel_enable FAILED: %s\n", esp_err_to_name(err));
        i2s_del_channel(rx_handle);
        return;
    }

    Serial.println("  I2S PDM enabled. Checking GPIO registers...");

    // Dump GPIO0 configuration during I2S operation
    uint32_t io_mux_0 = REG_READ(IO_MUX_GPIO0_REG);
    uint32_t gpio_func_out = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG);
    uint32_t gpio_enable = REG_READ(GPIO_ENABLE_REG);

    Serial.printf("  IO_MUX_GPIO0: 0x%08lX (func=%lu, pu=%d, pd=%d, drv=%lu)\n",
        io_mux_0,
        (io_mux_0 >> 12) & 0x7,
        (io_mux_0 >> 8) & 1,
        (io_mux_0 >> 7) & 1,
        (io_mux_0 >> 10) & 0x3);
    Serial.printf("  GPIO_FUNC0_OUT_SEL: 0x%08lX (signal=%lu)\n",
        gpio_func_out, gpio_func_out & 0x1FF);
    Serial.printf("  GPIO_ENABLE: 0x%08lX (GPIO0 enabled=%d)\n",
        gpio_enable, (gpio_enable >> 0) & 1);

    // Sample GPIO34 state rapidly
    int high_count = 0;
    for (int i = 0; i < 100000; i++) {
        if (gpio_get_level(PDM_DATA_PIN)) high_count++;
    }
    Serial.printf("  GPIO34 (DATA) sampling: %d/100000 HIGH (%.1f%%)\n",
        high_count, high_count / 1000.0f);

    // Read some I2S data
    int16_t buf[512];
    size_t bytes_read;
    i2s_channel_read(rx_handle, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(200));
    if (bytes_read > 0) {
        int16_t mn = INT16_MAX, mx = INT16_MIN;
        int32_t sum = 0;
        size_t count = bytes_read / 2;
        for (size_t i = 0; i < count; i++) {
            if (buf[i] < mn) mn = buf[i];
            if (buf[i] > mx) mx = buf[i];
            sum += buf[i];
        }
        Serial.printf("  I2S read: %u bytes, %u samples, min=%d, max=%d, mean=%ld\n",
            (unsigned)bytes_read, (unsigned)count, mn, mx, (long)(sum / (int32_t)count));
    }

    // Cleanup
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    Serial.println("  I2S PDM test complete.");
}

// ============================================================
// Main
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  M5StickC GPIO0/PDM Diagnostics");
    Serial.println("========================================");
    Serial.printf("  Chip: ESP32 rev %d\n", ESP.getChipRevision());
    Serial.printf("  IDF:  %s\n", esp_get_idf_version());
    Serial.printf("  Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.println("========================================\n");

    // LED off
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    // Configure GPIO34 as input (it's input-only anyway)
    gpio_set_direction(PDM_DATA_PIN, GPIO_MODE_INPUT);

    // Power up mic
    axp192_init();

    // Run all tests
    test_gpio_toggle();
    test_ledc_pwm();
    test_i2s_pdm_with_diag();

    Serial.println("\n========================================");
    Serial.println("  All diagnostics complete.");
    Serial.println("========================================");
}

void loop() {
    delay(10000);
}
