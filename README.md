# mp_lcd_bus (lcd_bus)

MicroPython User C Module providing non-blocking DMA-backed LCD display bus objects for ESP32.

Import name: `lcd_bus`

```python
import lcd_bus
```

## Bus Types

| Type | Backend | DMA | Async |
|---|---|---|---|
| `lcd_bus.SPIBus` | `spi_device_queue_trans` | ✅ | ✅ 4-deep |
| `lcd_bus.I80Bus` | `esp_lcd_i80` | ✅ | ✅ 4-deep |
| `lcd_bus.RGBBus` | `esp_lcd_rgb` | ✅ | ✅ 2-deep |
| `lcd_bus.I2CBus` | `esp_lcd_i2c` | ❌ | ❌ blocking |

## Unified API

All buses expose the same methods:

| Method | SPI | I2C | I80 | RGB |
|---|---|---|---|---|
| `write(buf)` → `trans_id` | ✅ async | ✅ | ✅ async | ✅ async |
| `readinto(buf, write_val=0)` → `trans_id` | ✅ async | ✅ | ❌ | ❌ |
| `write_readinto(wbuf, rbuf)` → `trans_id` | ✅ async | ❌ | ❌ | ❌ |
| `is_busy()` → `bool` | ✅ | ✅ | ✅ | ✅ |
| `pending()` → `int` | ✅ | ✅ | ✅ | ✅ |
| `wait(trans_id, timeout_ms=-1)` | ✅ | ✅ | ✅ | ✅ |
| `wait_all(timeout_ms=-1)` | ✅ | ✅ | ✅ | ✅ |
| `lane_count` (property) | ✅ | ✅ | ✅ | ✅ |
| `deinit()` | ✅ | ✅ | ✅ | ✅ |

## Design Principles

- **DC/CS not managed by bus** — upper panel driver is responsible
- **Auto-detect lane count from `data` tuple** — no mode flags needed
- **write / readinto / write_readinto all async** — return `trans_id`, check with `wait()`
- **GC-safe** — `ref_bufs[]` holds buffer reference, released on `wait()`

## Constructors

### SPIBus

```python
lcd_bus.SPIBus(data, clk, *, freq=40_000_000, host=1)
```

`data` tuple length determines lane count:

| len(data) | Mode |
|---|---|
| 1 | Standard SPI |
| 2 | Dual SPI |
| 4 | Quad SPI |
| 8 | Octal SPI |

```python
bus = lcd_bus.SPIBus(data=(35,), clk=36)                      # 1-lane
bus = lcd_bus.SPIBus(data=(35, 36, 37, 38), clk=39)           # Quad 4-lane
bus = lcd_bus.SPIBus(data=(35,36,37,38,39,40,41,42), clk=43)  # Octal 8-lane
```

### I2CBus

```python
lcd_bus.I2CBus(data, clk, addr, *, freq=10_000_000)
```

```python
bus = lcd_bus.I2CBus(data=(21,), clk=22, addr=0x3C)
```

### I80Bus (ESP32-S3/P4 only)

```python
lcd_bus.I80Bus(data, wr, *, cs=-1, freq=10_000_000)
```

`data` must be 8 or 16 pins:

```python
bus = lcd_bus.I80Bus(data=(d0,d1,d2,d3,d4,d5,d6,d7), wr=10)  # 8-bit
bus = lcd_bus.I80Bus(data=(d0..d15), wr=10)                    # 16-bit
```

### RGBBus (ESP32-S3/P4 only)

```python
lcd_bus.RGBBus(data, hsync, vsync, de, pclk, width, height, *,
               freq=8_000_000, disp=-1,
               hsync_front_porch=0, hsync_back_porch=0,
               hsync_pulse_width=1, hsync_idle_low=False,
               vsync_front_porch=0, vsync_back_porch=0,
               vsync_pulse_width=1, vsync_idle_low=False,
               de_idle_high=False, pclk_idle_high=False,
               pclk_active_neg=False, disp_active_low=False,
               refresh_on_demand=False, bb_size_px=0)
```

```python
bus = lcd_bus.RGBBus(
    data=(0,1,2,3,4,5,6,7),
    hsync=12, vsync=13, de=14, pclk=15,
    width=480, height=272,
)
```

## Usage Pattern

```python
import lcd_bus
import heap_caps
from machine import Pin

dc = Pin(37, Pin.OUT)
cs = Pin(38, Pin.OUT)

bus = lcd_bus.SPIBus(data=(35,), clk=36, freq=40_000_000)

fb = heap_caps.malloc(320 * 240 * 2, heap_caps.CAP_DMA)

cs.low()
dc.low()
bus.write(b'\x11')
bus.wait_all()

dc.high()
tid = bus.write(fb)         # fire-and-forget
while bus.is_busy():        # CPU does other work
    read_sensor()
bus.wait(tid)               # confirm done

bus.deinit()
heap_caps.free(fb)
```

## Double Buffering

4-level queue (SPI/I80) or 2-level (RGB) built in:

```python
buf_a = heap_caps.malloc(FRAME, heap_caps.CAP_DMA)
buf_b = heap_caps.malloc(FRAME, heap_caps.CAP_DMA)

while True:
    render(buf_a)
    bus.write(buf_a)
    render(buf_b)
    bus.write(buf_b)
    if bus.pending() >= 2:
        bus.wait_all()
```

## Build

CMake-based (ESP-IDF): add this repo as a User C Module.

```
Sources: modlcd_bus.c, lcd_types.c,
         esp32_src/spi_bus.c, esp32_src/i2c_bus.c,
         esp32_src/i80_bus.c, esp32_src/rgb_bus.c
```

Non-ESP32 ports: stubs raise `NotImplementedError`.

## License

MIT
