# tx_arena v2 —— 整倉雙緩衝「倉庫」+ C 層統一 TX 引擎

> **模型（用戶定義，v2 定稿）**：
> 建立一個**巨大緩衝區（倉庫）**，交給 lib 使用；用戶的**主要控制只有兩個動作**：
> 1. **寫倉庫**（把 LED / LCD / MP3 / 馬達…所有設備的數據，按約定位置寫進倉庫）
> 2. **叫 lib 更新渲染**（swap）
>
> 倉庫**整塊雙緩衝**：lib 渲染 A 時用戶寫 B，交換後寫 A，不斷輪替。
> 真正執行時，lib（C）內部自行決定每段數據「複製 / 讀取 / DMA / 各種 TX」。
> **日期**：2026-10-25

---

## 1. 模型正確性：成立，且只有一個真正的坑

| 環節 | 判定 |
|---|---|
| 整倉雙緩衝 + swap 換手 | ✅ 經典 frame-swap 模式（前後台緩衝），完全成立 |
| 用戶只管寫 + 叫渲染 | ✅ 這是模型最大的價值：用戶側零時序知識 |
| C 內按 bus 自行決定複製/DMA/TX | ✅ 決策表沉進 C（見 §4），Python 永不參與 |
| **唯一的坑** | ⚠️ **慢設備的 drain 時間 > 換手週期**（WS2812 60ms vs 20ms beat）→ 換手後 DMA 還在讀「剛變成 back」的舊倉庫，用戶同時寫它 → **撕裂** |

坑的解法（§3）：**換手瞬間「快照」慢 section，快 section 直接 DMA 讀倉庫** ——
倉庫永遠在 swap 當下就完整歸還用戶，雙緩衝語意 100% 乾淨。

---

## 2. API（用戶視角，就這麼多）

```python
from tx_arena import Warehouse

# 1) 建倉庫：用戶指定位置與大小（SRAM 或 PSRAM）；lib 內部切 A/B 兩半
wh = Warehouse(size=262144, psram=True)

# 2) 約定倉庫內的佈局（一次，init 時）：段 = 設備數據的固定位置
wh.add_section("lcd_fb",  offset=0,       size=153600, bus=lcd_spi)   # 快段:DMA 直讀
wh.add_section("apa102",  offset=153600,  size=16384,  bus=apa_spi)   # 快段
wh.add_section("ws2812",  offset=169984,  size=2704,   bus=ws_rmt)    # 慢段:swap 時快照
wh.add_section("mp3",     offset=172688,  size=64,     bus=uart2, min_interval_ms=10)
wh.add_section("motor",   offset=172752,  size=320,    bus=uart1, hash=True)
wh.add_section("display", offset=173072,  size=8,      bus=uart1)

# 3) 用戶主循環 —— 只有兩件事:
v = wh.back_view()          # 拿當前「可寫」那一半（memoryview，整倉直接寫）
# ...用戶把整倉所有 section 填好（不改動的 section 就留原值/上一幀值）...
ok = wh.swap()              # 叫 lib 渲染:back→front；False = 快段還沒排空（下個 beat 重試即可）
                            # swap 返回後,用戶可立刻寫新的 back
```

- `back_view()` 永遠回**當前可寫半倉**；`swap()` 是唯一換手點。
- `swap()` 由任何核心呼叫都安全（內部只做快照 + 置代號，見 §3、§5）。
- 快段若上一筆 DMA 還沒讀完 → `swap()` 回 False：**幀可丟**，用戶照常繼續寫 back，
  下一個 beat 再 swap（自然背壓，不阻塞）。

---

## 3. 撕裂問題的解法：快/慢段分類（swap 契約的核心）

swap 時 lib 對每個 section 做分類處理，**保證 swap 返回後整個舊 front 立即可寫**：

| 分類 | 判定 | swap 時的動作 | 之後的 TX |
|---|---|---|---|
| **快段** | drain ≤ beat（LCD 15–19ms、APA102 6.4ms、PCA 2ms） | 檢查上一筆已 done（沒 done → swap 回 False） | **DMA 直接讀倉庫**，零拷貝 |
| **慢段** | drain > beat（WS2812 60ms） | **立刻快照**（小源區拷進 lib 內部雙緩衝，如 676px RGB = 2.7KB） | 從快照編碼/發送，與倉庫無關 |
| **微段** | UART/I2C 小幀（MP3 10B、馬達 160B、顯示 5B） | 立刻拷進 lib 內部 ring（驅動 TX ring） | 驅動自行完成 |

效果：

- **倉庫雙緩衝語意完美**：swap 一返回，舊 front 已無任何 DMA 在讀，用戶閉眼寫。
- **慢段不拖快段**：WS2812 走快照自行 60ms 節拍；LCD 照樣每個 beat 換手。
- **快照成本可忽略**：慢/微段源區都是 KB 級；大段（LCD）反而零拷貝。
- 唯一硬約束寫成白紙黑字：**快段的 drain 必須 ≤ beat**。若將來出現「大且慢」的段
  （如 800KB 幀 + 低頻率），該段需要三緩衝或阻塞式 swap —— 目前實例板沒有這種段，
  列為已知限制即可。

---

## 4. C 內部決策表（倉庫的執行核心，用戶永遠看不見）

drain（渲染任務，core0，每 tick）對 front 半倉的每個 section 分發：

| src 記憶體 | bus | C 內動作 |
|---|---|---|
| PSRAM | DSI / RGB | PPA / LCD_CAM 硬體直讀，**零拷貝**（現有示範） |
| PSRAM | SPI / I80 | engine staging 池（內部 SRAM 32KB×2）過橋 → DMA（CPU 在渲染任務上下文 memcpy，**不進 ISR**） |
| 內部 SRAM | SPI / I80 | 直接 DMA 入隊，零拷貝 |
| 任意 | UART（MP3/馬達/顯示） | 驅動 TX ring 複製（swap 時已完成快照） |
| 任意 | I2C（PCA） | 驅動同步寫 ~2ms |
| 任意 | RMT（WS2812） | engine 內 byte→symbol 編碼（2×8KB 符號池）→ RMT DMA，邊編邊發 |

- 節拍全在 C：`min_interval_ms`（MP3 的 `sleep_ms(15)` 消失）、`hash`（馬達沒變不送）、
  快段 `pending` 檢查（背壓）。
- 同一實體 bus 的多 section（如 LCD+APA102 共 SPI host）按倉庫佈局順序排隊 ——
  不再有「兩邊各自入隊、無人排空」的現狀。

---

## 5. 關鍵正確性細節

1. **swap 的跨核安全**：swap 只做「快照慢/微段（拷進 lib 內部雙緩衝，與前次在飛的
   快照錯開）+ 置一個對齊 uint32 代號（generation）+ PSRAM 段 `esp_cache_msync(C2M)`」。
   真正 DMA 排程仍在渲染任務的 drain 執行。→ 零鎖，與現有 C queue 無鎖契約一致。
2. **快段 in-flight 保護**：swap 前檢查快段 done；未 done → False（用戶重試），
   **絕不讓 DMA 在飛的倉庫半塊變成 back**。
3. **慢段快照雙緩衝**：WS2812 快照目標 A/B 交替，上一幀還在發就寫另一塊快照，
   兩幀都在飛時 swap 回 False。
4. **對齊**：section offset/size 由 lib 強制 64B 對齊。
5. **整倉尺寸**：2 × 倉庫大小。實例（TFT 150KB + LED ~19KB + 控制 ~0.4KB ≈ 170KB）
   → 雙緩衝 340KB PSRAM，無壓力。SRAM 倉庫只適用小佈局（S3 DMA 可用 ~161KB → 半倉 ≤80KB）。

---

## 6. 與現有 Python 層的關係（遷移後消失/保留）

| 現有 | 遷移後 |
|---|---|
| `pixel_stream` hub（10 slots + 一次複製） | **消失**：producer 直寫 `back_view()` 對應 section |
| `PixelStreamer.show_all()` per-controller 寫入 | viper 轉換保留（結果寫進倉庫）；硬體寫入全在 engine |
| `SpiBusAdapter.write_frame_dma` per-chunk 循環 | engine 內 chunk 循環（§4 staging） |
| `mp3_tf_16p.sleep_ms(15)` | `min_interval_ms=10` |
| `_Rs485Uart.write`（DE 時序，doc 04） | 原樣保留，由 engine 呼叫 |
| `RenderTask`（core1） | 遷回 core0 單一 drain；WS2812 走快照後不再阻塞任何核心 |

---

## 7. 分期

| Phase | 內容 | 工作量 |
|---|---|---|
| 1 | `tx_arena` C 模組：Warehouse（A/B 切半）/ add_section / back_view / swap / drain / done / stats + 快段 SPI/I2C + 微段 UART + staging 池 | 3–5 天 |
| 2 | Python 遷移：LCD / APA102 / PCA（LVGL 直寫 `lcd_fb` section） | 1–2 天 |
| 3 | MP3 / 馬達 / 顯示微段 + 節拍 / hash | 1–2 天 |
| 4 | WS2812 慢段快照 + RMT+DMA 編碼 → 全設備歸一 | 3–5 天 |
| 5 | 統計 + 對照 doc 08 夜測 | 1–2 天 |

---

## 8. 風險

- PSRAM cache 一致性：swap 時 C2M msync、drain 後按需 M2C —— 寫進決策表；
- S3 SPI 讀 PSRAM 的 ISR 拷貝：staging 過橋把 memcpy 從 ISR 移回渲染任務上下文；
- 快段 drain > beat 的場景：swap 回 False（丟幀）是設計行為，不是 bug；
- DFPlayer 幀間隔、RS485 DE 時序：原語意保留；
- firmware build：與 lcd_bus 同倉庫共用 headers 編進同一個固件。
