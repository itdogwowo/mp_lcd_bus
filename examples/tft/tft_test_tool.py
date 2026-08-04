# tft_test_tool.py — 全能 TFT 測試工具 (所有 bus + 所有面板)
#
# 架構: 統一 bus API (lib/bus_adapter.py) ← TFT 控制層 (lib/TFT.py) ← 本工具
#
# 用法:
#   1. 改頂部 ⚙ 設定區 (BUS / DRIVER / 解析度 / 各 bus 參數)
#   2. import tft_test_tool; tft_test_tool.run_all()
#
# 支援 bus : spi / i80 / rgb / dsi / i2c (adapter 全部在 lib/bus_adapter.py)
# 支援面板: ST7735 / ST7789 / ST7796 / GC9A01 / ILI9341 / GC9D01 /
#           NV3030B / RM67162 / SH8601 / JD9165 (lib/TFT.py)
#
# 測試項目 (全部走 TFT 統一介面, 任何 bus/面板通用):
#   fill_colors / color_bars / gradient / checkerboard / shapes / animate (目視)
#   fps_test            — adapter.write_frame_dma 純吞吐
#   fps_test_tft        — TFT.show_frame 層
#   fps_test_present    — begin_display + present 管線
#   flush_bench         — flush/wait_all 代價

import gc, time, math, random

# ══════════════════════════════════════════════════════════════
#  ⚙ 設定區 — 改這裡就能測不同屏幕
# ══════════════════════════════════════════════════════════════

BUS      = "dsi"        # "spi" | "i80" | "rgb" | "dsi" | "i2c"
DRIVER   = "JD9165"     # ST7789 | ST7735 | ST7796 | GC9A01 | ILI9341 | GC9D01
                        # NV3030B | RM67162 | SH8601 | JD9165
WIDTH    = 1024
HEIGHT   = 600
PIXEL_FORMAT = "RGB565_BE"   # SPI/I80 面板: RGB565_BE 或 RGB565_LE; DSI 固定 RGB565

# ── DSI (ESP32-P4, JC1060P470 + JD9165 1024x600) ──
# timing 與 esp_lcd_jd9165 官方宏一致 (~65Hz); 換面板改這裡
DSI = dict(
    lanes=2, lane_bit_rate_mbps=550.0, dpi_clk_mhz=58.0,
    hsync_pulse_width=40, hsync_back_porch=160, hsync_front_porch=160,
    vsync_pulse_width=10, vsync_back_porch=23, vsync_front_porch=12,
    in_color_format=16, fb_count=2,
    rst=27, backlight=23, ldo_chan=3, ldo_mv=2500,
)

# ── SPI (1/2/4/8 線) ──
SPI = dict(
    host=1, clk=36, data=(35,),          # data 長度 = 線數 (1/2/4/8)
    dc=37, cs=38, rst=39, backlight=40,
    freq=40_000_000, variant=0,          # variant: ST7789 初始化深度 0..3
)

# ── I80 (8/16 位元並行) ──
I80 = dict(
    data=(0, 1, 2, 3, 4, 5, 6, 7),       # 8 或 16 支腳
    wr=10, dc=9, cs=-1, rst=39, backlight=40,
    freq=10_000_000, variant=0,
)

# ── RGB (8/16 位元並行, 記憶體映射) ──
RGB = dict(
    data=(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
    hsync=39, vsync=40, de=41, pclk=42, disp=-1,
    freq=8_000_000,
)

# ── I2C (慢速, 僅功能性測試) ──
I2C = dict(
    data=(19,), clk=18, addr=0x3C, rst=-1, cmd_ctrl=0x00, data_ctrl=0x40,
)

# ══════════════════════════════════════════════════════════════

_lcd = None
_bus = None
_hold = None      # LDO / 背光引用 — 必須保留, GC 掉會斷電/關背光


def _pin(n):
    from machine import Pin
    return Pin(n, Pin.OUT, value=1) if n >= 0 else None


def init_tft():
    """依 ⚙ 設定區自動初始化: bus + adapter + 面板。回傳 lcd (TFT 實例)。"""
    global _lcd, _bus, _hold
    import lcd_bus
    from lib import bus_adapter as BA
    from lib import TFT as TFTM

    drv = getattr(TFTM, DRIVER)
    print("=" * 62)
    print("  TFT init: bus={} driver={} {}x{}".format(BUS, DRIVER, WIDTH, HEIGHT))
    print("=" * 62)

    if BUS == "dsi":
        from esp32 import LDO
        c = DSI
        _hold = LDO(channel_id=c["ldo_chan"], voltage_mv=c["ldo_mv"])
        bl = _pin(c["backlight"])
        if bl is not None:
            _hold = (_hold, bl)
        _bus = lcd_bus.DSIBus(
            lanes=c["lanes"], width=WIDTH, height=HEIGHT,
            lane_bit_rate_mbps=c["lane_bit_rate_mbps"],
            dpi_clk_mhz=c["dpi_clk_mhz"],
            hsync_pulse_width=c["hsync_pulse_width"],
            hsync_back_porch=c["hsync_back_porch"],
            hsync_front_porch=c["hsync_front_porch"],
            vsync_pulse_width=c["vsync_pulse_width"],
            vsync_back_porch=c["vsync_back_porch"],
            vsync_front_porch=c["vsync_front_porch"],
            in_color_format=c["in_color_format"],
            fb_count=c["fb_count"], rst=c["rst"],
        )
        _lcd = drv(BA.DsiBusAdapter(_bus, WIDTH, HEIGHT), WIDTH, HEIGHT)

    elif BUS == "spi":
        c = SPI
        _bus = lcd_bus.SPIBus(data=c["data"], clk=c["clk"],
                              freq=c["freq"], host=c["host"])
        _hold = _pin(c["backlight"])
        adapter = BA.SpiBusAdapter(_bus, dc=_pin(c["dc"]), cs=_pin(c["cs"]),
                                   rst=_pin(c["rst"]))
        _lcd = drv(adapter=adapter, width=WIDTH, height=HEIGHT,
                   pixel_format=PIXEL_FORMAT, variant=c["variant"])

    elif BUS == "i80":
        c = I80
        _bus = lcd_bus.I80Bus(data=c["data"], wr=c["wr"], dc=c["dc"],
                              cs=c["cs"], freq=c["freq"])
        _hold = _pin(c["backlight"])
        adapter = BA.I80BusAdapter(_bus, dcx=_pin(c["dc"]), rst=_pin(c["rst"]))
        _lcd = drv(adapter=adapter, width=WIDTH, height=HEIGHT,
                   pixel_format=PIXEL_FORMAT, variant=c["variant"])

    elif BUS == "rgb":
        c = RGB
        _bus = lcd_bus.RGBBus(data=c["data"], hsync=c["hsync"], vsync=c["vsync"],
                              de=c["de"], pclk=c["pclk"], width=WIDTH,
                              height=HEIGHT, freq=c["freq"], disp=c["disp"])
        adapter = BA.RgbBusAdapter(_bus, WIDTH, HEIGHT)
        _lcd = drv(adapter=adapter, width=WIDTH, height=HEIGHT,
                   pixel_format=PIXEL_FORMAT)

    elif BUS == "i2c":
        c = I2C
        _bus = lcd_bus.I2CBus(data=c["data"], clk=c["clk"], addr=c["addr"])
        adapter = BA.I2cBusAdapter(_bus, c["addr"], rst=_pin(c["rst"]),
                                   cmd_ctrl=c["cmd_ctrl"], data_ctrl=c["data_ctrl"])
        _lcd = drv(adapter=adapter, width=WIDTH, height=HEIGHT,
                   pixel_format=PIXEL_FORMAT)

    else:
        raise ValueError("BUS must be spi/i80/rgb/dsi/i2c")

    print("  {} OK ({}x{})".format(BUS.upper(), WIDTH, HEIGHT))
    return _lcd


def deinit_tft():
    global _lcd, _bus, _hold
    if _bus is not None:
        try:
            _bus.deinit()
        except Exception:
            pass
        _bus = None
        print("  bus deinit 完成")
    _lcd = None
    _hold = None


# ══════════════════════════════════════════════════════════════
#  色彩工具
# ══════════════════════════════════════════════════════════════

def _color(r, g, b):
    """RGB 888 → RGB565 大端"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _hsv(h, s=100, v=100):
    """HSV → RGB565"""
    h = float(h % 360) / 60.0
    s, v = s / 100.0, v / 100.0
    i = int(h); f = h - i
    p = v * (1 - s); q = v * (1 - s * f); t = v * (1 - s * (1 - f))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return ((int(r * 31) & 0x1F) << 11) | ((int(g * 63) & 0x3F) << 5) | (int(b * 31) & 0x1F)


def _write_solid(color565):
    """全螢幕填色 — 走統一介面 (set_window + 流式 write_data_async)"""
    chunk = bytearray(8192)
    for i in range(4096):
        chunk[i * 2] = color565 >> 8
        chunk[i * 2 + 1] = color565 & 0xFF
    total = WIDTH * HEIGHT
    mv = memoryview(chunk)
    _lcd.set_window(0, 0, WIDTH - 1, HEIGHT - 1)
    written = 0
    while written < total:
        n = min(total - written, 4096)
        hn = _lcd._bus.write_data_async(mv[:n * 2])
        if hn is not None:
            _lcd._bus.wait(hn)
        written += n
    _lcd._bus.flush()


def _clear():
    _lcd.set_window(0, 0, WIDTH - 1, HEIGHT - 1)
    _write_solid(0x0000)


# ══════════════════════════════════════════════════════════════
#  目視測試 (透過 TFT 統一介面 — 所有 bus/面板通用)
# ══════════════════════════════════════════════════════════════

def fill_colors():
    """九色全螢幕填滿"""
    gc.collect()
    colors = [
        ("RED", 0xF800), ("GREEN", 0x07E0), ("BLUE", 0x001F),
        ("YELLOW", 0xFFE0), ("CYAN", 0x07FF), ("MAGENTA", 0xF81F),
        ("WHITE", 0xFFFF), ("GRAY", 0x8410), ("BLACK", 0x0000),
    ]
    for name, c in colors:
        print("  %s (0x%04X) ..." % (name, c))
        _write_solid(c)
        time.sleep_ms(400)
    print("fill_colors done")


def color_bars():
    """八色垂直條"""
    bar_h = HEIGHT // 8
    for i, c in enumerate([0xF800, 0x07E0, 0x001F, 0xFFFF,
                           0xFFE0, 0x07FF, 0xF81F, 0x0000]):
        y0, y1 = i * bar_h, (i + 1) * bar_h - 1 if i < 7 else HEIGHT - 1
        pixels = WIDTH * (y1 - y0 + 1)
        chunk = bytearray(4096 * 2)
        for j in range(4096):
            chunk[j * 2] = c >> 8
            chunk[j * 2 + 1] = c & 0xFF
        mv = memoryview(chunk)
        _lcd.set_window(0, y0, WIDTH - 1, y1)
        remaining = pixels
        while remaining > 0:
            n = min(remaining, 4096)
            hn = _lcd._bus.write_data_async(mv[:n * 2])
            if hn is not None:
                _lcd._bus.wait(hn)
            remaining -= n
        _lcd._bus.flush()
    time.sleep_ms(1200)
    print("color_bars done")


def gradient():
    """RGB 水平漸變"""
    gc.collect()
    row = bytearray(WIDTH * 2)
    for x in range(WIDTH):
        r = int(x * 255 / WIDTH)
        g = int((1 - abs(x - WIDTH / 2) / (WIDTH / 2)) * 255)
        b = int((WIDTH - x) * 255 / WIDTH)
        c = _color(r, g, b)
        row[x * 2] = c >> 8
        row[x * 2 + 1] = c & 0xFF

    BATCH = 40
    for y in range(0, HEIGHT, BATCH):
        h = min(BATCH, HEIGHT - y)
        buf = bytearray(WIDTH * h * 2)
        for i in range(h):
            off = i * WIDTH * 2
            buf[off:off + len(row)] = row
        _lcd.set_window(0, y, WIDTH - 1, y + h - 1)
        hn = _lcd._bus.write_data_async(buf)
        if hn is not None:
            _lcd._bus.wait(hn)
        _lcd._bus.flush()
    time.sleep_ms(1500)
    print("gradient done")


def checkerboard():
    """棋盤格 (40x40)"""
    sq = 40
    row_buf = bytearray(WIDTH * sq * 2)
    for row_y in range(0, HEIGHT, sq):
        for py in range(sq):
            for bx in range(0, WIDTH, sq):
                is_w = ((bx // sq) + (row_y // sq)) % 2 == 0
                c = 0xFFFF if is_w else 0x0000
                for px in range(sq):
                    idx = ((py * WIDTH) + bx + px) * 2
                    row_buf[idx] = c >> 8
                    row_buf[idx + 1] = c & 0xFF
        _lcd.set_window(0, row_y, WIDTH - 1, row_y + sq - 1)
        hn = _lcd._bus.write_data_async(row_buf)
        if hn is not None:
            _lcd._bus.wait(hn)
        _lcd._bus.flush()
    time.sleep_ms(1500)
    print("checkerboard done")


def shapes():
    """同心圓 + 放射線 (framebuf)"""
    import framebuf
    h_buf = min(HEIGHT, 400)
    buf = bytearray(WIDTH * h_buf * 2)
    fbuf = framebuf.FrameBuffer(buf, WIDTH, h_buf, framebuf.RGB565)

    def _pixel(x, y, c):
        if 0 <= x < WIDTH and 0 <= y < h_buf:
            fbuf.pixel(x, y, c)

    fbuf.fill(0)
    cx, cy = WIDTH // 2, h_buf // 2
    for r in range(20, min(WIDTH, h_buf) // 2 - 10, 15):
        c = _color(255, 255 - r, r)
        x, y, err = r, 0, 0
        while x >= y:
            for dx, dy in ((x, y), (y, x), (-x, y), (-y, x), (-x, -y), (-y, -x), (x, -y), (y, -x)):
                _pixel(cx + dx, cy + dy, c)
            y += 1; err += 1 + 2 * y
            if 2 * (err - x) + 1 > 0:
                x -= 1; err += 1 - 2 * x
    _lcd.show(buf, 0, (HEIGHT - h_buf) // 2, WIDTH, h_buf)
    time.sleep_ms(1500)

    fbuf.fill(0)
    for _ in range(40):
        a = random.uniform(0, 2 * math.pi)
        rl = random.randint(30, min(WIDTH, h_buf) // 2 - 10)
        ex = int(cx + math.cos(a) * rl)
        ey = int(cy + math.sin(a) * rl)
        c = _color(random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
        x0, y0 = cx, cy
        dx, dy = abs(ex - x0), -abs(ey - y0)
        sx = 1 if x0 < ex else -1
        sy = 1 if y0 < ey else -1
        err = dx + dy
        while True:
            _pixel(x0, y0, c)
            if x0 == ex and y0 == ey:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy; x0 += sx
            if e2 <= dx:
                err += dx; y0 += sy
    _lcd.show(buf, 0, (HEIGHT - h_buf) // 2, WIDTH, h_buf)
    time.sleep_ms(1500)
    _clear()
    print("shapes done")


def animate():
    """彈跳球 + 星空 — 走 TFT.show 統一介面"""
    import framebuf
    buf = bytearray(WIDTH * HEIGHT * 2)
    fbuf = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    print("animate: bouncing ball")
    bx, by = WIDTH // 2, HEIGHT // 2
    bdx, bdy = 5, 4
    fbuf.fill(0)
    for _ in range(120):
        fbuf.fill_rect(max(0, bx - 17), max(0, by - 17), 34, 34, 0)
        bx += bdx; by += bdy
        if bx <= 0 or bx >= WIDTH - 1:
            bdx = -bdx
        if by <= 0 or by >= HEIGHT - 1:
            bdy = -bdy
        fbuf.ellipse(bx, by, 15, 15, _color(255, 255, 0), True)
        _lcd.show(buf)
        time.sleep_ms(8)

    print("animate: starfield")
    stars = [(random.randint(0, WIDTH - 1), random.randint(0, HEIGHT - 1),
              random.randint(1, 3)) for _ in range(60)]
    fbuf.fill(0)
    for _ in range(120):
        for i, (sx, sy, spd) in enumerate(stars):
            fbuf.pixel(sx, sy, 0)
            sx = (sx + spd) % WIDTH
            stars[i] = (sx, sy, spd)
            fbuf.pixel(sx, sy, _color(spd * 80, spd * 80, spd * 80 + 60))
        _lcd.show(buf)
        time.sleep_ms(15)
    _clear()
    print("animate done")


# ══════════════════════════════════════════════════════════════
#  性能 / FPS 測試 (統一介面)
# ══════════════════════════════════════════════════════════════

def fps_test(frames=50):
    """黑白交替整幀 FPS — adapter.write_frame_dma 純吞吐。
    DSI 預期 ~60+ FPS (顯示上限 65Hz); SPI 視頻寬而定。"""
    gc.collect()
    total = WIDTH * HEIGHT * 2
    full_w = memoryview(bytearray(b'\xff\xff' * (total // 2))[:total])
    full_b = memoryview(bytearray(total))
    _lcd.set_window(0, 0, WIDTH - 1, HEIGHT - 1)
    t0 = time.ticks_us()
    for n in range(frames):
        tids = _lcd._bus.write_frame_dma(full_w if n & 1 else full_b)
        for tid in tids:
            _lcd._bus.wait(tid)
    elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
    fps = frames / elapsed
    mbps = total * frames / elapsed / (1024 * 1024)
    print("FPS(adapter): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
        fps, elapsed / frames * 1000, mbps))


def fps_test_tft(frames=50):
    """黑白交替 FPS — TFT.show_frame() 層 (library 開銷)"""
    gc.collect()
    total = WIDTH * HEIGHT * 2
    full_w = memoryview(bytearray(b'\xff\xff' * (total // 2))[:total])
    full_b = memoryview(bytearray(total))
    _lcd.set_window(0, 0, WIDTH - 1, HEIGHT - 1)
    t0 = time.ticks_us()
    for n in range(frames):
        _lcd.show_frame(full_w if n & 1 else full_b)
    elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
    fps = frames / elapsed
    mbps = total * frames / elapsed / (1024 * 1024)
    print("FPS(TFT): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
        fps, elapsed / frames * 1000, mbps))


def fps_test_present(frames=50):
    """begin_display + present() 管線 FPS — DMA 與下一幀準備重疊"""
    gc.collect()
    total = WIDTH * HEIGHT * 2
    full_w = memoryview(bytearray(b'\xff\xff' * (total // 2))[:total])
    full_b = memoryview(bytearray(total))
    _lcd.begin_display()
    t0 = time.ticks_us()
    for n in range(frames):
        _lcd.present(full_w if n & 1 else full_b)
        _lcd.present_wait()
    elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
    fps = frames / elapsed
    mbps = total * frames / elapsed / (1024 * 1024)
    print("FPS(present): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
        fps, elapsed / frames * 1000, mbps))


def flush_bench():
    """flush()/wait_all() 代價實測"""
    gc.collect()
    total = WIDTH * HEIGHT * 2
    c0 = bytearray(total)
    c1 = bytearray(b'\xff\x00' * (WIDTH * HEIGHT))

    t0 = time.ticks_ms()
    for i in range(10):
        _lcd._bus.write_data_async(c0 if i & 1 else c1)
        _lcd._bus.flush()
    t1 = time.ticks_ms()
    d = time.ticks_diff(t1, t0)
    print("  fill+flush 一輪 ×10 = {}ms → {:.1f} ms/輪".format(d, d / 10))

    t0 = time.ticks_ms()
    for _ in range(50):
        _lcd._bus.flush()
    t1 = time.ticks_ms()
    d = time.ticks_diff(t1, t0)
    print("  空 flush ×50 = {}ms → {:.3f} ms/次".format(d, d / 50))


# ══════════════════════════════════════════════════════════════
#  entry
# ══════════════════════════════════════════════════════════════

def run_all():
    """依序: FPS 三種路徑 → flush 代價 → 目視測試"""
    init_tft()
    try:
        fps_test(50)
        _clear(); time.sleep_ms(300)
        fps_test_tft(50)
        _clear(); time.sleep_ms(300)
        fps_test_present(50)
        _clear(); time.sleep_ms(300)
        flush_bench()
        _clear(); time.sleep_ms(300)
        fill_colors()
        _clear(); time.sleep_ms(300)
        color_bars()
        _clear(); time.sleep_ms(300)
        gradient()
        _clear(); time.sleep_ms(300)
        checkerboard()
        _clear(); time.sleep_ms(300)
        shapes()
        _clear(); time.sleep_ms(300)
        animate()
        _clear()
        print("=== all tests done ===")
    finally:
        deinit_tft()


if __name__ == "__main__":
    run_all()
