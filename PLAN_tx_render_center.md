# 單一渲染任務統一所有總線寫入 —— 研究與方案（TxCenter 落地設計）

> **對象**：mp_Net-Core `slave/` + mp_lcd_bus（C 層 DMA）
> **問題**：渲染任務有 DMA 需求，DMA 要求內部 SRAM；SRAM 有限 → 幀資料必須分層（chunk）→
> 每層一次中斷/一次 ISR 拷貝 → 中斷暴增、CPU 被 copy 吃掉。目標：用**一個渲染任務**完成**所有總線寫入**。
> **日期**：2026-10-25
> **對應既有文件**：mp_Net-Core `doc/03_notes/17_tx_render_center_plan.md`（問題與目標，本文件是「做法」）、
> `doc/03_notes/05_psram_zero_block_plan.md`（PSRAM 零拷貝）、`doc/03_notes/02_buffer_architecture.md`（L0–L5）、
> `doc/03_notes/23_pixel_lab_bringup_checklist.md`（核心分配實測）。

---

## 0. 結論（TL;DR）

1. **「分層 → 非常多中斷」的真正代價不是中斷本身，而是 ISR 內的 32KB memcpy**：
   每 chunk 一次 ≈ 2.3ms 純 CPU 拷貝（`05_psram_zero_block_plan.md` 實測）。
   一幀 150KB（240×320）→ 5 chunk → fire 阻塞 12ms；一幀 800KB（480×854）→ 25 chunk → 57.5ms。
   把 memcpy 拿掉之後，每個完成中斷的 ISR 成本只剩 µs 級，中斷「多」也不再痛。
2. **「一個渲染任務」要成立**：把「誰可以寫哪個硬體」收斂成單一決策點 TxCenter。
   所有模組只 submit（交資料 + 策略），硬體寫入只剩 TxCenter 一個地方。
3. **S3 上的妥協（有實測依據，非設計潔癖）**：LCD/SPI/I2S 只能 core0、WS2812-RMT 阻塞 85ms
   不能放 core0 → 落地先用「**一個邏輯渲染中心 + 兩個執行器（core0 / core1）**」；
   若要**嚴格單一 task**，需把 WS2812 改 RMT+DMA（C 層新 driver），列為 Phase 3 選配。
4. **C 層只差三刀**：(a) 拿掉 ISR memcpy（內部 staging 雙緩衝，或 PSRAM 直讀）；
   (b) 整幀一次提交、Python 每幀只參與一次（per-frame done 事件）；
   (c) 拉大 chunk（32KB → 64/128KB）減少層數。

---

## 1. 現況盤點：誰在寫哪條總線

### 1.1 渲染鏈（現況已收斂的部分）

```
producers（core0/1 計算端）               RenderTask（core1，播放端）
stream(0x3003) / pixel(PixelTask) /      hub.read_into(big_buffer)
pixel_lab ──▶ pixel_stream hub ────────▶ mark_dirty → show_all()
        （AtomicStreamHub，10 slots）        └─ per-controller 轉換 + 硬體寫入
```

`show_all()` 目前每 controller 一個 writer（`lib/sw/PixelController.py`）：

| controller | 硬體寫法 | 阻塞性 | 現況問題 |
|---|---|---|---|
| WS2812 | `neopixel.write()`（RMT，無 DMA） | **阻塞 85ms@2000 顆**（doc 23 實測） | 物理上限 11.7fps；放 core0 會拖垮 UI |
| APA102 | `spi.write(spi_buffer)` | lcd_bus：異步入隊**但沒人 wait_all** | queue 滿（深度 8）→ `RuntimeError("queue full")`；`ref_bufs` 釘住 GC。machine.SPI fallback：同步 6.4ms@4016 |
| i2c_pixel (PCA9685) | I2C `writeto_mem` 同步 | ~2ms | 可接受 |
| uartMotor1 | `uart.write(frame)` 同步 | 32 台 @9600 ≈ 17ms | 每幀重送、內容沒變也送（doc 17 P4） |

### 1.2 渲染鏈以外的寫入者（doc 17 已盤點，共 11 處）

| 接口 | 寫入者 | 核心 | 特性 |
|---|---|---|---|
| NC4 回覆（circuit UART） | `bus_decode` → `circuit_bus` | core0 | 不可丟、stop-and-wait、最優先 |
| NC4 回覆（WS/TCP/UDP） | `net_bus` | core0 | 不可丟 |
| TFT/LCD（SPI/I80/RGB） | `lvgl_task` → `bus_adapter` | **只能 core0** | DMA 分 chunk，可丟 |
| 音訊 I2S | `audio_player_task` | **只能 core0**（失步實測） | 硬體節拍阻塞 46ms/8KB |
| 馬達 UART-412 | `uart_motor`（經 st_show） | core1 | 可合併、可節流 |
| 顯示 5-byte 幀 | `action_task_1` | core0 | 事件驅動、可丟 |
| MP3-TF-16P | `mp3_tf_16p` | core0 | writer 內含 sleep_ms(15) |
| SD | `fast_io` | 雙核（已有 `_sd_lock`） | 儲存，非渲染輸出 |
| 日誌（REPL） | `log_task` | core0 | 可丟、低優先 |

**關鍵雷點**：APA102 走 `spi_list`（`spi_drv.py` 現在優先建 `lcd_bus.SPIBus`），而 TFT 也走同一
`spi_list` 的 host（config 可配成同一條）。若同一實體 SPI host 被 core1 的 RenderTask（APA102 入隊）
與 core0 的 lvgl_task（TFT 入隊/排空）共寫，就是**跨核共寫同一條 C queue，無任何仲裁**
（doc 17 P2 的實例化）。目前 APA102=0 的板子沒踩到，只是運氣。

---

## 2. DMA × SRAM × 分層 × 中斷 —— 問題本質

### 2.1 ESP32-S3 硬體事實

- 內部 SRAM 總量 512KB；扣除 IDF/系統後，**DMA 可用內部 RAM 實測 free ≈ 161KB**（doc 05）。
- GDMA 硬體**可以直讀 PSRAM**（`SOC_PSRAM_DMA_CAPABLE=1`），但 MicroPython v5.5 的
  SPI driver `esp_ptr_dma_capable()` 只認內部 SRAM → PSRAM buffer 一律被判「非 DMA」。
- 後果（doc 05 已定位）：**每 32KB chunk 在 SPI driver ISR 內同步
  `heap_caps_aligned_alloc + memcpy`（PSRAM→內部），≈ 2.3ms/chunk**。這不是 DMA 慢，是 CPU 被 ISR 拷貝吃掉。
- `lcd_bus` 現況（C 層）：單筆 ≤32KB 且內部 RAM → 直接入隊（queue depth 8）；大於 32KB 或 PSRAM →
  自動分 chunk 直送。I80 每 chunk 一次 `on_color_done`；RGB 走 `esp_lcd` bounce（`bb_size_px` 內部
  SRAM bounce，LCD_CAM 掃描時持續 refill → 中斷率 ∝ 行率 ÷ bounce 大小）。

### 2.2 中斷/CPU 成本量化

| 場景 | chunk 數/幀 | ISR 內 copy 成本 | 完成中斷/秒（@50fps） |
|---|---|---|---|
| APA102 4016（~16KB，PSRAM buffer） | 1 | 2.3ms | 50 |
| TFT 240×320×2 = 150KB | 5 | 12ms | 250 |
| LCD 480×854×2 = 800KB | 25 | **57.5ms** | 1250 |
| RGB 面板（常駐掃描） | bounce refill 每 bounce 一次 | 有（copy 進 bounce） | 行率級，持續不斷 |

**結論：分層本身不可避免（SRAM < 幀大小），但「每層一次 ISR memcpy + 每層一次 Python 參與」
才是真正的開銷。** 解法不是「不分層」，而是三件事：

1. **層與層之間不再拷貝**：PSRAM 直讀（方案 A）或 decode 直寫內部 staging（方案 B）。
2. **Python 每幀只參與一次**：C 層整幀自動分 chunk + 單一「幀完成」事件。
3. **層數盡量少**：chunk 加大、RGB bounce 加大、queue 只做流水線不做緩衝堆。

---

## 3. 目標架構：TxCenter（單一渲染中心）

### 3.1 唯一寫入權（doc 17 G1/G2）

新增 `slave/lib/sys/tx_center.py`：

```python
# 對外只有三件事：註冊、提交、排空。任何模組不得再直接碰 bus.write / uart.write / spi.write。
tc = TxCenter()
tc.register_iface("nc4_fast",  writer_fn, kind="fast", cap=8)      # 不可丟
tc.register_iface("motor",     writer_fn, kind="frame", cap=4)     # 可丟/可合併
tc.register_iface("apa102",    writer_fn, kind="frame", cap=3)     # 可丟
tc.register_iface("tft",       writer_fn, kind="frame", cap=2)     # 可丟（大幀 view 模式）
tc.register_iface("i2s",       writer_fn, kind="exclusive")        # 音訊流，獨佔節拍
tc.submit("nc4_fast", mv)                    # copy 模式，小
tc.submit("apa102", mv_or_view, tag=...)    # view 模式，零拷貝
```

- 每個接口底下一條 SPSC（`AtomicStreamHub`，用既有無鎖 ring，不新造輪子）。
- `writer_fn` 是**唯一**會呼叫硬體 API 的函式，全部集中在 TxCenter 兩個執行器內。

### 3.2 排空順序與丟棄策略（G3/G4）

- 節拍：tick = 1ms（fast 車道）；beat = 20ms 或 stream fps（幀車道，沿用 render.py 現有的
  `next_tick_us`/同步起播邏輯，原封不動搬進來）。
- 排空順序（每圈固定）：**fast 全排空 → frame 每接口取最新（舊的覆蓋丟棄）→ exclusive 有就送**。
- 節流：馬達接口內容 hash（值沒變不提交，doc 17 P4）；顯示幀可選去重；幀車道 queue 滿 →
  丟最舊（frame 類本來就可丟），fast 車道永不丟、滿了要能統計告警（G5）。

### 3.3 執行器部署（S3 硬限制下的取捨）

實測依據（不允許違反）：

| 限制 | 依據 |
|---|---|
| LCD/SPI 寫入只能在 core0（core1 會崩） | `task_manager._check_affinity` 已防呆；Core_Manager 註釋 |
| I2S 寫入只能在 core0（core1 DMA 失步） | Core_Manager 註釋（實測交換核心後修復） |
| WS2812 RMT 阻塞 85ms 不能放 core0 | doc 23 實測：UI 42.7→10.6fps |
| UART/PCA/`lcd_bus` queue 兩核皆可（C 層） | 馬達 core1 已上線運行 |

因此：

- **方案 A（推薦，Phase 1 落地）— 一個中心、兩個執行器**：
  - core0 執行器：fast 車道（NC4 回覆、MP3、顯示幀）+ DMA 幀（APA102、TFT/LCD、I2S）。
  - core1 執行器：阻塞幀（WS2812 RMT、馬達 UART）+ pixel 幀 beat 的取幀/轉換。
  - 同一套 TxCenter 類別、同一套優先級規則、同一個統計出口；對系統其餘部分它就是
    「**一個渲染任務**」。`PixelStreamer.show_all()` 的 per-controller 寫入拆成 writer 註冊進中心。
- **方案 B（嚴格單 task，Phase 3 選配）— WS2812 改 RMT+DMA**：非阻塞後全部寫入可收進
  core0 單執行器。C 工作：RMT TX + DMA + 中斷完成 + 雙緩衝（`led_strip` 風格）。
- **明確不做**：全部塞 core0 但 WS2812 維持 RMT 阻塞 —— 會重現 doc 23 的 10fps 災難。

### 3.4 pixel 幀流程（方案 A 下的完整路徑）

```
core1 計算（PixelTask/pixel_lab/stream producer）
   → pixel_stream hub（3 slots，PSRAM）
   → core1 執行器 @beat：read_into → mark_dirty → 分 controller：
        blocking 類（WS2812/馬達）→ core1 writer_fn 直接寫
        DMA 類（APA102）→ submit 到 core0 執行器（跨核 SPSC，傳 memoryview 零拷貝）
   → core0 執行器 @tick：pending < 閾值才入隊；滿 → 丟幀（可丟語意）；每幀一次 flush
```

---

## 4. C 層改造（mp_lcd_bus）：消滅「非常多中斷」的實質成本

### 4.1 內部 SRAM 預算（S3，DMA 可用 ≈161KB）

| 消費者 | 現況 | 目標 |
|---|---|---|
| pixel_stream hub | 10 slot × 4B×Npx：336px→13KB、2000px→**80KB**、4016px→**160KB（爆）** | 3 slot，slot 放 PSRAM（CPU 讀寫，不需 DMA） |
| APA102 `spi_buffer` | 4016px → 32KB（bytearray） | 放 PSRAM（走 staging/直讀路徑） |
| WS2812 neopixel buf | 2000px → 6KB | 不變（RMT 必須內部） |
| lcd_bus SPI `zero_buf` | 32KB 內部 | 不變 |
| **方案 B staging** | — | **2 × 32KB 內部雙緩衝 = 64KB** |
| RGB bounce | bb_size_px 視 config | 一整行（如 480px×2B≈1KB）內部 |

目標組合把 hub 從「最大 160KB 內部」砍到 ~10KB，騰出空間給 64KB staging 雙緩衝。

### 4.2 三招

1. **拿掉 ISR memcpy（最大收益）**
   - **方案 B（推薦，低風險）**：decode/scatter 直寫內部 `CAP_DMA` staging（2×32KB 雙緩衝），
     送出零拷貝、零 cache 問題；150KB 幀可用「staging 流水線」（一邊 DMA 送、一邊寫下一塊）；
     800KB 大幀一樣流水線，只是吞吐受 DMA 上限。
   - **方案 A（選配，收益更大）**：C 層手動 GDMA descriptor 直指 PSRAM +
     `esp_cache_msync(..., C2M)`（doc 05 方案 A 已寫原理與風險：花屏風險需逐 chunk 驗證）。
2. **減少層數**：`SPI_MAX_CHUNK` 32KB → 64/128KB（S3 GDMA 支持，需在 bus 初始化設定
   `max_transfer_sz` 並實測）；I80 同理。層數減半，中斷減半。
3. **Python 每幀只參與一次**：C 層新增 `write_frame(mv) → frame_id` + `frame_done(frame_id)`
   （內部自動 chunk 填 queue、最後一筆完成置 flag）；Python 端每幀只 poll 一次，
   不再 per-chunk `pending()/wait()` 循環。**中斷仍每 chunk 一次（DMA 本質），但 Python 與
   決策迴路完全不進 chunk 層。**

### 4.3 RGB 特例

- 常駐掃描的中斷率 = 行率 ÷ bounce 大小。改 `refresh_on_demand=1`：**有幀才 refill**，
  空閒停掃（省電 + 零中斷）；`bb_size_px` 設整行把 refill 次數壓到最低。
- 流式（`no_fb` + `draw_bitmap`）餵料改走 4.2 的 staging 路徑。

---

## 5. 分階段落地

| Phase | 內容 | 檔案 | 驗收 |
|---|---|---|---|
| **0（半天）** | 修 APA102 queue 沒排空的雷：show_all 後 `flush()`（或 pending 節流）；hub 10→3 slot + PSRAM；[GATE] 加 pending 統計 | `PixelController.py`、`Core_Manager.py` | 4016 APA102 跑 10 分鐘無 `queue full`、無 GC 飆升 |
| **1（1–2 天，純 Python）** | TxCenter 骨架 + 雙執行器；遷移順序：馬達（+hash 節流）→ MP3/顯示幀 → pixel 幀 beat（拆 show_all writers）→ 最後 NC4 fast lane | 新 `lib/sys/tx_center.py`；改 `render.py`、`uart_motor.py`、`mp3_tf_16p.py`、`action_task_1.py`、`circuit_bus.py`、`Core_Manager.py` | 全功能回歸 + 夜測不掉幀 |
| **2（1–2 天）** | LCD 併入：lvgl_task 只產 dirty rect → `tc.lcd_blit`；`write_frame_dma` 邏輯移進 LCD writer；RGB `refresh_on_demand` | `lvgl_task.py`、`bus_adapter.py`、RGB config | UI 幀率不降、RGB 空閒零中斷 |
| **3（C，3–5 天）** | mp_lcd_bus：`write_frame`/`frame_done`、chunk 加大、方案 B staging 雙緩衝；（選配）方案 A PSRAM 直讀；（選配）WS2812 RMT-DMA → 嚴格單 task | `spi_bus.c`、`i80_bus.c`、`rgb_bus.c`、（新 `ws2812_dma`） | 150KB 幀 fire <1ms；800KB 幀流水線 50fps；中斷數統計下降 |
| **4** | 可觀測性：每接口 queue/drop/max-latency → `bus.shared` + log schema；對照 doc 08 基準夜測 | `tx_center.py`、`log_task.py` | G5 全綠 |

**遷移鐵律**：一條實體線的 `writer_fn` 只在一個核心執行（先上板驗證 APA102 core0 執行器 +
core1 提交的跨核 SPSC，再動其餘接口）。

---

## 6. 風險

1. **跨核 SPSC + C queue 交互**：core1 提交、core0 排空同一 `lcd_bus` queue 的並發安全
   （C 層目前無鎖，靠「單一寫入者」隱含假設）→ Phase 1 先驗證、必要時 C 層加關斷保護。
2. **馬達 hash 節流改變幀率語意**（20ms 重送 vs 變化才送）→ config 開關，預設保守。
3. **NC4 延遲**：fast lane 1ms tick 是否影響 921600 stop-and-wait 吞吐 → 夜測對比 doc 08 基準。
4. **RGB refresh_on_demand**：需驗證無殘影/閃爍；bb_size 加大佔內部 SRAM，照 4.1 預算表走。
5. **方案 A 的 cache 同步**（`esp_cache_msync` C2M）：花屏風險，逐 chunk 驗證後才開預設。

---

## 7. 與既有文件的對照

- `17_tx_render_center_plan.md`：G1–G7 為目標；本文件補上「做法」（執行器拓撲、隊列策略、
  C 層三招、階段計畫）。
- `05_psram_zero_block_plan.md`：其方案 A/B 與本文 4.2 一致；本文新增「per-frame done 事件」
  與「chunk 加大」兩個正交手段。
- `02_buffer_architecture.md`：TxCenter 落在 L4/L5 交界；不新增 bounce buffer，
  沿用 `AtomicStreamHub` + `alloc_dma` 契約。
- `23_pixel_lab_bringup_checklist.md`：核心分配的實測依據（本文 3.3 的表格來源）。
