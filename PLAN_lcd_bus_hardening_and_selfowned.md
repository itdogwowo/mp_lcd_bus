# 計劃書：LCD/LED 底層改裝 — Soft Reboot 根治 + LED 並行 + MPY API 貼合（定稿）

> 狀態：**調查完成，定稿待執行**（不落地，等排期）
> 日期：2026-08-01
> 範圍：`mp_lcd_bus`（esp32_src：spi / i80 / rgb / dsi）+ 新增 LED 並行層
> 目標晶片：**ESP32-S3（現行）**、**ESP32-P4（未來）**

---

## 0. 決策摘要（TL;DR）

1. **顯示層**：維持 esp_lcd（i80/rgb/dsi 現況不動）；SPI 已全自研（`spi_bus.c`）。
2. **LED 並行層**：**改裝 esp_lcd 代碼**（複製 → 私有副本 → 改 API 面），不污染原始庫。
   - S3 = LCD_CAM（I80）raw，P4 = PARLIO raw。兩者共用 FastLED `transpose16x1` + DMA double-buffer 引擎。
3. **IDF 版本**：現以 **v5.5.1** 開發；已查證 **v6.0 保留 esp_lcd/i80/parlio**，API 僅型別整理（`gpio_num_t`、`dma_burst_size`）→ **不會白做**；改裝時預留 v6 相容（HAL include 解耦）。
4. **MPY API 貼合**：LED 層以 mpy 慣例設計（`write/pending/wait/wait_all/deinit`，對齊現有 bus），可適配更多操作。
5. **Soft Reboot 根治**：`cleanup()` + boot.py 開頭呼叫（繞過 `__del__` 不執行）。

---

## 1. 動機與目標（三議題）

| # | 議題 | 現況痛點 | 目標 |
|---|---|---|---|
| A | Soft Reboot 殘留 | 重建 bus 後常需硬重啟 | soft reboot 可完全乾淨 |
| B | LED 並行（16 GPIO 多條 WS2812） | esp_lcd 是「顯示」抽象，無法無空隙時序 + 完全掌控 | 自研/改裝並行層，S3=LCD_CAM、P4=PARLIO |
| C | MPY API 貼合 | 現有 C 層 API 不夠 mpy 慣例化、難適配更多操作 | API 對齊 mpy 習慣，擴充性高 |

---

## 2. 調查結論（證據）

### 2.1 Soft Reboot 根因（查 mpy 源碼 `ports/esp32/main.c`）

- soft reset 只 `gc_init + mp_init + machine_pins_init`，**不執行 C 物件 `__del__`**。
- `sys.atexit` 只在 `sys.exit()` 跑，**不是 reset hook**。
- 外設（SPI master / esp_lcd / ISR / DMA）原封不動 → 二次 init 殘留。
- SPI 已清 statics + handle（`s_spi_device`/`s_spi_zero_buf`/handle=NULL）；**i80/rgb/dsi 的 esp_lcd teardown 完整性待驗證**。

### 2.2 PSRAM 傳輸序列化（查 `spi_bus.c` + IDF `spi_master.c`）

- 舊 copy 路徑（每 chunk `memcpy → wait_queue_empty`）把可異步的傳輸強制序列化 → decode/DMA 無法重疊。
- v5.5 `setup_priv_desc`（ISR）自動處理外部 buffer：DMA-capable（S3 PSRAM）→ 直讀零 copy；unaligned → 內部 alloc+copy（不阻塞 Python）。
- 已改 async 直送（`df47f54`），**未驗證**（掛起事項 §8.1）。

### 2.3 esp_lcd 抽象（本專案現況）

- i80/rgb/dsi 走 esp_lcd 高層；i80 的資料路徑（queue/回呼/ref_bufs/deinit）已自研，僅「建 bus」用 esp_lcd。
- esp_lcd 是「面板」語義（cmd/param/dc/回呼），對 LED pattern 流是累贅 → 動機 B 成立。

### 2.4 IDF v6 前瞻（查本地 esp-idf v6.0 tag）

| 項目 | v5.5.1 | v6.0 | 影響 |
|---|---|---|---|
| `esp_lcd` 元件 | ✅ | ✅ 保留 | 不會白做 |
| `esp_lcd/i80/esp_lcd_panel_io_i80.c` | ✅ | ✅ 保留 | 改裝對象穩定 |
| `esp_lcd_io_i80.h` API | `int`/deprecated align | `gpio_num_t`、`dma_burst_size`、移除 deprecated | 型別整理，函數不變 |
| HAL 層 | 自含 register | **抽 `esp_hal_lcd`**（`hal/lcd_hal.h`） | i80 實作改 include HAL |
| `parlio` | `esp_driver_parlio` | `esp_driver_parlio` + `esp_hal_parlio` | 保留、穩定 |
| `mp_heap_caps`/`mp_lcd_bus` 等 mpy 模組 | v5.5 編譯 | 待驗證 | M0 驗證 |

**結論**：esp_lcd 支援最新 IDF v6，API 僅微調 → 以 v5.5.1 開發不白做；改裝檔預留 v6 解耦（HAL 層分離後 i80 檔多一個 include）。

### 2.5 FastLED 並行調查（查源碼 `src/platforms/esp/32/`）

| 平台 | 並行能力 | 機制（FastLED 源碼證實） |
|---|---|---|
| **ESP32-S3** | **16 條** + 4 條 RMT | `channel_driver_i2s.h` = "I2S **LCD_CAM** implementation for S3"：`data_gpios[16]`、PCLK 2.4MHz、`transpose16x1_noinline2`（Yves）、DMA double-buffer |
| **ESP32-P4** | 16/unit PARLIO（可 2 unit=32）+ LCD-RGB 16 條 | `parlio/clockless_parlio_esp32.h`：`data_width=16`、自動 1/2/4/8/16-bit、`parlio_tx_write` + GDMA |

**關鍵架構發現（S3）**：
- S3 並行 = **I2S 介面操作唯一一個 LCD_CAM（I80 bus）** — `SOC_LCDCAM_I80_NUM_BUSES=1`。
- **S3 顯示與並行 LED 共用 LCD_CAM** → 若顯示走 I80 會與 LED 並行衝突；**但本專案 TFT 走 SPI（host1）+ SD 走 SDMMC，LCD_CAM 完全空閒 → 現行板卡並行 LED 零衝突**。
- FastLED 標 I2S-LCD_CAM 為 EXPERIMENTAL（低優先）；P4 的 PARLIO/LCD-RGB 成熟。

### 2.6 使用者技術前提（自行確認）

- 「channel_engine_i2s_esp32s3 實際走 i80_bus」→ **證實**（S3 唯一 LCD_CAM = I80 bus）。
- 「LED 需要 RGB、I8080、P4 新介面」→ **證實**（S3=I2S-LCD_CAM 16 線、P4=PARLIO 16/unit）。

---

## 3. 決策

### 3.1 改裝 vs 自研 vs 直接用 esp_lcd

| 方案 | 優點 | 缺點 | 決策 |
|---|---|---|---|
| 直接用 esp_lcd | 零改動 | 顯示語義，無法無空隙時序 + 完全掌控 | ❌ |
| 純自研 | 完全掌控 | 踩 GPIO/DMA/PCLK 的坑，成本高 | ❌ |
| **複製改裝（fork）** | **保留驗證過的硬體層，只改 API 面**；不污染原始庫 | 需帶私有 helper 依賴 | ✅ |

### 3.2 目標架構（定稿）

```
mp_lcd_bus/
├── esp32_src/spi_bus.c        → 串列：LCD + WS2812 單線（已全自研，df47f54）
├── esp32_src/i80_bus.c        → 顯示：LCD 8080（esp_lcd 建 bus + 自研資料路徑，現況）
├── esp32_src/rgb_bus.c        → 顯示（esp_lcd，現況）
├── esp32_src/dsi_bus.c        → 顯示（esp_lcd，現況）
└── esp32_src/led/             → 並行 LED（新）
    ├── led_lcdcam_s3.c        → S3：LCD_CAM raw（改裝 esp_lcd_panel_io_i80.c）
    ├── led_parlio_p4.c        → P4：PARLIO raw（直打 esp_driver_parlio，v6 為 esp_hal_parlio）
    ├── led_engine.c           → 共用引擎：transpose16x1 + DMA double-buffer + 預編碼 pattern
    └── modled.c               → MPY 介面層
```

### 3.3 改裝範圍（複製清單）

| 來源（v5.5.1） | 目的 | 改裝 |
|---|---|---|
| `esp_lcd/i80/esp_lcd_panel_io_i80.c`（40KB） | LCD_CAM 的 DMA/GPIO/PCLK 底層 | 改 API 前綴 `esp_lcd_*`→`led_*`；**移除 panel 語義**（cmd/dc/param 相位）；保留 LCD_CAM init/GPIO/DMA descriptor/PCLK/teardown |
| `priv_include/esp_lcd_common.h` + `src/esp_lcd_common.c` | 私有 helper | 一併複製或改呼叫 |
| `esp_lcd_io_i80.h` | config 結構 | 精簡為 LED 版 config（無 dc/cs/panel 欄位） |
| FastLED `transpose16x1_noinline2` | 16 線位元轉置 | 照搬（Apache-2.0） |

**P4（parlio）不複製**：`esp_driver_parlio` 本身 raw，直接連結用。

### 3.4 IDF 版本策略

- **現在**：v5.5.1（現行工具鏈）。
- **未來升 v6**：改裝檔多 include `esp_hal_lcd`（i80 HAL 分離）；API 已型別整理。
- **M0 驗證**：v6 下 mp_lcd_bus/mp_heap_caps 等 mpy 模組能否編譯（未驗證項目）。

---

## 4. MPY API 設計（C 層 → mpy 層）

對齊現有 bus 風格（`write/pending/wait/wait_all/deinit`），適配更多操作：

```
# S3（led_lcdcam_s3.c）—— 16 線並行 WS2812
from lcd_bus import LedParallel
led = LedParallel(data=[1..16], pclk_hz=2400000)   # data=GPIO 列表（或 width + base）
led.write(pattern_buf)          # 送一幀預編碼 pattern（mpy buf 介面，零 copy）
led.start()                     # 持續輸出（DMA 循環模式）
led.stop()                      # 停止（等目前 frame 結束）
led.pending() / led.is_busy()   # queue 狀態（對齊 spi_bus）
led.wait() / led.wait_all()     # 同步等待（對齊 spi_bus）
led.clear()                     # 全滅（送全零 pattern）
led.deinit()                    # 釋放（對齊 bus deinit 清 handle）
```

- 支援**兩種輸出模式**：一次性 `write()`（靜態圖）+ 持續 `start()/stop()`（動畫/DMA 循環）。
- **內建 transpose**：mpy 層只給 RGB 資料，C 層 `led_engine` 轉置成 16 線 pattern（或 mpy 給預編碼，二選一，M5 定）。
- **多段**：`led.write_multi([buf1, buf2, ...])` 供大 pattern 分塊（避開 max_transfer_bytes）。

**顯示層 MPY API（現有，不動）**：`SPIBus/I80Bus/RGBBus/DSIBus` 維持。

---

## 5. 實作範圍

### 5.1 新增/修改檔案

| 檔 | 動作 |
|---|---|
| `esp32_src/led/led_lcdcam_s3.c` | 新增（改裝 esp_lcd i80） |
| `esp32_src/led/led_parlio_p4.c` | 新增（直打 parlio） |
| `esp32_src/led/led_engine.c/.h` | 新增（transpose + DMA double-buffer） |
| `esp32_src/led/modled.c` | 新增（MPY 介面） |
| `modlcd_bus.c` | 註冊 `LedParallel` 型別 |
| `esp32_src/spi_bus.c` | 已改（async 直送）待驗證 |
| `esp32_src/i80/rgb/dsi_bus.c` | deinit teardown 補強（§2.1 待驗證項） |
| `micropython.cmake` | 加入 led/ 源碼 |

### 5.2 不污染原始庫

改裝代碼放 `mp_lcd_bus/esp32_src/led/`，**不改 esp-idf 元件**（esp_lcd/esp_driver_parlio 維持原樣）。

---

## 6. 路線圖

| 里程碑 | 內容 | 驗證 | 時程 |
|---|---|---|---|
| **M0** | IDF v6 相容性探測（mpy 模組編譯） | v6 下 build + boot | ~1 天 |
| **M1** | Soft Reboot 根治：`cleanup()` + boot.py + i80/rgb/dsi teardown 補強 | soft reboot ×50，`tft_probe` 全 DMA | ~1-2 天 |
| **M2** | `make_new` 偵測殘留 → `esp_restart()` 保險（config 開關） | 刻意殘留 → 自動恢復 | ~半天 |
| **M3** | spi_bus.c async 直送驗證（重疊 + 花屏） | `tft_pipeline_probe` + animate | ~1 天 |
| **M4** | LED 串列：`mp_led`（SPI raw + 預編碼 pattern） | 燈帶點亮 + 計時 | ~1-2 天 |
| **M5a** | LED 並行 S3：`led_lcdcam_s3.c`（改裝 esp_lcd i80 + transpose 引擎） | 16 線並行 WS2812 點亮 + 示波器 | ~3-5 天 |
| **M5b** | LED 並行 P4：`led_parlio_p4.c` | 16/unit 並行 + DSI 顯示並存 | ~2-3 天 |
| **M6（可選）** | I80 顯示資料路徑自研（繞 esp_lcd 建 bus） | I80 bench + soft reboot | ~2-3 天 |

**順序**：M1 → M3 → M5a（S3 主戰場，板子現況 LCD_CAM 空閒零衝突）→ M5b → M4。M0/M2 視排期。

---

## 7. 風險

| 風險 | 影響 | 緩解 |
|---|---|---|
| S3 顯示改走 I80 時與並行 LED 衝突（共用 LCD_CAM） | 功能互斥 | doc 標註；LED 用 I80 時顯示改用 SPI |
| WS2812 無空隙時序 vs esp_lcd transaction 語義 | 花屏 | 改裝時不走 panel_io 高層，直打 LCD_CAM raw |
| PSRAM cache 一致性（S3 無 `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`） | 直送花屏 | M3 驗證；花屏則 double-buffer/回退 |
| IDF v6 升版 | 改裝檔 include 變動 | M0 預測；API 型別已確認 |

---

## 8. 掛起事項

### 8.1 PSRAM 異步直送（已改未驗證）

- root repo commit `df47f54`；**ext_mod 被還原到 `6d99e0e`** → 下次 build 前需同步。
- firmware `17:11` 含此修改，**未驗證**：
  1. `tft_pipeline_probe.run_all()` → work=15/50ms 應「✓ 重疊」
  2. `animate(fb_mode="auto")` → 不花屏
  3. 花屏 → 回退或 double-buffer

### 8.2 使用者專案檔案（`slave new/`）

- `tft_dma_bench.py`（crash 修復）+ `tft_min_dma_probe.py` + `tft_pipeline_probe.py` 已 commit `34ff5e2`；`tft_pipeline_probe.py` 待 commit。

---

## 9. 附錄：調查證據索引

- mpy soft reset：`lib/micropython/ports/esp32/main.c:133-190`
- IDF v6 esp_lcd/parlio：`git ls-tree v6.0 components/`（esp_lcd/esp_hal_lcd/esp_driver_parlio/esp_hal_parlio）
- v6 i80 API diff：`git diff v5.5.1 v6.0 -- components/esp_lcd/include/esp_lcd_io_i80.h`
- v6 i80 include HAL：`git show v6.0:components/esp_lcd/i80/esp_lcd_panel_io_i80.c`
- S3 LCD_CAM 唯一：`soc/esp32s3/include/soc/soc_caps.h:310-313`
- FastLED S3：`src/platforms/esp/32/drivers/i2s/channel_driver_i2s.h`
- FastLED P4：`src/platforms/esp/32/drivers/parlio/*` + `feature_flags/enabled.h`
- 本專案 C 修改：`mp_lcd_bus`（spi/rgb/dsi/i2c 4 檔，commit 6d99e0e + df47f54）
