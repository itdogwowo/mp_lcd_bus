# C 層統一 TX（TxBus）可行性評估 —— LED / LCD / MP3 等「只管寫入」設備

> **範圍**：把所有「只管寫入、其他什麼都不管」的設備的 TX 行為統一收進 C 層，
> Python 端只負責「交 buffer」（submit），不碰時序、分層、等待。
> **實例板卡**：P4-ETH_mp3（TFT ST7789 240×320 + MP3 + PCM5102 + PCA9685 + 676px + SD + RS485 UART×2）
> 與 S3 面板板 —— 螢幕 + MP3 + LED 同板，正好是本案的標的。
> **日期**：2026-10-25

---

## 1. 總判定：可行，而且是「低風險高收益」路線

| 理由 | 說明 |
|---|---|
| 現有 C 層已完成 90% 的基礎 | `lcd_bus` 五種 bus（SPI/I80/RGB/DSI/I2C）**已經有統一的異步 API**：`write(buf)→tid`、`pending()`、`wait(tid)`、`wait_all()`。統一 TX 只是加一層 dispatch + 排程，不是重寫。 |
| 「單一寫入者」假設與架構方向一致 | 現有 C queue **全部無鎖**，隱含契約就是「只有一個 task 呼叫」。你要的「一個渲染任務」正好滿足它 → 零並發風險。 |
| PSRAM 直進硬體已有現成示範 | `dsi_bus.c`（P4）已實現「PSRAM src 零拷貝直進 PPA、CPU 不碰像素」——SPI 的 ISR-memcpy 痛點可以照抄這個思路解決。 |
| 設備節奏都慢 | MP3/馬達/顯示幀都是 9600 baud、每秒幾十筆小幀；LCD/LED 是整幀可丟。C 端管節拍完全夠用。 |

**工作量估值**：核心 5–7 天（TxBus 模組 + Python 遷移 + WS2812 RMT-DMA），分四期，見 §6。

---

## 2. 設備盤點：C 現況 vs 差距

| 設備 | 現在（Python） | C 現況 | 差距 | 可行性 |
|---|---|---|---|---|
| LCD（SPI） | `lvgl_task` → `SpiBusAdapter`（32KB chunk 手動循環） | `SPIBus` 異步 queue 8-deep ✅ | per-frame done + 拿掉 ISR memcpy | **高（現成）** |
| LCD（I80/RGB/DSI） | `I80BusAdapter`/`RgbBusAdapter` | `I80Bus`/`RGBBus`/`DSIBus` 異步 ✅ | RGB 改 `refresh_on_demand`；其餘只差統一入口 | **高（現成）** |
| APA102 | `spi.write(spi_buffer)`（**沒人 wait_all，queue 滿會炸**） | `SPIBus` ✅ | 統一入口順手修掉排空 bug；buffer 雙緩衝 | **高（現成）** |
| PCA9685 | I2C 同步 ~2ms | `I2CBus`（阻塞、無 DMA）✅ | 直接包 | **高（現成）** |
| MP3（UART 9600） | `uart.write(frame)` + `sleep_ms(15)` | 無 UART 模組 | C drain 管節拍、移除 sleep | **高（C glue）** |
| 馬達（RS485 UART-412） | `_Rs485Uart.write`（DE 切換 + txdone） | 無 | C 呼叫現有 wrapper，或 C 自管 DE（注意 doc 04 時序） | **高（C glue）** |
| 顯示 5-byte 幀（UART） | `action_task_1._uart_send` | 無 | 同上，fast 類小幀 | **高（C glue）** |
| WS2812 | `neopixel.write()`（RMT **無 DMA、阻塞 85ms@2000px**） | 無 | 新 C：RMT+DMA + byte→symbol 即時編碼器 | **高（新 C driver，工作量最大）** |
| 音訊 I2S（PCM5102） | `machine.I2S` block 節拍 | 無 | 選配：保留原 task，只把寫入口歸 TxBus | 中（可選） |

---

## 3. 架構：一個 TxBus（C）+ 一個 RenderTask（Python）

```
producers（core0/1 計算端，只管寫 buffer + submit）
   pixel 幀 / LVGL dirty rect / MP3 指令 / 馬達幀 / 顯示幀 / 5-byte 幀
        │ submit(iface, mv)  ← 零拷貝 memoryview，Python 不碰任何時序
        ▼
   ┌────────────────────────────────────────────┐
   │ C: lcd_bus.TxBus（唯一的硬體寫入者）        │
   │  per-iface: slot(1–2) + buffer pin + 節拍   │
   │  drain(): fast → frame(取最新/丟舊) → paced │
   │  writers: SPIBus/I80/RGB/DSI/I2CBus/UART/   │
   │           RMT(WS2812)                       │
   └────────────────────────────────────────────┘
        ▲ drain() 由單一 RenderTask 呼叫（tick=1ms）
```

- **C 端 `lcd_bus.TxBus` API（Python 可見）**：
  ```python
  tx = lcd_bus.TxBus(
      ("tft",     spi_bus_obj, kind="frame",  cap=2),   # 可丟、新覆舊
      ("apa102",  spi_bus_obj, kind="frame",  cap=2),
      ("pca",     i2c_bus_obj, kind="frame",  cap=1),
      ("mp3",     uart_obj,    kind="paced",  min_interval_ms=10),
      ("motor",   uart_obj,    kind="paced",  min_interval_ms=0, hash=True),
      ("display", uart_obj,    kind="fast"),            # 不可丟、排隊
      ("ws2812",  rmt_cfg,     kind="frame",  cap=2),
  )
  ok = tx.submit("apa102", mv)     # False = 舊 buffer 還在飛，請換一塊（雙緩衝）
  tx.drain()                        # 渲染任務每 tick 呼叫
  tx.stats("tft")                   # queued/dropped/max_latency
  ```
- **核心紀律（決定可行性的一條鐵律）**：`drain()` 只由**一個** RenderTask 呼叫 →
  所有 C queue 維持無鎖；另一核的 producer 一律先過 Python 層的 `AtomicStreamHub`
  才交給 RenderTask，**跨核交換發生在 C 之外**。

### 3.1 緩衝區契約（「只需要調動緩衝區」的具體化）

1. **零拷貝**：`submit(iface, mv)` 只存 `(ptr, len, ref)`；C 用現有 `ref_bufs` 模式 pin 住
   buffer（防 GC 回收），drain 完（`get_trans_result` 成功）才放 ref。
2. **雙緩衝 ping-pong**：frame 類接口 cap=2 —— producer 交 A，C 在飛時再交 B；
   `submit` 回 False 表示兩個 slot 都在飛（可丟語意：producer 直接放棄本幀）。
   例：APA102 現在單一 `spi_buffer` 每幀重用 + 異步 DMA 是**隱性撕裂**，改雙緩衝後消滅。
3. **覆蓋不釋放**：新幀覆蓋舊幀時，舊 buffer 的 ref **仍 pin 到其 DMA 交易完成為止**
   （DMA 還在讀它），只是不再等它做節拍 —— 這點 C 必須照現有 `ref_bufs` 語意做，不能提前釋放。
4. **Python 端零複製**：pixel 的 `big_buffer`、LVGL 的 fb、MP3 的 10-byte 指令、
   顯示的 5-byte 幀，全部直接 submit 自己手上的 buffer；除了雙緩衝的「第二塊」，
   不新增任何中間層。

---

## 4. 各 writer 的 C 做法（具體到函式層級）

### 4.1 LCD / APA102 / PCA —— 包現有 bus（1–2 天）
- `TxBus` 的 writer 直接呼叫現有 `write()`/`pending()`/`wait_all()`。
- APA102：`spi_buffer` 雙緩衝（2×~16KB，放 PSRAM），drain 送出一塊後才允許下一塊 ——
  順手修掉「沒人 wait_all」的現存 bug。
- LCD 大幀：C 內自動分 chunk（現有 32KB 邏輯搬進 writer），Python 不再碰
  `write_frame_dma` 的 per-chunk 循環。

### 4.2 MP3 / 馬達 / 顯示幀 —— UART glue（1–2 天）
- 第一版（零風險）：`TxBus` 存 `machine.UART`（或 `_Rs485Uart`）物件，drain 時從 C
  以 `mp_call_method` 呼叫其 `write` —— **從 RenderTask 同一 task 呼叫，安全**。
  - MP3：移除 `sleep_ms(15)`，改 C drain 的 `min_interval_ms=10`（DFPlayer 幀間隔需求，不可省）。
  - 馬達：`hash=True` → C 記上幀內容 hash，值沒變不送（doc 17 P4 節流）。
  - RS485：`_Rs485Uart.write()` 內含 DE 切換 + `txdone()` 等時序（doc 04），**原樣保留，
    由 C 呼叫**；不要拆散到 C 重寫。
- 第二版（選配）：`lcd_bus.UARTBus` 自管 IDF uart driver（TX ring/DMA），徹底非阻塞；
  注意與 `machine.UART` 的端口佔用衝突 —— 這些線必須全部改走 UARTBus。

### 4.3 WS2812 —— 新 C：RMT + DMA + 即時編碼（3–5 天，最大收益）
- 現況 85ms 阻塞（doc 23 實測）→ 目標 **0 阻塞**。
- 做法（`led_strip` 風格，成熟路徑）：
  1. C 內 `byte→symbol` 即時編碼器（RGB bytearray → WS2812 RMT symbols）；
  2. 分 chunk：編 8KB symbols（≈340px）→ `rmt_transmit`（DMA）→ 邊發邊編下一塊；
  3. 符號 staging 用內部 SRAM 2×8KB（RMT DMA 最穩）；PSRAM 直發留作選配
     （需 `esp_cache_msync` C2M + 對齊驗證）。
- 676px：符號總量 130KB → ~16 塊/幀，**全部 C 內完成，每幀只回一個 done**；
  Python 只看見「下一幀可交」。
- 這一步完成後，WS2812 不再阻塞 → **RenderTask 可以完全回到 core0 → 嚴格單一 task 成立**。

### 4.4 音訊 I2S（選配）
- `machine.I2S` block 寫是硬體節拍（46ms/8KB），本質上「自己就是鐘」。
- 建議保留 `audio_player_task` 的節拍，只把寫入口改為 `tx.submit("i2s", mv, kind="exclusive")`，
  由 TxBus 的 core0 drain 送出 —— 等價行為、單一寫入權。

---

## 5. 記憶體與中斷預期（676px + 240×320 TFT 實例）

### 5.1 內部 SRAM 預算（S3，DMA 可用 ≈161KB）

| 消費者 | 大小 | 說明 |
|---|---|---|
| TFT/LCD 幀 | 150KB | **PSRAM**（大 buffer 全放外） |
| APA102 雙緩衝 | 2×~16KB | PSRAM（走 4.1，DMA 前由 C staging 或直讀） |
| WS2812 符號 staging | 2×8KB 內部 | RMT DMA 最穩 |
| 通用 DMA staging | 2×32KB 內部 | 消 ISR memcpy 用（doc 05 方案 B） |
| LVGL draw buffer | ~40–60KB 內部 | 視 config |
| pixel hub | 3 slot × 2.7KB | PSRAM |

### 5.2 中斷數（統一後）

| 來源 | 統一前 | 統一後 |
|---|---|---|
| LCD 150KB | 5 tx/幀 + **每 tx 一次 ISR 2.3ms memcpy** | 5 tx/幀、ISR µs 級（staging 後）→ 250/s 可忽略 |
| WS2812 | **85ms CPU 阻塞** | ~16 DMA 中斷/幀，C 內消化，每幀 1 次 done |
| RGB | 常駐掃描持續 refill | `refresh_on_demand` → 空閒零中斷 |
| MP3/馬達/顯示 | 各自 sleep/txdone 阻塞 | UART ring，drain 管節拍，無阻塞 |

→ 「分層導致非常多中斷」的兩個元兇（ISR memcpy + Python per-chunk 參與）都被移除。

---

## 6. 分期與工作量

| Phase | 內容 | 工作量 | 驗收 |
|---|---|---|---|
| **1** | `lcd_bus.TxBus` C 模組（dispatch + pin/drop 規則 + drain + stats）；RenderTask 遷移為唯一 drain 者；LCD/APA102/PCA 併入 | 3–5 天 | APA102 跑 10 分鐘無 `queue full`；LVGL 幀率不降 |
| **2** | MP3/馬達/顯示幀 UART 併入（去 sleep、hash 節流、DE 時序保留） | 1–2 天（UARTBus 選配 +2 天） | MP3 指令間隔 ≥10ms 不變；馬達幀數下降 |
| **3** | WS2812 RMT+DMA（4.3）→ 嚴格單一 task；SPI staging 雙緩衝 + per-frame done | 5–7 天 | WS2812 2000px 不再阻塞任何核心；50fps 達成 |
| **4** | 觀測（per-iface queued/dropped/latency）+ 對照 doc 08 基準夜測 | 1–2 天 | G5 全綠、不掉幀 |

**總計約 10–16 天**；Phase 1 完成即獲得「統一入口 + 消滅 APA102 雷」的核心價值，
Phase 3 完成達成「嚴格單一渲染任務完成所有總線寫入」。

---

## 7. 風險與對策

| 風險 | 對策 |
|---|---|
| GC pin 生命週期（DMA 在飛時 buffer 被 GC） | 沿用 `ref_bufs`：ref 只在本交易 `get_trans_result` 成功後釋放；覆蓋不提前放 |
| PSRAM cache 一致性（直讀路徑） | 直讀一律 `esp_cache_msync(C2M)`；預設走 staging 保守路徑 |
| DFPlayer 幀間隔不能省 | `min_interval_ms=10` 硬節拍，測試確認 |
| RS485 DE 時序（doc 04） | `_Rs485Uart.write()` 原樣保留由 C 呼叫，不重寫 |
| 單一寫入者紀律 | `drain()` 只有 RenderTask 呼叫；code review 擋掉其他直接寫 |
| UART 端口佔用（UARTBus 選配） | 遷移期兩者共存時，同一端口只能有一個主人，config 擋 |

---

## 8. 落地第一步建議

1. `mp_lcd_bus` 加 `TxBus`（先只做 `frame` 類 + SPI/I2C writer，見 §3 API）；
2. `mp_Net-Core`：`slave/tasks/render.py` 加 drain 呼叫，APA102/LCD 先併入；
3. 用 P4-ETH_mp3 實例（螢幕+MP3+LED 齊全）做整機驗證。
