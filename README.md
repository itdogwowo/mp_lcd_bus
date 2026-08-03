# mp_lcd_bus (lcd_bus)

MicroPython User C Module providing non-blocking DMA-backed LCD display bus objects for ESP32.

Import name: `lcd_bus`

```python
import lcd_bus
```

## Bus Types

| Type | Backend | DMA | Async |
|---|---|---|---|---|
| `lcd_bus.SPIBus` | `spi_device_queue_trans` | ✅ | ✅ 8-deep |
| `lcd_bus.I80Bus` | `esp_lcd_i80` | ✅ | ✅ 4-deep |
| `lcd_bus.RGBBus` | `esp_lcd_rgb` | ✅ | ✅ 2-deep |
| `lcd_bus.DSIBus` | `esp_lcd_mipi_dsi` (ESP32-P4 only) | ✅ | ✅ 4-deep |
| `lcd_bus.I2CBus` | `esp_lcd_i2c` | ❌ | ❌ blocking |

## Off-Bus Control Pins

All control pins are **optional** (default `-1` = not managed). Pass a GPIO number to let C/esp_lcd manage it, or leave `-1` to control it externally with `machine.Pin`.

| Bus | Pin | Managed by | Default | Notes |
|---|---|---|---|---|
| `SPIBus` | dc/cs | — (external only) | — | Pure SPI bus; dc/cs handled by user/adapter |
| `I80Bus` | `dc` | esp_lcd | `-1` | **Strongly recommended** — I80 needs dc for cmd/data switching |
| `I80Bus` | `cs` | esp_lcd | `-1` | |
| `RGBBus` | `disp` | esp_lcd | `-1` | Display enable |
| `DSIBus` | `rst` | C module | `-1` | esp_lcd DPI panel has no reset_gpio support |

## Unified API

All buses expose the same methods:

| Method | SPI | I2C | I80 | RGB | DSI |
|---|---|---|---|---|---|
| `write(buf)` → `trans_id` | ✅ async | ✅ | ✅ async | ✅ async | ✅ async |
| `write(buf, *, cmd=, addr=, multiline=)` → `None` | ✅ sync | ❌ | ❌ | ❌ | ❌ |
| `readinto(buf, write_val=0)` → `trans_id` | ✅ async | ✅ | ❌ | ❌ | ❌ |
| `write_readinto(wbuf, rbuf)` → `trans_id` | ✅ async | ❌ | ❌ | ❌ | ❌ |
| `cmd(cmd, param=b'')` → `None` | ❌ | ❌ | ❌ | ❌ | ✅ sync |
| `is_busy()` → `bool` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `pending()` → `int` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `wait(trans_id, timeout_ms=-1)` → `bool` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `wait_all(timeout_ms=-1)` → `None` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `lane_count()` → `int` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `frame_buffer(idx)` → `bytearray` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `flush(idx=0, *, x=0, y=0, w=0, h=0)` → `None` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `set_pattern(pat)` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `deinit()` | ✅ | ✅ | ✅ | ✅ | ✅ |

## Constructors

### SPIBus

```python
lcd_bus.SPIBus(data, clk, *, freq=40_000_000, host=1, queue_depth=8)
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
lcd_bus.I80Bus(data, wr, *, cs=-1, freq=10_000_000, queue_depth=4)
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
               refresh_on_demand=False, bb_size_px=0, queue_depth=2)
```

### DSIBus (ESP32-P4 only)

```python
lcd_bus.DSIBus(lanes, width, height, lane_bit_rate_mbps, *,
               dpi_clk_mhz=30.0,
               hsync_pulse_width=1, hsync_back_porch=10, hsync_front_porch=10,
               vsync_pulse_width=1, vsync_back_porch=10, vsync_front_porch=10,
               hsync_idle_pixel=0, vsync_idle_line=0,
               in_color_format=16, fb_count=2, rst=-1,
               virtual_channel=0,
               cmd_bits=8, param_bits=8,
               use_dma2d=True, queue_depth=4)
```

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `lanes` | int | required | Number of MIPI DSI data lanes (1-4) |
| `width`, `height` | int | required | Panel resolution |
| `lane_bit_rate_mbps` | float | required | DSI PHY lane bit rate in Mbps (e.g. 1000) |
| `dpi_clk_mhz` | float | 30.0 | Pixel clock (DPI) frequency in MHz |
| `hsync/vsync_*` | int | 1/10/10 | Video timing (porches & pulse width, in px/lines) |
| `hsync_idle_pixel`, `vsync_idle_line` | int | 0 | Idle blanking period in the timing (some panels require non-zero) |
| `in_color_format` | int | 16 | `16` = RGB565, `24` = RGB888 |
| `fb_count` | int | 2 | Number of internal frame buffers (1-3), allocated in PSRAM by the driver |
| `rst` | int | -1 | Panel hardware reset GPIO (`-1` = not used) |
| `virtual_channel` | int | 0 | DSI virtual channel (0-3) |
| `cmd_bits`, `param_bits` | int | 8 | DBI command/parameter bit width (rare panels use other widths) |
| `use_dma2d` | bool | True | `True` = copy via DMA2D, `False` = fall back to CPU copy |
| `queue_depth` | int | 4 | Async write pipeline depth (1-8); more slots = more memory, deeper pipelining |

> **All panel-specific parameters are configurable from Python** — nothing in the
> C module needs editing to bring up a different panel. Panel init commands
> (`cmd()` sequence), reset/backlight pins, and the DSI PHY LDO are handled at
> the Python layer too (see `test_dsi.py`).

The driver allocates screen-sized frame buffers internally; `frame_buffer(idx)` returns a zero-copy `bytearray` view of one. Writes are asynchronous — the bus copies the buffer into a frame buffer, and `write()` returns a `trans_id` you can `wait()` on:

```python
bus = lcd_bus.DSIBus(lanes=2, width=800, height=480,
                     lane_bit_rate_mbps=1000, rst=20)

bus.cmd(0x11)                    # SLPOUT (no params)
bus.cmd(0x36, b'\x00')           # MADCTL
bus.cmd(0x3A, b'\x70')           # COLMOD RGB565
bus.cmd(0x29)                    # DISPON

tid = bus.write(fb_bytes)        # async copy into frame buffer
bus.wait(tid)

fb = bus.frame_buffer(0)         # zero-copy view of internal fb
memoryview(fb)[:2] = b'\xf8\x00' # draw directly into the framebuffer
bus.flush()                      # ⚠ write dirty L2 cache lines back to PSRAM

bus.set_pattern(1)               # built-in test pattern (0=none,1=ver bar,2=hor bar,3=BER)
bus.set_pattern(0)               # back to normal
```

### DSIBus frame buffer writes & double buffering (ESP32-P4)

**⚠️ `flush()` is mandatory after direct writes through `frame_buffer(idx)`.**

On ESP32-P4 the frame buffers live in PSRAM. CPU writes go through the
write-back L2 cache, but the DPI DMA reads PSRAM **directly** (it bypasses the
cache). Without `esp_cache_msync()` the DMA keeps reading stale, not-yet
written-back lines — visible as ghosting / black bands / flicker. `flush()`
writes the dirty cache lines of the given region back to PSRAM (same operation
the esp_lcd driver does internally), so always call it after drawing into a
`frame_buffer()` view:

```python
fb = bus.frame_buffer(0)
fill_screen(fb, color)   # CPU writes into PSRAM (cached)
bus.flush()              # write-back before the DPI DMA sees it
```

**Tearing rules (`fb_count` semantics):**

| fb_count | `write(buf)` with external buffer | Direct `frame_buffer()` writes |
|---|---|---|
| 1 | DMA2D copies into the *displayed* fb → tearing during the copy (~ms) | Tearing while writing the scanned buffer; inherent to single buffering |
| 2+ | Same as above — external buffers always land on the *displayed* fb | Tear-free only when using the swap pattern below |

**Tear-free double buffering** (works with `fb_count=2`): the esp_lcd P4 driver
keeps one DMA link list per fb and switches to the fb you pass to
`draw_bitmap()` **at the frame boundary** — but only when the draw buffer *is*
an internal fb (no copy, just cache write-back + swap). So draw the full frame
into the fb that is **not** currently displayed, then hand that fb view to
`write()`:

```python
fb0 = bus.frame_buffer(0)
fb1 = bus.frame_buffer(1)

while True:
    draw_full_frame(fb1, frame_a)   # fb0 is displayed, draw into fb1
    bus.write(fb1); bus.wait_all()  # msync + switch at next frame boundary
    draw_full_frame(fb0, frame_b)   # fb1 is displayed, draw into fb0
    bus.write(fb0); bus.wait_all()
```

Rules: always update the **whole** frame (or keep both fbs in sync), because
after the switch the un-drawn region shows that fb's old content. Partial
updates with multiple fbs will flip between stale contents every frame.

For reference, `test_dsi.py` covers all three paths: direct writes + `flush()`
(colors/gradient), async `write()` with external buffers, and the tear-free
double-buffer swap demo.

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
| Queue depth | Up to `queue_depth` (default 8) | No queue |
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

9th write raises `RuntimeError("queue full")` immediately (does not block) — depth 8:

```python
try:
    for _ in range(9):
        bus.write(bytearray(64))
except RuntimeError as e:
    print(e)  # "queue full"
```

### Queue Depth

`queue_depth` is not a hardware DMA limit — it's a memory optimization (~28 bytes/slot). It is configurable per bus through the constructor (range 1-8): `SPIBus(..., queue_depth=8)`, `I80Bus(..., queue_depth=4)`, `RGBBus(..., queue_depth=2)`, `DSIBus(..., queue_depth=4)`. 8 is sufficient for the common `write → wait` serial pattern.

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
| 1 | `test_spi` | SPI init, write, queue-depth queue, queue-full guard, deinit | 11 |
| 2 | `test_spi_multilane` | Auto-detect 1/2/4/8-lane from `data` tuple | 4 |
| 3 | `test_spi_official` | Full-duplex (sck/mosi/miso), `readinto()` | 5 |
| 4 | `test_speed` | Throughput benchmarks (1/2/4 lanes, 40/80 MHz) | output only |
| 5 | `test_i80` | I80 parallel bus (SKIP if not available) | 0 |
| 6 | `test_rgb` | RGB parallel bus (SKIP if not available) | 0 |
| 7 | `test_rapid` | Rapid init→write→deinit stress (3 cycles) | 3 |

## Build

CMake-based (ESP-IDF): add this repo as a User C Module.

`DSIBus` is compiled into every ESP32 build, but the real MIPI DSI driver code inside
`esp32_src/dsi_bus.c` is guarded by `#if SOC_MIPI_DSI_SUPPORTED`, which is only defined
on the ESP32-P4. On other ESP32 chips (e.g. ESP32-S3) it compiles down to a stub that
raises `NotImplementedError`, so no per-chip build configuration is needed.

Non-ESP32 ports: stubs raise `NotImplementedError`.

### DSI Build Requirements (ESP32-P4)

The MIPI DSI DPI panel continuously streams pixel data from a frame buffer in PSRAM.
Without sufficient PSRAM bandwidth, the DPI controller reports underrun errors
(`can't fetch data from external memory fast enough`) and the screen flickers.

When building with **mp_Make-Tools**, the required sdkconfig keys are declared in
[`sdkconfig.require.json`](sdkconfig.require.json) and injected automatically based
on the target chip — no manual sdkconfig editing needed.

| Key | Why |
|---|---|
| `CONFIG_SPIRAM_SPEED_200M` | PSRAM at 200 MHz (default is 80 MHz, too slow for DSI) |
| `CONFIG_CACHE_L2_CACHE_256KB` | L2 cache for efficient DMA reads from PSRAM |
| `CONFIG_CACHE_L2_CACHE_LINE_128B` | L2 cache line size (must match) |
| `CONFIG_SPIRAM_XIP_FROM_PSRAM` | Execute code from PSRAM, freeing internal RAM |

If building without mp_Make-Tools, add these keys to your `sdkconfig.defaults` manually.

## License

MIT
