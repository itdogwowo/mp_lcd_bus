# mp_lcd_bus（lcd_bus）

MicroPython User C Module，提供非阻塞 DMA 驅動的 LCD 顯示 bus 物件，適用於 ESP32。

匯入名稱：

```python
import lcd_bus
```

## Bus 類型

| 類型 | 底層 | DMA | 非同步 |
|---|---|---|---|
| `lcd_bus.SPIBus` | `spi_device_queue_trans` | ✅ | ✅ 8 層隊列 |
| `lcd_bus.I80Bus` | `esp_lcd_i80` | ✅ | ✅ 4 層隊列 |
| `lcd_bus.RGBBus` | `esp_lcd_rgb` | ✅ | ✅ 2 層隊列 |
| `lcd_bus.DSIBus` | `esp_lcd_mipi_dsi`（僅 ESP32-P4） | ✅ | ✅ 4 層隊列 |
| `lcd_bus.I2CBus` | `esp_lcd_i2c` | ❌ | ❌ 阻塞式 |

## 總線外控制腳

所有控制腳均為**選填**（預設 `-1` = 不管理）。傳入 GPIO 編號讓 C/esp_lcd 管理，或留 `-1` 自行用 `machine.Pin` 外部控制。

| Bus | 腳位 | 管理者 | 預設 | 備註 |
|---|---|---|---|---|
| `SPIBus` | dc/cs | —（僅外部） | — | 純 SPI bus；dc/cs 由使用者/adapter 處理 |
| `I80Bus` | `dc` | esp_lcd | `-1` | **強烈建議填寫** — I80 需要 dc 切換命令/資料 |
| `I80Bus` | `cs` | esp_lcd | `-1` | |
| `RGBBus` | `disp` | esp_lcd | `-1` | 顯示使能 |
| `DSIBus` | `rst` | C module | `-1` | esp_lcd DPI panel 不支援 reset_gpio |

## 統一 API

所有 bus 共用相同方法：

| 方法 | SPI | I2C | I80 | RGB | DSI |
|---|---|---|---|---|---|
| `write(buf)` → `trans_id` | ✅ 非同步 | ✅ | ✅ 非同步 | ✅ 非同步 | ✅ 非同步 |
| `write(buf, *, cmd=, addr=, multiline=)` → `None` | ✅ 同步 | ❌ | ❌ | ❌ | ❌ |
| `readinto(buf, write_val=0)` → `trans_id` | ✅ 非同步 | ✅ | ❌ | ❌ | ❌ |
| `write_readinto(wbuf, rbuf)` → `trans_id` | ✅ 非同步 | ❌ | ❌ | ❌ | ❌ |
| `cmd(cmd, param=b'')` → `None` | ❌ | ❌ | ❌ | ❌ | ✅ 同步 |
| `is_busy()` → `bool` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `pending()` → `int` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `wait(trans_id, timeout_ms=-1)` → `bool` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `wait_all(timeout_ms=-1)` → `None` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `lane_count()` → `int` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `frame_buffer(idx)` → `bytearray` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `flush(idx=0, *, x=0, y=0, w=0, h=0)` → `None` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `set_pattern(pat)` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `deinit()` | ✅ | ✅ | ✅ | ✅ | ✅ |

## 建構子

### SPIBus

```python
lcd_bus.SPIBus(data, clk, *, freq=40_000_000, host=1, queue_depth=8)
```

`data` tuple 長度自動決定 lane 數：

| len(data) | 模式 |
|---|---|
| 1 | 標準 SPI |
| 2 | Dual SPI |
| 4 | Quad SPI（QSPI） |
| 8 | Octal SPI |

```python
bus = lcd_bus.SPIBus(data=(35,), clk=36)                      # 1 線
bus = lcd_bus.SPIBus(data=(35, 36, 37, 38), clk=39)           # Quad 4 線
bus = lcd_bus.SPIBus(data=(35,36,37,38,39,40,41,42), clk=43)  # Octal 8 線
```

### I2CBus

```python
lcd_bus.I2CBus(data, clk, addr, *, freq=10_000_000)
```

```python
bus = lcd_bus.I2CBus(data=(21,), clk=22, addr=0x3C)
```

### I80Bus（僅 ESP32-S3/P4）

```python
lcd_bus.I80Bus(data, wr, *, cs=-1, freq=10_000_000, queue_depth=4)
```

```python
bus = lcd_bus.I80Bus(data=(d0,d1,d2,d3,d4,d5,d6,d7), wr=10)  # 8-bit
bus = lcd_bus.I80Bus(data=(d0..d15), wr=10)                    # 16-bit
```

### RGBBus（僅 ESP32-S3/P4）

```python
lcd_bus.RGBBus(data, hsync, vsync, de, pclk, width, height, *,
               freq=8_000_000, disp=-1, ...)
```

### DSIBus（僅 ESP32-P4）

```python
lcd_bus.DSIBus(lanes, width, height, lane_bit_rate_mbps, *,
               dpi_clk_mhz=30.0,
               hsync_pulse_width=1, hsync_back_porch=10, hsync_front_porch=10,
               vsync_pulse_width=1, vsync_back_porch=10, vsync_front_porch=10,
               in_color_format=16, fb_count=2, rst=-1,
               virtual_channel=0,
               cmd_bits=8, param_bits=8,
               use_dma2d=True, queue_depth=4)
```

| 參數 | 類型 | 預設 | 說明 |
|------|------|------|------|
| `lanes` | int | 必填 | MIPI DSI data lane 數（1-4） |
| `width`, `height` | int | 必填 | 面板解析度 |
| `lane_bit_rate_mbps` | float | 必填 | DSI PHY lane bit rate（Mbps，如 1000） |
| `dpi_clk_mhz` | float | 30.0 | 像素（DPI）時脈 MHz |
| `hsync/vsync_*` | int | 1/10/10 | video timing（porch 與 pulse width，單位 px/行） |
| `in_color_format` | int | 16 | `16` = RGB565，`24` = RGB888 |
| `fb_count` | int | 2 | 內部 frame buffer 數（1-3），由 driver 配置於 PSRAM |
| `rst` | int | -1 | 面板硬體 reset GPIO（`-1` = 不使用） |
| `virtual_channel` | int | 0 | DSI virtual channel（0-3） |
| `cmd_bits`, `param_bits` | int | 8 | DBI 命令/參數位寬（少數面板不同） |
| `use_dma2d` | bool | True | `True` = DMA2D 拷貝，`False` = 退回 CPU 拷貝 |
| `queue_depth` | int | 4 | 非同步寫入管線深度（1-8）；越大越吃記憶體、管線越深 |

> **所有面板相關參數都從 Python 層輸入** —— 換面板不需要改 C module。
> 面板 init 命令序列（`cmd()`）、reset/背光腳位、DSI PHY LDO 也都在
> Python 層處理（見 `test_dsi.py`）。

driver 內部自動配置整張畫面的 frame buffer；`frame_buffer(idx)` 回傳零拷貝的
`bytearray` 視圖。`write()` 是非同步的——bus 會把資料複製進 frame buffer，
回傳 `trans_id` 可搭配 `wait()`：

```python
bus = lcd_bus.DSIBus(lanes=2, width=800, height=480,
                     lane_bit_rate_mbps=1000, rst=20)

bus.cmd(0x11)                    # SLPOUT（無參數）
bus.cmd(0x36, b'\x00')           # MADCTL
bus.cmd(0x3A, b'\x70')           # COLMOD RGB565
bus.cmd(0x29)                    # DISPON

tid = bus.write(fb_bytes)        # 非同步複製進 frame buffer
bus.wait(tid)

fb = bus.frame_buffer(0)         # 內部 fb 的零拷貝視圖
memoryview(fb)[:2] = b'\xf8\x00' # 直接寫進 framebuffer
bus.flush()                      # ⚠ 把髒 L2 cache line 寫回 PSRAM

bus.set_pattern(1)               # 內建測試圖案（0=無,1=直條,2=橫條,3=BER）
bus.set_pattern(0)               # 恢復正常顯示
```

### DSIBus frame buffer 直寫與雙緩衝（ESP32-P4）

**⚠️ 透過 `frame_buffer(idx)` 直寫後一定要 `flush()`。**

ESP32-P4 的 frame buffer 在 PSRAM。CPU 寫入走 write-back L2 cache，
但 DPI DMA 是**直接讀 PSRAM（不經 cache）**。沒有 `esp_cache_msync()`
的話，DMA 會一直讀到「還沒寫回」的舊資料 —— 症狀就是殘留 / 黑帶 /
閃爍。`flush()` 把指定區域的髒 cache line 寫回 PSRAM（與 esp_lcd
driver 內部做的操作相同），所以每次直寫 fb 之後都要呼叫：

```python
fb = bus.frame_buffer(0)
fill_screen(fb, color)   # CPU 寫入 PSRAM（先進 cache）
bus.flush()              # 寫回，DPI DMA 才看得到
```

**撕裂規則（`fb_count` 語義）：**

| fb_count | `write(buf)` 外部 buffer | `frame_buffer()` 直寫 |
|---|---|---|
| 1 | DMA2D 拷進「顯示中」的 fb → 拷貝期間會撕裂（約 ms 級） | 寫正在被掃描的 buffer 必撕裂；單緩衝的物理限制 |
| 2+ | 同上 —— 外部 buffer 一律落在「顯示中」的 fb | 要完全不撕裂請用下面的交換模式 |

**不撕裂的雙緩衝**（需 `fb_count=2`）：esp_lcd P4 driver 每塊 fb 各有一條
DMA link list，當 `draw_bitmap()` 的 draw buffer **是內部 fb** 時（不做拷貝，
只 cache writeback + 切換指標），會在**幀邊界**自動切到那塊 fb。所以用法是：
在「目前沒在顯示」的那塊 fb 畫完整幀，再把該 fb view 丟給 `write()`：

```python
fb0 = bus.frame_buffer(0)
fb1 = bus.frame_buffer(1)

while True:
    draw_full_frame(fb1, frame_a)   # fb0 顯示中，畫 fb1
    bus.write(fb1); bus.wait_all()  # msync + 下一幀邊界切換
    draw_full_frame(fb0, frame_b)   # fb1 顯示中，畫 fb0
    bus.write(fb0); bus.wait_all()
```

規則：每次都要**整幀**更新（或維持兩塊 fb 內容同步），否則切換過去後，
沒畫到的區域顯示的是那一塊的舊內容 —— 多 fb + 部分更新會每幀在兩塊
舊內容之間翻動。

`test_dsi.py` 涵蓋全部三種路徑：直寫 + `flush()`（純色/漸層）、
外部 buffer 的異步 `write()`、以及雙緩衝不撕裂示範。

---

## write(buf, *, cmd=-1, addr=0, multiline=True)

根據 `cmd` 參數決定兩種模式：

| | DMA 模式 `write(buf)` | Polling 模式 `write(buf, cmd=...)` |
|---|---|---|
| 觸發 | 不傳 `cmd=`（預設 -1） | `cmd >= 0` |
| 傳輸 | `spi_device_queue_trans`（硬體 DMA） | `spi_device_polling_transmit`（同步阻塞） |
| 回傳 | `trans_id` (int) | `None` |
| cmd/addr phase | 無 | 有（8-bit cmd + 24-bit addr） |
| 同步？ | 非同步，立刻返回 | 同步，完成才返回 |
| 隊列深度 | 最多 `queue_depth`（預設 8） | 不走隊列 |
| 用途 | 大筆像素資料 | LCD 命令、像素前導 |

**參數說明：**

| 參數 | 類型 | 預設 | 說明 |
|------|------|------|------|
| `buf` | buffer | 必填 | 要發送的資料，可為空 `b''` |
| `cmd` | int | -1 | QSPI command byte，>=0 觸發 polling 模式 |
| `addr` | int | 0 | QSPI address（24-bit），如 `cmd<<8` 或 `0x002C00` |
| `multiline` | bool | True | True=cmd/addr 4線，False=cmd/addr 只走 D0 |

**Polling 模式細節：**
- 先 `spi_drain_pending()` 清空 DMA queue，確保順序
- 使用 `spi_transaction_ext_t` + `SPI_TRANS_VARIABLE_CMD|SPI_TRANS_VARIABLE_ADDR`
- Per-transaction `command_bits=8, address_bits=24`（不影響 device config）
- `multiline=True` → 加 `MULTILINE_CMD|MULTILINE_ADDR`（cmd/addr 4線）
- `multiline=False` → cmd/addr 只走 D0 1線
- 空 `buf`（`b''`）→ 只發 cmd+addr phase，不發 data phase

### QSPI 顯示器範例（RM67162）

```python
cs = Pin(6, Pin.OUT, value=1)

def tx_param(cmd, params=b''):
    cs.low()
    bus.write(params if params else b'\x00', cmd=0x02, addr=cmd << 8)
    cs.high()

def tx_color_pre():
    cs.low()
    bus.write(b'', cmd=0x32, addr=0x002C00, multiline=False)
    cs.high()  # CS 在整個 pixel 傳輸期間保持 low

# 初始化序列
tx_param(0x11)           # SLPOUT
time.sleep_ms(120)
tx_param(0x36, b'\x00')  # MADCTL
tx_param(0x36, b'\x00')  # MADCTL（RM67162 規格需發兩次）
tx_param(0x3A, b'\x75')  # COLMOD 16bpp
tx_param(0x29)           # DISPON

# 發送像素資料（CS 在 preamble + 所有 chunk 期間保持 low）
cs.low()
bus.write(b'', cmd=0x32, addr=0x002C00, multiline=False)
for chunk in chunks:
    tid = bus.write(chunk)
    bus.wait(tid)
cs.high()
```

---

## DMA Queue 使用模式

### 模式 1：write + wait（最安全，buffer 可復用）

```python
for chunk in chunks:
    tid = bus.write(chunk)    # DMA 開始讀 chunk
    bus.wait(tid)             # 等完成才能覆寫 chunk
```

### 模式 2：fire-and-forget 多筆（需獨立 buffer）

```python
bus.write(buf_a)   # DMA 排隊
bus.write(buf_b)   # DMA 排隊
bus.write(buf_c)   # DMA 排隊
bus.wait_all()     # 等全部完成
# ⚠️ buf_a/buf_b/buf_c 在 wait_all 前不能覆寫
```

### 模式 3：非阻塞 CPU 並行

```python
tid = bus.write(chunk)      # 發送 32KB，立刻返回
calculate_something()        # CPU 同時做事
bus.wait(tid)               # 回來等 DMA
```

### 模式 4：指定等某一筆

```python
tid_a = bus.write(chunk_a)
tid_b = bus.write(chunk_b)
bus.wait(tid_a)             # 只等 chunk_a
bus.wait(tid_b)
```

### Queue Full

第 `queue_depth + 1` 筆不阻塞，直接拋 `RuntimeError("queue full")`（SPI 預設深度 8）：

```python
try:
    for _ in range(9):
        bus.write(bytearray(64))
except RuntimeError as e:
    print(e)  # "queue full"
```

### Queue Depth

`queue_depth` 不是硬體 DMA 限制，是為省記憶體（每 slot ~28 bytes）。可透過
建構子調整（各 bus 皆支援，範圍 1-8）：`SPIBus(..., queue_depth=8)`、
`I80Bus(..., queue_depth=4)`、`RGBBus(..., queue_depth=2)`、
`DSIBus(..., queue_depth=4)`。8 對 `write → wait` 串列場景已足夠。

### 非阻塞查詢

```python
done = bus.wait(tid, timeout_ms=0)   # True=完成, False=還在傳
bus.is_busy()                         # True=有未完成
bus.pending()                         # 未完成數量（0~4）
```

---

## 標準 SPI 範例

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
while bus.is_busy():        # CPU 做其他事
    read_sensor()
bus.wait(tid)               # 確認完成
cs.high()

bus.deinit()
```

## 測試

在 ESP32 裝置上執行：

```python
import test_bus
test_bus.run_all()
```

| # | 測試 | 說明 | 斷言 |
|---|------|------|------|
| 1 | `test_spi` | SPI 初始化、寫入、queue_depth 隊列、queue-full、deinit | 11 |
| 2 | `test_spi_multilane` | 1/2/4/8 線自動檢測 | 4 |
| 3 | `test_spi_official` | 全雙工、`readinto()` | 5 |
| 4 | `test_speed` | 吞吐量基準（1/2/4 線、40/80 MHz） | 純輸出 |
| 5 | `test_i80` | I80 並行（不支援則 SKIP） | 0 |
| 6 | `test_rgb` | RGB 並行（不支援則 SKIP） | 0 |
| 7 | `test_rapid` | 快速 init→write→deinit 壓力（3 次） | 3 |

## 建置

CMake（ESP-IDF）：將本 repo 加入為 User C Module。

`DSIBus` 在每個 ESP32 建置都會編譯，但 `esp32_src/dsi_bus.c` 內的真正 MIPI DSI
驅動程式碼由 `#if SOC_MIPI_DSI_SUPPORTED` 保護——該巨集只有在 ESP32-P4 上才定義。
其他 ESP32（如 ESP32-S3）會編譯成拋出 `NotImplementedError` 的 stub，因此不需要
針對晶片修改建置設定（自動分流）。

非 ESP32：stub 拋 `NotImplementedError`。

### DSI 建置需求（ESP32-P4）

MIPI DSI DPI 面板持續從 PSRAM 的 frame buffer 串流像素資料。若 PSRAM 頻寬不足，
DPI 控制器會回報 underrun（`can't fetch data from external memory fast enough`），
導致螢幕閃爍。

使用 **mp_Make-Tools** 建置時，所需的 sdkconfig 配置宣告於
[`sdkconfig.require.json`](sdkconfig.require.json)，建置工具會依據目標晶片
自動注入——無需手動編輯 sdkconfig。

| Key | 原因 |
|---|---|
| `CONFIG_SPIRAM_SPEED_200M` | PSRAM 跑 200MHz（預設 80MHz 太慢，DSI 不夠用） |
| `CONFIG_CACHE_L2_CACHE_256KB` | L2 cache，提升 DMA 讀取 PSRAM 效率 |
| `CONFIG_CACHE_L2_CACHE_LINE_128B` | L2 cache line 大小（需搭配） |
| `CONFIG_SPIRAM_XIP_FROM_PSRAM` | 程式從 PSRAM 執行，釋放內部 RAM |

若不用 mp_Make-Tools 建置，需手動將這些 key 加到 `sdkconfig.defaults`。

## License

MIT
