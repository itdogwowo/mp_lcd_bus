# tft_test_tool.py — 全能 TFT 測試工具 (所有 bus + 所有面板)
#
# 架構 (三層解耦, 每層只吃上一層的產物, 互不綁定):
#   Layer 1  create_bus(cfg)             → (bus, adapter, holds)  # 電源/reset + bus + adapter
#   Layer 2  create_panel(cfg, adapter)  → tft                    # 依 driver 名建面板驅動
#   Layer 3  TftTest(tft, w, h, ...)     → run_all()              # 測試只認 tft 物件
#   Runner   run(board)                  → config → bus → panel → 測試 → deinit
#
# 一板一 config: boards/<板名>/config.json — 自行增刪板子, 每次只讀一個 config。
#
# 用法:
#   import tft_test_tool
#   tft_test_tool.run()                            # 讀根目錄 config.json (上傳哪塊測哪塊)
#   tft_test_tool.run(config_path="my.json")       # 或直接給 config 路徑
#   tft_test_tool.run("dsi_jd9165_1024x600")       # 或從 boards/<名稱>/config.json 讀
#   tft_test_tool.list_boards()                    # 看 boards/ 下有哪些範本
#
# 也可逐層手動組合 (不透過 Runner):
#   cfg = tft_test_tool.load_board("spi_st7789_240x320")
#   bus, adapter, holds = tft_test_tool.create_bus(cfg)
#   tft = tft_test_tool.create_panel(cfg, adapter)
#   ... 自己的應用 ...
#   bus.deinit()
#
# 支援 bus : spi / i80 / rgb / dsi / i2c (adapter 全部在 lib/bus_adapter.py)
# 支援面板: ST7735 / ST7789 / ST7796 / GC9A01 / ILI9341 / GC9D01 /
#           NV3030B / RM67162 / SH8601 / JD9165 / ST7701 (lib/TFT.py)
#
# 測試項目 (全部走 TFT 統一介面, 任何 bus/面板通用):
#   fill_colors / color_bars / gradient / checkerboard / shapes / animate (目視)
#   fps_test            — adapter.write_frame_dma 純吞吐
#   fps_test_tft        — TFT.show_frame 層
#   fps_test_present    — begin_display + present 管線
#   fps_test_blit       — 整頁原子 (page-flip)
#   fps_breakdown / vsync_period — DSI 分段計時診斷
#   flush_bench         — flush/wait_all 代價

import gc, time, math, random, json, os

BOARDS_DIR = "boards"


# ══════════════════════════════════════════════════════════════
#  config — 一板一檔 (boards/<板名>/config.json)
# ══════════════════════════════════════════════════════════════

def board_config_path(board, base=BOARDS_DIR):
    return "{}/{}/config.json".format(base, board)


def load_board(board=None, config_path=None):
    """讀取一個板子的 config。
    預設 → 根目錄 config.json (主機端選好上傳哪塊就測哪塊)
    board="名稱" → boards/<名稱>/config.json
    config_path → 直接給路徑"""
    if config_path is None:
        if board is None:
            config_path = "config.json"
        else:
            config_path = board_config_path(board)
    try:
        with open(config_path, "r") as f:
            cfg = json.load(f)
    except OSError:
        print("  找不到 {}".format(config_path))
        if board is None:
            print("  提示: 上傳目標板子的 config 到根目錄 config.json,")
            print("        或 run(\"板子名\") 從 boards/<板子名>/config.json 讀")
            list_boards()
        raise
    # 相容舊格式 {"LCD": {"profiles": {...}}} — 取第一個 profile
    if "LCD" in cfg:
        profs = cfg["LCD"]["profiles"]
        name = cfg["LCD"].get("active") or list(profs.keys())[0]
        cfg = profs[name]
    cfg.setdefault("width", int(cfg["width"]))
    cfg.setdefault("height", int(cfg["height"]))
    return cfg


def list_boards(base=BOARDS_DIR):
    """列出 boards/ 下所有板子 config"""
    try:
        names = os.listdir(base)
    except OSError:
        print("boards/ 目錄不存在")
        return []
    found = []
    for n in sorted(names):
        path = board_config_path(n, base)
        try:
            with open(path, "r") as f:
                cfg = json.load(f)
        except Exception:
            continue
        found.append(n)
        print("  {:28s} bus={:4s} driver={:8s} {}x{}  {}".format(
            n, cfg.get("bus", "?"), cfg.get("driver", "?"),
            cfg.get("width"), cfg.get("height"), cfg.get("desc", "")))
    return found


# ══════════════════════════════════════════════════════════════
#  Layer 1 — bus 建立 (電源/reset 前置 + bus + adapter)
# ══════════════════════════════════════════════════════════════

def _pin(n):
    from machine import Pin
    return Pin(n, Pin.OUT, value=1) if n >= 0 else None


def _ch32v003_init(step):
    """CH32V003 IO expander 開機序列 — 對齊 WS_CH32_IO::initDisplayPower:
    DIR=0xFF (全輸出) → OUT=0x00 (reset) → 200ms → OUT=SYS_EN|LCD_RST|TP_RST。
    addr/sda/scl/freq 可透過 config 覆寫 (預設 0x24 / 15 / 7 / 400kHz)。"""
    import machine
    addr = step.get("addr", 0x24)
    i2c = machine.I2C(step.get("id", 0), sda=machine.Pin(step.get("sda", 15)),
                      scl=machine.Pin(step.get("scl", 7)),
                      freq=step.get("freq", 400000))

    def _wr(reg, val):
        i2c.writeto(addr, bytes([reg, val]))

    for _ in range(3):
        if addr not in i2c.scan():
            time.sleep_ms(20)
            continue
        _wr(0x02, 0xFF)      # REG_DIRECTION
        _wr(0x03, 0x00)      # REG_OUTPUT = reset
        time.sleep_ms(200)
        _wr(0x02, 0xFF)
        _wr(0x03, 0x2C)      # SYS_EN | LCD_RST | TP_RST → display on
        time.sleep_ms(200)
        print("  pre: CH32V003 @0x{:02X} OK".format(addr))
        if "pwm" in step:
            _wr(0x05, step["pwm"])     # REG_PWM → 背光
        return i2c
    raise RuntimeError("CH32V003 @0x{:02X} no ack on I2C{}".format(addr, step.get("id", 0)))


def _run_pre(pre_list):
    """執行 bus 建立前的電源/reset 前置動作 (config "pre" 列表)。

    支援 type:
      ldo      — esp32.LDO 電源 (P4 DSI PHY)
      pin      — GPIO 輸出拉高 (背光/EN)
      ch32v003 — CH32V003 IO expander I2C 初始化 (ESP32-S3-Touch-LCD-4:
                 面板 reset + 系統電源開機序列), 可選 pwm 背光
    回傳需長期持有引用的物件列表 (防 GC 斷電)。"""
    holds = []
    for step in pre_list:
        t = step.get("type")
        if t == "ldo":
            from esp32 import LDO
            holds.append(LDO(channel_id=step["chan"], voltage_mv=step["mv"]))
        elif t == "pin":
            holds.append(_pin(step["gpio"]))
        elif t == "ch32v003":
            holds.append(_ch32v003_init(step))
        else:
            print("  pre: unknown type '{}' skip".format(t))
    return holds


def create_bus(cfg):
    """Layer 1: 依 config 建 (raw bus, adapter, holds)。

    holds = 電源/背光等需長期持有引用的物件 (防 GC 斷電), caller 保留到 deinit。
    adapter 是 lib/bus_adapter.py 的統一介面, 供 Layer 2 面板驅動使用。"""
    import lcd_bus
    from lib import bus_adapter as BA

    bus_type = cfg.get("bus", "spi")
    width, height = int(cfg["width"]), int(cfg["height"])
    holds = []
    if cfg.get("pre"):
        holds = _run_pre(cfg["pre"])

    if bus_type == "dsi":
        c = cfg["dsi"]
        bus = lcd_bus.DSIBus(
            lanes=c["lanes"], width=width, height=height,
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
        adapter = BA.DsiBusAdapter(bus, width, height)

    elif bus_type == "spi":
        c = cfg["spi"]
        bus = lcd_bus.SPIBus(data=c["data"], clk=c["clk"],
                             freq=c["freq"], host=c["host"])
        adapter = BA.SpiBusAdapter(bus, dc=_pin(c["dc"]), cs=_pin(c["cs"]),
                                   rst=_pin(c["rst"]))

    elif bus_type == "i80":
        c = cfg["i80"]
        bus = lcd_bus.I80Bus(data=c["data"], wr=c["wr"], dc=c["dc"],
                             cs=c["cs"], freq=c["freq"])
        adapter = BA.I80BusAdapter(bus, dcx=_pin(c["dc"]), rst=_pin(c["rst"]))

    elif bus_type == "rgb":
        c = cfg["rgb"]
        kw = dict(
            data=c["data"], hsync=c["hsync"], vsync=c["vsync"],
            de=c["de"], pclk=c["pclk"], width=width, height=height,
            freq=c["freq"], disp=c.get("disp", -1),
            hsync_pulse_width=c.get("hsync_pulse_width", 1),
            hsync_back_porch=c.get("hsync_back_porch", 0),
            hsync_front_porch=c.get("hsync_front_porch", 0),
            vsync_pulse_width=c.get("vsync_pulse_width", 1),
            vsync_back_porch=c.get("vsync_back_porch", 0),
            vsync_front_porch=c.get("vsync_front_porch", 0),
            hsync_idle_low=bool(c.get("hsync_idle_low", 0)),
            vsync_idle_low=bool(c.get("vsync_idle_low", 0)),
            pclk_active_neg=bool(c.get("pclk_active_neg", 0)),
        )
        if "bb_size_px" in c:
            kw["bb_size_px"] = c["bb_size_px"]
        if "queue_depth" in c:
            kw["queue_depth"] = c["queue_depth"]
        bus = lcd_bus.RGBBus(**kw)
        adapter = BA.RgbBusAdapter(bus, width, height)

    elif bus_type == "i2c":
        c = cfg["i2c"]
        bus = lcd_bus.I2CBus(data=c["data"], clk=c["clk"], addr=c["addr"])
        adapter = BA.I2cBusAdapter(bus, c["addr"], rst=_pin(c["rst"]),
                                   cmd_ctrl=c["cmd_ctrl"], data_ctrl=c["data_ctrl"])

    else:
        raise ValueError("bus must be spi/i80/rgb/dsi/i2c, got '{}'".format(bus_type))

    return bus, adapter, holds


# ══════════════════════════════════════════════════════════════
#  Layer 2 — 面板驅動 (只依 config 的 driver 名 + adapter)
# ══════════════════════════════════════════════════════════════

def _panel_kw(cfg):
    """面板共用 kwargs (rotation/color_order/invert — config 選填)"""
    kw = {}
    if int(cfg.get("rotation", 0)):
        kw["rotation"] = int(cfg["rotation"])
    if cfg.get("color_order", "RGB") != "RGB":
        kw["color_order"] = cfg["color_order"]
    if int(cfg.get("invert", 0)):
        kw["invert"] = True
    return kw


def create_panel(cfg, adapter):
    """Layer 2: 依 config["driver"] 從 lib/TFT.py 取驅動類別建面板。
    各 bus 的建構差異 (DSI/RGB 走 adapter 位置參數; SPI/I80/I2C 走 kwargs)
    封裝在此 — 上層不需要知道。"""
    from lib import TFT as TFTM

    drv = getattr(TFTM, cfg["driver"])
    width, height = int(cfg["width"]), int(cfg["height"])
    bus_type = cfg.get("bus", "spi")
    pf = cfg.get("pixel_format", "RGB565_BE")

    if bus_type == "dsi":
        # video mode 面板 (JD9165): adapter 位置參數
        return drv(adapter, width, height)

    if bus_type == "rgb":
        kw = _panel_kw(cfg)
        # ST7701 等 RGB 面板需 3-wire 控制 SPI 做 init
        if cfg.get("ctrl"):
            c = cfg["ctrl"]
            kw["ctrl"] = TFTM.ST7701CtrlSPI(cs=c["cs"], sck=c["sck"], mosi=c["mosi"])
        rgb = cfg.get("rgb") or {}
        if rgb.get("colmod") is not None:
            kw["colmod"] = rgb["colmod"]
        return drv(adapter=adapter, width=width, height=height,
                   pixel_format=pf, **kw)

    if bus_type == "i2c":
        return drv(adapter=adapter, width=width, height=height, pixel_format=pf)

    # spi / i80 — 命令式面板, 帶 variant
    bc = cfg.get(bus_type) or {}
    return drv(adapter=adapter, width=width, height=height,
               pixel_format=pf, variant=int(bc.get("variant", 0)), **_panel_kw(cfg))


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


def _alloc_psram_frame(size):
    """整幀 buffer 放 PSRAM — DSI write() 對 PSRAM src 零拷貝直進 PPA 硬體 blit。
    DRAM bytearray 會被 C 層先 CPU memcpy 到 PSRAM bounce, 拖慢 write()。
    自動路由: PSRAM 滿 → gc.collect() 重試 (heap_caps buffer 靠 GC 回收)
    → 仍失敗才掉 DRAM bytearray (C 層會自動 bounce, 只慢不壞)。"""
    for _attempt in range(2):
        try:
            import heap_caps
            b = heap_caps.malloc(size, heap_caps.CAP_SPIRAM)
            if b is not None:
                return b
        except ImportError:
            break
        except MemoryError:
            pass
        gc.collect()
    return bytearray(size)


def _make_test_frames(total):
    """白/黑整幀 (PSRAM) — FPS 測試用, 預先填好, 計時迴圈零分配"""
    full_w = _alloc_psram_frame(total)
    full_w[:] = b'\xff\xff' * (total // 2)
    full_b = _alloc_psram_frame(total)
    full_b[:] = bytes(total)
    return full_w, full_b


# ══════════════════════════════════════════════════════════════
#  Layer 3 — TftTest: 測試只認 (tft, w, h), 不碰 config/bus 細節
# ══════════════════════════════════════════════════════════════

class TftTest:
    """一套測試綁定一個 tft 物件 — 同一套測試可用在任何板子/bus。

    tft       : Layer 2 產物 (lib/TFT.py 面板驅動)
    raw_bus   : Layer 1 的原始 C bus (供 DSI 診斷 / deinit; 選填)
    bus_type  : "spi"/"i80"/"rgb"/"dsi"/"i2c" (診斷測試分派用)"""

    def __init__(self, tft, width, height, bus_type="spi", raw_bus=None):
        self.tft = tft
        self.w = width
        self.h = height
        self.bus_type = bus_type
        self.raw = raw_bus

    # ── 基礎 ─────────────────────────────────────────────

    def _solid_frame(self, color565):
        """整幀單色 buffer (PSRAM) — 快取複用同一塊, 每次只重填內容。
        避免每次填色都 alloc/free 1MB+ 導致 heap 碎片化 MemoryError。"""
        total = self.w * self.h * 2
        buf = getattr(self, "_solid_buf", None)
        if buf is None or len(buf) < total:
            gc.collect()
            buf = _alloc_psram_frame(total)
            self._solid_buf = buf
        buf[:2] = bytes([color565 >> 8, color565 & 0xFF])
        n = 2
        while n < total:
            m = min(n * 2, total)        # 最後一段不足 2n 時收斂, 避免 lhs/rhs 長度不符
            buf[n:m] = buf[:m - n]
            n = m
        return buf

    def _fill_solid(self, color565):
        """全螢幕填色 — 組整幀 → blit (整頁原子更新, 零撕裂)"""
        self.tft.blit(self._solid_frame(color565))

    def _clear(self):
        self._fill_solid(0x0000)

    # ── 目視測試 ─────────────────────────────────────────

    def fill_colors(self):
        """九色全螢幕填滿"""
        gc.collect()
        colors = [
            ("RED", 0xF800), ("GREEN", 0x07E0), ("BLUE", 0x001F),
            ("YELLOW", 0xFFE0), ("CYAN", 0x07FF), ("MAGENTA", 0xF81F),
            ("WHITE", 0xFFFF), ("GRAY", 0x8410), ("BLACK", 0x0000),
        ]
        for name, c in colors:
            print("  %s (0x%04X) ..." % (name, c))
            self._fill_solid(c)
            time.sleep_ms(400)
        print("fill_colors done")

    def color_bars(self):
        """八色水平帶 — 組整幀 → blit (整頁原子)"""
        W, H = self.w, self.h
        bar_h = H // 8
        palette = [0xF800, 0x07E0, 0x001F, 0xFFFF, 0xFFE0, 0x07FF, 0xF81F, 0x0000]
        buf = _alloc_psram_frame(W * H * 2)
        mv = memoryview(buf)
        rowbytes = W * 2
        for i, c in enumerate(palette):
            y0, y1 = i * bar_h, (i + 1) * bar_h if i < 7 else H
            band = bytes([c >> 8, c & 0xFF]) * (W * (y1 - y0))   # C 速度填整帶
            mv[y0 * rowbytes:y1 * rowbytes] = band
        self.tft.blit(buf)
        time.sleep_ms(1200)
        print("color_bars done")

    def gradient(self):
        """RGB 水平漸變 — 組整幀 → blit (整頁原子)"""
        gc.collect()
        W, H = self.w, self.h
        row = bytearray(W * 2)
        for x in range(W):
            r = int(x * 255 / W)
            g = int((1 - abs(x - W / 2) / (W / 2)) * 255)
            b = int((W - x) * 255 / W)
            c = _color(r, g, b)
            row[x * 2] = c >> 8
            row[x * 2 + 1] = c & 0xFF

        buf = _alloc_psram_frame(W * H * 2)
        for y in range(H):
            off = y * W * 2
            buf[off:off + len(row)] = row
        self.tft.blit(buf)
        time.sleep_ms(1500)
        print("gradient done")

    def checkerboard(self):
        """棋盤格 (40x40) — 組整幀 → blit (整頁原子)。
        只建兩種行 (白起始/黑起始) 再逐行切片複製, 避免逐字節 Python 迴圈。"""
        W, H = self.w, self.h
        sq = 40
        rowbytes = W * 2

        def _make_row(start_white):
            r = bytearray(rowbytes)
            for bx in range(0, W, sq):
                is_w = ((bx // sq) % 2 == 0) == start_white
                hi, lo = (0xFF, 0xFF) if is_w else (0x00, 0x00)
                x0, x1 = bx * 2, min(bx + sq, W) * 2     # ⚠ 最後一塊可能不足 sq
                r[x0:x1] = bytes([hi, lo]) * ((x1 - x0) // 2)
            return bytes(r)

        even_row = _make_row(True)
        odd_row = _make_row(False)
        buf = _alloc_psram_frame(W * H * 2)
        mv = memoryview(buf)
        for y in range(H):
            mv[y * rowbytes:(y + 1) * rowbytes] = even_row if (y // sq) % 2 == 0 else odd_row
        self.tft.blit(buf)
        time.sleep_ms(1500)
        print("checkerboard done")

    def shapes(self):
        """同心圓 + 放射線 (framebuf) — 整幀 blit, 圖形垂直置中"""
        import framebuf
        W, H = self.w, self.h
        h_draw = min(H, 400)
        yoff = (H - h_draw) // 2          # 圖形在整幀中的垂直偏移 (置中)
        buf = _alloc_psram_frame(W * H * 2)
        fbuf = framebuf.FrameBuffer(buf, W, H, framebuf.RGB565)

        def _pixel(x, y, c):
            if 0 <= x < W and 0 <= y < h_draw:
                fbuf.pixel(x, y + yoff, c)

        fbuf.fill(0)
        cx, cy = W // 2, h_draw // 2
        for r in range(20, min(W, h_draw) // 2 - 10, 15):
            c = _color(255, 255 - r, r)
            x, y, err = r, 0, 0
            while x >= y:
                for dx, dy in ((x, y), (y, x), (-x, y), (-y, x), (-x, -y), (-y, -x), (x, -y), (y, -x)):
                    _pixel(cx + dx, cy + dy, c)
                y += 1; err += 1 + 2 * y
                if 2 * (err - x) + 1 > 0:
                    x -= 1; err += 1 - 2 * x
        self.tft.blit(buf)
        time.sleep_ms(1500)

        fbuf.fill(0)
        for _ in range(40):
            a = random.uniform(0, 2 * math.pi)
            rl = random.randint(30, min(W, h_draw) // 2 - 10)
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
        self.tft.blit(buf)
        time.sleep_ms(1500)
        self._clear()
        print("shapes done")

    def animate(self):
        """彈跳球 + 星空 — 走 TFT.blit 統一介面"""
        import framebuf
        W, H = self.w, self.h
        buf = _alloc_psram_frame(W * H * 2)
        fbuf = framebuf.FrameBuffer(buf, W, H, framebuf.RGB565)

        print("animate: bouncing ball")
        bx, by = W // 2, H // 2
        bdx, bdy = 5, 4
        fbuf.fill(0)
        for _ in range(120):
            fbuf.fill_rect(max(0, bx - 17), max(0, by - 17), 34, 34, 0)
            bx += bdx; by += bdy
            if bx <= 0 or bx >= W - 1:
                bdx = -bdx
            if by <= 0 or by >= H - 1:
                bdy = -bdy
            fbuf.ellipse(bx, by, 15, 15, _color(255, 255, 0), True)
            self.tft.blit(buf)
            time.sleep_ms(8)

        print("animate: starfield")
        stars = [(random.randint(0, W - 1), random.randint(0, H - 1),
                  random.randint(1, 3)) for _ in range(60)]
        fbuf.fill(0)
        for _ in range(120):
            for i, (sx, sy, spd) in enumerate(stars):
                fbuf.pixel(sx, sy, 0)
                sx = (sx + spd) % W
                stars[i] = (sx, sy, spd)
                fbuf.pixel(sx, sy, _color(spd * 80, spd * 80, spd * 80 + 60))
            self.tft.blit(buf)
            time.sleep_ms(15)
        self._clear()
        print("animate done")

    # ── 性能 / FPS 測試 ──────────────────────────────────

    def fps_test(self, frames=50):
        """黑白交替整幀 FPS — adapter.write_frame_dma 純吞吐。
        DSI 預期 ~60+ FPS (顯示上限 ~65Hz); SPI 視頻寬而定。"""
        gc.collect()
        total = self.w * self.h * 2
        full_w, full_b = _make_test_frames(total)
        self.tft.set_window(0, 0, self.w - 1, self.h - 1)
        t0 = time.ticks_us()
        for n in range(frames):
            tids = self.tft._bus.write_frame_dma(full_w if n & 1 else full_b)
            for tid in tids:
                self.tft._bus.wait(tid)
        elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
        fps = frames / elapsed
        mbps = total * frames / elapsed / (1024 * 1024)
        print("FPS(adapter): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
            fps, elapsed / frames * 1000, mbps))

    def fps_test_tft(self, frames=50):
        """黑白交替 FPS — TFT.show_frame() 層 (library 開銷)"""
        gc.collect()
        total = self.w * self.h * 2
        full_w, full_b = _make_test_frames(total)
        self.tft.set_window(0, 0, self.w - 1, self.h - 1)
        t0 = time.ticks_us()
        for n in range(frames):
            self.tft.show_frame(full_w if n & 1 else full_b)
        elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
        fps = frames / elapsed
        mbps = total * frames / elapsed / (1024 * 1024)
        print("FPS(TFT): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
            fps, elapsed / frames * 1000, mbps))

    def fps_test_present(self, frames=50):
        """begin_display + present() 管線 FPS — DMA 與下一幀準備重疊"""
        gc.collect()
        total = self.w * self.h * 2
        full_w, full_b = _make_test_frames(total)
        self.tft.begin_display()
        t0 = time.ticks_us()
        for n in range(frames):
            self.tft.present(full_w if n & 1 else full_b)
            self.tft.present_wait()
        elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
        fps = frames / elapsed
        mbps = total * frames / elapsed / (1024 * 1024)
        print("FPS(present): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
            fps, elapsed / frames * 1000, mbps))

    def fps_test_blit(self, frames=50):
        """整頁原子 (blit/page-flip) FPS — memcpy 離屏 + 幀邊界切換。

        DSI: write() PPA blit 進後台 fb + present() 等幀邊界 (零撕裂)。
        SPI/其他: RAMWR 整幀, 無 page-flip。"""
        gc.collect()
        total = self.w * self.h * 2
        full_w, full_b = _make_test_frames(total)
        t0 = time.ticks_us()
        for n in range(frames):
            self.tft.blit(full_w if n & 1 else full_b)
        elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
        fps = frames / elapsed
        mbps = total * frames / elapsed / (1024 * 1024)
        print("FPS(blit): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
            fps, elapsed / frames * 1000, mbps))

    def fps_test_pipeline(self, frames=50):
        """3-fb 管線 FPS — DSI fb_count>=3 專用。

        CPU memcpy 直寫閒置 fb + 3 緩衝輪轉,讓「拷貝」與「DMA 掃上一幀」重疊。
        對照 FPS(blit) (PPA 序列路徑) 驗證管線化效益。目標逼近面板上限 (~65 FPS)。"""
        if self.bus_type != "dsi":
            print("FPS(pipeline): 僅 DSI 適用, 略過")
            return
        if not getattr(self.tft._bus, "_pipeline", False):
            print("FPS(pipeline): 需 fb_count>=3, 略過")
            return
        gc.collect()
        total = self.w * self.h * 2
        full_w, full_b = _make_test_frames(total)
        bus = self.tft._bus
        raw = bus._bus                       # 原始 C DSIBus (診斷用)
        dbg = hasattr(raw, "_pl_state")
        if dbg:
            print("    [dbg] pre-loop _pl_state={}".format(raw._pl_state()))
        t0 = time.ticks_us()
        for n in range(frames):
            if dbg:
                print("    [dbg] #{} pre-submit state={}".format(n, raw._pl_state()))
            try:
                tid = bus.blit_pipeline(full_w if n & 1 else full_b)
            except RuntimeError as e:
                print("    [dbg] #{} blit_pipeline RAISE: {} state={}".format(
                    n, e, raw._pl_state()))
                return
            if dbg:
                print("    [dbg] #{} post-submit tid={} state={}".format(n, tid, raw._pl_state()))
            bus.blit_pipeline_wait(tid)
            if dbg:
                print("    [dbg] #{} post-wait state={}".format(n, raw._pl_state()))
            if n >= 2 and dbg:
                dbg = False                  # 印前 3 幀就好, 別洗版
        elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
        fps = frames / elapsed
        mbps = total * frames / elapsed / (1024 * 1024)
        print("FPS(pipeline): {:.0f}  ({:.1f} ms/frame, {:.1f} MB/s)".format(
            fps, elapsed / frames * 1000, mbps))

    def fps_breakdown(self, frames=30):
        """DSI 分段計時 — 定位 write() 與等幀邊界各佔多少。
        t_write 大 → blit/cache 是瓶頸; t_wait 大且 ≈ 整數倍幀週期 → 相位鎖定。"""
        if self.bus_type != "dsi" or self.raw is None:
            print("  breakdown: 僅 DSI 適用, 略過")
            return
        gc.collect()
        total = self.w * self.h * 2
        full_w, full_b = _make_test_frames(total)
        self.raw.set_window(0, 0, self.w - 1, self.h - 1)
        t_w = t_p = 0
        t0 = time.ticks_us()
        for n in range(frames):
            ta = time.ticks_us()
            self.raw.write(full_w if n & 1 else full_b)
            tb = time.ticks_us()
            tid = self.raw.present()
            self.raw.wait(tid)
            tc = time.ticks_us()
            t_w += time.ticks_diff(tb, ta)
            t_p += time.ticks_diff(tc, tb)
        tot = time.ticks_diff(time.ticks_us(), t0)
        print("  breakdown: write={:.1f} ms  present+wait={:.1f} ms  total={:.1f} ms/幀".format(
            t_w / frames / 1000, t_p / frames / 1000, tot / frames / 1000))

    def vsync_period(self, frames=50):
        """面板真實幀週期 — 迴圈內只 present 不寫像素, wait 時間 = 純幀邊界間隔。
        若 FPS(vsblank) ≈ FPS(adapter) → 面板刷新率就是上限, 非軟體瓶頸。"""
        if self.bus_type != "dsi" or self.raw is None:
            print("  vsblank: 僅 DSI 適用, 略過")
            return
        gc.collect()
        self.tft.set_window(0, 0, self.w - 1, self.h - 1)
        # 先翻一次讓 cur_fb 穩定
        tid = self.raw.present()
        self.raw.wait(tid)
        t0 = time.ticks_us()
        for _ in range(frames):
            tid = self.raw.present()
            self.raw.wait(tid)
        elapsed = time.ticks_diff(time.ticks_us(), t0) / 1_000_000
        print("FPS(vsblank): {:.0f}  ({:.2f} ms/frame)".format(frames / elapsed, elapsed / frames * 1000))

    def dsi_writepath_breakdown(self, frames=30):
        """DSI write 路徑分段量測 — 量化「拷貝 vs 翻頁」各佔多少,決定修法方向。

        目標:確認 write=25.6ms 是 PPA blit 本身,還是 bounce memcpy / cache 開銷,
        以及「直寫 fb + present」是否比「write(PPA) + present」更快。
        每項輸出 ms/幀 (MB/s);MB/s 以幀位元組數計算 (僅 #1-3,#5 有像素搬運)。"""
        if self.bus_type != "dsi" or self.raw is None:
            print("  writepath breakdown: 僅 DSI 適用, 略過")
            return
        gc.collect()
        total = self.w * self.h * 2
        raw = self.raw
        mb = total / 1024 / 1024

        def _ms_per_frame(elapsed_us):
            return elapsed_us / frames / 1000

        def _mbps(elapsed_us):
            s = elapsed_us / 1_000_000
            return mb * frames / s if s > 0 else 0.0

        # ── #1 PPA blit, PSRAM src (現況 write() 路徑, 零 bounce) ──
        psram_src = _alloc_psram_frame(total)
        psram_src[:] = b'\xff\xff' * (total // 2)
        raw.set_window(0, 0, self.w - 1, self.h - 1)
        t0 = time.ticks_us()
        for _ in range(frames):
            raw.write(psram_src)
        t1_a = time.ticks_diff(time.ticks_us(), t0)
        t1_ms = _ms_per_frame(t1_a)
        print("  #1 write PPA (PSRAM src) : {:.2f} ms/幀  ({:.1f} MB/s)".format(t1_ms, _mbps(t1_a)))

        # ── #2 PPA blit, DRAM src (強制走 bounce memcpy 路徑) ──
        #   P4 內部 DRAM ≈ 700KB < 1.17MB 整幀 → 配不到時降級告知, 不中斷量測。
        dram_src = None
        try:
            import heap_caps
            dram_src = heap_caps.malloc(total, heap_caps.CAP_INTERNAL | heap_caps.CAP_8BIT)
        except Exception:
            pass
        if dram_src is not None:
            dram_src[:] = b'\x00\xf8' * (total // 2)
            raw.set_window(0, 0, self.w - 1, self.h - 1)
            t0 = time.ticks_us()
            for _ in range(frames):
                raw.write(dram_src)
            t2_a = time.ticks_diff(time.ticks_us(), t0)
            t2_ms = _ms_per_frame(t2_a)
            print("  #2 write PPA (DRAM  src) : {:.2f} ms/幀  ({:.1f} MB/s)  Δ={:+.2f} vs #1 (bounce 代價)".format(
                t2_ms, _mbps(t2_a), t2_ms - t1_ms))
        else:
            print("  #2 write PPA (DRAM  src) : 跳過 (內部 DRAM < {:.2f}MB, 配不到整幀)".format(mb))

        # ── #3 CPU memcpy 直寫 fb (back_buffer view, 不經 PPA) ──
        back_view = raw.back_buffer()
        t0 = time.ticks_us()
        for _ in range(frames):
            back_view[:] = psram_src
        t1 = time.ticks_diff(time.ticks_us(), t0)
        t3_ms = _ms_per_frame(t1)
        print("  #3 back_buf[:]= (CPU memcpy): {:.2f} ms/幀  ({:.1f} MB/s)".format(t3_ms, _mbps(t1)))

        # ── #4 純 present 連續翻頁 (頁面翻轉天花板, 應 ≈ vsblank) ──
        #   先翻一次穩定 cur_fb
        tid = raw.present(); raw.wait(tid)
        t0 = time.ticks_us()
        for _ in range(frames):
            tid = raw.present()
            raw.wait(tid)
        t1 = time.ticks_diff(time.ticks_us(), t0)
        print("  #4 present only (翻頁天花板): {:.2f} ms/幀  ({:.0f} FPS)".format(
            _ms_per_frame(t1), frames / (t1 / 1_000_000) if t1 > 0 else 0))

        # ── #5 back_buffer 直寫 + present 端到端 (對照 write+present) ──
        raw.set_window(0, 0, self.w - 1, self.h - 1)
        t0 = time.ticks_us()
        for _ in range(frames):
            back_view[:] = psram_src
            tid = raw.present()
            raw.wait(tid)
        t1 = time.ticks_diff(time.ticks_us(), t0)
        t5_ms = _ms_per_frame(t1)
        print("  #5 back_buf + present (端到端): {:.2f} ms/幀  ({:.1f} MB/s, {:.0f} FPS)".format(
            t5_ms, _mbps(t1), frames / (t1 / 1_000_000) if t1 > 0 else 0))

        # ── 解讀提示 ──
        print("  ─ 解讀: #1 若 ≈25ms → PPA blit 本身慢 (走管線化); "
              "#3 < #1 → back_buffer 直寫是更快替代; "
              "#2 > #1 → PSRAM 配置關鍵")

    def flush_bench(self):
        """flush()/wait_all() 代價實測"""
        gc.collect()
        total = self.w * self.h * 2
        c0 = bytearray(total)
        c1 = bytearray(b'\xff\x00' * (self.w * self.h))

        t0 = time.ticks_ms()
        for i in range(10):
            self.tft._bus.write_data_async(c0 if i & 1 else c1)
            self.tft._bus.flush()
        t1 = time.ticks_ms()
        d = time.ticks_diff(t1, t0)
        print("  fill+flush 一輪 ×10 = {}ms → {:.1f} ms/輪".format(d, d / 10))

        t0 = time.ticks_ms()
        for _ in range(50):
            self.tft._bus.flush()
        t1 = time.ticks_ms()
        d = time.ticks_diff(t1, t0)
        print("  空 flush ×50 = {}ms → {:.3f} ms/次".format(d, d / 50))

    def test_streaming(self):
        """整頁更新測試 — 8 色水平帶 (整幀組好 → blit)。目視應為乾淨 8 條色帶。"""
        gc.collect()
        W, H = self.w, self.h
        band_h = H // 8
        palette = [0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xFFFF, 0x0000]
        buf = _alloc_psram_frame(W * H * 2)
        mv = memoryview(buf)
        rowbytes = W * 2
        for i, c in enumerate(palette):
            y0, y1 = i * band_h, (i + 1) * band_h if i < 7 else H
            band = bytes([c >> 8, c & 0xFF]) * (W * (y1 - y0))   # C 速度填整帶
            mv[y0 * rowbytes:y1 * rowbytes] = band
        self.tft.blit(buf)
        print("  streaming: 8 色水平帶整幀 blit — 請目視乾淨無漸進")
        time.sleep_ms(1500)

    def test_window(self):
        """整頁更新測試 — 上半紅 / 下半藍 / 中間黃條 (整幀組好 → blit)。"""
        gc.collect()
        W, H = self.w, self.h
        buf = _alloc_psram_frame(W * H * 2)
        mv = memoryview(buf)
        rowbytes = W * 2
        half = H // 2
        mv[0:half * rowbytes] = bytes([0x00, 0xF8]) * (W * half)              # 上半紅
        mv[half * rowbytes:H * rowbytes] = bytes([0x1F, 0x00]) * (W * (H - half))  # 下半藍
        y0, y1 = half - 5, half + 5
        mv[y0 * rowbytes:y1 * rowbytes] = bytes([0xFF, 0xE0]) * (W * (y1 - y0))  # 中間黃條
        self.tft.blit(buf)
        print("  window: 上半紅 / 下半藍 / 中間黃條 (整幀 blit) — 請目視確認")
        time.sleep_ms(1500)

    # ── 整套 ─────────────────────────────────────────────

    def run_all(self):
        """依序: FPS 各路徑 → DSI 診斷 → flush 代價 → 視窗測試 → 目視測試。"""
        self.fps_test(50)
        self._clear(); time.sleep_ms(300)
        self.fps_test_tft(50)
        self._clear(); time.sleep_ms(300)
        self.fps_test_present(50)
        self._clear(); time.sleep_ms(300)
        self.fps_test_blit(50)
        self._clear(); time.sleep_ms(300)
        self.fps_test_pipeline(50)
        self._clear(); time.sleep_ms(300)
        self.fps_breakdown(30)
        self.dsi_writepath_breakdown(30)
        self.vsync_period(50)
        self._clear(); time.sleep_ms(300)
        self.flush_bench()
        self._clear(); time.sleep_ms(300)
        self.test_streaming()
        self._clear(); time.sleep_ms(300)
        self.test_window()
        self._clear(); time.sleep_ms(300)
        self.fill_colors()
        self._clear(); time.sleep_ms(300)
        self.color_bars()
        self._clear(); time.sleep_ms(300)
        self.gradient()
        self._clear(); time.sleep_ms(300)
        self.checkerboard()
        self._clear(); time.sleep_ms(300)
        self.shapes()
        self._clear(); time.sleep_ms(300)
        self.animate()
        self._clear()
        print("=== all tests done ===")


# ══════════════════════════════════════════════════════════════
#  Runner — config → bus → panel → 測試 → deinit
# ══════════════════════════════════════════════════════════════

def _deinit_bus(bus):
    if bus is None:
        return
    try:
        bus.deinit()
        print("  bus deinit 完成")
    except Exception:
        pass


def run(board=None, config_path=None):
    """測一個板子: 讀 config → Layer1 bus → Layer2 panel → Layer3 全套測試。

    board="名稱"      → boards/<名稱>/config.json
    config_path=路徑  → 直接指定 config 檔
    都不給            → 列出 boards/ 下可用板子"""
    cfg = load_board(board, config_path)
    name = cfg.get("name", board or config_path)
    print("=" * 62)
    print("  TFT init: {} — bus={} driver={} {}x{}".format(
        name, cfg.get("bus"), cfg.get("driver"), cfg["width"], cfg["height"]))
    print("=" * 62)

    bus, adapter, holds = create_bus(cfg)
    try:
        tft = create_panel(cfg, adapter)
        print("  {} OK ({}x{})".format(str(cfg.get("bus")).upper(),
                                        cfg["width"], cfg["height"]))
        test = TftTest(tft, int(cfg["width"]), int(cfg["height"]),
                       bus_type=cfg.get("bus", "spi"), raw_bus=bus)
        test.run_all()
    finally:
        _deinit_bus(bus)


if __name__ == "__main__":
    run()
