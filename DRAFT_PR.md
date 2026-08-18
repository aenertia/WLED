## fix(audioreactive): skip i2s_set_clk() for PDM microphones on IDF 5.x

**Forgejo**: Fixes #17

### Problem

The audioreactive usermod calls `i2s_set_clk()` after `i2s_driver_install()`
to configure sample rate and channel format. On ESP-IDF 5.x, this destroys
PDM mode configuration — the driver reverts to I2S standard mode internally,
producing all-zero samples from PDM microphones like the SPM1423 (M5StickC
built-in) and INMP441 in PDM mode.

Root cause: `i2s_set_clk()` recalculates clock dividers assuming standard
I2S timing. PDM uses a completely different clock tree (PDM_CLK = sample_rate
× decimation_factor), and `i2s_driver_install()` already configures this
correctly. The post-install `i2s_set_clk()` call overwrites the PDM clock
config with I2S standard values → silence.

Secondary issue: the PDM channel format macros used `I2S_CHANNEL_FMT_ONLY_LEFT`
/ `ONLY_RIGHT`, which don't work for PDM on IDF 5.x. PDM requires
`I2S_CHANNEL_FMT_ALL_LEFT` / `ALL_RIGHT` — the "ALL" variants route PDM
data correctly through the decimation filter.

### Fix

1. Guard `i2s_set_clk()` with `if (!(_config.mode & I2S_MODE_PDM))` — PDM
   drivers skip the redundant clock reconfiguration entirely.

2. Define separate `I2S_PDM_MIC_CHANNEL` macros using `ALL_LEFT`/`ALL_RIGHT`
   instead of aliasing to the standard `ONLY_*` variants.

Both changes are no-ops for standard I2S microphones (INMP441 in I2S mode,
ICS-43434, etc.) — the guard only skips `i2s_set_clk()` when `I2S_MODE_PDM`
is set in the config mode bitmask.

### Files changed

- `usermods/audioreactive/audio_source.h` — PDM channel format macros, i2s_set_clk guard

### Testing

Tested on M5StickC (ESP32-PICO-D4) with SPM1423 PDM microphone on
Tasmota Arduino Core 3.x (ESP-IDF 5.x). Before fix: all-zero samples,
no audio response. After fix: full audio spectrum, FFT works correctly,
GEQ effect responds to music.

Verified standard I2S path (INMP441) still works — `i2s_set_clk()` runs
normally when PDM mode is not set.

### Related

- ESP-IDF issues #8850, #9635 (i2s_set_clk PDM regression)
- WLED issue #4583 (audioreactive silence on IDF 5.x)
- Affects any ESP32 board using PDM microphones with Arduino Core 3.x