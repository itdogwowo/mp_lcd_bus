# mp_lcd_bus（lcd_bus）

這是一個 MicroPython User C Module，提供 LCD panel I/O 的「bus」物件，讓你在 MicroPython 中用一致的 API 來操作多種實體匯流排（SPI、I80/8080、I2C、RGB）。在 ESP32 平台上，本模組使用 ESP-IDF 的 `esp_lcd` 作為底層實作；在非 ESP32 平台上，提供通用實作（部分 bus 會直接回 `NotImplementedError`）。

在 MicroPython 中的匯入名稱：

```python
import lcd_bus
```

## Project Summary

本模組提供的 MicroPython 物件可封裝：

- Bus 的配置與初始化（腳位、時脈/時序、buffer sizing）
- 傳送命令/參數與像素資料（依 bus/platform 支援狀況而定）
- 可選的傳輸完成 callback、lane count 查詢

典型使用情境：

- 作為顯示器驅動（panel/controller driver）之下的 I/O 層（panel 驅動本身不在這個 repo）
- 需要跨不同板子/匯流排維持一致 API 的專案整合

## Features

- 提供 `lcd_bus.SPIBus`、`lcd_bus.I80Bus`、`lcd_bus.I2CBus`、`lcd_bus.RGBBus` 四種 type。
- 常見生命週期：
  - 建立 bus 物件（保存配置）
  - 呼叫 `init(width, height, bpp, buffer_size, rgb565_byte_swap)` 以配置/分配底層 I/O
  - 呼叫 `tx_param` / `tx_color` / `rx_param`（可用性依 bus/platform 而定）
  - 呼叫 `deinit()`（或依賴 `__del__`）釋放資源
- `get_lane_count()`：回傳 bus 的 lane 數（每種 bus 的定義不同）。
- `register_callback(cb)`：設定傳輸完成 callback（DMA/傳輸完成路徑使用）。
- 平台感知建置：
  - ESP32（CMake `ESP_PLATFORM`）：使用 `esp32_src/*` + ESP-IDF `esp_lcd`
  - 非 ESP32：使用 `common_src/*`（部分 bus 為 stub，會丟 `NotImplementedError`）

## API Reference

### 模組：`lcd_bus`

只輸出 types（除 `__name__` 外沒有模組層級函式/常數）：

- `lcd_bus.SPIBus`
- `lcd_bus.I2CBus`
- `lcd_bus.I80Bus`
- `lcd_bus.RGBBus`

### 共用方法（多數 bus type 具有）

以下方法來自 [modlcd_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/modlcd_bus.c) 的共用 locals dict。

#### `init(self, width: int, height: int, bpp: int, buffer_size: int, rgb565_byte_swap: bool) -> None`

用途：
- 初始化底層 bus I/O；當 `bpp == 16` 時保存 `rgb565_byte_swap` 設定。

參數：
- `width`, `height`：傳入底層實作使用。
- `bpp`：每像素 bit 數。當 `bpp != 16` 時，wrapper 會強制把 `rgb565_byte_swap` 設為 `False`。
- `buffer_size`：傳入底層實作作為 buffer sizing 依據。
- `rgb565_byte_swap`：啟用時（且支援時）會在送出路徑對 RGB565 buffer 進行就地 byte swap。

回傳值：
- `None`

失敗行為：
- 若底層回傳非 0 錯誤碼，丟出 `OSError`。
- 部分 ESP32 底層 bus 會在 init 內部直接丟出 `ValueError`/`OSError`（會原樣向上傳遞）。

#### `deinit(self) -> None` 與 `__del__(self) -> None`

用途：
- 釋放底層資源。

回傳值：
- `None`

失敗行為：
- 若底層回傳非 0，丟出 `ValueError`。

#### `get_lane_count(self) -> int`

用途：
- 回傳 bus 的 lane 數（例如 SPI 可能是 1/2/4，I80/RGB 可能是 bus width）。

回傳值：
- lane count（int）

失敗行為：
- 若底層回傳非 0，丟出 `OSError`。

#### `register_callback(self, callback) -> None`

用途：
- 設定傳輸完成 callback。

參數：
- `callback`：原樣保存。

回傳值：
- `None`

失敗行為：
- setter 不做檢查；完成路徑只在 `callback != None` 且 `callable(callback)` 時才會呼叫。

#### `tx_param(self, cmd: int, params: bytes|bytearray|memoryview|None = None) -> None`

用途：
- 傳送命令 + 可選參數 buffer（取決於底層 bus 是否支援）。

參數：
- `cmd`：命令值（int）。
- `params`：`None` 或任何可讀 buffer。

回傳值：
- `None`

失敗行為：
- 底層回傳非 0 → 丟出 `OSError`。
- 若底層不支援參數傳送，底層回「not supported」，wrapper 仍會以 `OSError` 表現。

#### `rx_param(self, cmd: int, data: bytearray|memoryview) -> None`

用途：
- 讀取參數資料到可寫 buffer（取決於底層 bus 是否支援）。

參數：
- `cmd`：命令值（int）。
- `data`：可寫 buffer（必須支援 `MP_BUFFER_WRITE`）。

回傳值：
- `None`

失敗行為：
- 底層回傳非 0（包含 not supported）→ 丟出 `OSError`。

#### `tx_color(self, cmd: int, data: bytes|bytearray|memoryview, x_start: int, y_start: int, x_end: int, y_end: int) -> None`

用途：
- 傳送像素資料。座標參數存在是為了 API 一致性。

參數：
- `cmd`：命令值（int）。
- `data`：像素資料 buffer（可讀）。
- `x_start/y_start/x_end/y_end`：wrapper 會接受；底層實作可能忽略。

回傳值：
- `None`

失敗行為：
- 底層回傳非 0 → 丟出 `OSError`。

阻塞行為：
- 當 `callback` 為 `None` 時，wrapper 會 busy-wait 等待傳輸完成旗標。

### Type：`lcd_bus.SPIBus`

建構子（ESP32）來源：[esp32_src/spi_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/spi_bus.c)

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

備註：
- ESP32 版本中 `host/sclk/freq/mosi/...` 以整數方式解析使用；`dc` 以 object 解析但在目前程式中以整數使用，請傳入 GPIO number（int）。

建構子（非 ESP32）來源：[common_src/spi_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/spi_bus.c)

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

備註：
- 通用版本需要 port 提供 `mp_hal_pin_output`；否則會丟 `NotImplementedError("LCD SPI but is not available for this MCU")`。
- 腳位相關參數會透過 `mp_hal_get_pin_obj(...)` 轉換。

### Type：`lcd_bus.I2CBus`

建構子（ESP32）來源：[esp32_src/i2c_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/i2c_bus.c)

```python
lcd_bus.I2CBus(
    sda, scl, addr,
    *,
    host=0, control_phase_bytes=1, dc_bit_offset=6, freq=10_000_000,
    cmd_bits=8, param_bits=8,
    dc_low_on_data=False, sda_pullup=True, scl_pullup=True, disable_control_phase=False,
)
```

建構子（非 ESP32）來源：[common_src/i2c_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/i2c_bus.c)
- 一律丟 `NotImplementedError("I2C display bus is not supported")`。

### Type：`lcd_bus.I80Bus`

建構子（ESP32）來源：[esp32_src/i80_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/i80_bus.c)

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

建構子（非 ESP32）來源：[common_src/i80_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/i80_bus.c)

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

備註：
- 通用版本需要 port 提供 `mp_hal_pin_output`；否則會丟 `NotImplementedError("LCD I80 but is not available for this MCU")`。
- 通用 I80 路徑沒有實作 `rx_param()`；呼叫會由共用 wrapper 以 `OSError` 表現。

### Type：`lcd_bus.RGBBus`

建構子（ESP32、且 `SOC_LCD_RGB_SUPPORTED`）來源：[esp32_src/rgb_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/esp32_src/rgb_bus.c)

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

建構子（非 ESP32）來源：[common_src/rgb_bus.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/common_src/rgb_bus.c)
- 一律丟 `NotImplementedError("RGB display bus is not supported")`。

ESP32 RGBBus 方法可用性：
- 有：`get_lane_count`、`register_callback`、`tx_color`、`init`、`deinit`、`__del__`
- 沒有：`tx_param`、`rx_param`

## Examples（可直接複製執行）

### ESP32：SPIBus 基本使用

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

bus.tx_param(0x36, b"\x00")

frame = bytearray(240 * 240 * 2)
bus.tx_color(0x2C, frame, 0, 0, 240, 240)

bus.deinit()
```

### 非 ESP32：I2CBus / RGBBus stub 行為

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

## Safety / pitfalls（重要注意事項）

- Callback 執行環境（ESP32）：傳輸完成 callback 會在 ISR context 被呼叫（參考 [lcd_types.c](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/lcd_types.c)）。callback 應保持精簡，並避免 heap allocation。
- Callback 過濾：`register_callback` 不做檢查；完成路徑只在 `callback != None` 且 `callable(callback)` 時才會呼叫。
- Buffer 會被改寫：
  - 當 `rgb565_byte_swap=True`，送出路徑會對 RGB565 buffer 做就地 byte swap（ESP32 與非 ESP32 路徑都有）。
  - ESP32 的 `RGBBus.tx_color` 在啟用 swap 時，會以可寫 buffer 取得資料並就地處理。
- 傳輸阻塞：當 `callback` 為 `None`，`tx_color` 會 busy-wait 等待完成旗標。
- API 一致性 vs 實際用途：共用 wrapper 的 `tx_color(..., x_start, y_start, x_end, y_end)` 在部分 bus 底層會忽略座標參數。

## Build / integration（整合到 MicroPython build）

此 repo 是 MicroPython 的 User C Module。MicroPython build 會依 port 選用下列其中之一：

- [micropython.cmake](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/micropython.cmake)
  - CMake-based ports 使用。
  - 當 `ESP_PLATFORM` 存在時選用 `esp32_src/*`，否則選用 `common_src/*`。
  - 建立 `INTERFACE` library，加入 include paths，並連結到 `usermod` target。
- [micropython.mk](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/micropython.mk)
  - Make-based ports 使用。
  - 加入 include paths 並編譯 `common_src/*`。

整合步驟：

1. 將此 repo 放到 MicroPython build 可存取的位置（常見作法是用 submodule）。
2. 透過 MicroPython 的 User C Module 機制把此目錄加入 build（讓 build system 能讀到 `micropython.cmake` 或 `micropython.mk`）。
3. 編譯韌體；產出的韌體會包含 `lcd_bus` 模組。

## Compatibility

- 新版 MicroPython 可能不再提供 `STATIC` 巨集。本 repo 使用標準 C `static` 以避免 `unknown type name 'STATIC'`。
- 平台差異為既定行為：
  - ESP32：使用 ESP-IDF `esp_lcd`（`esp32_src/*`）
  - 非 ESP32：使用 `common_src/*`；`I2CBus` 與 `RGBBus` 未實作，會丟 `NotImplementedError`

## License

MIT License。詳見 [LICENSE](file:///c:/Users/bl91920/Documents/code/git/mp_lcd_bus/LICENSE)。

