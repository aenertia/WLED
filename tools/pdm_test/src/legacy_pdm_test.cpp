/**
 * Standalone Legacy I2S PDM Microphone Test  -- M5StickC SPM1423
 *
 * Tests the SPM1423 PDM mic using the legacy driver/i2s.h API,
 * matching M5Stack official example config (44100Hz, 16-bit, ALL_RIGHT).
 *
 * Designed to isolate whether the mic hardware works independently of WLED.
 * Prints sample statistics every 500ms. Blinks LED on G10 when signal detected.
 *
 * Serial commands:
 *   'c'  -- toggle i2s_set_clk() call (tests whether it breaks PDM)
 *   'a'  -- toggle APLL on/off
 *   'r'  -- reinitialize I2S with current settings
 *   's'  -- print current config summary
 *
 * Build: pio run -e legacy_pdm -t upload && pio device monitor -b 115200
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>

// --- Hardware pins (M5StickC) ---
#define PDM_CLK_PIN   GPIO_NUM_0   // SPM1423 CLK (directly to I2S WS)
#define PDM_DATA_PIN  GPIO_NUM_34  // SPM1423 DATA (input-only GPIO)
#define LED_PIN       10           // Red LED, active low
#define I2C_SDA       21
#define I2C_SCL       22
#define AXP192_ADDR   0x34

// --- Test config ---
#define SAMPLE_RATE     44100
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     128
#define READ_BUF_SAMPLES 1024
#define NOISE_THRESHOLD  100       // Absolute sample value threshold for "signal detected"
#define PRINT_INTERVAL   500       // ms between stat prints

// --- State ---
static bool use_set_clk = false;   // Start WITHOUT i2s_set_clk (safer)
static bool use_apll = true;       // APLL on by default (better PDM clock quality)
static bool i2s_running = false;

// ============================================================
// AXP192 Power Management  -- Enable LDOio0 for mic power
// ============================================================

static void axp192_init() {
    Wire.begin(I2C_SDA, I2C_SCL, 400000);

    // Enable LDO2 (LCD backlight)  -- 2.6V
    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x28);  // LDO2/3 voltage reg
    Wire.write(0x0C);  // LDO2=2.6V, LDO3=1.8V
    Wire.endTransmission();

    // Turn on LDO2 + LDO3 via output control reg 0x12
    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x12);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)AXP192_ADDR, (uint8_t)1);
    uint8_t val = Wire.read();
    val |= (1 << 2) | (1 << 3);  // LDO2 + LDO3 enable bits
    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x12);
    Wire.write(val);
    Wire.endTransmission();

    // Enable LDOio0 (GPIO0 as LDO output for mic power)  -- 3.3V
    // Reg 0x90: GPIO0 function = LDO output (mode 0x02)
    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x90);
    Wire.write(0x02);  // GPIO0 = LDO output mode
    Wire.endTransmission();

    // Reg 0x91: LDOio0 voltage = 3.3V (0xF0 = 15 * 0.1V + 1.8V = 3.3V)
    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x91);
    Wire.write(0xF0);
    Wire.endTransmission();

    // Verify LDOio0 readback
    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x90);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)AXP192_ADDR, (uint8_t)1);
    uint8_t reg90 = Wire.read();

    Wire.beginTransmission(AXP192_ADDR);
    Wire.write(0x91);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)AXP192_ADDR, (uint8_t)1);
    uint8_t reg91 = Wire.read();

    Serial.printf("[AXP192] LDOio0: reg90=0x%02X (expect 0x02), reg91=0x%02X (expect 0xF0)\n", reg90, reg91);
    Serial.printf("[AXP192] Mic power rail: %s\n",
        (reg90 == 0x02 && reg91 == 0xF0) ? "OK (3.3V)" : "FAILED  -- check I2C wiring");

    delay(50);  // Let power rail stabilize
}

// ============================================================
// I2S PDM Driver  -- Legacy API
// ============================================================

static void i2s_init() {
    if (i2s_running) {
        i2s_driver_uninstall(I2S_NUM_0);
        i2s_running = false;
        delay(100);
    }

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,  // M5Stack convention  -- safest for single PDM mic
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = use_apll,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_PIN_NO_CHANGE,   // No BCK for PDM
        .ws_io_num = PDM_CLK_PIN,           // PDM CLK
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PDM_DATA_PIN,        // PDM DATA
    };

    Serial.printf("\n[I2S] Installing driver: PDM mode, %dHz, 16-bit, ALL_RIGHT, APLL=%s\n",
        SAMPLE_RATE, use_apll ? "ON" : "OFF");

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: i2s_driver_install failed: %s (0x%x)\n", esp_err_to_name(err), err);
        return;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: i2s_set_pin failed: %s (0x%x)\n", esp_err_to_name(err), err);
        i2s_driver_uninstall(I2S_NUM_0);
        return;
    }

    if (use_set_clk) {
        Serial.println("[I2S] Calling i2s_set_clk() after install (THIS MAY BREAK PDM)...");
        err = i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
        if (err != ESP_OK) {
            Serial.printf("[I2S] ERROR: i2s_set_clk failed: %s (0x%x)\n", esp_err_to_name(err), err);
        } else {
            Serial.println("[I2S] i2s_set_clk() succeeded");
        }
    } else {
        Serial.println("[I2S] Skipping i2s_set_clk()  -- config set entirely by i2s_driver_install()");
    }

    i2s_running = true;
    Serial.printf("[I2S] Driver installed. Chip rev: %d, APLL: %s, i2s_set_clk: %s\n",
        ESP.getChipRevision(), use_apll ? "ON" : "OFF", use_set_clk ? "YES" : "NO");
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
    SampleStats stats = { .min_val = INT16_MAX, .max_val = INT16_MIN, .sum = 0, .abs_sum = 0, .count = 0, .bytes_read = 0, .has_signal = false };

    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, read_buf, sizeof(read_buf), &bytes_read, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        Serial.printf("[READ] ERROR: i2s_read failed: %s\n", esp_err_to_name(err));
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

    return stats;
}

// ============================================================
// Serial Command Handler
// ============================================================

static void handle_serial() {
    if (!Serial.available()) return;

    char c = Serial.read();
    switch (c) {
        case 'c':
        case 'C':
            use_set_clk = !use_set_clk;
            Serial.printf("\n>>> i2s_set_clk: %s  -- send 'r' to reinitialize\n", use_set_clk ? "ENABLED" : "DISABLED");
            break;
        case 'a':
        case 'A':
            use_apll = !use_apll;
            Serial.printf("\n>>> APLL: %s  -- send 'r' to reinitialize\n", use_apll ? "ON" : "OFF");
            break;
        case 'r':
        case 'R':
            Serial.println("\n>>> Reinitializing I2S...");
            i2s_init();
            break;
        case 's':
        case 'S':
            Serial.printf("\n>>> Config: APLL=%s, i2s_set_clk=%s, rate=%d, chip_rev=%d\n",
                use_apll ? "ON" : "OFF", use_set_clk ? "YES" : "NO", SAMPLE_RATE, ESP.getChipRevision());
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
    Serial.println("  M5StickC SPM1423 PDM Mic Test");
    Serial.println("  Legacy I2S API (driver/i2s.h)");
    Serial.println("========================================");
    Serial.printf("  Chip: ESP32 rev %d\n", ESP.getChipRevision());
    Serial.printf("  IDF:  %s\n", esp_get_idf_version());
    Serial.printf("  Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.println("  Commands: c=toggle_set_clk, a=toggle_apll, r=reinit, s=status");
    Serial.println("========================================\n");

    // LED setup (active low)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // Off

    // Power up mic via AXP192
    axp192_init();

    // Initialize I2S PDM
    i2s_init();

    Serial.println("\n[READY] Listening for PDM audio...\n");
    Serial.println("  Timestamp  | bytes | samples |   min   |   max   |  mean  | abs_mean | signal");
    Serial.println("  -----------|-------|---------|---------|---------|--------|----------|-------");
}

void loop() {
    static uint32_t last_print = 0;
    static uint32_t read_count = 0;
    static SampleStats accum = { .min_val = INT16_MAX, .max_val = INT16_MIN, .sum = 0, .abs_sum = 0, .count = 0, .bytes_read = 0, .has_signal = false };

    handle_serial();

    if (!i2s_running) {
        delay(100);
        return;
    }

    SampleStats stats = read_samples();
    read_count++;

    // Accumulate stats for the print interval
    if (stats.min_val < accum.min_val) accum.min_val = stats.min_val;
    if (stats.max_val > accum.max_val) accum.max_val = stats.max_val;
    accum.sum += stats.sum;
    accum.abs_sum += stats.abs_sum;
    accum.count += stats.count;
    accum.bytes_read += stats.bytes_read;
    if (stats.has_signal) accum.has_signal = true;

    // Blink LED on signal
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
            Serial.printf("  %10lu | NO DATA  -- i2s_read returned 0 bytes\n", now);
        }

        // Reset accumulator
        accum = { .min_val = INT16_MAX, .max_val = INT16_MIN, .sum = 0, .abs_sum = 0, .count = 0, .bytes_read = 0, .has_signal = false };
        last_print = now;
    }
}
