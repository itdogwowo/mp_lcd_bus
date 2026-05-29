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
| `write(buf, *, cmd=, addr=, multiline=)` → `None` | ✅ sync | ❌ | ❌ | ❌ |
| `readinto(buf, write_val=0)` → `trans_id` | ✅ async | ✅ | ❌ | ❌ |
| `write_readinto(wbuf, rbuf)` → `trans_id` | ✅ async | ❌ | ❌ | ❌ |
| `is_busy()` → `bool` | ✅ | ✅ | ✅ | ✅ |
| `pending()` → `int` | ✅ | ✅ | ✅ | ✅ |
| `wait(trans_id, timeout_ms=-1)` → `bool` | ✅ | ✅ | ✅ | ✅ |
| `wait_all(timeout_ms=-1)` → `None` | ✅ | ✅ | ✅ | ✅ |
| `lane_count()` → `int` | ✅ | ✅ | ✅ | ✅ |
| `deinit()` | ✅ | ✅ | ✅ | ✅ |

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
| 4 | Quad SPI (QSPI) |
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

---

## write(buf, *, cmd=-1, addr=0, multiline=True)

Two modes determined by `cmd`:

| | DMA mode `write(buf)` | Polling mode `write(buf, cmd=...)` |
|---|---|---|
| Trigger | `cmd` not passed (default -1) | `cmd >= 0` |
| Transfer | `spi_device_queue_trans` (hardware DMA) | `spi_device_polling_transmit` (blocking) |
| Returns | `trans_id` (int) | `None` |
| cmd/addr phase | None | Yes (8-bit cmd + 24-bit addr) |
| Async? | Async, returns immediately | Sync, returns after completion |
| Queue depth | Up to 4 (`SPI_DMA_QUEUE_DEPTH`) | No queue |
| Use case | Bulk pixel data | LCD commands, pixel preamble |

**Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `buf` | buffer | required | Data to send, can be empty `b''` |
| `cmd` | int | -1 | QSPI command byte; >=0 triggers polling mode |
| `addr` | int | 0 | QSPI address (24-bit), e.g. `cmd<<8` or `0x002C00` |
| `multiline` | bool | True | `True`=cmd/addr on 4 lines, `False`=cmd/addr on D0 only |

**Polling mode details:**
- Calls `spi_drain_pending()` first to flush DMA queue and preserve ordering
- Uses `spi_transaction_ext_t` + `SPI_TRANS_VARIABLE_CMD|SPI_TRANS_VARIABLE_ADDR`
- Per-transaction `command_bits=8, address_bits=24` (device config unchanged)
- `multiline=True` → adds `MULTILINE_CMD|MULTILINE_ADDR` (cmd/addr 4-line)
- `multiline=False` → cmd/addr on D0 only (1-line, no MULTILINE flags)
- Empty `buf` (`b''`) → sends only cmd+addr phases, no data phase

### QSPI Display Example (RM67162)

```python
cs = Pin(6, Pin.OUT, value=1)

def tx_param(cmd, params=b''):
    cs.low()
    bus.write(params if params else b'\x00', cmd=0x02, addr=cmd << 8)
    cs.high()

def tx_color_pre():
    cs.low()
    bus.write(b'', cmd=0x32, addr=0x002C00, multiline=False)
    # CS stays low across all pixel data chunks
    cs.high()

# Init sequence
tx_param(0x11)           # SLPOUT
time.sleep_ms(120)
tx_param(0x36, b'\x00')  # MADCTL
tx_param(0x36, b'\x00')  # MADCTL (sent twice per RM67162 spec)
tx_param(0x3A, b'\x75')  # COLMOD 16bpp
tx_param(0x29)           # DISPON

# Send pixel data (CS low across preamble + all chunks)
cs.low()
bus.write(b'', cmd=0x32, addr=0x002C00, multiline=False)
for chunk in chunks:
    tid = bus.write(chunk)
    bus.wait(tid)
cs.high()
```

---

## DMA Queue Usage Patterns

### Pattern 1: write + wait (safe, buffer reuse)

```python
for chunk in chunks:
    tid = bus.write(chunk)    # DMA starts reading chunk
    bus.wait(tid)             # wait → chunk safe to overwrite
```

### Pattern 2: Fire-and-forget multiple (independent buffers)

```python
bus.write(buf_a)   # DMA enqueue
bus.write(buf_b)   # DMA enqueue
bus.write(buf_c)   # DMA enqueue
bus.wait_all()     # wait for all
# ⚠️ buf_a/buf_b/buf_c must not be overwritten until wait_all
```

### Pattern 3: Non-blocking CPU parallel

```python
tid = bus.write(chunk)      # fire 32KB, returns immediately
calculate_something()        # CPU works in parallel
bus.wait(tid)               # wait for DMA
```

### Pattern 4: Wait for specific transaction

```python
tid_a = bus.write(chunk_a)
tid_b = bus.write(chunk_b)
bus.wait(tid_a)             # wait only for chunk_a
bus.wait(tid_b)
```

### Queue Full

5th write raises `RuntimeError("queue full")` immediately (does not block):

```python
try:
    for _ in range(5):
        bus.write(bytearray(64))
except RuntimeError as e:
    print(e)  # "queue full"
```

### Queue Depth

`SPI_DMA_QUEUE_DEPTH = 4` is not a hardware DMA limit — it's a memory optimization (~28 bytes/slot). Adjust in `esp32_include/spi_bus.h`. 4 is sufficient for the common `write → wait` serial pattern.

### Non-blocking Check

```python
done = bus.wait(tid, timeout_ms=0)   # True=done, False=still in-flight
bus.is_busy()                         # True=any pending
bus.pending()                         # count of pending (0~4)
```

---

## Standard SPI Example

```python
import lcd_bus
from machine import Pin

dc = Pin(37, Pin.OUT)
cs = Pin(38, Pin.OUT)

bus = lcd_bus.SPIBus(data=(35,), clk=36, freq=40_000_000)

cs.low()
dc.low()
bus.write(b'\x11')
bus.wait_all()
dc.high()

tid = bus.write(fb)         # fire-and-forget
while bus.is_busy():        # CPU does other work
    read_sensor()
bus.wait(tid)               # confirm done
cs.high()

bus.deinit()
```

## Testing

Run on ESP32 device:

```python
import test_bus
test_bus.run_all()
```

Test suite (7 areas, 23 assertions):

| # | Test | Description | Assertions |
|---|------|-------------|------------|
| 1 | `test_spi` | SPI init, write, 4-deep queue, queue-full guard, deinit | 11 |
| 2 | `test_spi_multilane` | Auto-detect 1/2/4/8-lane from `data` tuple | 4 |
| 3 | `test_spi_official` | Full-duplex (sck/mosi/miso), `readinto()` | 5 |
| 4 | `test_speed` | Throughput benchmarks (1/2/4 lanes, 40/80 MHz) | output only |
| 5 | `test_i80` | I80 parallel bus (SKIP if not available) | 0 |
| 6 | `test_rgb` | RGB parallel bus (SKIP if not available) | 0 |
| 7 | `test_rapid` | Rapid init→write→deinit stress (3 cycles) | 3 |

## Build

CMake-based (ESP-IDF): add this repo as a User C Module.

Non-ESP32 ports: stubs raise `NotImplementedError`.

## License

MIT
