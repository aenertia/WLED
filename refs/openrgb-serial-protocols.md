# OpenRGB Serial Protocol Reference

## OpenRGB Serial Device Support

OpenRGB's `LEDStripController` supports 4 serial protocols:

```c
enum {
    LED_PROTOCOL_KEYBOARD_VISUALIZER,  // 0
    LED_PROTOCOL_ADALIGHT,             // 1
    LED_PROTOCOL_TPM2,                 // 2
    LED_PROTOCOL_BASIC_I2C             // 3
};
```

### Detection: Manual Configuration Only

OpenRGB does **NOT** auto-detect serial LED devices. They are configured manually in `OpenRGB.json`:

```json
{
    "LEDStripDevices": {
        "devices": [
            {
                "name": "WLED USB Controller",
                "port": "/dev/ttyUSB0",
                "baud": 115200,
                "num_leds": 60,
                "protocol": "adalight"
            }
        ]
    }
}
```

Valid `protocol` values: `"keyboard_visualizer"`, `"adalight"`, `"tpm2"`, `"basic_i2c"`.

The detector opens the serial port at the specified baud rate and starts sending data — **no handshake, no discovery, no enumeration**.

## Adalight Protocol — Exact Byte Format

```
+------+------+------+----------+----------+--------------+
| 0x41 | 0x64 | 0x61 | CountHi  | CountLo  |  Checksum    |
| 'A'  | 'd'  | 'a'  | LED>>8   | LED&0xFF | Hi^Lo^0x55   |
+------+------+------+----------+----------+--------------+
| R0 G0 B0 | R1 G1 B1 | ... | Rn Gn Bn                   |
+----------------------------------------------------------+
```

- **Magic bytes**: `0x41 0x64 0x61` ("Ada")
- **LED count**: 16-bit big-endian. OpenRGB sends **raw count** (N)
- **Checksum**: `count_hi XOR count_lo XOR 0x55`
- **Payload**: RGB triplets, 3 bytes per LED, R-G-B order
- **Total packet**: `6 + (num_leds * 3)` bytes

### WLED Adalight Count Difference

WLED adds 1 to the low byte value in its parser:

```cpp
case AdaState::Header_CountLo:
    count += next + 1;  // +1 here
```

OpenRGB sends raw count N; original Adalight spec sends N-1. In practice this works because WLED clamps to its configured LED count via `setRealtimePixel()`.

### Throughput at Various Baud Rates

| Baud Rate | Bytes/sec | Max LEDs @ 30fps | Max LEDs @ 60fps |
|---|---|---|---|
| 115200 | 11,520 | ~128 | ~64 |
| 230400 | 23,040 | ~256 | ~128 |
| 460800 | 46,080 | ~512 | ~256 |
| 921600 | 92,160 | ~1024 | ~512 |
| 1000000 | 100,000 | ~1111 | ~555 |
| 1500000 | 150,000 | ~1666 | ~833 |

Formula: `max_leds = (baud_rate / 10) / (3 * fps)` (10 bits per byte with start/stop)

## TPM2 Serial Protocol — Exact Byte Format

```
+------+------+----------+----------+------------+------+
| 0xC9 | 0xDA | SizeHi   | SizeLo   | RGB Data   | 0x36 |
| Start| Data | payload  | size     | R G B ...  | End  |
+------+------+----------+----------+------------+------+
```

- **Start byte**: `0xC9`
- **Type byte**: `0xDA` (data frame), `0xC0` (command), `0xAA` (ping)
- **Payload size**: 16-bit big-endian, in **bytes** (= num_leds × 3)
- **End byte**: `0x36`
- **Ping**: Send `0xC9 0xAA`, expect `0xAC` back

## Keyboard Visualizer Protocol

```
+------+------------+----------+----------+
| 0xAA | RGB Data   | Chk MSB  | Chk LSB  |
| Start| R G B ...  | sum>>8   | sum&0xFF |
+------+------------+----------+----------+
```

Checksum = sum of all bytes from 0xAA through last data byte.

## OpenRGB WLED Network Integration (DDP)

OpenRGB has **no dedicated WLED controller**. It uses **DDP (Distributed Display Protocol)** over UDP:

```json
{
    "DDPDevices": {
        "devices": [
            {
                "name": "WLED Strip",
                "ip": "192.168.1.100",
                "port": 4048,
                "num_leds": 60
            }
        ]
    }
}
```

Also supports E1.31/sACN. Both are network-only (UDP).

## CorsairLightingProtocol Library

### USB Descriptor: HID, NOT CDC Serial

Presents as USB HID with Corsair VID/PID:

```c
#define CORSAIR_VID 0x1B1C
#define CORSAIR_LNP_PID 0x0C0B   // Lighting Node PRO
#define CORSAIR_CP_PID 0x0C10    // Commander PRO
#define CORSAIR_LNC_PID 0x0C1A   // Lighting Node CORE
```

Protocol: 64-byte HID OUT reports (commands), 16-byte HID IN reports (responses). Vendor-specific HID, not CDC serial.

### ESP32 Compatibility: NOT COMPATIBLE (standard ESP32)

Supported architectures: `avr, samd, rp2040`

**Why standard ESP32 won't work:**
1. USB goes through CP2102/CH340/FTDI UART bridge → CDC serial port, not native USB HID
2. Cannot present custom USB descriptors through a UART bridge
3. ESP32-S2/S3 with native USB-OTG *might* work via TinyUSB backend but untested

### CorsairLightingProtocolSerial (Bridge Mode)

A polling protocol for two-Arduino setups:
- Slave sends `'*'` (0x2A) to request data
- Master (HID Arduino) responds with 64-byte command blocks
- Runs at 1 Mbaud
- **Not useful for standalone USB serial controller**

## Feasibility Matrix

### Option A: Adalight over USB Serial — RECOMMENDED

| Aspect | Assessment |
|---|---|
| Compatibility | Any ESP32 variant (UART bridge or native USB) |
| OpenRGB support | Native — `"protocol": "adalight"` in settings |
| WLED support | Already implemented in `wled_serial.cpp` |
| Implementation effort | Minimal — WLED does this out of the box |
| Detection | Manual config (port + baud + LED count) |
| Max throughput | ~500 LEDs @ 30fps at 921600 baud |
| Bidirectional | One-way (host → device) for pixel data |

### Option B: TPM2 over USB Serial — Also Works

| Aspect | Assessment |
|---|---|
| Compatibility | Same as Adalight |
| OpenRGB support | Native — `"protocol": "tpm2"` |
| WLED support | Already in same state machine |
| Advantage | Slightly more robust framing (start/end bytes) |
| Disadvantage | Less widely used |

### Option C: Corsair Emulation — NOT RECOMMENDED

| Aspect | Assessment |
|---|---|
| Requires | Native USB (ESP32-S2/S3 only, NOT standard ESP32) |
| M5StickC | **Impossible** — uses FTDI UART bridge, not native USB |
| Max LEDs | 204 per device (Corsair protocol limit) |
| Implementation | High effort — full Corsair HID protocol |
| Advantage | Auto-detected by OpenRGB/iCUE |

### Option D: DDP over WiFi — Already Works (network only)

| Aspect | Assessment |
|---|---|
| Compatibility | Any ESP32 with WiFi |
| OpenRGB support | Native — DDPDevices config |
| WLED support | Built-in DDP receiver |
| Advantage | No USB cable, higher throughput |
| Disadvantage | Requires WiFi, adds latency, not USB-serial |

## Recommended OpenRGB Configuration

```json
{
    "LEDStripDevices": {
        "devices": [{
            "name": "WLED M5StickC USB",
            "port": "/dev/ttyUSB0",
            "baud": 500000,
            "num_leds": 60,
            "protocol": "adalight"
        }]
    }
}
```

On Windows, replace `"/dev/ttyUSB0"` with `"COM3"` (or whichever port the FTDI driver assigns).

## Baud Rate Switching

WLED supports runtime baud rate switching via single-byte commands:

```python
import serial
ser = serial.Serial('/dev/ttyUSB0', 115200)
ser.write(b'\xB3')  # Switch to 500000
ser.close()
# Reconnect at new baud
```
