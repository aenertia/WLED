# fix(watchdog): use esp_task_wdt_config_t struct API for IDF 5.x

## Summary

`esp_task_wdt_init(timeout, panic)` was removed in ESP-IDF 5.x. The new API takes an `esp_task_wdt_config_t` struct. Without this fix, the watchdog silently fails to initialise on IDF 5.x builds — the device won't reset on a hung loop task, which is the entire point of having a watchdog.

## What changed

Added a compile-time `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)` branch in `WLED::enableWatchdog()` (in `wled00/wled.cpp`):

- **IDF 5.x path**: uses `esp_task_wdt_config_t` struct with `timeout_ms`, `idle_core_mask=0` (only our loop task, not idle tasks), and `trigger_panic=true`. Falls back to `esp_task_wdt_reconfigure()` if the TWDT was already initialized by IDF startup.
- **IDF 4.x path**: unchanged two-argument `esp_task_wdt_init(timeout, panic)` call.

## Impact

- Watchdog now initialises correctly on both IDF 4.x and 5.x builds.
- No runtime change on IDF 4.x — the old code path is preserved behind `#else`.
- On IDF 5.x, a hung loop task will now correctly trigger a panic and reset, instead of silently running forever.

## Testing

- Verified on ESP32 with Tasmota Arduino Core 3.x (ESP-IDF 5.x) — watchdog initialises and fires on deliberate `while(1)` in loop.
- Verified backwards compatibility with IDF 4.x builds (no compilation errors, watchdog behaviour unchanged).
