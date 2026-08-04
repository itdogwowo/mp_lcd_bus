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
RM67162 / SH8601），新增 **`JD9165`**（1024x600 MIPI DSI）。

TFT API（所有面板一致）：`show()` / `show_async()` / `show_frame()` /
`begin_display()` + `present()` + `present_wait()` / `set_rotation()` /
`set_color_order()` / `invert_display()` / `fill()` / framebuf 相容。

## 層次 3 — 全能測試工具（`tft_test_tool.py`）

**一個檔案測所有屏幕**：頂部 ⚙ 設定區改 `BUS` / `DRIVER` / 解析度 / 各 bus 參數，
不用改任何其他程式碼：

```python
import tft_test_tool
tft_test_tool.run_all()          # 依 ⚙ 設定區自動初始化 → FPS×3 → flush → 目視
```

| 設定 | 可選值 |
|---|---|
| `BUS` | `spi` / `i80` / `rgb` / `dsi` / `i2c` |
| `DRIVER` | ST7735 / ST7789 / ST7796 / GC9A01 / ILI9341 / GC9D01 / NV3030B / RM67162 / SH8601 / **JD9165** |
| `WIDTH` / `HEIGHT` | 依面板 |
| `SPI` / `I80` / `RGB` / `DSI` / `I2C` dict | 各 bus 的腳位 / timing / 頻率 |

測試項目（全部走 TFT 統一介面，任何 bus/面板通用）：

| 測試 | 量什麼 |
|---|---|
| `fps_test` | adapter 層純吞吐（`write_frame_dma`） |
| `fps_test_tft` | TFT.show_frame 層（library 開銷） |
| `fps_test_present` | begin_display + present 管線 |
| `flush_bench` | flush/wait_all 代價 |
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
