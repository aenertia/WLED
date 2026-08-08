# ARGB Passthrough Behavior Design

## Principles

1. **Passthrough = sync receive** — in our USB controller context, "sync" always means
   "receive external ARGB data from motherboard header and mirror to LEDs"
2. **Non-sync = manual control** — WLED effects, presets, DDP from OpenRGB
3. **PPP connection does NOT dictate LED behavior** — PPP is transport only
4. **Boot delay** — motherboard POST colors always visible for N seconds before
   any preset kicks in, so users see POST status indicators

## Boot Sequence

```
t=0    Power on
       │
       ▼
t=0    ARGB passthrough starts (RMT RX → setRealtimePixel)
       LEDs mirror motherboard POST colors (red=error, amber=training, etc.)
       │
       │  Configurable delay: ARGB_BOOT_DELAY_MS (default 5000ms)
       │  During this window: passthrough ALWAYS active, even if bootPreset set
       │
t=5s   Boot delay expires
       │
       ├─ bootPreset > 0? ──YES──→ applyPreset(bootPreset)
       │                           exitRealtime() → WLED effects take over
       │                           (smooth transition via transitionDelay)
       │
       └─ bootPreset == 0? ─────→ Stay in passthrough
                                   LEDs continue mirroring motherboard
                                   Until user manually takes control
       │
t=?    PPP connects (pppd starts on host)
       │  Does NOT change LED mode
       │  Just enables: web dashboard, DDP, JSON API, mDNS
       │
t=?    User interaction (any of):
       ├─ Dashboard toggle sync off → exitRealtime → WLED effects
       ├─ OpenRGB sends DDP → realtimeLock(DDP) overrides passthrough
       ├─ JSON API sets effect → exitRealtime → new effect
       └─ Preset button press → exitRealtime → preset
```

## Sync Toggle = Passthrough Toggle

WLED's existing sync system (`receiveGroups`, `udpn.recv` in JSON API):

| WLED Concept | Our Mapping | JSON API |
|---|---|---|
| Sync receive ON | ARGB passthrough active | `{"udpn":{"recv":true}}` |
| Sync receive OFF | WLED effects/preset active | `{"udpn":{"recv":false}}` |
| Realtime (DDP/E1.31) | Overrides both (standard priority) | `{"lor":0}` |

The sync toggle in the WLED dashboard UI controls passthrough on/off.
No new UI needed — just repurpose the existing sync controls.

## Priority Chain (existing WLED pattern, unchanged)

```
1. DDP/E1.31/Art-Net realtime  (highest — realtimeLock takes over)
2. ARGB passthrough            (our new mode — same as UDP sync receive)
3. Manual control / preset     (normal WLED operation)
4. Boot default (orange)       (lowest — only if nothing else configured)
```

DDP from OpenRGB ALWAYS wins over passthrough. This is correct behavior:
when the user actively controls LEDs from OpenRGB, motherboard data stops.
When OpenRGB stops sending, passthrough resumes (realtime timeout expires).

## realtimeOverride Integration

WLED's `lor` (Live Override) setting:
- `REALTIME_OVERRIDE_NONE (0)`: realtime data (DDP) takes priority (default)
- `REALTIME_OVERRIDE_ONCE (1)`: next local change overrides realtime once
- `REALTIME_OVERRIDE_ALWAYS (2)`: local effects always override realtime

For passthrough:
- `lor=0`: DDP overrides passthrough (correct for OpenRGB use)
- `lor=2`: preset/effect overrides passthrough (user explicitly wants effects)

## Implementation Changes

### wled_argb_passthrough.cpp — Add boot delay

```cpp
#ifndef ARGB_BOOT_DELAY_MS
#define ARGB_BOOT_DELAY_MS 5000  // 5 seconds of POST visibility
#endif

static uint32_t argb_boot_delay_end = 0;
static bool argb_boot_delay_active = false;

void startARGBPassthrough() {
    realtimeLock(UINT32_MAX, REALTIME_MODE_ARGB_PASSTHROUGH);
    argb_boot_delay_active = true;
    argb_boot_delay_end = millis() + ARGB_BOOT_DELAY_MS;
}

void handleARGBBootDelay() {
    if (!argb_boot_delay_active) return;
    if (millis() < argb_boot_delay_end) return;
    
    argb_boot_delay_active = false;
    
    // Boot delay expired — check if preset should take over
    if (bootPreset > 0) {
        exitRealtime();  // release passthrough lock
        applyPreset(bootPreset, CALL_MODE_INIT);
        // Preset applied — LEDs transition to preset via transitionDelay
    }
    // If no bootPreset: stay in passthrough (realtimeLock still held)
}
```

### wled.cpp — Move preset application AFTER passthrough + delay

Current boot order:
1. `beginStrip()` → applies bootPreset immediately
2. `initPPP()`
3. `initARGBPassthrough()` + `startARGBPassthrough()`

**Problem**: bootPreset is applied BEFORE passthrough starts. The preset
overrides passthrough, so POST colors are never visible.

**Fix**: When ARGB passthrough is enabled, DEFER bootPreset application:
```cpp
// In beginStrip(), change:
if (bootPreset > 0) {
    #ifdef WLED_ENABLE_ARGB_PASSTHROUGH
    // Don't apply boot preset yet — passthrough delay will handle it
    #else
    applyPreset(bootPreset, CALL_MODE_INIT);
    #endif
}
```

Then in the main loop, `handleARGBBootDelay()` applies the preset after the delay.

### wled_ppp.cpp — Remove passthrough start/stop from PPP events

Current (WRONG):
```cpp
if (event_id == IP_EVENT_PPP_GOT_IP) {
    ppp_connected = true;
    stopARGBPassthrough();  // ← REMOVE THIS
}
if (event_id == IP_EVENT_PPP_LOST_IP) {
    ppp_connected = false;
    startARGBPassthrough(); // ← REMOVE THIS
}
```

New (PPP is transport only):
```cpp
if (event_id == IP_EVENT_PPP_GOT_IP) {
    ppp_connected = true;
    // PPP is just transport — does NOT change LED mode
}
if (event_id == IP_EVENT_PPP_LOST_IP) {
    ppp_connected = false;
    // PPP is just transport — does NOT change LED mode
}
```

### Sync toggle integration

Hook passthrough into WLED's sync receive toggle. When user changes
`receiveGroups` via JSON API or UI:

```cpp
// In json.cpp, after receiveGroups is updated:
#ifdef WLED_ENABLE_ARGB_PASSTHROUGH
if (receiveGroups != 0 && !isARGBPassthroughActive()) {
    startARGBPassthrough();
} else if (receiveGroups == 0 && isARGBPassthroughActive()) {
    stopARGBPassthrough();
}
#endif
```

## Build Flags

```ini
-D WLED_ENABLE_ARGB_PASSTHROUGH
-D ARGB_RX_PIN=32        # or 36 for Pico variant
-D ARGB_MAX_LEDS=100
-D ARGB_BOOT_DELAY_MS=5000  # 5 seconds POST visibility
```

## User Experience

### Scenario 1: Boot with no preset, no PPP
- LEDs mirror motherboard colors during entire boot
- POST red → amber → rainbow → stays in passthrough forever
- User connects USB → opens wled.local → manually sets effect

### Scenario 2: Boot with preset, no PPP  
- t=0: LEDs mirror motherboard POST colors
- t=5s: Smooth transition to boot preset effect
- User connects USB → dashboard shows preset running

### Scenario 3: Boot with preset + PPP auto-start
- t=0: LEDs mirror motherboard POST colors
- t=2s: PPP connects (network available, dashboard ready)
- t=5s: Boot preset applies, LEDs transition to preset
- User opens wled.local → dashboard shows preset + sync toggle

### Scenario 4: OpenRGB realtime control
- User running OpenRGB → DDP packets arrive
- realtimeLock(DDP) overrides passthrough OR preset
- OpenRGB stops → realtime timeout → previous mode resumes
  (passthrough if sync on, preset/effect if sync off)

## Web UI Configuration

The boot delay timer is a runtime setting, not just a compile-time flag.
Stored in `cfg.json` alongside other sync settings, editable in the
Sync Settings page (`/settings/sync`).

### WLED Config Pattern (cfg.cpp)

Follows the existing pattern for sync settings:

```cpp
// wled.h — add global
WLED_GLOBAL uint16_t argbBootDelay _INIT(50);  // x100ms, so 50 = 5000ms = 5 seconds

// cfg.cpp — deserialize (read from cfg.json)
CJSON(argbBootDelay, if_sync_recv["abd"]); // abd = ARGB Boot Delay

// cfg.cpp — serialize (write to cfg.json)
if_sync_recv["abd"] = argbBootDelay;
```

JSON path in cfg.json: `if.sync.recv.abd`
Units: centiseconds (x100ms) — matches WLED's `transitionDelay` convention.
Range: 0-600 (0 = no delay, 600 = 60 seconds)
Default: 50 (5 seconds)

### Sync Settings Page Integration

Add to `data/settings_sync.htm` in the "Receive" section, alongside
the existing receive group checkboxes:

```html
ARGB Boot Delay: <input type="number" name="BD" min="0" max="600" 
  value="50"> x100ms (0=disabled)
<br><i>Seconds to show motherboard POST colors before boot preset loads</i>
```

### set.cpp — Handle form submission

```cpp
// In handleSettingsSet(), sync section:
t = request->arg(F("BD")).toInt();
if (t >= 0 && t <= 600) argbBootDelay = t;
```

### JSON API Access

Query: `GET /json/state` includes `udpn.abd`
Set: `POST /json {"udpn":{"abd":50}}`

### Implementation Priority

This is a cfg.cpp + set.cpp + settings_sync.htm change.
The ARGB passthrough code reads `argbBootDelay * 100` at runtime
instead of the compile-time `ARGB_BOOT_DELAY_MS`.
