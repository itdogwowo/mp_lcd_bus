"""
test_dsi.py — ESP32-P4 (JC1060P470) MIPI DSI 點屏測試

面板: JD9165, 1024x600, 2-lane DSI, lane bit rate 550 Mbps, DPI clk 58 MHz
用法:
    import test_dsi
    test_dsi.run_all()        # 跑全部
    test_dsi.run_pattern()    # 只跑 DSI controller 內建 pattern
    test_dsi.run_colors()     # 只跑純色/漸層

⚠️ 此腳本針對 JC1060P470 板, 參數在下方常數區可調。
⚠️ DSI PHY 電源 (LDO3 2.5V) 由 esp32.LDO 在 Python 層開啟,
   必須在建 DSIBus 之前開, 並全程保留引用 (GC 掉會斷電)。

⚠️ 已知畫面問題的根因 (2026-08-03 排查):
   屏幕閃 / 上下黑帶 / 撕裂 / 殘留 全部發生在「frame buffer 階段」,
   set_pattern 階段完全正常 → DSI timing/PHY/面板沒問題, 問題在 fb 資料路徑:

   1. ESP32-P4 的 frame buffer 在 PSRAM, CPU 直寫走 write-back L2 cache,
      DPI DMA 卻直接讀 PSRAM (不經 cache)。frame_buffer() 直寫後若沒
      flush() (esp_cache_msync 寫回), DMA 會一直讀到舊資料 →
      殘留/黑帶/閃爍。→ 本版已加 bus.flush(), 每個直寫階段後呼叫。
   2. 直接寫「正在被掃描」的那塊 fb 必然撕裂 (單緩衝的物理限制)。
      → 要完全不撕裂請用 fb_count=2 雙緩衝 + 整幀更新 (test_double_buffer)。
   3. 面板規格響應時間 25~40ms (JC1060M070N 承認書), 快速切色短暫殘影
      屬面板本身特性, 不是驅動 bug。
"""

import time
import lcd_bus
from machine import Pin

# ==== 板子硬體參數 (JC1060P470 + JD9165 1024x600) ====
WIDTH = 1024
HEIGHT = 600
LANES = 2
LANE_BIT_RATE_MBPS = 550.0       # 與廠商 Demo (esp32_p4_function_ev_board) 一致
DPI_CLK_MHZ = 58.0               # 與廠商 Demo (esp_lcd_jd9165 官方宏) 一致
RESET_PIN = 27                   # BSP_LCD_RST (GPIO27)
BACKLIGHT_PIN = 23               # BSP_LCD_BACKLIGHT (GPIO23)

# DSI PHY 電源 (對應 BSP display.h: BSP_MIPI_DSI_PHY_PWR_LDO_CHAN/VOLTAGE)
# ESP32-P4 的 DSI DPHY 必須靠外部 LDO 供電才能從 No-Power → Shutdown 狀態。
DSI_PHY_LDO_CHAN = 3             # LDO_VO3 接 VDD_MIPI_DPHY
DSI_PHY_LDO_VOLTAGE_MV = 2500

# Video timing — 與 Espressif esp_lcd_jd9165 官方宏
# JD9165_1024_600_PANEL_60HZ_DPI_CONFIG 完全一致 (廠商 Demo 也是用這組)。
# 實際刷新率 = 58e6 / (1024+160+40+160) / (600+23+10+12) ≈ 64.9 Hz。
# 若面板廠的 MTK dtsi 有精確 60Hz 參數 (HS=24 HBP=136 HFP=160 / VS=2 VBP=21
# VFP=12 / DOTCLK=51.2MHz), 可換那組試, 但這組是出貨 Demo 的參考值。
HSYNC_PULSE_WIDTH = 40
HSYNC_BACK_PORCH = 160
HSYNC_FRONT_PORCH = 160
VSYNC_PULSE_WIDTH = 10
VSYNC_BACK_PORCH = 23
VSYNC_FRONT_PORCH = 12

# ==== 實驗: 面板廠 dtsi 的精確 60Hz timing ====
# 若「屏閃/畫面延遲/顏色不齊」持續出現, 試試這組 (dtsi 的 QD070AS01-1 原生值):
#   HSYNC_PULSE_WIDTH = 24
#   HSYNC_BACK_PORCH = 136
#   HSYNC_FRONT_PORCH = 160
#   VSYNC_PULSE_WIDTH = 2
#   VSYNC_BACK_PORCH = 21
#   VSYNC_FRONT_PORCH = 12
#   DPI_CLK_MHZ = 51.2   → 剛好 60.0 Hz (58e6/... 是 64.9Hz, 與 IC 內部
#   60Hz 時基有 ~5Hz 拍頻, 可能造成週期性跳幀/閃爍)

IN_COLOR_FORMAT = 16             # 16=RGB565, 24=RGB888
FB_COUNT = 2

# ==== JD9165 init sequence (from JC1060P470 BSP readme.txt) ====
# 格式: (cmd, data_bytes, delay_ms)
# 0x30 = page (bank) 切換命令, 同一 cmd 在不同 page 指向不同暫存器,
#        順序不可調換。
# 已逐項比對 Espressif esp_lcd_jd9165.c 官方 vendor_specific_init_default,
# 與面板廠 MTK dtsi (JD9165BA_HKC7.0 QD070AS01-1) 完全一致。
JD9165_INIT = [
    (0x30, b'\x00', 0),
    (0xF7, b'\x49\x61\x02\x00', 0),
    (0x30, b'\x01', 0),
    (0x04, b'\x0C', 0),
    (0x05, b'\x00', 0),
    (0x06, b'\x00', 0),
    (0x0B, b'\x11', 0),
    (0x17, b'\x00', 0),
    (0x20, b'\x04', 0),
    (0x1F, b'\x05', 0),
    (0x23, b'\x00', 0),
    (0x25, b'\x19', 0),
    (0x28, b'\x18', 0),
    (0x29, b'\x04', 0),
    (0x2A, b'\x01', 0),
    (0x2B, b'\x04', 0),
    (0x2C, b'\x01', 0),
    (0x30, b'\x02', 0),
    (0x01, b'\x22', 0),
    (0x03, b'\x12', 0),
    (0x04, b'\x00', 0),
    (0x05, b'\x64', 0),
    (0x0A, b'\x08', 0),
    (0x0B, b'\x0A\x1A\x0B\x0D\x0D\x11\x10\x06\x08\x1F\x1D', 0),
    (0x0C, b'\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D', 0),
    (0x0D, b'\x16\x1B\x0B\x0D\x0D\x11\x10\x07\x09\x1E\x1C', 0),
    (0x0E, b'\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D', 0),
    (0x0F, b'\x16\x1B\x0D\x0B\x0D\x11\x10\x1C\x1E\x09\x07', 0),
    (0x10, b'\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D', 0),
    (0x11, b'\x0A\x1A\x0D\x0B\x0D\x11\x10\x1D\x1F\x08\x06', 0),
    (0x12, b'\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D\x0D', 0),
    (0x14, b'\x00\x00\x11\x11', 0),
    (0x18, b'\x99', 0),
    (0x30, b'\x06', 0),
    (0x12, b'\x36\x2C\x2E\x3C\x38\x35\x35\x32\x2E\x1D\x2B\x21\x16\x29', 0),
    (0x13, b'\x36\x2C\x2E\x3C\x38\x35\x35\x32\x2E\x1D\x2B\x21\x16\x29', 0),
    (0x30, b'\x0A', 0),
    (0x02, b'\x4F', 0),
    (0x0B, b'\x40', 0),
    (0x12, b'\x3E', 0),
    (0x13, b'\x78', 0),
    (0x30, b'\x0D', 0),
    (0x0D, b'\x04', 0),
    (0x10, b'\x0C', 0),
    (0x11, b'\x0C', 0),
    (0x12, b'\x0C', 0),
    (0x13, b'\x0C', 0),
    (0x30, b'\x00', 0),
    (0x11, b'\x00', 120),           # SLPOUT
    (0x29, b'\x00', 50),            # DISPON
]

# ==== RGB565 顏色 ====
def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

RED   = rgb565(255, 0, 0)
GREEN = rgb565(0, 255, 0)
BLUE  = rgb565(0, 0, 255)
WHITE = rgb565(255, 255, 255)
BLACK = rgb565(0, 0, 0)

# ==== 輔助 ====
W = 62

def hdr(m):
    print("\n" + "=" * W + "\n  " + m + "\n" + "=" * W)

def sec(m):
    print(f"\n  --- {m} ---")

def fill_color(fb, color):
    """用指定 RGB565 顏色填滿整個 frame buffer (只寫 PSRAM, 尚未寫回 cache)。"""
    lo = color & 0xFF
    hi = (color >> 8) & 0xFF
    row = bytes([lo, hi]) * WIDTH
    fb[:] = row * HEIGHT


# ==== 核心初始化 ====
_bus = None
_backlight = None
_phy_ldo = None    # DSI PHY LDO, 必須全程保留引用, GC 掉會斷電


def init_panel():
    """開 DSI PHY 電源、背光、建立 DSIBus、送 JD9165 init sequence。回傳 bus。"""
    global _bus, _backlight, _phy_ldo

    hdr("DSI 初始化")

    sec("DSI PHY 電源 (LDO{})".format(DSI_PHY_LDO_CHAN))
    # 必須在建 DSI bus 之前開 — PHY 無電則 esp_lcd_new_dsi_bus 直接失敗。
    # 引用保留在 _phy_ldo, deinit 時才釋放。
    from esp32 import LDO
    # 注意: 此版韌體參數名為 channel_id (非文件寫的 channel)
    _phy_ldo = LDO(channel_id=DSI_PHY_LDO_CHAN, voltage_mv=DSI_PHY_LDO_VOLTAGE_MV)
    print(f"  LDO{DSI_PHY_LDO_CHAN} = {DSI_PHY_LDO_VOLTAGE_MV}mV (VDD_MIPI_DPHY)")

    sec("背光")
    _backlight = Pin(BACKLIGHT_PIN, Pin.OUT, value=1)
    print(f"  backlight GPIO{BACKLIGHT_PIN} = HIGH")

    sec("建立 DSIBus")
    # 所有面板相關參數都從 Python 層輸入, 不需要改 C module —
    # 換面板時: 改 timing / dpi_clk / lane rate / init sequence 即可。
    _bus = lcd_bus.DSIBus(
        lanes=LANES, width=WIDTH, height=HEIGHT,
        lane_bit_rate_mbps=LANE_BIT_RATE_MBPS,
        dpi_clk_mhz=DPI_CLK_MHZ,
        hsync_pulse_width=HSYNC_PULSE_WIDTH,
        hsync_back_porch=HSYNC_BACK_PORCH,
        hsync_front_porch=HSYNC_FRONT_PORCH,
        vsync_pulse_width=VSYNC_PULSE_WIDTH,
        vsync_back_porch=VSYNC_BACK_PORCH,
        vsync_front_porch=VSYNC_FRONT_PORCH,
        in_color_format=IN_COLOR_FORMAT,
        fb_count=FB_COUNT,
        rst=RESET_PIN,
        cmd_bits=8, param_bits=8,        # DBI 命令/參數位寬 (少數面板不同)
        use_dma2d=True,                  # False 改 CPU 拷貝 (不用 DMA2D)
        queue_depth=4,                   # 非同步寫入管線深度 1..8
    )
    print(f"  DSIBus OK, lanes={_bus.lane_count()}, "
          f"{WIDTH}x{HEIGHT} RGB{IN_COLOR_FORMAT}")

    # 診斷: 實際刷新率與 PSRAM 帶寬需求
    h_total = WIDTH + HSYNC_PULSE_WIDTH + HSYNC_BACK_PORCH + HSYNC_FRONT_PORCH
    v_total = HEIGHT + VSYNC_PULSE_WIDTH + VSYNC_BACK_PORCH + VSYNC_FRONT_PORCH
    fps = DPI_CLK_MHZ * 1e6 / (h_total * v_total)
    bw = DPI_CLK_MHZ * 1e6 * IN_COLOR_FORMAT / 8
    print(f"  refresh ≈ {fps:.1f} Hz, PSRAM 讀取帶寬需求 ≈ {bw / 1e6:.0f} MB/s")
    print("  ⚠ 若序列埠出現 \"can't fetch data from external memory fast "
          "enough\" (underrun) →")
    print("    韌體需 PSRAM 200MHz + L2 cache 設定 (sdkconfig.require.json)")

    sec("JD9165 init sequence ({})".format(len(JD9165_INIT)))
    for cmd, data, delay in JD9165_INIT:
        _bus.cmd(cmd, param=data)
        if delay:
            time.sleep_ms(delay)
    print("  init code 送完")
    return _bus


def deinit_panel():
    global _bus, _backlight, _phy_ldo
    if _bus:
        _bus.deinit()
        _bus = None
        print("  DSIBus deinit 完成")
    if _backlight:
        _backlight(0)
        _backlight = None
    if _phy_ldo:
        _phy_ldo.release()
        _phy_ldo = None
        print("  DSI PHY LDO released")


# ==== 測試項 ====

def test_pattern():
    """DSI controller 內建測試 pattern — 不經過 frame buffer / PSRAM,
    由 DPI controller 內部直接產生畫面, 最直接驗證 DSI PHY / lane /
    DPI controller 本身。此階段若乾淨, 代表 timing/PHY/面板都沒問題。

    ⚠ pattern 3 (BER_VERTICAL) 只顯示一小部分是 ESP32-P4 的正常行為:
    BER 圖案由 DSI *host controller* 的 pattern generator 產生
    (set_pattern(非0) 時 bridge 的 DPI 輸出會被停掉), 它不是完整的
    1024x600 影片流, 而是給 PHY 位元錯誤率測試用的工具圖案 —
    畫面顯示不完整不代表硬體有問題。bar (1/2) 才是滿屏圖案。"""
    bus = _bus
    hdr("DSI 內建 Pattern 測試")
    for pat, name in [(1, "垂直條紋"), (2, "水平條紋"), (3, "BER 測試")]:
        sec(name)
        bus.set_pattern(pat)
        print(f"  set_pattern({pat}) — 請目視確認螢幕顯示 {name}")
        time.sleep(2)
    sec("關閉 pattern")
    bus.set_pattern(0)
    print("  set_pattern(0) — 回到正常 frame buffer 輸出")
    time.sleep(1)


def test_solid_colors():
    """純色填充 — 透過 frame_buffer() 零 copy 直寫 PSRAM,
    驗證完整資料路徑 (frame buffer → DPI → DSI → 面板)。

    ⚠ P4 的 L2 cache 是 write-back: 直寫 fb 後必須 bus.flush()
    把髒 cache line 寫回 PSRAM, 否則 DMA 讀到舊資料 (殘留/黑帶)。
    撕裂在此階段仍可能出現 — 因為是直接寫「正在掃描」的 fb,
    這是單緩衝直寫的物理限制; 要不撕裂請看 test_double_buffer()。

    計時診斷: 印出 fill / flush 各花多少 ms —
      * fill 異常慢 (數百 ms) → MicroPython GC 或 heap 問題
      * flush 異常慢 (>100ms) → cache 操作問題
      * 都很快但畫面仍延遲 → 問題在 DMA/TCON 端, 試 60Hz timing
    """
    bus = _bus
    hdr("純色填充測試")
    fb = bus.frame_buffer(0)
    print(f"  frame_buffer(0) 大小 = {len(fb)} bytes")

    for color, name in [(RED, "紅"), (GREEN, "綠"), (BLUE, "藍"),
                        (WHITE, "白"), (BLACK, "黑")]:
        sec(f"{name}色 (0x{color:04X})")
        t0 = time.ticks_ms()
        fill_color(fb, color)
        t1 = time.ticks_ms()
        bus.flush()                  # ⚠ 髒 cache line 寫回 PSRAM
        t2 = time.ticks_ms()
        print(f"  fill={time.ticks_diff(t1, t0)}ms, flush={time.ticks_diff(t2, t1)}ms")
        # DPI panel 持續從 fb 讀取, 填入即顯示, 不需要 write()
        time.sleep(1)
        print(f"  滿屏 {name} — 請目視確認")


def test_gradient():
    """水平/垂直漸層 — 驗證每行不同顏色、無大面積同色遮蔽問題。
    先全部算好再一次 slice 寫入 + flush, 減少逐行寫入的撕裂時間。"""
    bus = _bus
    hdr("漸層測試")
    fb = bus.frame_buffer(0)
    buf = bytearray(WIDTH * HEIGHT * IN_COLOR_FORMAT // 8)

    sec("水平漸層 (左黑→右白)")
    row = bytearray(WIDTH * 2)
    for x in range(WIDTH):
        v = (x * 255) // (WIDTH - 1)   # 0..255
        c = rgb565(v, v, v)
        row[x * 2]     = c & 0xFF
        row[x * 2 + 1] = (c >> 8) & 0xFF
    row_bytes = bytes(row)
    for y in range(HEIGHT):
        off = y * WIDTH * 2
        buf[off:off + WIDTH * 2] = row_bytes
    fb[:] = buf
    bus.flush()
    time.sleep(1)
    print("  水平灰階漸層 — 請目視確認漸層平滑、無斷層")

    sec("垂直漸層 (上藍→下紅)")
    band = HEIGHT // 8
    for i in range(8):
        c_r = (i * 255) // 7
        c_b = 255 - c_r
        c = rgb565(c_r, 0, c_b)
        lo, hi = c & 0xFF, (c >> 8) & 0xFF
        band_row = bytes([lo, hi]) * WIDTH
        for y in range(i * band, min((i + 1) * band, HEIGHT)):
            off = y * WIDTH * 2
            buf[off:off + WIDTH * 2] = band_row
    fb[:] = buf
    bus.flush()
    time.sleep(1)
    print("  紅→藍垂直漸層 — 請目視確認")


def test_async_write():
    """驗證 write() 異步路徑 — 統一 API:
    set_window() 設視窗 → write(buf) 流式寫入 (面板 RAMWR 模型)。
    視窗沒設時預設全螢幕, write(buf) 即整幀。

    ⚠ write() (外部 buffer) 會用 DMA2D 拷進「目前正在顯示」的那塊 fb
    (esp_lcd P4 driver 的 dpi_panel_draw_bitmap 行為), 所以拷貝期間
    仍可能看到撕裂線; 內容是正確的。要完全不撕裂 → 內部 fb 雙緩衝。"""
    bus = _bus
    hdr("write() 異步寫入測試")

    sec("write 半屏 (set_window 上半)")
    half = WIDTH * (HEIGHT // 2) * (IN_COLOR_FORMAT // 8)
    top_buf = bytearray(half)
    for i in range(0, half, 2):
        top_buf[i] = 0x00       # 綠
        top_buf[i + 1] = 0xE0
    bus.set_window(0, 0, WIDTH - 1, HEIGHT // 2 - 1)   # 上半視窗
    tid = bus.write(top_buf)
    print(f"  write 上半屏綠色 -> trans_id={tid}")
    bus.wait(tid)
    print("  wait 完成")
    time.sleep(1)

    sec("write 另一半 (set_window 下半)")
    bot_buf = bytearray(half)
    for i in range(0, half, 2):
        bot_buf[i] = 0x1F       # 藍
        bot_buf[i + 1] = 0x00
    bus.set_window(0, HEIGHT // 2, WIDTH - 1, HEIGHT - 1)  # 下半視窗
    tid = bus.write(bot_buf)
    print(f"  write 下半屏藍色 -> trans_id={tid}")
    bus.wait(tid)
    print("  wait 完成 — 請目視確認上半綠、下半藍")
    time.sleep(2)


def test_double_buffer():
    """雙緩衝 (tear-free) 示範 — 唯一能完全避免撕裂的用法。

    原理 (esp_lcd P4 driver):
      * 每塊 fb 各自有一條 DMA link list, DMA 在「目前」那條上循環掃描。
      * draw_bitmap 的 draw buffer 若是內部 fb, 就只做 cache writeback,
        並把 cur_fb_index 指到該 fb; 目前這幀掃完 (幀邊界) 後,
        DMA 自動切到新 fb 的 link list → 切換發生在幀邊界 → 不撕裂。

    規則:
      * 每次都要「整幀」更新 (或保持兩塊 fb 內容同步),
        因為切換過去後, 沒畫到的區域顯示的是那一塊的舊內容。
      * 畫的對象永遠是「目前沒在顯示」的那塊 (fb0/fb1 輪流)。
    """
    bus = _bus
    hdr("雙緩衝 (tear-free) 測試")
    bus.set_window(0, 0, WIDTH - 1, HEIGHT - 1)   # 視窗重置全螢幕 (流式位置歸零)
    fb0 = bus.frame_buffer(0)
    fb1 = bus.frame_buffer(1)
    colors = [RED, GREEN, BLUE, WHITE, BLACK, RED]
    for i, color in enumerate(colors):
        target = fb1 if i % 2 == 0 else fb0
        fill_color(target, color)
        # write(內部 fb view) → 只 msync + 幀邊界切換, 不做 DMA2D 拷貝
        tid = bus.write(target)
        bus.wait(tid)                    # 同步完成 (無拷貝, 立即)
        print(f"  第 {i + 1} 幀: fb{'1' if i % 2 == 0 else '0'} 滿屏 "
              f"0x{color:04X} (已切換) — 請目視確認無撕裂")
        time.sleep(1)
    print("  若全程無撕裂線 → 雙緩衝路徑 OK")


def test_double_buffer_fps(duration_ms=10000):
    """雙緩衝整幀切換吞吐測試 — 固定時間內能跑幾幀。
    反向計量: 不量「N 幀花幾 ms」, 而是「固定 10s 內跑完幾幀」,
    長時間平均較穩, 順便統計單幀最長 (GC/卡頓偵測) 與最慢窗口。
    1 秒快速版: test_double_buffer_fps(1000)

    ⚠ 每幀 = 整幀 memcpy + 白帶一行 + write() 切換 — 這是生產路徑的
    真實成本。之前的版本用 Python 逐行畫 600 行 (每幀 ~30-50ms 純繪圖),
    那是測試腳本的瓶頸, 不是資料路徑的。"""
    bus = _bus
    hdr(f"雙緩衝吞吐測試 ({duration_ms // 1000}s)")
    bus.set_window(0, 0, WIDTH - 1, HEIGHT - 1)   # 視窗重置全螢幕
    fb0 = bus.frame_buffer(0)
    fb1 = bus.frame_buffer(1)
    c0 = bytes([0xF8, 0x00]) * (WIDTH * HEIGHT)   # 紅整幀 (預先做好, 零分配)
    c1 = bytes([0x1F, 0x00]) * (WIDTH * HEIGHT)   # 藍整幀
    white_row = bytes([0xFF, 0xFF]) * WIDTH

    t_start = time.ticks_ms()
    deadline = t_start + duration_ms
    n = 0
    max_copy_ms = 0
    max_swap_ms = 0
    stalls = 0
    win_frames = 0
    win_start = t_start
    min_win = 10 ** 9
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        target = fb1 if n & 1 else fb0
        t0 = time.ticks_ms()
        target[:] = c0 if n & 1 else c1           # 整幀 memcpy (唯一大成本)
        off = ((n * (HEIGHT // 12)) % HEIGHT) * WIDTH * 2
        target[off:off + WIDTH * 2] = white_row   # 白帶一行
        t_mid = time.ticks_ms()
        bus.write(target)                         # msync + 幀邊界切換
        bus.wait_all()                            # 內部 fb → 同步完成
        t1 = time.ticks_ms()
        copy_ms = time.ticks_diff(t_mid, t0)
        swap_ms = time.ticks_diff(t1, t_mid)
        if copy_ms > max_copy_ms:
            max_copy_ms = copy_ms
        if swap_ms > max_swap_ms:
            max_swap_ms = swap_ms
        if time.ticks_diff(t1, t0) > 50:
            stalls += 1
        n += 1
        win_frames += 1
        now = time.ticks_ms()
        if time.ticks_diff(now, win_start) >= 100:
            if win_frames < min_win:
                min_win = win_frames
            win_frames = 0
            win_start = now
    elapsed = time.ticks_diff(time.ticks_ms(), t_start)
    h_total = WIDTH + HSYNC_PULSE_WIDTH + HSYNC_BACK_PORCH + HSYNC_FRONT_PORCH
    v_total = HEIGHT + VSYNC_PULSE_WIDTH + VSYNC_BACK_PORCH + VSYNC_FRONT_PORCH
    disp_fps = DPI_CLK_MHZ * 1e6 / (h_total * v_total)
    print(f"  {duration_ms / 1000:.0f}s 內完成 {n} 幀 → 平均 {n * 1000 / elapsed:.1f} FPS")
    print(f"  顯示上限 ≈ {disp_fps:.1f} Hz (實際看到 = 兩者取小)")
    print(f"  memcpy 最長 {max_copy_ms}ms / write(msync+切換) 最長 {max_swap_ms}ms, "
          f">50ms 卡頓 {stalls} 次")
    print(f"  最慢 100ms 窗口 {min_win} 幀 (其餘窗口 ≈ 平均)")
    print("  請目視: 紅/藍每幀交替、白帶平滑下移、無撕裂線/無閃 → 生產路徑 OK")
    print("  若最慢窗口明顯偏低 (0-1) → 該時間點有停頓, 貼上來")


def test_write_dma2d_throughput(duration_ms=10000):
    """外部 buffer write() (DMA2D 硬體拷貝) 吞吐 — LVGL 用的路徑。
    每幀: 直接 write(整幀 bytes) → DMA2D 硬體把 1.2MB 拷進顯示中 fb,
    CPU 完全不碰 fb (只做 msync 來源, 來源乾淨 → ~0)。
    代價: DMA2D 拷貝 ~4ms 期間有一條撕裂線掃過 (全幀更新可接受)。
    若此路徑 ≈60 FPS → 全螢幕動畫用 write() 外部 buffer 就對了。"""
    bus = _bus
    hdr(f"write(DMA2D) 吞吐測試 ({duration_ms // 1000}s)")
    bus.set_window(0, 0, WIDTH - 1, HEIGHT - 1)   # 視窗重置全螢幕
    c0 = bytes([0xF8, 0x00]) * (WIDTH * HEIGHT)   # 紅整幀 (零分配)
    c1 = bytes([0x1F, 0x00]) * (WIDTH * HEIGHT)   # 藍整幀

    t_start = time.ticks_ms()
    deadline = t_start + duration_ms
    n = 0
    max_w = 0
    stalls = 0
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        t0 = time.ticks_ms()
        tid = bus.write(c0 if n & 1 else c1)       # DMA2D 硬體拷 1.2MB
        bus.wait(tid)
        t1 = time.ticks_ms()
        w = time.ticks_diff(t1, t0)
        if w > max_w:
            max_w = w
        if w > 50:
            stalls += 1
        n += 1
    elapsed = time.ticks_diff(time.ticks_ms(), t_start)
    print(f"  {duration_ms / 1000:.0f}s 內完成 {n} 幀 → 平均 {n * 1000 / elapsed:.1f} FPS")
    print(f"  單幀 write 最長 {max_w}ms, >50ms 卡頓 {stalls} 次")
    print("  請目視: 紅/藍每幀交替、無閃爍 (撕裂線若有應極短) → write 路徑 OK")


def bench_flush():
    """flush() 代價實測:
    1) fill+flush 一輪 — 真實使用成本
    2) 全屏空 flush — 純呼叫開銷
    3) 200x200 髒行 flush — 區域寫回成本
    flush = esp_cache_msync(DIR_C2M), 只寫回「髒」的 cache line,
    所以全屏 fill 後通常只剩最後 ~256KB 要寫回。"""
    bus = _bus
    hdr("flush() 代價測試")
    fb = bus.frame_buffer(0)

    # 1) 真實使用: fill 後立即 flush (10 輪)
    t0 = time.ticks_ms()
    for i in range(10):
        fill_color(fb, 0xF800 if i & 1 else 0x0000)
        bus.flush()
    t1 = time.ticks_ms()
    d = time.ticks_diff(t1, t0)
    print(f"  fill+flush 一輪 ×10 = {d}ms → {d / 10:.1f} ms/輪 "
          f"(含 fill 的 1.2MB 寫入 + temp 分配)")

    # 2) 空 flush (已乾淨, 50 次)
    t0 = time.ticks_ms()
    for _ in range(50):
        bus.flush()
    t1 = time.ticks_ms()
    d = time.ticks_diff(t1, t0)
    print(f"  全屏 flush(無髒行) ×50 = {d}ms → {d / 50:.3f} ms/次")

    # 3) 弄髒 200x200 再 flush (50 次)
    for y in range(200):
        off = y * WIDTH * 2
        fb[off:off + 400] = b'\xff\x00' * 200
    t0 = time.ticks_ms()
    for _ in range(50):
        bus.flush(x=0, y=0, w=200, h=200)
    t1 = time.ticks_ms()
    d = time.ticks_diff(t1, t0)
    print(f"  200x200 髒行 flush ×50 = {d}ms → {d / 50:.3f} ms/次")


# ==== 主入口 ====

def run_all():
    init_panel()
    try:
        test_pattern()            # 1. DSI controller 內建 pattern (不經 PSRAM)
        test_solid_colors()       # 2. 純色 (fb 直寫 + flush)
        test_gradient()           # 3. 漸層 (fb 直寫 + flush)
        test_async_write()        # 4. write() 異步路徑 (DMA2D 拷進顯示中 fb)
        test_double_buffer()      # 5. 雙緩衝 tear-free 示範
    finally:
        deinit_panel()
    print("\n  全部測試完成。")


def run_pattern():
    """只跑 DSI controller pattern — 最快確認硬體/通道。"""
    init_panel()
    try:
        test_pattern()
    finally:
        deinit_panel()


def run_colors():
    """只跑純色 + 漸層。"""
    init_panel()
    try:
        test_solid_colors()
        test_gradient()
    finally:
        deinit_panel()


def run_fps():
    """生產路徑驗證: 內部雙緩衝 + 外部 write(DMA2D) + flush 代價。"""
    init_panel()
    try:
        test_double_buffer_fps()
        test_write_dma2d_throughput()
        bench_flush()
    finally:
        deinit_panel()


if __name__ == "__main__":
    run_all()
