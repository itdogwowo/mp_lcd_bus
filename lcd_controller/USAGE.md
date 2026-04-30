# lcd_controller

`mp_lcd_bus` 的 Python 層 LCD 控制器驅動。在 `lcd_bus` 提供匯流排傳輸（SPI / I80 / RGB）的基礎上，封裝 LCD 晶片的初始化序列與繪圖操作。

## 依賴

| 模組 | 必要性 | 說明 |
|------|:---:|------|
| `lcd_bus` | 必須 | 底層匯流排 C 模組 |
| `machine` | 必須 | MicroPython 標準庫，用於 Pin 控制 |
| `heap_caps` | 可選 | ESP32 DMA buffer 分配；fallback 到 `bytearray` |

## 快速開始

```python
import lcd_bus
from lcd_controller import ST7789

bus = lcd_bus.SPIBus(
    dc=11, host=1, sck=14, mosi=13, miso=12, cs=15,
    freq=80_000_000
)

lcd = ST7789(
    bus=bus,
    rst=43,
    width=240,
    height=320,
    color_space="RGB565",
    rgb565_byte_swap=True,
)

lcd.init()          # 硬體 reset + 寫入暫存器初始化序列
lcd.fill((255,0,0)) # 全螢幕紅色
```

---

## API 參考

### 建構子（所有驅動共用）

```python
lcd = ST7789(
    bus,                    # lcd_bus.SPIBus / I80Bus / RGBBus
    rst,                    # int → machine.Pin(rst, OUT)；或直接傳 machine.Pin
    width,                  # 邏輯寬度
    height,                 # 邏輯高度
    frame_buffer=None,      # None → 自動分配單緩衝
    rotation=0,             # 0 / 90 / 180 / 270
    color_order="RGB",      # "RGB" / "BGR"
    invert=False,           # 顏色反轉
    color_space="RGB565",   # "RGB565" / "RGB888"
    rgb565_byte_swap=False, # RGB565 byte swap
)
```

`__init__` 階段會：
1. 設定 rst pin
2. 分配 framebuffer（若 `None`）
3. 呼叫 `bus.init(width, height, bpp, buffer_size, rgb565_byte_swap)`

### `lcd.init()`

第二階段初始化。執行 `_hardware_reset()` 後依序寫入 `_build_init_sequence()` 回傳的暫存器序列。

```python
lcd.init()
```

### 寫入命令 / 資料

```python
lcd.write_cmd(0x11)                 # 只發命令
lcd.write_cmd_data(0x36, b'\x08')   # 命令 + 參數
```

### 設定顯示區域

```python
lcd.set_window(x1, y1, x2, y2)     # CASET + RASET + RAMWR
```

內部發送 `0x2A` (CASET) → `0x2B` (RASET) → `0x2C` (RAMWR)。

### 寫入像素資料

```python
lcd.write_color(data, x1, y1, x2, y2)
```

發送 `bus.tx_color(0x2C, data, x1, y1, x2, y2)`。

### 全螢幕填色

```python
lcd.fill(0xF800)          # RGB565 數值
lcd.fill((255, 0, 0))     # RGB tuple → 自動轉 RGB565
```

### 旋轉 / 顏色順序 / 反轉

```python
lcd.set_rotation(90)
lcd.set_color_order("BGR")
lcd.invert_display(True)

r = lcd.get_rotation()
o = lcd.get_color_order()
i = lcd.get_inversion_state()
```

### 釋放

```python
lcd.deinit()    # 呼叫 bus.deinit()
```

---

## framebuffer 配置

`frame_buffer` 參數支援三種形式：

```python
# 1. None — 自動分配單緩衝（heap_caps DMA 或 bytearray）
lcd = ST7789(bus, rst, 240, 320)

# 2. 自訂 buffer
import heap_caps
buf = heap_caps.malloc(240 * 320 * 2, heap_caps.CAP_DMA)
lcd = ST7789(bus, rst, 240, 320, frame_buffer=[buf])

# 3. 雙緩衝
buf1 = bytearray(240 * 320 * 2)
buf2 = bytearray(240 * 320 * 2)
lcd = ST7789(bus, rst, 240, 320, frame_buffer=[buf1, buf2])
```

自動分配時的降級策略：

1. `heap_caps.CAP_INTERNAL | heap_caps.CAP_DMA`
2. `heap_caps.CAP_SPIRAM | heap_caps.CAP_DMA`
3. 無 `heap_caps` 時 → `bytearray`

---

## 新增驅動

繼承 `LCDController`，覆寫三個方法：

```python
from lcd_controller import LCDController

class ILI9341(LCDController):

    def _build_init_sequence(self):
        return [
            (0x01, None, 120),          # SWRESET
            (0x11, None, 120),          # SLPOUT
            (0x36, self._get_madctl_cmd(), 10),   # MADCTL
            (0x3A, bytes([self._colmod_val]), 10), # COLMOD
            ...
            (0x29, None, 10),           # DISPON
        ]

    def _get_madctl_cmd(self):
        rotation_map = {0: 0x48, 90: 0x28, 180: 0x88, 270: 0xE8}
        base = rotation_map.get(self._rotation, 0x48)
        if self._color_order == "BGR":
            base |= 0x08
        return bytes([base])

    def _get_inversion_cmd(self):
        return 0x21 if self._invert else 0x20

    def _update_madctl(self):
        self._bus.tx_param(0x36, self._get_madctl_cmd())

    def _update_inversion(self):
        self._bus.tx_param(self._get_inversion_cmd())
```

每個 tuple 格式為 `(cmd, data, delay_ms)`：
- `cmd`：8-bit 命令碼
- `data`：`bytes`（或 `None` 表示只發命令不帶參數）
- `delay_ms`：發送後等待毫秒數

`self._colmod_val` 由基底的 `color_space` 參數自動計算（`"RGB565"` → `0x55`，`"RGB888"` → `0x77`）。

---

## 已內建驅動

| 類別 | 晶片 |
|------|------|
| `ST7789` | ST7789 / ST7789V |

---

## 完整範例（ESP32-S3 + ST7789）

```python
import lcd_bus
import machine
from lcd_controller import ST7789

bus = lcd_bus.SPIBus(
    dc=11,
    host=1,
    sck=14,
    freq=80_000_000,
    mosi=13,
    miso=12,
    cs=15,
)

lcd = ST7789(
    bus=bus,
    rst=43,
    width=240,
    height=320,
    rotation=0,
    color_order="RGB",
    color_space="RGB565",
    rgb565_byte_swap=True,
)

lcd.init()
lcd.fill((0, 0, 0))
```
