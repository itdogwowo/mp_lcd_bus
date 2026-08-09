# examples/tft — 統一 bus API + TFT 控制層 + 性能測試

從 mp_Net-Core (`slave new/lib/`) 抽出的獨立 TFT 顯示層，架構三層：

```
統一 bus API (lib/bus_adapter.py)   ← 所有總線同一套介面
        ↑
TFT 控制層 (lib/TFT.py)             ← 只透過 BusAdapter 控制，不碰總線細節
        ↑
測試工具 (tft_test_tool.py)         ← 性能 / FPS / 目視測試
```

## 層次 1 — 統一 bus API（`lib/bus_adapter.py`）

`BusAdapter` 定義統一介面，各總線實作同一語義：

| 方法 | 語義 |
|---|---|
| `write_cmd(cmd)` | 命令（SPI: DC=0；QSPI: cmd=0x02；DSI: DBI 通道） |
| `write_cmd_data(cmd, data)` | 命令 + 參數 |
| `set_window(x0,y0,x1,y1)` | 設定像素區域（基類: CASET→PASET→RAMWR；DSI: 記錄區域） |
| `write_data_async(data)` → tid | 像素資料（DMA 非同步），大 buffer 自動分 chunk |
| `write_frame_dma(data)` → tids | 整幀 DMA，回傳 **tids 列表** |
| `flush()` / `wait(handle)` | 等 DMA 完成 |
| `reset()` | 面板 reset |

Adapter 實作（各總線的 cmd 規劃見各類別 docstring）：

| Adapter | 適用 | cmd 規劃 |
|---|---|---|
| `SpiBusAdapter` | SPI/QSPI | QSPI: `cmd=0x02`(寫參數) / `cmd=0x32`(寫像素) + addr；DMA 模式用 DC 腳 |
| `I2cBusAdapter` | I2C | cmd_ctrl=0x00 / data_ctrl=0x40 前導位元組 |
| `I80BusAdapter` | I80 並行 | DCX 腳切換命令/資料 |
| `RgbBusAdapter` | RGB 並行 | 無命令介面（硬體掃描），寫入即顯示 |
| `DsiBusAdapter` | **MIPI DSI (P4)** | DBI 通道送命令；像素 = `bus.write(data, 區域)` DMA2D 硬拷進顯示中 fb |

## 層次 2 — TFT 控制層（`lib/TFT.py`）

完整轉移自 mp_Net-Core：`VideoStreamReader` + `TFT` 基類 + 面板子類
（ST7735 / ST7789 / ST7796 / GC9A01 / ILI9341 / GC9D01 / NV3030B /
RM67162 / SH8601），新增 **`JD9165`**（1024x600 MIPI DSI）與 **`ST7701`**
（480x480 RGB，如 Waveshare ESP32-S3-Touch-LCD-4，init 走 `ST7701CtrlSPI`
3-wire 9-bit 控制通道）。

TFT API（所有面板一致）：`show()` / `show_async()` / `show_frame()` /
`begin_display()` + `present()` + `present_wait()` / `set_rotation()` /
`set_color_order()` / `invert_display()` / `fill()` / framebuf 相容。

## 層次 3 — 全能測試工具（`tft_test_tool.py` + `boards/`）

**三層解耦，每層只吃上一層的產物**，互不綁定：

```
Layer 1  create_bus(cfg)              → (bus, adapter, holds)  # 電源/reset + bus + adapter
Layer 2  create_panel(cfg, adapter)   → tft                    # 依 driver 名建面板驅動
Layer 3  TftTest(tft, w, h, ...)      → run_all()              # 測試只認 tft 物件
Runner   run(board)                   → config → bus → panel → 測試 → deinit
```

**一板一 config**：`boards/<板名>/config.json`，自行增刪板子，每次只讀一個：

```python
import tft_test_tool
tft_test_tool.list_boards()                     # 看 boards/ 下有哪些板子
tft_test_tool.run("dsi_jd9165_1024x600")       # 測指定板子
tft_test_tool.run(config_path="my.json")       # 或直接給 config 路徑
tft_test_tool.run()                            # 不給名 → 列出可用板子
```

也可逐層手動組合（不透過 Runner）：

```python
cfg = tft_test_tool.load_board("spi_st7789_240x320")
bus, adapter, holds = tft_test_tool.create_bus(cfg)   # Layer 1
tft = tft_test_tool.create_panel(cfg, adapter)        # Layer 2
test = tft_test_tool.TftTest(tft, 240, 320, "spi", raw_bus=bus)
test.fps_test()                                       # Layer 3 單項或 test.run_all()
bus.deinit()
```

### boards/<板名>/config.json 結構

單板 config（無 profiles 包裝層，直接是板子描述）：

```json
{
    "name": "rgb_st7701_touch_lcd4",
    "desc": "Waveshare ESP32-S3-Touch-LCD-4 4\" 480x480 RGB (ST7701)",
    "bus": "rgb|spi|i80|dsi|i2c",
    "driver": "ST7701|ST7789|...",
    "width": 480, "height": 480,
    "pixel_format": "RGB565_BE",
    "pre":  [ { "type": "ch32v003|ldo|pin", ... } ],
    "ctrl": { "cs": 42, "sck": 2, "mosi": 1 },
    "rgb":  { "data": [...16 pins], "hsync": 38, ...timing... }
}
```

| 欄位 | 說明 |
|---|---|
| `pre` | bus 建立前的電源/reset 前置動作：`ldo`(P4 DSI PHY) / `pin`(背光拉高) / `ch32v003`(LCD-4 的 IO expander 開機序列+背光 PWM) |
| `ctrl` | ST7701 等 RGB 面板的 3-wire 控制 SPI 腳位 |
| `rgb` | RGB 並行腳位 + hsync/vsync porch/pulse timing + `freq` (pclk)，選填 `bb_size_px` / `queue_depth` / `colmod` |
| `dsi`/`spi`/`i80`/`i2c` | 各 bus 參數，欄位名與 `lcd_bus.*Bus()` kwargs 對應 |

內建 5 個板子：`rgb_st7701_touch_lcd4`（Waveshare 4" RGB）、
`dsi_jd9165_1024x600`（P4）、`spi_st7789_240x320`、`i80_st7789_240x320`、
`i2c_generic_0x3C`。新增板子 = 複製一個資料夾改 config，不用動程式碼。
`load_board()` 相容舊格式 `{"LCD": {"profiles": ...}}`（自動取 active profile）。

測試項目（全部走 TFT 統一介面，任何 bus/面板通用）：

| 測試 | 量什麼 |
|---|---|
| `fps_test` | adapter 層純吞吐（`write_frame_dma`） |
| `fps_test_tft` | TFT.show_frame 層（library 開銷） |
| `fps_test_present` | begin_display + present 管線 |
| `fps_test_blit` | 整頁原子更新（blit/page-flip） |
| `fps_breakdown` | DSI 分段計時：write vs present+wait |
| `vsync_period` | 面板真實幀週期（只翻頁不寫像素） |
| `flush_bench` | flush/wait_all 代價 |
| `test_streaming` | **隱性視窗** — 不 set_window 連續分批寫入（自動接續 + wrap） |
| `test_window` | **顯式 set_window** — 區域獨立更新（上半/下半/中條） |
| `fill_colors` / `color_bars` / `gradient` / `checkerboard` / `shapes` / `animate` | 目視驗證 |

### DSI 性能預期（JC1060P470 + JD9165 1024x600）

| 測試 | 預期 | 說明 |
|---|---|---|
| `fps_test` | ~60+ FPS | DMA2D 硬拷 1.2MB ~4ms，顯示上限 ~65Hz |
| `fps_test_tft` | 同上 | TFT 層開銷 ~µs 級 |
| `fps_test_present` | 同上 | 管線模式 |
| Python 直寫 fb | ~21 FPS | CPU 寫 PSRAM 被 XIP/頻寬拖慢 — 生產不要用這條 |

### DSI 注意事項

- **撕裂**：DMA2D 拷進「顯示中」fb，拷貝 ~4ms 一條撕裂線 — 整幀更新可接受。
  要零撕裂：fb_count=2 + 內部 fb 雙緩衝（畫沒顯示的 fb → write(fb view) 幀邊界切換），
  但 Python 整幀 CPU 寫入只有 ~21 FPS。
- **flash 來源 buffer**：frozen bytes 等 flash 映射位址不能給 DMA2D 讀 — 先拷進 RAM。
- **`decode_into(fb_view)` 前**：確保 fb 無未 flush 的 CPU 髒行（首次 `flush()` 一次）。

## 與 mp_Net-Core 的差異

- 移除 `lib.sys_bus` / `boot.py` / `log_service` 依賴（錯誤訊息 fallback 到 print）
- `TFT.__init__` 必須傳 `adapter`（不再 fallback SpiBusAdapter）
- 新增 `DsiBusAdapter` + `JD9165`
- `fps_test` 系列改走統一介面（原版直接操作 `._spi` / `._dc` / `._cs`）
