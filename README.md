# mp_lcd_bus (lcd_bus)

MicroPython User C Module that provides LCD panel I/O “bus” objects for driving displays from MicroPython. It exposes a small, unified Python API across multiple physical buses (SPI, I80/8080, I2C, RGB), with an ESP32 implementation backed by ESP-IDF `esp_lcd` and a generic fallback implementation for non-ESP32 ports.

Import name in MicroPython:

```python
import lcd_bus
```

## Project Summary

This module implements MicroPython objects that encapsulate:

- Bus allocation / initialization (pin mapping, bus timing, buffer sizing)
- Sending command/parameter bytes and pixel buffers (where supported)
- Optional completion callback and lane-count querying

Typical usage:

- As the “I/O layer” for a display driver (panel/controller driver lives elsewhere)
- When you need a consistent API across different boards/buses

## Features

- Provides `lcd_bus.SPIBus`, `lcd_bus.I80Bus`, `lcd_bus.I2CBus`, `lcd_bus.RGBBus` types.
- Common lifecycle:
  - Construct a bus object (stores configuration)
  - Call `init(width, height, bpp, buffer_size, rgb565_byte_swap)` to allocate/prepare underlying I/O
  - Use `tx_param` / `tx_color` / `rx_param` (availability depends on bus/platform)
  - Call `deinit()` (or rely on `__del__`) to release resources
- `get_lane_count()` returns the configured/active lane count (implementation-defined per bus).
- `register_callback(cb)` installs a completion callback used by DMA/transfer completion paths.
- Platform-aware build:
  - ESP32 (`ESP_PLATFORM` in CMake): uses `esp32_src/*` + ESP-IDF `esp_lcd`
  - Non-ESP32: uses `common_src/*` (some buses are stubs that raise `NotImplementedError`)

## API Reference

### Module: `lcd_bus`

Exports types only (no module-level functions/constants besides `__name__`):

- `lcd_bus.SPIBus`
- `lcd_bus.I2CBus`
- `lcd_bus.I80Bus`
- `lcd_bus.RGBBus`

### Common Methods (on most bus types)

These methods come from the shared locals dict in [modlcd_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/modlcd_bus.c).

#### `init(self, width: int, height: int, bpp: int, buffer_size: int, rgb565_byte_swap: bool) -> None`

Purpose:
- Initializes the underlying bus I/O and stores `rgb565_byte_swap` when `bpp == 16`.

Parameters:
- `width`, `height`: Used by the underlying implementation.
- `bpp`: Bits-per-pixel. When `bpp != 16`, `rgb565_byte_swap` is forced to `False` by the wrapper.
- `buffer_size`: Buffer sizing passed down to the bus implementation.
- `rgb565_byte_swap`: When enabled and supported, swaps byte order in-place for RGB565 buffers.

Returns:
- `None`

Failure behavior:
- Raises `OSError` if the underlying init returns a non-zero error code.
- Some ESP32 bus implementations raise `ValueError`/`OSError` directly during init (inside the bus implementation), which propagates unchanged.

#### `deinit(self) -> None` and `__del__(self) -> None`

Purpose:
- Releases underlying resources.

Returns:
- `None`

Failure behavior:
- Raises `ValueError` if the underlying delete returns a non-zero error code.

#### `get_lane_count(self) -> int`

Purpose:
- Returns the active lane count for the bus (e.g. 1/2/4 for SPI, bus width for I80/RGB).

Returns:
- Integer lane count.

Failure behavior:
- Raises `OSError` if the underlying function returns a non-zero error code.

#### `register_callback(self, callback) -> None`

Purpose:
- Sets a completion callback.

Parameters:
- `callback`: Stored as-is.

Returns:
- `None`

Failure behavior:
- No validation in the setter.
- The transfer-complete path checks `callback is not None` and `callable(callback)` before invoking it.

#### `tx_param(self, cmd: int, params: bytes|bytearray|memoryview|None = None) -> None`

Purpose:
- Sends a command + optional parameter buffer (when the underlying bus supports it).

Parameters:
- `cmd`: Integer command value.
- `params`: `None` or any readable buffer object.

Returns:
- `None`

Failure behavior:
- Raises `OSError` if the underlying function returns a non-zero error code.
- For buses that do not implement parameter transmit, the underlying layer returns “not supported” and the wrapper raises `OSError`.

#### `rx_param(self, cmd: int, data: bytearray|memoryview) -> None`

Purpose:
- Reads parameter bytes into a writable buffer (when supported).

Parameters:
- `cmd`: Integer command value.
- `data`: Writable buffer (must support `MP_BUFFER_WRITE`).

Returns:
- `None`

Failure behavior:
- Raises `OSError` if the underlying function returns a non-zero error code (including “not supported”).

#### `tx_color(self, cmd: int, data: bytes|bytearray|memoryview, x_start: int, y_start: int, x_end: int, y_end: int) -> None`

Purpose:
- Sends pixel data for a command. The coordinate arguments exist for API consistency.

Parameters:
- `cmd`: Integer command value.
- `data`: Readable buffer containing pixel data.
- `x_start/y_start/x_end/y_end`: Accepted by the wrapper; some implementations ignore them.

Returns:
- `None`

Failure behavior:
- Raises `OSError` if the underlying function returns a non-zero error code.

Blocking behavior:
- If `callback` is `None`, the wrapper busy-waits until the transfer is marked complete.

### Type: `lcd_bus.SPIBus`

Constructor (ESP32 build) from [esp32_src/spi_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/spi_bus.c):

```python
lcd_bus.SPIBus(
    dc, host, sclk, freq, mosi,
    *,
    miso=-1, cs=-1, wp=-1, hd=-1,
    quad_spi=False, tx_only=False,
    cmd_bits=8, param_bits=8,
    dc_low_on_data=False, sio_mode=False, lsb_first=False, cs_high_active=False,
    spi_mode=0,
)
```

Notes:
- `host/sclk/freq/mosi/...` are parsed as integers in the ESP32 implementation. `dc` is parsed as an object but used as an integer in the current code; pass an integer GPIO number.

Constructor (non-ESP32 build) from [common_src/spi_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/spi_bus.c):

```python
lcd_bus.SPIBus(
    dc, host, sclk, freq, mosi,
    *,
    miso=None, cs=None, wp=None, hd=None,
    quad_spi=False, tx_only=False,
    cmd_bits=8, param_bits=8,
    dc_low_on_data=False, sio_mode=False, lsb_first=False, cs_high_active=False,
    spi_mode=0,
)
```

Notes:
- The generic implementation requires `mp_hal_pin_output` support; otherwise it raises `NotImplementedError("LCD SPI but is not available for this MCU")`.
- Pin-like arguments are converted through `mp_hal_get_pin_obj(...)`.

### Type: `lcd_bus.I2CBus`

Constructor (ESP32 build) from [esp32_src/i2c_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/i2c_bus.c):

```python
lcd_bus.I2CBus(
    sda, scl, addr,
    *,
    host=0, control_phase_bytes=1, dc_bit_offset=6, freq=10_000_000,
    cmd_bits=8, param_bits=8,
    dc_low_on_data=False, sda_pullup=True, scl_pullup=True, disable_control_phase=False,
)
```

Constructor (non-ESP32 build) from [common_src/i2c_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/i2c_bus.c):
- Always raises `NotImplementedError("I2C display bus is not supported")`.

### Type: `lcd_bus.I80Bus`

Constructor (ESP32 build) from [esp32_src/i80_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/i80_bus.c):

```python
lcd_bus.I80Bus(
    dc, wr, data0, data1, data2, data3, data4, data5, data6, data7,
    *,
    data8=-1, data9=-1, data10=-1, data11=-1, data12=-1, data13=-1, data14=-1, data15=-1,
    cs=-1, freq=10_000_000,
    dc_idle_high=False, dc_cmd_high=False, dc_dummy_high=False, dc_data_high=True,
    cmd_bits=8, param_bits=8,
    cs_active_high=False, reverse_color_bits=False, swap_color_bytes=False,
    pclk_active_low=False, pclk_idle_low=False,
)
```

Constructor (non-ESP32 build) from [common_src/i80_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/i80_bus.c):

```python
lcd_bus.I80Bus(
    dc, wr, data0, data1, data2, data3, data4, data5, data6, data7,
    *,
    data8=None, data9=None, data10=None, data11=None, data12=None, data13=None, data14=None, data15=None,
    cs=None, freq=10_000_000,
    dc_idle_high=False, dc_cmd_high=False, dc_dummy_high=False, dc_data_high=True,
    cmd_bits=8, param_bits=8,
    cs_active_high=False, reverse_color_bits=False, swap_color_bytes=False,
    pclk_active_low=False, pclk_idle_low=False,
)
```

Notes:
- The generic implementation requires `mp_hal_pin_output` support; otherwise it raises `NotImplementedError("LCD I80 but is not available for this MCU")`.
- `rx_param()` is not implemented in the generic I80 path; calling it raises `OSError` via the shared wrapper.

### Type: `lcd_bus.RGBBus`

Constructor (ESP32 build, when `SOC_LCD_RGB_SUPPORTED`) from [esp32_src/rgb_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/rgb_bus.c):

```python
lcd_bus.RGBBus(
    hsync, vsync, de, disp, pclk,
    data0, data1, data2, data3, data4, data5, data6, data7,
    *,
    data8=-1, data9=-1, data10=-1, data11=-1, data12=-1, data13=-1, data14=-1, data15=-1,
    freq=8_000_000,
    bb_size_px=0,
    hsync_front_porch=0, hsync_back_porch=0, hsync_pulse_width=0,
    hsync_idle_low=False,
    vsync_front_porch=0, vsync_back_porch=0, vsync_pulse_width=1,
    vsync_idle_low=False,
    de_idle_high=False, pclk_idle_high=False, pclk_active_neg=False,
    disp_active_low=False,
    refresh_on_demand=False,
    bb_inval_cache=False,
)
```

Constructor (non-ESP32 build) from [common_src/rgb_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/rgb_bus.c):
- Always raises `NotImplementedError("RGB display bus is not supported")`.

Method availability on ESP32 RGBBus:
- Provided: `get_lane_count`, `register_callback`, `tx_color`, `init`, `deinit`, `__del__`
- Not provided: `tx_param`, `rx_param`

## Examples (copy/paste)

### ESP32: SPIBus basic bring-up

```python
from lcd_bus import SPIBus

bus = SPIBus(
    21,   # dc
    1,    # host
    18,   # sclk
    40_000_000,  # freq
    23,   # mosi
    miso=-1,
    cs=5,
)

bus.init(width=240, height=240, bpp=16, buffer_size=240 * 240 * 2, rgb565_byte_swap=False)
print("lanes:", bus.get_lane_count())

# Send a command with parameters
bus.tx_param(0x36, b"\x00")

# Send pixel data (wrapper accepts coords but some buses ignore them)
frame = bytearray(240 * 240 * 2)
bus.tx_color(0x2C, frame, 0, 0, 240, 240)

bus.deinit()
```

### Non-ESP32: I2CBus/RGBBus stubs

```python
import lcd_bus

try:
    lcd_bus.I2CBus(0, 0, 0x3C)
except NotImplementedError as e:
    print("I2CBus:", e)

try:
    lcd_bus.RGBBus(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
except NotImplementedError as e:
    print("RGBBus:", e)
```

## Safety / Pitfalls

- Callback context (ESP32): the completion callback is executed from ISR context (see [lcd_types.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/lcd_types.c)). Keep callbacks short and avoid heap allocations.
- Callback filtering: `register_callback` does not validate; the completion path only calls the callback when it is not `None` and `callable(callback)`.
- Buffer mutation:
  - `rgb565_byte_swap=True` performs an in-place byte swap of the color buffer on the transmit path (ESP32 and non-ESP32 paths).
  - ESP32 `RGBBus.tx_color` may also swap in-place by requesting a writable buffer when swapping is enabled.
- Blocking transmit: `tx_color` busy-waits when `callback` is `None`.
- API consistency vs use: `tx_color(..., x_start, y_start, x_end, y_end)` exists on the shared wrapper even when the underlying bus implementation ignores coordinates.

## Build / Integration (MicroPython User C Modules)

This repository is a MicroPython “User C Module”. MicroPython’s build system consumes one of these integration files depending on the port:

- [micropython.cmake](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/micropython.cmake)
  - Used by CMake-based ports.
  - Selects ESP32 sources when `ESP_PLATFORM` is set, otherwise selects `common_src/*`.
  - Adds include directories and links an `INTERFACE` library into the `usermod` target.
- [micropython.mk](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/micropython.mk)
  - Used by Make-based ports.
  - Adds include paths and compiles the `common_src/*` implementation.

Integration steps:

1. Add this repo somewhere accessible to your MicroPython build (commonly as a submodule).
2. Configure your MicroPython build to include this module via the standard User C Module mechanism (point the build system at this directory so it can pick up `micropython.cmake` or `micropython.mk`).
3. Build MicroPython; the resulting firmware includes the `lcd_bus` module.

## Compatibility

- Newer MicroPython builds may not provide a `STATIC` macro. This repo uses standard C `static` for internal linkage to avoid `unknown type name 'STATIC'`.
- Platform differences are built-in:
  - ESP32: backed by ESP-IDF `esp_lcd` (`esp32_src/*`)
  - Non-ESP32: uses `common_src/*`; `I2CBus` and `RGBBus` are not implemented and raise `NotImplementedError`

## License

MIT License. See [LICENSE](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/LICENSE).
